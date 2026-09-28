// Agents: lifecycle, needs, health, movement and persistence.
#include "icarus/agents/agents.h"
#include "icarus/agents/footprint.h"
#include "icarus/fauna/fauna.h"

#include <algorithm>
#include <cmath>

#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
// Per-character data added in version 2 (see Agents::save).
constexpr u64 kCharBlockVersion = 6;  // 2: age, partner, parents, awakening; 3: empowerment;
                                      // 4: the last fruitless search for food; 5: afloat;
                                      // 6: under way (others keep to the right of her)
}  // namespace

Agents::Agents(SimContext& ctx) : ctx_(ctx) {}

void Agents::reset(u64 seed) {
    rng_.seed(seed, 0xA6E27);
    chars_.clear();
    chars_.resize(1);
    dangers_.clear();
    water_spots_.clear();
    water_regions_.clear();
    region_map_.clear();
    region_anchors_.clear();
    region_built_ = 0;
    survey_ = Survey{};
    ctx_.nav->major_dirty = ctx_.nav->minor_dirty = true;
    day = {};
    fail_ring_.fill(0);
    fail_ring_pos_ = 0;
}

EntityId Agents::spawn(CharKind kind, const std::string& name, bool female, const Vec3i& foot, u16 polity) {
    auto c = std::make_unique<Character>();
    c->id = (EntityId)chars_.size();
    c->kind = kind;
    c->name = name;
    c->female = female;
    c->polity = polity;
    c->born = ctx_.now;
    c->inv = ctx_.econ->create_store(StoreKind::Carried, foot, polity, c->id, tune.carry_capacity);
    c->next_think = ctx_.now + (Tick)(c->id % 20);
    if (kind == CharKind::MagicalGirl) c->girl = std::make_unique<GirlData>();
    chars_.push_back(std::move(c));
    Character& ref = *chars_.back();
    set_first_age(ref);
    place_at(ref, foot);
    return ref.id;
}

std::vector<Character*> Agents::living() {
    std::vector<Character*> out;
    for (auto& c : chars_)
        if (c && c->alive && !c->departed) out.push_back(c.get());
    return out;
}

int Agents::count_alive(u16 polity) const {
    int n = 0;
    for (auto& c : chars_)
        if (c && c->alive && !c->departed && c->polity == polity) ++n;
    return n;
}

void Agents::randomize(Character& c, Rng& rng) {
    for (int i = 0; i < Personality::kCount; ++i) c.pers.at(i) = clampv(rng.normalish(0.5f, 0.2f), 0.02f, 0.98f);
    for (int s = 0; s < kSkillCount; ++s) c.skills[s] = clampv(rng.normalish(0.3f, 0.12f), 0.05f, 0.9f);
    int spec = (int)rng.below(kSkillCount);
    c.skills[spec] = clampv(c.skills[spec] + 0.35f, 0.0f, 0.95f);
    static const u32 skins[] = {0xF1D3BD, 0xE8C4A8, 0xD9A98A, 0xC68F6E, 0xF5E0D0};
    static const u32 hairs[] = {0x3B2A20, 0x5A3E2B, 0x8A6A4A, 0x2A2A32, 0xC9A36B, 0x7A3B2E, 0xB8B8C8, 0x4A5A7A};
    static const u32 cloths[] = {0x6B7F99, 0x8A7560, 0x6F8A6B, 0x9A6B6B, 0x7A6F92, 0xA89A7A, 0x5E7A80};
    c.look.skin = skins[rng.below(5)];
    c.look.hair = hairs[rng.below(8)];
    c.look.cloth = cloths[rng.below(7)];
    c.look.accent = 0xC9B27A;
    c.look.long_hair = c.female && rng.chance(0.6f);
    c.look.dress = c.female && rng.chance(0.4f);
    c.needs.food = rng.uniform(0.6f, 0.95f);
    c.needs.water = rng.uniform(0.6f, 0.95f);
    c.needs.rest = rng.uniform(0.7f, 1.0f);
    c.needs.social = rng.uniform(0.5f, 0.9f);
    c.mood = 0.6f;
    c.body.build(c.look);
}

void Agents::place_at(Character& c, const Vec3i& foot) {
    c.foot = foot;
    c.pos = Vec3f((float)foot.x + 0.5f, (float)foot.y, (float)foot.z + 0.5f);
    c.fall_speed = 0;
    c.path.clear();
    if (Store* s = ctx_.econ->store(c.inv)) s->pos = foot;
}

namespace {

// Region labels, sorted for a canonical encoding.
void write_region_cells(BinWriter& w, const RegionMap& map) {
    std::vector<std::pair<Vec3i, u16>> cells;
    cells.reserve(map.size());
    map.for_each([&](const Vec3i& p, u16 id) { cells.push_back({p, id}); });
    std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    w.varu(cells.size());
    Vec3i prev{0, 0, 0};
    for (auto& [p, id] : cells) {
        w.vari(p.x - prev.x);
        w.vari(p.y - prev.y);
        w.vari(p.z - prev.z);
        w.varu(id);
        prev = p;
    }
}

void read_region_cells(BinReader& r, RegionMap& map) {
    map.clear();
    const u64 nc = r.varu();
    map.reserve((size_t)nc);
    Vec3i prev{0, 0, 0};
    for (u64 k = 0; k < nc; ++k) {
        Vec3i p{prev.x + (i32)r.vari(), prev.y + (i32)r.vari(), prev.z + (i32)r.vari()};
        map.set(p, (u16)r.varu());
        prev = p;
    }
}

}  // namespace

// ------------------------------------------------------------------------------ main loop

void Agents::step(Tick now) {
    now_ = now;
    path_spent_ = 0;
    index_crowd();
    if (now % kTicksPerHour == 0) {
        fail_ring_pos_ = (fail_ring_pos_ + 1) % (int)fail_ring_.size();
        fail_ring_[(size_t)fail_ring_pos_] = 0;
    }
    if (now % kTicksPerHour == kTicksPerHour / 2) {
        assign_homes();
        appoint_scholars();
    }
    if (now % kTicksPerDay == kTicksPerHour * 6 + 17) {
        daily_life();
        daily_drama();
    }
    if (survey_.active) survey_step();
    if (now % 600 == 0) refresh_water_spots();
    if (now % 50 == 0) {
        ctx_.jobs->expire(now);
        ctx_.econ->expire_reservations(now);
        generate_jobs();
    }
    // Physics hazards → damage and danger memory.
    for (const AreaDamage& d : ctx_.physics->damage_queue()) {
        apply_area_damage(d);
        dangers_.push_back({d.center, d.radius * 2.0f + 3.0f});
    }
    if (now % 100 == 0 && !dangers_.empty()) dangers_.erase(dangers_.begin(), dangers_.begin() + (long)(dangers_.size() + 1) / 2);

    for (size_t i = 1; i < chars_.size(); ++i) {
        Character* cp = chars_[i].get();
        if (!cp || !cp->alive || cp->departed) continue;
        Character& c = *cp;
        update_physics(c);
        if (!c.alive) continue;
        update_needs(c);
        update_health(c);
        if (!c.alive) continue;
        if ((now + c.id) % kTicksPerHour == 0) hourly(c);
        if (now >= c.next_think || c.task.type == TaskType::None) think(c);
        run_task(c);
    }
    keep_apart();
}

void Agents::index_crowd() {
    crowd_.clear();
    std::vector<Vec3i> beds;
    for (size_t i = 1; i < chars_.size(); ++i) {
        const Character* c = chars_[i].get();
        if (!c || !c->alive || c->departed) continue;
        crowd_.push_back({crowd_key(c->foot.x >> 2, c->foot.z >> 2), (u32)i});
        if (!c->sleeping) continue;
        beds.push_back(c->foot);
        const Vec3i& ax = c->task.target2;
        if (c->task.type == TaskType::Sleep && c->foot == c->task.target && (ax.x || ax.z)) {
            beds.push_back(c->foot + ax);
            beds.push_back(c->foot - ax);
        }
    }
    std::sort(crowd_.begin(), crowd_.end());
    ctx_.nav->set_soft(std::move(beds));
}

