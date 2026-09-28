// Agents: the course of a life. People grow up, pair off, have children when home and
// larder allow, grow old and die; magical girls awaken among the people when a polity
// has fewer than it should, their drive shaped by what the polity has lived through.
// Everything here runs once a day (early morning) with the agents' own random stream.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/farming.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/scenario.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
const Json& life(const Registry& reg) { return reg.doc("life"); }
float life_f(const Registry& reg, const char* key, float def) { return life(reg).flt(key, def); }
float span(const Registry& reg, const char* key, int i, float def) {
    const Json& a = life(reg)[key];
    return a.is_array() && a.size() > (size_t)i ? a[(size_t)i].as_float(def) : def;
}
}  // namespace

float Agents::age_years(const Character& c) const {
    const Tick now = ctx_.now;
    const float days = (float)(now >= c.born ? now - c.born : 0) / (float)kTicksPerDay;
    return c.age0 + days * life_f(*ctx_.reg, "years_per_day", 0.75f);
}

bool Agents::is_child(const Character& c) const { return age_years(c) < life_f(*ctx_.reg, "adult_age", 14.0f); }

bool Agents::is_elder(const Character& c) const { return age_years(c) >= life_f(*ctx_.reg, "elder_age", 55.0f); }

void Agents::set_first_age(Character& c) {
    const Registry& reg = *ctx_.reg;
    const bool girl = c.kind == CharKind::MagicalGirl;
    const float lo = span(reg, girl ? "girl_first_ages" : "first_ages", 0, girl ? 15.0f : 16.0f);
    const float hi = span(reg, girl ? "girl_first_ages" : "first_ages", 1, girl ? 19.0f : 44.0f);
    c.age0 = lo + (hi - lo) * hash_to_unit(hash3(0xA9E5ull, (i32)c.id, (i32)(c.born & 0x7FFFFFFF), girl ? 1 : 0));
}

std::vector<EntityId> Agents::children_of(EntityId id) const {
    std::vector<EntityId> out;
    for (const auto& cp : chars_)
        if (cp && cp->alive && (cp->parents[0] == id || cp->parents[1] == id)) out.push_back(cp->id);
    return out;
}