bool Agents::crowded(const Vec3i& p, const Character& c) {
    bool hit = false;
    for_near(p, [&](const Character& o) {
        if (&o != &c && o.alive && !o.departed && !o.moving && !o.sleeping && o.foot == p) hit = true;
    });
    return hit;
}

void Agents::keep_apart() {
    // Someone standing still is a disc of about the shoulders' width; a sleeper covers
    // her bed. People standing in each other ease apart (within their own cube, so every
    // rule that goes by the cube stays true); passers-by are left to walk on.
    constexpr float R = 0.42f;
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character* cp = chars_[i].get();
        if (!cp || !cp->alive || cp->departed || cp->moving || cp->sleeping || cp->fall_speed > 0.0f) continue;
        Character& c = *cp;
        float px = 0.0f, pz = 0.0f;
        for_near(c.foot, [&](const Character& o) {
            if (&o == &c || !o.alive || o.departed || o.moving || std::abs(o.foot.y - c.foot.y) > 1) return;
            if (o.sleeping) {
                const bool bed = o.task.type == TaskType::Sleep && o.foot == o.task.target;
                const BedPrint b = bed_print(o.foot, bed ? o.task.target2 : Vec3i{});
                const float l = c.pos.x - (b.x0 - R), r = (b.x1 + R) - c.pos.x;
                const float d = c.pos.z - (b.z0 - R), u = (b.z1 + R) - c.pos.z;
                if (l <= 0 || r <= 0 || d <= 0 || u <= 0) return;
                const float m = std::min(std::min(l, r), std::min(d, u));
                if (m == l) px -= l;
                else if (m == r) px += r;
                else if (m == d) pz -= d;
                else pz += u;
                return;
            }
            float dx = c.pos.x - o.pos.x, dz = c.pos.z - o.pos.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 >= 4.0f * R * R) return;
            float d = std::sqrt(d2);
            const float over = (2.0f * R - d) * 0.5f;
            if (d < 1e-3f) {
                // On the very same spot: part along a direction the pair agrees on.
                const u32 lo = std::min(c.id, o.id), hi = std::max(c.id, o.id);
                const float a = (float)((lo * 2654435761u + hi * 40503u) % 6283u) / 1000.0f;
                dx = std::cos(a) * (c.id == lo ? 1.0f : -1.0f);
                dz = std::sin(a) * (c.id == lo ? 1.0f : -1.0f);
                d = 1.0f;
            }
            px += dx / d * over;
            pz += dz / d * over;
        });
        if (px == 0.0f && pz == 0.0f) continue;
        const float len = std::sqrt(px * px + pz * pz), step = 0.06f;
        if (len > step) {
            px *= step / len;
            pz *= step / len;
        }
        // Within her own cube, leaning a little into an open one beside it (never into
        // a wall).
        Nav& nav = *ctx_.nav;
        auto open = [&](int dx, int dz) {
            const Vec3i q = c.foot + Vec3i{dx, 0, dz};
            return nav.passable(q) && nav.passable(q + Vec3i{0, 1, 0});
        };
        const float fx = (float)c.foot.x, fz = (float)c.foot.z;
        c.pos.x = clampv(c.pos.x + px, fx + (open(-1, 0) ? -0.2f : 0.12f), fx + (open(1, 0) ? 1.2f : 0.88f));
        c.pos.z = clampv(c.pos.z + pz, fz + (open(0, -1) ? -0.2f : 0.12f), fz + (open(0, 1) ? 1.2f : 0.88f));
    }
}

void Agents::update_needs(Character& c) {
    const float per_tick = 1.0f / (float)kTicksPerDay;
    float activity = c.task.type == TaskType::Work ? 1.25f : (c.sleeping ? 0.6f : 1.0f);
    if (const Polity* pp = ctx_.society->polity(c.polity)) activity *= 1.0f - pp->passive("numb");
    float appetite = 1.0f;
    if (c.is_girl() && c.girl->drive == "gluttony" && c.girl->level >= 2) appetite = 3.0f;  // 无底
    if (!c.is_girl() && c.age0 < 14.0f && is_child(c)) appetite *= 0.6f;  // children eat less
    c.needs.food = clampv(c.needs.food - tune.food_per_day * per_tick * activity * appetite, 0.0f, 1.0f);
    c.needs.water = clampv(c.needs.water - tune.water_per_day * per_tick * activity, 0.0f, 1.0f);
    if (c.sleeping) {
        c.needs.rest = clampv(c.needs.rest + 3.0f * per_tick * 1.1f, 0.0f, 1.0f);
    } else {
        c.needs.rest = clampv(c.needs.rest - tune.rest_per_day * per_tick * activity, 0.0f, 1.0f);
    }
    c.needs.social = clampv(c.needs.social - tune.social_per_day * per_tick, 0.0f, 1.0f);
    // Safety recovers when away from danger.
    float dz = danger_at(c);
    if (dz > 0.1f) c.needs.safety = clampv(c.needs.safety - 0.02f * dz, 0.0f, 1.0f);
    else c.needs.safety = clampv(c.needs.safety + 0.0015f, 0.0f, 1.0f);
}

void Agents::update_health(Character& c) {
    Body& b = c.body;
    const float per_tick = 1.0f / (float)kTicksPerDay;
    float loss = b.bleeding * per_tick;
    if (c.needs.food <= 0.0f) loss += 0.6f * per_tick;   // starving
    if (c.needs.water <= 0.0f) loss += 1.2f * per_tick;  // dehydrated
    if (loss > 0) b.vitality -= loss;
    else b.vitality = std::min(1.0f, b.vitality + 0.3f * per_tick);
    b.bleeding = std::max(0.0f, b.bleeding - 0.9f * per_tick);
    if (b.vitality <= 0.0f || b.fatal()) {
        std::string cause = "伤势过重";
        if (c.needs.food <= 0.0f && b.bleeding < 0.1f) cause = "饿死";
        if (c.needs.water <= 0.0f && b.bleeding < 0.1f) cause = "渴死";
        kill(c, cause, 0);
    }
}

void Agents::update_physics(Character& c) {
    if (c.in_boat) return;  // afloat: the boat carries her (see the boat's errand)
    World& w = *ctx_.world;
    Nav& nav = *ctx_.nav;
    Vec3i below{c.foot.x, c.foot.y - 1, c.foot.z};
    bool ground = w.in_bounds(below) && w.material(below).solid && !w.material(below).passable;
    if (!ground || c.fall_speed > 0) {
        // Falling: ground vanished (dug out, blasted, collapsed).
        c.fall_speed = std::min(1.2f, c.fall_speed + 0.02f);
        c.pos.y -= c.fall_speed;
        c.path.clear();
        Vec3i f{(int)std::floor(c.pos.x), (int)std::floor(c.pos.y), (int)std::floor(c.pos.z)};
        if (c.pos.y < -10.0f) {
            EventId e = 0;
            kill(c, "坠入云海深渊", e);
            return;
        }
        Vec3i fb{f.x, f.y - 1, f.z};
        if (w.in_bounds(fb) && w.material(fb).solid && !w.material(fb).passable) {
            float speed = c.fall_speed;
            c.fall_speed = 0;
            place_at(c, f);
            if (speed > 0.35f) damage(c, clampv((speed - 0.3f) * 0.25f, 0.0f, 0.6f), -1, "坠落", 0);
        } else {
            c.foot = f;
        }
        return;
    }
    // Buried (something solid now occupies the body space): step up / aside.
    if (!nav.passable(c.foot) || !nav.passable(c.foot + Vec3i{0, 1, 0})) {
        Vec3i np;
        if (nav.find_standable_near(c.foot + Vec3i{0, 1, 0}, np, 3)) place_at(c, np);
        else damage(c, 0.01f, kTorso, "被掩埋", 0);
    }
    // Standing in fire.
    if (vburning(w.get(c.foot)) || vburning(w.get(below))) damage(c, 0.004f, -1, "烧伤", 0);
}

void Agents::assign_homes() {
    // Who lives where: homes list their living occupants; people without a (standing)
    // home of their own polity move into the nearest one with a free bed.
    std::vector<Building*> homes;
    for (const Building& bc : ctx_.buildings->all()) {
        if (!bc.alive || !bc.complete || bc.beds <= 0) continue;
        Building* b = ctx_.buildings->get(bc.id);
        b->residents.clear();
        homes.push_back(b);
    }
    for (auto& cp : chars_) {
        if (!cp || !cp->alive || cp->departed || cp->is_girl() || !cp->home) continue;
        Building* h = ctx_.buildings->get(cp->home);
        if (h && h->complete && h->beds > 0 && h->polity == cp->polity && h->functional) h->residents.push_back(cp->id);
        else if (!h || h->beds > 0) cp->home = 0;  // gone, ruined or lost to another polity
    }
    for (auto& cp : chars_) {
        if (!cp || !cp->alive || cp->departed || cp->is_girl() || cp->home) continue;
        Building* best = nullptr;
        i64 bd = 0;
        // Family first: a partner's or a parent's home with a bed free.
        for (EntityId kin : {cp->partner, cp->parents[0], cp->parents[1]}) {
            const Character* k = get(kin);
            Building* kb = k && k->alive ? ctx_.buildings->get(k->home) : nullptr;
            if (kb && kb->polity == cp->polity && kb->functional && (int)kb->residents.size() < kb->beds) {
                best = kb;
                break;
            }
        }
        const bool family = best != nullptr;
        for (Building* b : homes) {
            if (family) break;
            if (b->polity != cp->polity || !b->functional || (int)b->residents.size() >= b->beds) continue;
            const i64 d = b->entrance.dist2(cp->foot);
            if (!best || d < bd) {
                best = b;
                bd = d;
            }
        }
        if (!best) continue;
        cp->home = best->id;
        best->residents.push_back(cp->id);
        cp->remember(now_, MemoryKind::Helped, kNoEntity, 0.15f, 0);
    }
}

void Agents::hourly(Character& c) {
    update_equipment(c);
    // Cold, rain and the night against what one wears: comfort goes, and a night in the
    // snow without warm clothes bites.
    {
        const float ex = exposure(c);
        if (ex > 0.0f) {
            c.needs.comfort = std::max(0.0f, c.needs.comfort - 0.03f * ex);
            if (ex > 0.55f && is_night(now_) && c.body.total_alive() > 0) damage(c, 0.002f * ex, -1, "冻伤", 0);
        } else if (c.clothes != kNoItem) {
            c.needs.comfort = std::min(1.0f, c.needs.comfort + 0.01f);
        }
    }
    // Regeneration costs food: missing voxels regrow slowly when fed and not bleeding.
    if (c.needs.food > 0.3f && c.needs.water > 0.3f && c.body.bleeding < 0.05f) {
        int missing = c.body.total_voxels() - c.body.total_alive();
        if (missing > 0) {
            const float rate = c.treated_until > now_ ? (c.treated_well ? 0.05f : 0.03f) : 0.012f;
            int grow = std::max(1, (int)(c.body.total_voxels() * rate));
            int done = c.body.regrow(std::min(grow, missing), c.look);
            c.needs.food = std::max(0.0f, c.needs.food - 0.0008f * (float)done);
        }
    }
    // Mood drifts toward a target shaped by needs, memories and circumstances.
    const Needs& n = c.needs;
    float target = 0.18f + 0.22f * n.food + 0.18f * n.water + 0.12f * n.rest + 0.12f * n.social * (0.5f + c.pers.sociability) +
                   0.1f * n.safety + 0.08f * n.comfort;
    if (c.home == 0) target -= 0.06f;
    float mem = 0;
    for (auto& m : c.memories) {
        float age_days = (float)(now_ - m.tick) / (float)kTicksPerDay;
        mem += m.valence * std::exp(-age_days / 2.5f);
    }
    target += clampv(mem, -0.4f, 0.3f);
    float injury = 1.0f - (float)c.body.total_alive() / (float)std::max(1, c.body.total_voxels());
    target -= injury * 0.5f;
    // Passive spells of the polity's magical girls colour everyone's days.
    if (const Polity* pp = ctx_.society->polity(c.polity)) {
        target = std::max(target, 4.0f * pp->passive("mood_floor"));
        target -= 0.5f * pp->passive("fear_rule");
        target = std::min(target, 1.0f - 2.0f * pp->passive("numb"));
        c.fear = std::max(c.fear, pp->passive("fear_rule"));
    }
    target = clampv(target, 0.0f, 1.0f);
    c.mood += (target - c.mood) * 0.25f;
    c.stress = clampv(c.stress * 0.95f + (c.mood < 0.3f ? 0.05f : 0.0f), 0.0f, 1.0f);
    c.fear = clampv(c.fear * 0.97f, 0.0f, 1.0f);
    if (c.needs.food < 0.15f) c.remember(now_, MemoryKind::Hungry, kNoEntity, -0.08f, 0);
    if (c.needs.water < 0.15f) c.remember(now_, MemoryKind::Thirsty, kNoEntity, -0.08f, 0);
    // Forget stale memories.
    c.memories.erase(std::remove_if(c.memories.begin(), c.memories.end(),
                                    [&](const Memory& m) { return now_ - m.tick > kTicksPerDay * 8; }),
                     c.memories.end());
    // Unreachable blacklist expiry.
    c.unreachable.erase(std::remove_if(c.unreachable.begin(), c.unreachable.end(),
                                       [&](const std::pair<Vec3i, Tick>& u) { return u.second <= now_; }),
                        c.unreachable.end());
    // Duty: during work hours, time not spent working accrues as shirked duty.
    if (is_work_time(c) && c.task.type != TaskType::Work && c.task.type != TaskType::Eat &&
        c.task.type != TaskType::Drink && c.task.type != TaskType::Sleep && c.task.type != TaskType::Govern)
        c.work_debt = std::min(12.0f, c.work_debt + 1.0f);
    else if (c.task.type == TaskType::Work)
        c.work_debt = std::max(0.0f, c.work_debt - 1.0f);
    // Skill practice.
    if (c.task.type == TaskType::Work && c.task.job) {
        if (const Job* j = ctx_.jobs->get(c.task.job)) {
            int s = job_skill(j->type);
            c.skills[s] = std::min(1.0f, c.skills[s] + 0.004f);
        }
    }
}

void Agents::kill(Character& c, const std::string& cause, EventId ev_cause) {
    if (!c.alive) return;
    c.alive = false;
    c.died = now_;
    c.death_cause = cause;
    c.sleeping = false;
    if (c.task.job) ctx_.jobs->release(c.task.job, c.id);
    ctx_.econ->release_agent(c.id);
    // Belongings drop where they fell.
    if (ctx_.econ->store(c.inv)) {
        Store* s = ctx_.econ->store(c.inv);
        if (!s->empty()) ctx_.econ->drop(c.inv, c.foot);
    }
    Event e;
    e.type = EventType::Death;
    e.severity = c.is_girl() ? 5 : 3;
    e.pos = c.foot;
    e.actor = c.id;
    e.polity = c.polity;
    e.causes[0] = ev_cause;
    e.text = cause == "年老" ? strfmt("%s安详离世，享年 %d 岁", c.name.c_str(), (int)age_years(c))
                             : strfmt("%s死亡：%s", c.name.c_str(), cause.c_str());
    e.data.set("cause", cause);
    c.death_event = ctx_.chron->emit(std::move(e));
    // Partner and children mourn.
    if (Character* pt = get(c.partner); pt && pt->partner == c.id) {
        pt->partner = kNoEntity;
        pt->remember(now_, MemoryKind::Bereaved, c.id, -0.35f, c.death_event);
    }
    for (auto& o : chars_)
        if (o && o->alive && (o->parents[0] == c.id || o->parents[1] == c.id))
            o->remember(now_, MemoryKind::Bereaved, c.id, -0.3f, c.death_event);
    if (Polity* p = ctx_.society->polity(c.polity)) p->deaths_total++;
    // Witnesses and friends remember.
    for (auto& o : chars_) {
        if (!o || !o->alive || o->id == c.id) continue;
        float aff = o->affinity(c.id);
        if (aff > 0.3f) o->remember(now_, MemoryKind::FriendDied, c.id, -0.25f * aff, c.death_event);
        else if (o->pos.dist_sq(c.pos) < 144.0f) o->remember(now_, MemoryKind::SawDeath, c.id, -0.08f, c.death_event);
    }
    if (c.girl) girl_died(c, c.death_event);
}