void Agents::daily_life() {
    now_ = ctx_.now;
    const Registry& reg = *ctx_.reg;
    const float adult = life_f(reg, "adult_age", 14.0f);
    const float per_day = life_f(reg, "years_per_day", 0.75f);
    auto emit = [&](EventType type, u8 sev, const Character& who, EntityId other, EventId cause, std::string text) {
        Event e;
        e.type = type;
        e.severity = sev;
        e.pos = who.foot;
        e.actor = who.id;
        e.target = other;
        e.polity = who.polity;
        e.causes[0] = cause;
        e.text = std::move(text);
        return ctx_.chron->emit(std::move(e));
    };

    // Old age.
    const float d0 = span(reg, "death_age", 0, 60.0f), d1 = span(reg, "death_age", 1, 84.0f);
    const float girl_f = life_f(reg, "girl_life_factor", 1.25f);
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character& c = *chars_[i];
        if (!c.alive || c.departed) continue;
        const float f = c.is_girl() ? girl_f : 1.0f;
        const float age = age_years(c), lo = d0 * f, hi = d1 * f;
        if (age < lo) continue;
        const float k = clampv((age - lo) / std::max(1.0f, hi - lo), 0.0f, 1.0f);
        if (age >= hi || rng_.chance(0.04f + 0.4f * k * k)) kill(c, "年老", 0);
    }

    // Coming of age.
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character& c = *chars_[i];
        if (!c.alive || c.departed || c.is_girl()) continue;
        const float age = age_years(c);
        if (age >= adult && age - per_day < adult) {
            // What they took to while growing up becomes their trade.
            int top = 0;
            for (int s = 1; s < kSkillCount; ++s)
                if (c.skills[s] > c.skills[top]) top = s;
            c.occupation = (top == kFarming || top == kCooking) ? "food" : (top == kBuilding ? "build" : "gather");
            emit(EventType::Life, 1, c, kNoEntity, 0, strfmt("%s长大成人", c.name.c_str()));
        }
    }

    // Living under one roof (or around one fire) brings people closer, a little each day.
    const float near = life_f(reg, "housemate_affinity", 0.03f);
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character& a = *chars_[i];
        if (!a.alive || a.departed || a.is_girl()) continue;
        for (size_t k = i + 1; k < chars_.size(); ++k) {
            Character& b = *chars_[k];
            if (!b.alive || b.departed || b.is_girl() || b.polity != a.polity) continue;
            if (a.home != b.home) continue;  // both homeless: the band around its fire
            const float d = a.affinity(b.id) < -0.1f ? 0.0f : near;
            if (d <= 0.0f) continue;
            a.affinity_ref(b.id) = std::min(1.0f, a.affinity(b.id) + d);
            b.affinity_ref(a.id) = std::min(1.0f, b.affinity(a.id) + d);
        }
    }

    // Partnerships: two unattached adults of a polity who have grown fond of each other.
    const float want = life_f(reg, "partner_affinity", 0.3f);
    const float wed_age = life_f(reg, "partner_age", 18.0f);
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character& a = *chars_[i];
        if (!a.alive || a.departed || a.is_girl() || a.partner || age_years(a) < wed_age) continue;
        Character* best = nullptr;
        float bs = want;
        for (size_t k = i + 1; k < chars_.size(); ++k) {
            Character& b = *chars_[k];
            if (!b.alive || b.departed || b.is_girl() || b.partner || b.polity != a.polity || age_years(b) < wed_age)
                continue;
            // Not with one's own parent or child.
            if (b.parents[0] == a.id || b.parents[1] == a.id || a.parents[0] == b.id || a.parents[1] == b.id) continue;
            const float s = std::min(a.affinity(b.id), b.affinity(a.id));
            if (s > bs) {
                bs = s;
                best = &b;
            }
        }
        if (!best) continue;
        a.partner = best->id;
        best->partner = a.id;
        a.remember(now_, MemoryKind::Partnered, best->id, 0.2f, 0);
        best->remember(now_, MemoryKind::Partnered, a.id, 0.2f, 0);
        // They move in together when one of them has room.
        for (auto [x, y] : {std::pair<Character*, Character*>{&a, best}, {best, &a}}) {
            const Building* h = ctx_.buildings->get(x->home);
            if (!h || !h->functional || x->home == y->home) continue;
            int in = 0;
            for (const auto& cp : chars_)
                if (cp && cp->alive && cp->home == x->home) ++in;
            if (in < h->beds) {
                y->home = x->home;
                break;
            }
        }
        emit(EventType::Life, 2, a, best->id, 0, strfmt("%s与%s结为伴侣", a.name.c_str(), best->name.c_str()));
    }

    // Births.
    const float chance = life_f(reg, "birth_chance", 0.3f);
    const Tick cooldown = (Tick)(life_f(reg, "birth_cooldown_days", 10.0f) * (float)kTicksPerDay);
    const int max_children = life(reg).integer("max_children", 3);
    const float min_mood = life_f(reg, "min_mood", 0.45f), min_days = life_f(reg, "min_food_days", 2.0f);
    const float cap = life_f(reg, "soft_cap", 120.0f);
    const Json& names = reg.doc("names");
    std::vector<std::pair<EntityId, EntityId>> couples;
    for (size_t i = 1; i < chars_.size(); ++i) {
        const Character& a = *chars_[i];
        const Character* b = get(a.partner);
        if (a.alive && !a.departed && b && b->alive && !b->departed && a.id < b->id && a.polity == b->polity)
            couples.push_back({a.id, b->id});
    }
    for (auto [ia, ib] : couples) {
        Character& a = *get(ia);
        Character& b = *get(ib);
        const Polity* p = ctx_.society->polity(a.polity);
        if (!p) continue;
        if (now_ - std::max(a.last_child, b.last_child) < cooldown && std::max(a.last_child, b.last_child) > 0) continue;
        if ((int)children_of(a.id).size() >= max_children) continue;
        if (is_elder(a) || is_elder(b) || a.is_girl() || b.is_girl()) continue;
        if ((a.mood + b.mood) * 0.5f < min_mood) continue;
        const bool band = ctx_.society->foraging_band(*p);
        // Nobody starts a family while going hungry; beyond that, whether the people can
        // feed and house more mouths for good is the steward's judgement (the plan's
        // birth allowance: none when stores are short, more when the land has room).
        if (p->stats.food_access < 0.85f) continue;
        const float allow = p->plan.at ? p->plan.birth : (p->stats.food_days >= min_days ? 1.0f : 0.0f);
        float pr = chance * allow * clampv(1.0f - (float)p->stats.population / std::max(10.0f, cap), 0.1f, 1.0f);
        const Building* h = ctx_.buildings->get(a.home);
        if (!h || !h->functional) {
            pr *= band ? life_f(reg, "band_factor", 0.5f) : life_f(reg, "crowding", 0.3f);
        } else {
            int in = 0;
            for (const auto& cp : chars_)
                if (cp && cp->alive && !cp->departed && cp->home == a.home) ++in;
            if (in >= h->beds) pr *= life_f(reg, "crowding", 0.3f);
        }
        if (!rng_.chance(pr)) continue;
        // A child: something of each parent in looks and temper.
        const bool female = rng_.chance(0.5f);
        // A name nobody living bears.
        const Json& pool = names[female ? "female_names" : "male_names"];
        std::string nm = "?";
        for (int tries = 0; tries < 40 && pool.is_array() && pool.size(); ++tries) {
            nm = pool[(size_t)rng_.below((u32)pool.size())].as_str();
            bool taken = false;
            for (const auto& cp : chars_)
                if (cp && cp->alive && cp->name == nm) taken = true;
            if (!taken) break;
            if (tries == 39) nm = "小" + nm;
        }
        const Vec3i at = h && h->functional ? h->entrance : a.foot;
        const EntityId kid_id = spawn(CharKind::Resident, nm, female, at, a.polity);
        Character& kid = *get(kid_id);
        // `a` and `b` may have moved in memory: fetch them again.
        Character& pa = *get(ia);
        Character& pb = *get(ib);
        randomize(kid, rng_);
        kid.age0 = 0.0f;
        kid.born = now_;
        kid.parents[0] = pa.id;
        kid.parents[1] = pb.id;
        kid.home = pa.home;
        for (int k = 0; k < Personality::kCount; ++k)
            kid.pers.at(k) = clampv(0.5f * (pa.pers.at(k) + pb.pers.at(k)) + rng_.normalish(0.0f, 0.1f), 0.02f, 0.98f);
        for (float& s : kid.skills) s = clampv(s * 0.3f, 0.02f, 0.3f);
        kid.look.hair = rng_.chance(0.5f) ? pa.look.hair : pb.look.hair;
        kid.look.skin = rng_.chance(0.5f) ? pa.look.skin : pb.look.skin;
        kid.body.build(kid.look);
        kid.needs.food = kid.needs.water = kid.needs.rest = 1.0f;
        for (const Support& s : pa.support) kid.support_ref(s.girl) = 0.5f * (s.value + pb.support_for(s.girl));
        kid.affinity_ref(pa.id) = kid.affinity_ref(pb.id) = 0.6f;
        pa.affinity_ref(kid.id) = pb.affinity_ref(kid.id) = 0.6f;
        pa.last_child = pb.last_child = now_;
        pa.remember(now_, MemoryKind::ChildBorn, kid.id, 0.3f, 0);
        pb.remember(now_, MemoryKind::ChildBorn, kid.id, 0.3f, 0);
        emit(EventType::Life, 2, kid, pa.id, 0, strfmt("%s与%s的孩子%s出生了", pa.name.c_str(), pb.name.c_str(), kid.name.c_str()));
    }

    // Magical girls awaken among the people of a polity that has too few.
    const int wanted = life(reg).integer("girls_wanted", 3);
    const Tick gap = (Tick)(life_f(reg, "awaken_days", 3.0f) * (float)kTicksPerDay);
    for (const Polity& pc : ctx_.society->polities()) {
        if (!pc.alive) continue;
        int girls = 0, people = 0;
        Tick last = 0;
        for (const auto& cp : chars_) {
            if (!cp || !cp->alive || cp->departed || cp->polity != pc.id) continue;
            if (cp->is_girl()) {
                ++girls;
                last = std::max(last, cp->girl->awakened);
            } else if (!is_child(*cp)) {
                ++people;
            }
        }
        // A leaderless people soon finds a girl among them; otherwise it takes time.
        const Tick since = std::max(last, pc.founded);
        if (girls >= wanted || people < 4) continue;
        if (girls > 0 && now_ - since < gap) continue;
        if (!rng_.chance(girls == 0 ? 0.9f : life_f(reg, "awaken_chance", 0.2f))) continue;
        awaken(pc, 0);
    }
}