void Agents::damage(Character& c, float fraction, int part, const std::string& what, EventId cause) {
    if (!c.alive || fraction <= 0) return;
    // Armour absorbs part of the harm.
    if (c.armor != kNoItem) fraction *= 1.0f - ctx_.reg->item(c.armor).armor;
    // Magical girls are far tougher than artificial humans (不屈 hardens them further).
    if (c.is_girl()) {
        fraction *= 0.3f;
        for (const Json& d : ctx_.reg->doc("drives")["drives"].items())
            if (d.str("key") == c.girl->drive)
                for (const Json& sp : d["spells"].items())
                    if (sp.str("effect") == "toughness" && c.girl->level >= sp.integer("level", 1))
                        fraction *= 1.0f - sp.flt("amount", 0.0f);
    }
    DamageReport r = c.body.damage_spread(fraction, rng_, part);
    if (r.removed == 0) return;
    float frac = (float)r.removed / (float)std::max(1, c.body.total_voxels());
    if (frac > 0.03f || r.severed_part >= 0) {
        Event e;
        e.type = EventType::Injury;
        e.severity = r.severed_part >= 0 ? 3 : 2;
        e.pos = c.foot;
        e.actor = c.id;
        e.polity = c.polity;
        e.causes[0] = cause;
        e.text = r.severed_part >= 0 ? strfmt("%s因%s失去了%s", c.name.c_str(), what.c_str(), body_part_name_zh(r.severed_part))
                                     : strfmt("%s因%s受伤", c.name.c_str(), what.c_str());
        EventId id = ctx_.chron->emit(std::move(e));
        c.remember(now_, MemoryKind::Injured, kNoEntity, -0.15f - frac, id);
        cause = id;
    }
    c.needs.safety = std::max(0.0f, c.needs.safety - 0.3f);
    if (r.lethal) kill(c, what, cause);
}

void Agents::apply_area_damage(const AreaDamage& d) {
    for (auto& cp : chars_) {
        if (!cp || !cp->alive || cp->departed) continue;
        Character& c = *cp;
        Vec3f center = c.pos + Vec3f(0, 1.2f, 0);
        float dist = (center - d.center).length();
        if (dist > d.radius) continue;
        float falloff = 1.0f - dist / std::max(0.1f, d.radius);
        const char* what = d.kind == 1 ? "火焰" : (d.kind == 2 ? "坠落物砸中" : "冲击");
        damage(c, d.amount * falloff, -1, what, d.cause);
        c.needs.safety = 0.0f;
        if (c.alive) c.remember(now_, MemoryKind::Disaster, kNoEntity, -0.12f, d.cause);
    }
}

float Agents::danger_at(const Character& c) const {
    float worst = 0;
    for (auto& [p, r] : dangers_) {
        float d = (c.pos - p).length();
        if (d < r) worst = std::max(worst, 1.0f - d / r);
    }
    // Beasts on the prowl (hunters and soldiers stand their ground).
    if (ctx_.fauna && !c.drafted && !(c.task.type == TaskType::Work && c.weapon != kNoItem))
        worst = std::max(worst, ctx_.fauna->threat_at(c.pos));
    return worst;
}

bool Agents::is_work_time(const Character& c) const {
    float h = hour_of(now_);
    const Polity* p = ctx_.society->polity(c.polity);
    float hours = p ? p->policies.work_hours : 9.0f;
    float start = 7.0f;
    return h >= start && h < start + hours + 1.0f;  // includes a meal break hour
}

// ------------------------------------------------------------------------------ movement

bool Agents::blacklisted(Character& c, const Vec3i& p) {
    for (auto& u : c.unreachable)
        if (u.first == p) return true;
    return false;
}

void Agents::blacklist(Character& c, const Vec3i& p, Tick duration) {
    c.unreachable.push_back({p, now_ + duration});
    if (c.unreachable.size() > 32) c.unreachable.erase(c.unreachable.begin());
}

Agents::Move Agents::move_to(Character& c, const Vec3i& goal, bool adjacent_ok, int reach_up, int reach_xz) {
    auto arrived = [&]() {
        if (c.foot == goal) return true;
        if (!adjacent_ok) return false;
        return std::abs(c.foot.x - goal.x) <= reach_xz && std::abs(c.foot.z - goal.z) <= reach_xz &&
               c.foot.y - goal.y <= 2 && goal.y - c.foot.y <= reach_up;
    };
    if (arrived()) {
        c.moving = false;
        c.path.clear();
        return Move::Arrived;
    }
    Nav& nav = *ctx_.nav;
    if (!c.path.valid() || c.path_goal != goal) {
        c.path.clear();
        if (blacklisted(c, goal)) return Move::Failed;
        // The region survey tells cheaply whether the goal can be reached at all.
        int budget = 40000;
        if (auto rf = region_map_.find(c.foot); rf != region_map_.end()) {
            bool same = false, other = false;
            const int r = adjacent_ok ? reach_xz : 0;
            for (int dy = adjacent_ok ? -reach_up : 0; dy <= (adjacent_ok ? 2 : 0) && !same; ++dy)
                for (int dz = -r; dz <= r && !same; ++dz)
                    for (int dx = -r; dx <= r && !same; ++dx) {
                        auto it = region_map_.find(goal + Vec3i{dx, dy, dz});
                        if (it == region_map_.end()) continue;
                        if (it->second == rf->second) same = true;
                        else other = true;
                    }
            // Another survey region is truly out of reach only when both floods ran their
            // course; the edge of a survey says nothing (settlements far apart).
            if (!same) {
                u16 gl = 0;
                for (int dy = -reach_up; dy <= 2 && !gl; ++dy)
                    if (auto it = region_map_.find(goal + Vec3i{0, dy, 0}); it != region_map_.end()) gl = it->second;
                auto open = [&](u16 id) { return id < region_open_.size() && region_open_[id]; };
                budget = !other ? 6000 : (open(rf->second) && (gl == 0 || open(gl)) ? 60000 : 0);
                // A builder on a ladder reaches up past the regions the survey saw (a roof
                // top can look like a separate, closed region): always search.
                if (reach_up > 3) budget = std::max(budget, 20000);
            }
        }
        // Long searches are shared out over the ticks: when this tick's share is spent
        // (an army setting out all at once), the search waits for the next tick.
        if (budget > 8000 && path_spent_ >= kPathTickBudget) {
            c.moving = false;
            return Move::Moving;
        }
        // Far away: the next leg along the coarse route (the path ends on the way; the
        // goal stays the goal, and the leg after is sought where this one ends).
        if (budget > 0 && std::max(std::abs(goal.x - c.foot.x), std::abs(goal.z - c.foot.z)) > kLegFar) {
            std::vector<Vec3i> vias;
            if (route_vias(c.foot, goal, vias))
                for (const Vec3i& v : vias) {
                    const u64 before = nav.stats.expansions;
                    const bool ok = nav.find_path(c.foot, v, true, c.path, 8000, 3, 3);
                    path_spent_ += nav.stats.expansions - before;
                    if (ok && c.path.valid()) {
                        c.path_goal = goal;
                        break;
                    }
                    c.path.clear();
                }
        }
        const u64 expanded = nav.stats.expansions;
        const bool found = c.path.valid() ||
                           (budget > 0 && nav.find_path(c.foot, goal, adjacent_ok, c.path, budget, reach_up, reach_xz));
        path_spent_ += nav.stats.expansions - expanded;
        if (!found) {
            blacklist(c, goal, kTicksPerHour * 3);
            day.path_failures++;
            fail_ring_[(size_t)fail_ring_pos_]++;
            if (c.task.type == TaskType::Work && c.task.job)
                if (Job* j = ctx_.jobs->get(c.task.job); j && ++j->path_fails >= 3) {
                    j->path_fails = 0;
                    j->suspended_until = now_ + kTicksPerHour * 12;
                }
            if (debug_path_failures.size() < 200) debug_path_failures.push_back({c.id, c.foot, goal, now_});
            return Move::Failed;
        }
        c.path_goal = goal;
        if (!c.path.valid()) return arrived() ? Move::Arrived : Move::Failed;
    }
    Vec3i next = c.path.nodes[c.path.next];
    // The world may have changed: validate the step.
    if (!nav.standable(next)) {
        c.path.clear();
        return Move::Moving;  // repath next tick
    }
    const World& w = *ctx_.world;
    (void)w;
    float speed = tune.walk_speed * c.body.mobility();
    if (c.needs.rest < 0.15f) speed *= 0.7f;
    if (c.age0 < 14.0f && is_child(c)) speed *= 0.8f;
    else if (is_elder(c)) speed *= 0.85f;
    if (c.needs.food < 0.1f || c.needs.water < 0.1f) speed *= 0.75f;
    if (c.fear > 0.5f && c.task.type == TaskType::Flee) speed *= 1.3f;
    float cost = nav.step_cost(next);
    speed /= std::max(0.6f, cost);
    Vec3f target((float)next.x + 0.5f, (float)next.y, (float)next.z + 0.5f);
    // Keep to the right of anyone coming the other way, so the two pass side by side.
    {
        float fx = target.x - c.pos.x, fz = target.z - c.pos.z;
        const float len = std::sqrt(fx * fx + fz * fz);
        if (len > 0.05f) {
            fx /= len;
            fz /= len;
            bool oncoming = false;
            for_near(c.foot, [&](const Character& o) {
                if (oncoming || &o == &c || !o.alive || !o.moving || o.departed || std::abs(o.foot.y - c.foot.y) > 2) return;
                const float ox = o.pos.x - c.pos.x, oz = o.pos.z - c.pos.z;
                const float ahead = ox * fx + oz * fz;
                if (ahead <= 0.0f || ahead > 3.0f || std::abs(ox * fz - oz * fx) > 0.9f) return;
                if (std::sin(o.yaw) * fx + std::cos(o.yaw) * fz < -0.5f) oncoming = true;
            });
            if (oncoming) {
                target.x -= fz * 0.28f;
                target.z += fx * 0.28f;
            }
        }
    }
    Vec3f d = target - c.pos;
    float dist = d.length();
    c.moving = true;
    if (dist > 0.01f) c.yaw = std::atan2(d.x, d.z);
    if (dist <= speed) {
        c.pos = target;
        c.foot = next;
        c.path.next++;
        if (Store* s = ctx_.econ->store(c.inv)) s->pos = c.foot;
        if (arrived()) {
            // Someone already stands here (at a store, a workbench, the edge of a field):
            // a free cube beside it that is just as good, if there is one.
            if (adjacent_ok && crowded(c.foot, c)) {
                Vec3i nb[8];
                float nc[8];
                const int n = nav.neighbors(c.foot, nb, nc);
                for (int k = 0; k < n; ++k) {
                    const Vec3i q = nb[k];
                    // Aside on the same level only (never up onto a wall or a roof in the making).
                    if (q.y != c.foot.y || std::abs(q.x - goal.x) > reach_xz || std::abs(q.z - goal.z) > reach_xz ||
                        q.y - goal.y > 2 || goal.y - q.y > reach_up || crowded(q, c))
                        continue;
                    c.path.nodes.assign(1, q);
                    c.path.next = 0;
                    c.path_goal = goal;
                    return Move::Moving;
                }
            }
            c.moving = false;
            c.path.clear();
            return Move::Arrived;
        }
    } else {
        c.pos += d * (speed / dist);
    }
    c.walk_phase += speed * 2.2f;
    return Move::Moving;
}

float Agents::carried_weight(const Character& c) const {
    const Store* s = ctx_.econ->store(c.inv);
    return s ? ctx_.econ->weight(*s) : 0.0f;
}

void Agents::deposit_all(Character& c, StoreId to) {
    Store* s = ctx_.econ->store(c.inv);
    if (!s || s->empty()) return;
    // Everything except one of each piece of equipment in use.
    std::vector<ItemStack> items = s->items;
    for (const ItemStack& st : items) {
        i32 keep = (st.item == c.tool || st.item == c.weapon || st.item == c.armor || st.item == c.cart ||
                    st.item == c.clothes)
                       ? 1
                       : 0;
        keep += cargo_kept(c, st.item);
        if (st.count > keep) ctx_.econ->transfer(c.inv, to, st.item, st.count - keep);
    }
    // What the store could not take is set down here.
    s = ctx_.econ->store(c.inv);
    bool leftovers = false;
    for (const ItemStack& st : s->items) {
        i32 keep = (st.item == c.tool || st.item == c.weapon || st.item == c.armor || st.item == c.cart ||
                    st.item == c.clothes)
                       ? 1
                       : 0;
        keep += cargo_kept(c, st.item);
        if (st.count > keep) leftovers = true;
    }
    if (leftovers) {
        StoreId pile = ctx_.econ->pile_at(c.foot);
        items = ctx_.econ->store(c.inv)->items;
        for (const ItemStack& st : items) {
            i32 keep = (st.item == c.tool || st.item == c.weapon || st.item == c.armor || st.item == c.cart ||
                        st.item == c.clothes)
                           ? 1
                           : 0;
            keep += cargo_kept(c, st.item);
            if (st.count > keep) ctx_.econ->transfer(c.inv, pile, st.item, st.count - keep);
        }
    }
    // Standing at the store anyway: a good moment to pick up better tools or a cart.
    update_equipment(c);
}