bool Agents::awaken(const Polity& pc, EventId cause) {
    const Registry& reg = *ctx_.reg;
    auto emit = [&](EventType type, u8 sev, const Character& who, EntityId other, EventId because, std::string text) {
        Event e;
        e.type = type;
        e.severity = sev;
        e.pos = who.foot;
        e.actor = who.id;
        e.target = other;
        e.polity = who.polity;
        e.causes[0] = because;
        e.text = std::move(text);
        return ctx_.chron->emit(std::move(e));
    };
    {
        // She is the one the times weigh on most: strong memories, strong feelings.
        Character* pick = nullptr;
        float best = -1e9f;
        float good = 0, bad = 0;
        for (const auto& cp : chars_) {
            if (!cp || !cp->alive || cp->departed || cp->polity != pc.id || cp->is_girl() || is_child(*cp)) continue;
            if (cp->drafted || is_elder(*cp) || !cp->female) continue;
            float weight = 0;
            for (const Memory& m : cp->memories) {
                weight += std::fabs(m.valence);
                (m.valence > 0 ? good : bad) += std::fabs(m.valence);
            }
            weight += 0.3f * std::fabs(cp->mood - 0.5f) + 0.2f * hash_to_unit(hash3(now_, (i32)cp->id, 5, 9));
            if (weight > best) {
                best = weight;
                pick = cp.get();
            }
        }
        if (!pick) return false;
        // The drive answers what the polity has been through.
        const Polity& p = pc;
        const bool hungry = p.crisis(CrisisKind::Food) && p.crisis(CrisisKind::Food)->active;
        const bool war = !p.wars.empty();
        const bool oppressed = p.policies.punishment > 0.5f || p.stats.ruler_support < -0.2f;
        const bool bright = pick->mood > 0.55f && good >= bad;
        std::string drive;
        if (war) drive = bright ? "courage" : "wrath";
        else if (hungry) drive = bright ? "gourmet" : "gluttony";
        else if (oppressed) drive = bright ? "light" : (pick->pers.ambition > 0.5f ? "envy" : "despair");
        else drive = bright ? "hope" : (pick->pers.ambition > 0.55f ? "envy" : "light");
        Character& g = *pick;
        g.kind = CharKind::MagicalGirl;
        g.girl = std::make_unique<GirlData>();
        const Json none = Json::object();
        make_girl(ctx_, g.id, drive, rng_, none);
        g.girl->level = 1;
        g.girl->awakened = now_;
        g.girl->loyalty = clampv(0.5f + g.support_for(p.ruler) * 0.5f, 0.0f, 1.0f);
        g.drafted = false;
        g.home = 0;
        g.task = Task{};
        g.next_think = now_;
        std::string dname = drive;
        for (const Json& d : reg.doc("drives")["drives"].items())
            if (d.str("key") == drive) dname = d.str("name");
        const EventId ev = emit(EventType::Awakening, 4, g, kNoEntity, cause,
                                strfmt("%s觉醒为魔法少女，源动力：%s%s", g.name.c_str(), dname.c_str(),
                                       war ? "（生于战火）" : hungry ? "（生于饥馑）" : oppressed ? "（生于压迫）" : ""));
        take_student(g, ev);
    }
    return true;
}

}  // namespace icarus