void Agents::update_equipment(Character& c) {
    const Registry& reg = *ctx_.reg;
    Store* inv = ctx_.econ->store(c.inv);
    auto verify = [&](ItemId& slot) {
        if (slot != kNoItem && (!inv || inv->count(slot) <= 0)) slot = kNoItem;
    };
    const ItemId held = c.tool;
    verify(c.tool);
    verify(c.weapon);
    verify(c.armor);
    verify(c.cart);
    verify(c.clothes);
    if (c.tool != held) c.tool_wear = 0;
    if (inv) inv->capacity = carry_capacity(c);
    if (c.is_girl() || !c.body.can_hold()) return;
    const bool child = c.age0 < 14.0f && is_child(c);  // children take only something to wear
    // Pick up better gear from a public store within reach.
    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
        const Store* st = ctx_.econ->store(sid);
        if (!st || st->pos.dist2(c.foot) > 5 * 5) continue;
        auto upgrade = [&](ItemId& slot, const char* tag, bool want) {
            if (!want) return;
            float cur = slot != kNoItem ? std::max({reg.item(slot).power, reg.item(slot).armor, reg.item(slot).warmth}) : 0.0f;
            ItemId best = kNoItem;
            float bv = cur;
            for (const ItemStack& is : st->items) {
                const ItemDef& d = reg.item(is.item);
                if (!d.has_tag(tag) || ctx_.econ->available(sid, is.item, c.id) <= 0) continue;
                float v = std::max({d.power, d.armor, d.warmth});
                if (v > bv + 1e-4f) {
                    bv = v;
                    best = is.item;
                }
            }
            if (best == kNoItem) return;
            if (ctx_.econ->transfer(sid, c.inv, best, 1) == 1) {
                if (slot != kNoItem) ctx_.econ->transfer(c.inv, sid, slot, 1);  // return the old one
                slot = best;
            }
        };
        // The tool of one's trade (any tool rather than none); a better one of the same
        // kind replaces it.
        if (!child) {
            const std::string trade = occupation_tool(c.occupation);
            const float cur = tool_factor(c, trade);
            ItemId best = kNoItem;
            float bv = c.tool == kNoItem ? 0.0f : cur;
            for (const ItemStack& is : st->items) {
                const ItemDef& d = reg.item(is.item);
                if (d.tool_kind.empty() || ctx_.econ->available(sid, is.item, c.id) <= 0) continue;
                // A tool of the trade counts fully; any other only while the hands are empty.
                const float v = (d.tool_kind == trade || trade.empty()) ? d.power
                                : d.tool_kind == "kit"                  ? d.power * 0.8f
                                                                        : 0.05f;
                if (v > bv + 1e-4f) {
                    bv = v;
                    best = is.item;
                }
            }
            if (best != kNoItem && ctx_.econ->transfer(sid, c.inv, best, 1) == 1) {
                if (c.tool != kNoItem) ctx_.econ->transfer(c.inv, sid, c.tool, 1);  // return the old one
                c.tool = best;
                c.tool_wear = 0;
            }
        }
        upgrade(c.clothes, "clothes", true);
        upgrade(c.weapon, "weapon", c.drafted);
        upgrade(c.armor, "armor", c.drafted);
        // A cart for anyone who hauls (soldiers march without one).
        if (c.cart == kNoItem && !c.drafted && !child)
            for (const ItemStack& is : st->items)
                if (reg.item(is.item).has_tag("cart") && ctx_.econ->available(sid, is.item, c.id) > 0 &&
                    ctx_.econ->transfer(sid, c.inv, is.item, 1) == 1) {
                    c.cart = is.item;
                    if (Store* iv = ctx_.econ->store(c.inv)) iv->capacity = carry_capacity(c);
                    break;
                }
        break;
    }
    // Discharged soldiers hand their arms back when they pass a store (hunters keep
    // their spear or bow).
    const bool hunting_gear = c.weapon != kNoItem && reg.item(c.weapon).has_tag("hunting") && c.armor == kNoItem;
    if (!c.drafted && !hunting_gear && (c.weapon != kNoItem || c.armor != kNoItem))
        for (StoreId sid : ctx_.society->public_stores(c.polity)) {
            const Store* st = ctx_.econ->store(sid);
            if (!st || st->kind != StoreKind::Stockpile || st->pos.dist2(c.foot) > 5 * 5) continue;
            if (c.weapon != kNoItem && ctx_.econ->transfer(c.inv, sid, c.weapon, 1) == 1) c.weapon = kNoItem;
            if (c.armor != kNoItem && ctx_.econ->transfer(c.inv, sid, c.armor, 1) == 1) c.armor = kNoItem;
            break;
        }
}

// ------------------------------------------------------------------------------ persistence

namespace {
void save_pers(BinWriter& w, const Personality& p) {
    for (int i = 0; i < Personality::kCount; ++i) w.f32(p.at(i));
}
void load_pers(BinReader& r, Personality& p) {
    for (int i = 0; i < Personality::kCount; ++i) p.at(i) = r.f32();
}
}  // namespace

void Agents::save(BinWriter& w) const {
    size_t sec = w.begin_section("AGNT");
    w.u64v(rng_.state());
    w.u64v(rng_.inc());
    w.varu(chars_.size());
    for (size_t i = 1; i < chars_.size(); ++i) {
        const Character& c = *chars_[i];
        w.u8v((u8)c.kind);
        w.str(c.name);
        w.boolean(c.female);
        w.boolean(c.alive);
        w.boolean(c.departed);
        w.u64v(c.born);
        w.u64v(c.died);
        w.str(c.death_cause);
        w.u32v(c.death_event);
        w.u16v(c.polity);
        w.vec3f(c.pos);
        w.vec3i(c.foot);
        w.f32(c.yaw);
        w.f32(c.fall_speed);
        w.f32(c.walk_phase);
        c.body.save(w);
        // The seven stored colours (mouth and blush are derived from the skin).
        for (u32 col : {c.look.skin, c.look.hair, c.look.cloth, c.look.accent, c.look.shoes, c.look.eyes, 0xE8E0D0u})
            w.u32v(col);
        w.boolean(c.look.long_hair);
        w.boolean(c.look.dress);
        w.boolean(c.look.ribbon);
        w.f32(c.needs.food);
        w.f32(c.needs.water);
        w.f32(c.needs.rest);
        w.f32(c.needs.social);
        w.f32(c.needs.safety);
        w.f32(c.needs.comfort);
        w.f32(c.mood);
        w.f32(c.stress);
        w.f32(c.fear);
        w.f32(c.work_debt);
        save_pers(w, c.pers);
        for (float s : c.skills) w.f32(s);
        w.varu(c.relations.size());
        for (auto& r : c.relations) {
            w.u32v(r.other);
            w.f32(r.affinity);
        }
        w.varu(c.support.size());
        for (auto& s : c.support) {
            w.u32v(s.girl);
            w.f32(s.value);
        }
        w.varu(c.memories.size());
        for (auto& m : c.memories) {
            w.u64v(m.tick);
            w.u8v((u8)m.kind);
            w.u32v(m.subject);
            w.f32(m.valence);
            w.u32v(m.event);
        }
        w.u32v(c.inv);
        w.u16v(c.tool);
        w.u16v(c.weapon);
        w.u16v(c.armor);
        w.u32v(c.home);
        w.str(c.occupation);
        // Task.
        w.u8v((u8)c.task.type);
        w.u8v(c.task.step);
        w.u32v(c.task.job);
        w.vec3i(c.task.target);
        w.vec3i(c.task.target2);
        w.u32v(c.task.store);
        w.u16v(c.task.item);
        w.vari(c.task.count);
        w.u32v(c.task.other);
        w.u64v(c.task.started);
        w.u64v(c.task.until);
        w.f32(c.task.utility);
        w.vari(c.task.fails);
        w.str(c.task.label);
        w.varu(c.path.nodes.size());
        for (auto& p : c.path.nodes) w.vec3i(p);
        w.varu(c.path.next);
        w.vec3i(c.path_goal);
        w.vec3i(c.water_spot);
        w.varu(c.region);
        w.u64v(c.next_think);
        w.u64v(c.last_ate);
        w.u64v(c.last_drank);
        w.u64v(c.last_slept);
        w.u64v(c.last_social);
        w.u64v(c.last_punished);
        w.varu(c.unreachable.size());
        for (auto& u : c.unreachable) {
            w.vec3i(u.first);
            w.u64v(u.second);
        }
        w.boolean(c.drafted);
        w.boolean(c.rebel);
        w.boolean(c.sleeping);
        w.str(c.status_text);
        w.boolean(c.girl != nullptr);
        if (c.girl) {
            const GirlData& g = *c.girl;
            w.str(g.drive);
            w.vari(g.level);
            w.f32(g.xp);
            w.f32(g.mana);
            save_pers(w, g.persona);
            w.str(g.temperament);
            w.str(g.role);
            w.str(g.domain);
            w.f32(g.loyalty);
            w.f32(g.ambition_pressure);
            w.varu(g.spell_cooldowns.size());
            for (u32 cd : g.spell_cooldowns) w.u32v(cd);
            w.u64v(g.last_decision);
            w.varu(g.decisions.size());
            for (u32 d : g.decisions) w.u32v(d);
            w.str(g.stance);
            w.varu(g.experience.size());
            for (auto& e : g.experience) {
                w.str(e.first);
                w.f32(e.second);
            }
            w.u32v(g.grudge);
        }
    }
    w.varu(dangers_.size());
    for (auto& [p, r] : dangers_) {
        w.vec3f(p);
        w.f32(r);
    }
    w.varu(water_spots_.size());
    for (size_t i = 0; i < water_spots_.size(); ++i) {
        w.vec3i(water_spots_[i]);
        w.varu(water_regions_[i]);
    }
    // Region cache (sorted for a canonical encoding).
    w.u8v((u8)((ctx_.nav->major_dirty ? 1 : 0) | (ctx_.nav->minor_dirty ? 2 : 0)));
    w.u64v(region_built_);
    w.varu(region_anchors_.size());
    for (const Vec3i& a : region_anchors_) w.vec3i(a);
    write_region_cells(w, region_map_);
    // Carts and dressed wounds (added later; loaders of older saves skip it).
    std::vector<const Character*> extra;
    for (const auto& c : chars_)
        if (c && (c->cart != kNoItem || c->treated_until != 0)) extra.push_back(c.get());
    w.varu(extra.size());
    for (const Character* c : extra) {
        w.u32v(c->id);
        w.u32v(c->cart);
        w.u64v(c->treated_until);
        w.boolean(c->treated_well);
    }
    // Daily counters and the rolling failure window (they feed crises and stats).
    for (int v : {day.harvested, day.ate_public, day.refused_food, day.thefts, day.path_failures, day.drinks,
                  day.hungry_no_food, day.thirsty_no_water})
        w.vari(v);
    for (int v : fail_ring_) w.vari(v);
    w.vari(fail_ring_pos_);
    // Version 2 per-character data. The block version comes first; later versions only
    // append fields, so older loaders read what they know.
    w.varu(kCharBlockVersion);
    w.varu(chars_.size());
    // (region_open_ follows the character block, see below)
    for (size_t i = 1; i < chars_.size(); ++i) {
        const Character& c = *chars_[i];
        w.u16v(c.clothes);
        w.u16v(c.tool_wear);
        w.u8v(c.task.resume);
        w.f32(c.age0);
        w.u32v(c.partner);
        w.u32v(c.parents[0]);
        w.u32v(c.parents[1]);
        w.u64v(c.last_child);
        w.u64v(c.girl ? c.girl->awakened : 0);
        w.u64v(c.empowered_until);
        w.f32(c.empowered);
        w.u64v(c.no_food_until);
        w.boolean(c.in_boat);
        w.boolean(c.moving);
    }
    w.varu(region_open_.size());
    for (u8 o : region_open_) w.u8v(o);
    // The magical girls' drama (version 1): bonds, trauma and solace, drive history, duels.
    w.varu(1);
    w.varu(chars_.size());
    for (size_t i = 1; i < chars_.size(); ++i) {
        const Character& c = *chars_[i];
        w.boolean(c.girl != nullptr);
        if (!c.girl) continue;
        const GirlData& g = *c.girl;
        w.varu(g.bonds.size());
        for (const Bond& b : g.bonds) {
            w.u32v(b.other);
            w.u8v((u8)b.kind);
            w.u64v(b.since);
            w.u32v(b.event);
        }
        w.f32(g.trauma);
        w.f32(g.solace);
        w.varu(g.marks.size());
        for (EventId m : g.marks) w.u32v(m);
        w.str(g.born_drive);
        w.u64v(g.drive_changed);
        w.u32v(g.duel);
        w.u64v(g.duel_since);
    }
    // A region survey in progress (version 1).
    w.varu(1);
    w.boolean(survey_.active);
    if (survey_.active) {
        const Survey& s = survey_;
        w.varu(s.anchors.size());
        for (const Vec3i& a : s.anchors) w.vec3i(a);
        w.varu(s.anchor);
        w.varu(s.open.size());
        for (u8 o : s.open) w.u8v(o);
        write_region_cells(w, s.map);
        w.varu(s.queue.size() - s.head);
        for (size_t i = s.head; i < s.queue.size(); ++i) w.vec3i(s.queue[i]);
        w.u32v(s.pushed);
        w.vec3i(s.seed);
        w.u16v(s.id);
        w.boolean(s.cut);
    }
    w.end_section(sec);
}

void Agents::load(BinReader& outer) {
    BinReader r = outer.section("AGNT");
    u64 st = r.u64v(), inc = r.u64v();
    rng_.set_raw(st, inc);
    u64 n = r.varu();
    chars_.clear();
    chars_.resize(1);
    for (size_t i = 1; i < (size_t)n; ++i) {
        auto cp = std::make_unique<Character>();
        Character& c = *cp;
        c.id = (EntityId)i;
        c.kind = (CharKind)r.u8v();
        c.name = r.str();
        c.female = r.boolean();
        c.alive = r.boolean();
        c.departed = r.boolean();
        c.born = r.u64v();
        c.died = r.u64v();
        c.death_cause = r.str();
        c.death_event = r.u32v();
        c.polity = r.u16v();
        c.pos = r.vec3f();
        c.foot = r.vec3i();
        c.yaw = r.f32();
        c.fall_speed = r.f32();
        c.walk_phase = r.f32();
        c.body.load(r);
        c.look.skin = r.u32v();
        c.look.hair = r.u32v();
        c.look.cloth = r.u32v();
        c.look.accent = r.u32v();
        c.look.shoes = r.u32v();
        c.look.eyes = r.u32v();
        r.u32v();  // bone colour (fixed)
        c.look.long_hair = r.boolean();
        c.look.dress = r.boolean();
        c.look.ribbon = r.boolean();
        c.needs.food = r.f32();
        c.needs.water = r.f32();
        c.needs.rest = r.f32();
        c.needs.social = r.f32();
        c.needs.safety = r.f32();
        c.needs.comfort = r.f32();
        c.mood = r.f32();
        c.stress = r.f32();
        c.fear = r.f32();
        c.work_debt = r.f32();
        load_pers(r, c.pers);
        for (float& s : c.skills) s = r.f32();
        u64 nr = r.varu();
        for (u64 k = 0; k < nr; ++k) {
            Relation rel;
            rel.other = r.u32v();
            rel.affinity = r.f32();
            c.relations.push_back(rel);
        }
        u64 ns = r.varu();
        for (u64 k = 0; k < ns; ++k) {
            Support s;
            s.girl = r.u32v();
            s.value = r.f32();
            c.support.push_back(s);
        }
        u64 nm = r.varu();
        for (u64 k = 0; k < nm; ++k) {
            Memory m;
            m.tick = r.u64v();
            m.kind = (MemoryKind)r.u8v();
            m.subject = r.u32v();
            m.valence = r.f32();
            m.event = r.u32v();
            c.memories.push_back(m);
        }
        c.inv = r.u32v();
        c.tool = r.u16v();
        c.weapon = r.u16v();
        c.armor = r.u16v();
        c.home = r.u32v();
        c.occupation = r.str();
        c.task.type = (TaskType)r.u8v();
        c.task.step = r.u8v();
        c.task.job = r.u32v();
        c.task.target = r.vec3i();
        c.task.target2 = r.vec3i();
        c.task.store = r.u32v();
        c.task.item = r.u16v();
        c.task.count = (i32)r.vari();
        c.task.other = r.u32v();
        c.task.started = r.u64v();
        c.task.until = r.u64v();
        c.task.utility = r.f32();
        c.task.fails = (int)r.vari();
        c.task.label = r.str();
        u64 np = r.varu();
        for (u64 k = 0; k < np; ++k) c.path.nodes.push_back(r.vec3i());
        c.path.next = (size_t)r.varu();
        c.path_goal = r.vec3i();
        c.water_spot = r.vec3i();
        c.region = (u16)r.varu();
        c.next_think = r.u64v();
        c.last_ate = r.u64v();
        c.last_drank = r.u64v();
        c.last_slept = r.u64v();
        c.last_social = r.u64v();
        c.last_punished = r.u64v();
        u64 nu = r.varu();
        for (u64 k = 0; k < nu; ++k) {
            Vec3i p = r.vec3i();
            Tick t = r.u64v();
            c.unreachable.push_back({p, t});
        }
        c.drafted = r.boolean();
        c.rebel = r.boolean();
        c.sleeping = r.boolean();
        c.status_text = r.str();
        if (r.boolean()) {
            c.girl = std::make_unique<GirlData>();
            GirlData& g = *c.girl;
            g.drive = r.str();
            g.level = (int)r.vari();
            g.xp = r.f32();
            g.mana = r.f32();
            load_pers(r, g.persona);
            g.temperament = r.str();
            g.role = r.str();
            g.domain = r.str();
            g.loyalty = r.f32();
            g.ambition_pressure = r.f32();
            u64 nc = r.varu();
            g.spell_cooldowns.clear();
            for (u64 k = 0; k < nc; ++k) g.spell_cooldowns.push_back(r.u32v());
            g.last_decision = r.u64v();
            u64 nd = r.varu();
            for (u64 k = 0; k < nd; ++k) g.decisions.push_back(r.u32v());
            g.stance = r.str();
            u64 ne = r.varu();
            for (u64 k = 0; k < ne; ++k) {
                std::string key = r.str();
                float v = r.f32();
                g.experience.push_back({key, v});
            }
            g.grudge = r.u32v();
        }
        chars_.push_back(std::move(cp));
    }
    dangers_.clear();
    u64 ndg = r.varu();
    for (u64 k = 0; k < ndg; ++k) {
        Vec3f p = r.vec3f();
        float rad = r.f32();
        dangers_.push_back({p, rad});
    }
    water_spots_.clear();
    u64 nws = r.varu();
    water_regions_.clear();
    for (u64 k = 0; k < nws; ++k) {
        water_spots_.push_back(r.vec3i());
        water_regions_.push_back((u16)r.varu());
    }
    u8 dirty = r.u8v();
    ctx_.nav->major_dirty = (dirty & 1) != 0;
    ctx_.nav->minor_dirty = (dirty & 2) != 0;
    region_built_ = r.u64v();
    region_anchors_.clear();
    u64 na = r.varu();
    for (u64 k = 0; k < na; ++k) region_anchors_.push_back(r.vec3i());
    read_region_cells(r, region_map_);
    if (!r.at_end()) {
        u64 ne = r.varu();
        for (u64 k = 0; k < ne; ++k) {
            EntityId id = r.u32v();
            ItemId cart = (ItemId)r.u32v();
            Tick until = r.u64v();
            bool well = r.boolean();
            if (Character* c = get(id)) {
                c->cart = cart;
                c->treated_until = until;
                c->treated_well = well;
            }
        }
    }
    day = {};
    fail_ring_.fill(0);
    fail_ring_pos_ = 0;
    if (!r.at_end()) {
        int* counters[] = {&day.harvested, &day.ate_public, &day.refused_food, &day.thefts, &day.path_failures,
                           &day.drinks, &day.hungry_no_food, &day.thirsty_no_water};
        for (int* v : counters) *v = (int)r.vari();
        for (int& v : fail_ring_) v = (int)r.vari();
        fail_ring_pos_ = (int)r.vari();
    }
    if (!r.at_end()) {
        const u64 version = r.varu();
        const u64 count = r.varu();
        for (size_t i = 1; i < (size_t)count; ++i) {
            Character* c = i < chars_.size() ? chars_[i].get() : nullptr;
            Character dummy;
            Character& ch = c ? *c : dummy;
            if (version >= 1) {
                ch.clothes = r.u16v();
                ch.tool_wear = r.u16v();
                ch.task.resume = r.u8v();
            }
            if (version >= 2) {
                ch.age0 = r.f32();
                ch.partner = r.u32v();
                ch.parents[0] = r.u32v();
                ch.parents[1] = r.u32v();
                ch.last_child = r.u64v();
                const Tick aw = r.u64v();
                if (ch.girl) ch.girl->awakened = aw;
            }
            if (version >= 3) {
                ch.empowered_until = r.u64v();
                ch.empowered = r.f32();
            }
            if (version >= 4) ch.no_food_until = r.u64v();
            if (version >= 5) ch.in_boat = r.boolean();
            if (version >= 6) ch.moving = r.boolean();
        }
    }
    region_open_.assign(1, 0);
    if (!r.at_end()) {
        region_open_.clear();
        const u64 n = r.varu();
        for (u64 k = 0; k < n; ++k) region_open_.push_back(r.u8v());
    } else {
        // Older saves: every survey region may reach beyond itself.
        u16 top = 0;
        region_map_.for_each([&](const Vec3i&, u16 id) { top = std::max(top, id); });
        region_open_.assign((size_t)top + 1, 1);
    }
    if (!r.at_end()) {
        r.varu();  // drama block version
        const u64 count = r.varu();
        for (size_t i = 1; i < (size_t)count; ++i) {
            if (!r.boolean()) continue;
            GirlData dummy;
            Character* c = i < chars_.size() ? chars_[i].get() : nullptr;
            GirlData& g = c && c->girl ? *c->girl : dummy;
            const u64 nb = r.varu();
            g.bonds.clear();
            for (u64 k = 0; k < nb; ++k) {
                Bond b;
                b.other = r.u32v();
                b.kind = (BondKind)r.u8v();
                b.since = r.u64v();
                b.event = r.u32v();
                g.bonds.push_back(b);
            }
            g.trauma = r.f32();
            g.solace = r.f32();
            const u64 nm = r.varu();
            g.marks.clear();
            for (u64 k = 0; k < nm; ++k) g.marks.push_back(r.u32v());
            g.born_drive = r.str();
            g.drive_changed = r.u64v();
            g.duel = r.u32v();
            g.duel_since = r.u64v();
        }
    }
    survey_ = Survey{};
    if (!r.at_end()) {
        r.varu();  // survey block version
        if (r.boolean()) {
            Survey& s = survey_;
            s.active = true;
            const u64 na = r.varu();
            for (u64 k = 0; k < na; ++k) s.anchors.push_back(r.vec3i());
            s.anchor = (size_t)r.varu();
            const u64 no = r.varu();
            for (u64 k = 0; k < no; ++k) s.open.push_back(r.u8v());
            read_region_cells(r, s.map);
            const u64 nq = r.varu();
            for (u64 k = 0; k < nq; ++k) s.queue.push_back(r.vec3i());
            s.pushed = r.u32v();
            s.seed = r.vec3i();
            s.id = r.u16v();
            s.cut = r.boolean();
        }
    }
}

u64 Agents::hash() const {
    u64 h = hash_combine(rng_.state(), rng_.inc());
    for (auto& c : chars_) {
        if (!c) continue;
        h = fnv1a64(&c->pos, sizeof(c->pos), h);
        h = fnv1a64(&c->needs, sizeof(c->needs), h);
        h = hash_combine(h, (u64)c->task.type);
        h = hash_combine(h, (u64)c->body.total_alive());
        h = hash_combine(h, ((u64)c->partner << 32) ^ (u64)c->parents[0] ^ ((u64)c->parents[1] << 16));
        h = fnv1a64(&c->age0, sizeof(c->age0), h);
        h = hash_combine(h, c->empowered_until);
        if (c->girl) {
            h = hash_combine(h, ((u64)c->girl->bonds.size() << 32) ^ (u64)c->girl->duel ^ ((u64)c->girl->drive_changed << 8));
            h = fnv1a64(&c->girl->trauma, sizeof(float), h);
        }
    }
    if (survey_.active)
        h = hash_combine(h, ((u64)survey_.anchor << 48) ^ ((u64)survey_.pushed << 16) ^ (u64)survey_.map.size());
    return h;
}

}  // namespace icarus
