#include "icarus/fauna/fauna.h"

#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/economy.h"
#include "icarus/society/society.h"
#include "icarus/sim/chronicle.h"
#include "icarus/sim/clock.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
constexpr int kNearPeople = 96;        // cubes: animals this close to someone run every tick
constexpr int kIndexCell = 16;         // people index granularity
constexpr Tick kCarcassTicks = kTicksPerDay * 2;
constexpr float kGrownDays = 5.0f;

// Fish swim a third of a cube above the floor of the cube they are in.
float lift(const SpeciesDef& s) { return s.aquatic ? 0.35f : 0.0f; }

i64 cell_key(int x, int z) { return ((i64)floordiv(x, kIndexCell) << 32) ^ (i64)(u32)floordiv(z, kIndexCell); }

Temper temper_from(const std::string& t) {
    if (t == "defensive") return Temper::Defensive;
    if (t == "predator") return Temper::Predator;
    if (t == "territorial") return Temper::Territorial;
    return Temper::Shy;
}

Biome biome_from(const std::string& k) {
    for (int b = 0; b < (int)Biome::Count; ++b)
        if (k == biome_key((Biome)b)) return (Biome)b;
    return Biome::Sky;
}

u32 parse_hex(const std::string& s) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    return (u32)std::strtoul(h.c_str(), nullptr, 16);
}
}  // namespace

void Fauna::load_species(const Registry& reg) {
    species_.clear();
    const Json& list = reg.doc("animals")["species"];
    for (const Json& e : list.items()) {
        SpeciesDef s;
        s.id = (u16)species_.size();
        s.key = e.str("key");
        s.name = e.str("name", s.key);
        const Json& sz = e["size"];
        if (sz.is_array() && sz.size() >= 3) s.size = {(float)sz[0].as_num(), (float)sz[1].as_num(), (float)sz[2].as_num()};
        s.tall = s.size.y > 1.1f;
        s.aquatic = e.str("habitat", "land") == "water";
        s.water_density = e.flt("water_density", 0.0f);
        s.speed = e.flt("speed", 0.2f);
        s.run = e.flt("run", 0.5f);
        s.stamina = e.integer("stamina", 300);
        s.hp = std::max(0.05f, e.flt("hp", 0.5f));
        s.temper = temper_from(e.str("temper", "shy"));
        s.flee = e.flt("flee", 10.0f);
        s.attack = e.flt("attack", 0.0f);
        s.guard = e.flt("guard", 0.0f);
        const Json& herd = e["herd"];
        if (herd.is_array() && herd.size() >= 2) {
            s.herd_min = herd[0].as_int();
            s.herd_max = herd[1].as_int();
        }
        const Json& lit = e["litter"];
        if (lit.is_array() && lit.size() >= 2) {
            s.litter_min = lit[0].as_int();
            s.litter_max = lit[1].as_int();
        }
        s.lifespan_days = e.flt("lifespan_days", 100.0f);
        s.breed_days = std::max(1.0f, e.flt("breed_days", 10.0f));
        for (const auto& [k, v] : e["biomes"].members()) s.biomes.push_back({biome_from(k), (float)v.as_num()});
        for (const auto& [k, v] : e["yield"].members()) {
            ItemId it = reg.find_item(k);
            if (it != kNoItem) s.yield.push_back({it, v.as_int()});
        }
        for (const Json& c : e["colors"].items()) s.colors.push_back(parse_hex(c.as_str()));
        for (const Json& l : e["look"].items()) s.look.push_back(l.as_str());
        species_.push_back(std::move(s));
    }
    // Prey by key, now that every species has an id.
    for (size_t i = 0; i < species_.size(); ++i) {
        const Json& e = list[i];
        for (const Json& p : e["prey"].items()) {
            const int id = species_id(p.as_str());
            if (id >= 0) species_[i].prey.push_back((u16)id);
        }
    }
    capacity_.assign(species_.size(), 0);
}

int Fauna::species_id(const std::string& key) const {
    for (const SpeciesDef& s : species_)
        if (s.key == key) return s.id;
    return -1;
}

void Fauna::reset(u64 seed) {
    rng_.seed(seed, 0xFA0A);
    animals_.clear();
    capacity_.assign(species_.size(), 0);
    next_id_ = 1;
    now_ = 0;
    people_.clear();
    people_at_ = ~0ull;
}

Animal* Fauna::get(u32 id) {
    auto it = std::lower_bound(animals_.begin(), animals_.end(), id, [](const Animal& a, u32 v) { return a.id < v; });
    return (it != animals_.end() && it->id == id) ? &*it : nullptr;
}

const Animal* Fauna::get(u32 id) const {
    auto it = std::lower_bound(animals_.begin(), animals_.end(), id, [](const Animal& a, u32 v) { return a.id < v; });
    return (it != animals_.end() && it->id == id) ? &*it : nullptr;
}

int Fauna::count_alive(int species) const {
    int n = 0;
    for (const Animal& a : animals_)
        if (a.alive && (species < 0 || a.species == species)) ++n;
    return n;
}

// ------------------------------------------------------------------------ the ground

bool Fauna::walkable(const Vec3i& p, bool tall) const {
    const World& w = *ctx_.world;
    if (p.y < 1 || !w.in_bounds(p) || !w.in_bounds(p + Vec3i{0, 1, 0})) return false;
    if (rough_) {
        const ColumnInfo col = w.gen().column(p.x, p.z);
        return col.land && col.water_top < 0 && p.y == col.top + 1;
    }
    const Registry& reg = *ctx_.reg;
    const Material& below = reg.mat(vmat(w.peek(p - Vec3i{0, 1, 0})));
    if (!below.solid || below.passable || below.foliage || below.fluid) return false;
    auto clear = [&](const Vec3i& q) {
        const Material& m = reg.mat(vmat(w.peek(q)));
        return (!m.solid || m.passable) && !m.fluid;
    };
    return clear(p) && (!tall || clear(p + Vec3i{0, 1, 0}));
}

bool Fauna::swimmable(const Vec3i& p) const {
    const World& w = *ctx_.world;
    if (p.y < 1 || !w.in_bounds(p)) return false;
    if (rough_) {
        const ColumnInfo col = w.gen().column(p.x, p.z);
        return col.water_top >= 0 && p.y > col.top && p.y < col.water_top + (col.frozen ? 0 : 1);
    }
    const Voxel v = w.peek(p);
    return vmat(v) == ctx_.reg->m().water && vlevel(v) >= 4;
}

bool Fauna::fits(const SpeciesDef& s, const Vec3i& p) const {
    return s.aquatic ? swimmable(p) : walkable(p, s.tall);
}

bool Fauna::find_spot(Vec3i& p, const SpeciesDef& s) const {
    if (s.aquatic) {
        // Mid-water: the middle of the water column under p.
        int lo = 1 << 30, hi = -(1 << 30);
        for (int dy = -6; dy <= 3; ++dy)
            if (swimmable(p + Vec3i{0, dy, 0})) {
                lo = std::min(lo, dy);
                hi = std::max(hi, dy);
            }
        if (lo > hi) return false;
        const int mid = (lo + hi) / 2;
        p.y += swimmable(p + Vec3i{0, mid, 0}) ? mid : lo;
        return true;
    }
    for (int dy = 3; dy >= -6; --dy) {
        const Vec3i q = p + Vec3i{0, dy, 0};
        if (walkable(q, s.tall)) {
            p = q;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------ population

u32 Fauna::spawn(u16 s, const Vec3i& foot, u32 herd, Tick now) {
    Animal a;
    a.id = next_id_++;
    a.species = s;
    a.female = rng_.chance(0.5f);
    a.foot = foot;
    a.next = foot;
    a.goal = foot;
    a.home = foot;
    a.pos = Vec3f((float)foot.x + 0.5f, (float)foot.y + lift(species_[s]), (float)foot.z + 0.5f);
    a.yaw = rng_.uniform(0.0f, 6.2831853f);
    a.herd = herd ? herd : a.id;
    a.born = now;
    a.next_think = now + rng_.below(60);
    animals_.push_back(a);
    return a.id;
}

void Fauna::spawn_herd(u16 s, const Vec3i& at, int n, Tick now) {
    u32 leader = 0;
    for (int i = 0; i < n; ++i) {
        Vec3i p = at + Vec3i{rng_.range(-2, 2), 0, rng_.range(-2, 2)};
        if (!find_spot(p, species_[s])) continue;
        const u32 id = spawn(s, p, leader, now);
        if (!leader) leader = id;
        // Start at any age up to half a life.
        Animal* a = get(id);
        a->born = now - (Tick)(rng_.uniform(kGrownDays, species_[s].lifespan_days * 0.5f) * (float)kTicksPerDay);
        if (now < (Tick)0) a->born = 0;
    }
}

void Fauna::populate() {
    rough_ = true;  // the start: the terrain is as generated away from the sites
    const World& w = *ctx_.world;
    const WorldGen& gen = w.gen();
    const IslandFeatures& f = gen.features();
    const bool continent = w.config().layout == WorldLayout::Continent;
    constexpr int G = 8;
    const int W = w.size_x(), D = w.size_z();
    std::vector<int> spawned(species_.size(), 0);
    for (int z = G / 2; z < D; z += G)
        for (int x = G / 2; x < W; x += G) {
            // Cheap reject: far from every island.
            bool near_island = false;
            for (const IslandDef& is : gen.islands()) {
                const float dx = (float)x - is.cx, dz = (float)z - is.cz;
                if (dx * dx + dz * dz < is.radius * is.radius * 1.7f) near_island = true;
            }
            if (!near_island) continue;
            const ColumnInfo col = gen.column_uncached(x, z);
            // Fish: schools in deep water (lakes, ponds, the village's spring).
            if (col.water_top >= 0 && col.water_top - col.top >= 2)
                for (const SpeciesDef& s : species_) {
                    if (!s.aquatic || s.water_density <= 0.0f || !rng_.chance(s.water_density)) continue;
                    const int n = rng_.range(s.herd_min, s.herd_max);
                    const size_t before = animals_.size();
                    spawn_herd(s.id, Vec3i{x, col.water_top, z}, n, 0);
                    spawned[s.id] += (int)(animals_.size() - before);
                }
            if (!col.land || col.water_top >= 0 || col.reserved) continue;
            float site_d = 1e9f;
            for (const Site& s : f.sites)
                site_d = std::min(site_d, std::sqrt((float)((s.center.x - x) * (s.center.x - x) + (s.center.z - z) * (s.center.z - z))));
            for (const SpeciesDef& s : species_) {
                if (s.aquatic) continue;
                const bool fierce = s.temper == Temper::Predator || s.temper == Temper::Territorial;
                // The classic island is small and peaceful; beasts keep well away from homes.
                if (fierce && (!continent || site_d < 110.0f)) continue;
                if (site_d < 40.0f) continue;
                const float p = s.density(col.biome) * (float)(G * G);
                if (p <= 0.0f || !rng_.chance(p)) continue;
                const int n = rng_.range(s.herd_min, s.herd_max);
                const size_t before = animals_.size();
                spawn_herd(s.id, Vec3i{x, col.top + 1, z}, n, 0);
                spawned[s.id] += (int)(animals_.size() - before);
            }
        }
    for (size_t s = 0; s < species_.size(); ++s)
        capacity_[s] = std::max(spawned[s] > 0 ? 4 : 0, (int)std::ceil((float)spawned[s] * 1.25f));
}

// ------------------------------------------------------------------------ people nearby

void Fauna::index_people() {
    people_.clear();
    const World& w = *ctx_.world;
    const int cw = w.cells_x(), cd = w.cells_z();
    near_.assign((size_t)cw * cd, 0);
    people_at_ = now_;
    if (!ctx_.agents) return;
    const int r = kNearPeople / kCellSize + 1;
    for (const auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed) continue;
        people_.push_back({cell_key(cp->foot.x, cp->foot.z), cp->id});
        const int cx = cp->foot.x >> kCellBits, cz = cp->foot.z >> kCellBits;
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                const int x = cx + dx, z = cz + dz;
                if (x >= 0 && z >= 0 && x < cw && z < cd) near_[(size_t)z * cw + x] = 1;
            }
    }
    std::sort(people_.begin(), people_.end());
}

bool Fauna::people_near(const Vec3i& p, int radius) const {
    (void)radius;  // the grid is built for kNearPeople
    const World& w = *ctx_.world;
    const int cx = p.x >> kCellBits, cz = p.z >> kCellBits;
    if (cx < 0 || cz < 0 || cx >= w.cells_x() || cz >= w.cells_z() || near_.empty()) return false;
    return near_[(size_t)cz * w.cells_x() + cx] != 0;
}

const Character* Fauna::nearest_person(const Vec3f& p, float radius) const {
    if (!ctx_.agents) return nullptr;
    const Character* best = nullptr;
    float bd = radius * radius;
    const int r = (int)radius / kIndexCell + 1;
    const int cx = floordiv((int)std::floor(p.x), kIndexCell), cz = floordiv((int)std::floor(p.z), kIndexCell);
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
            const i64 k = ((i64)(cx + dx) << 32) ^ (i64)(u32)(cz + dz);
            for (auto it = std::lower_bound(people_.begin(), people_.end(), std::make_pair(k, (EntityId)0));
                 it != people_.end() && it->first == k; ++it) {
                const Character* c = ctx_.agents->get(it->second);
                if (!c || !c->alive || c->departed) continue;
                const float d = c->pos.dist_sq(p);
                if (d < bd) {
                    bd = d;
                    best = c;
                }
            }
        }
    return best;
}

// ------------------------------------------------------------------------ behaviour

void Fauna::step(Tick now) {
    now_ = now;
    if (species_.empty()) return;
    // Every tick (cheap): an index kept across ticks would differ after a load.
    index_people();
    if (now % kTicksPerDay == kTicksPerDay / 4 && now > 0) daily();
    chasers_.clear();
    for (const Animal& a : animals_)
        if (a.alive && a.state == AnimalState::Chase && !a.target_char) chasers_.push_back(a.id);
    const size_t n = animals_.size();
    for (size_t i = 0; i < n; ++i) {
        Animal& a = animals_[i];
        if (!a.alive) continue;
        const bool due = (now + a.id) % kFarStep == 0;
        const bool near = people_near(a.foot, kNearPeople);
        if (!near && !due) continue;
        rough_ = !near;
        const float dt = near ? 1.0f : (float)kFarStep;
        const SpeciesDef& s = species_[a.species];
        if (s.temper == Temper::Predator || s.temper == Temper::Territorial)
            a.hunger += dt / (float)(kTicksPerDay * (s.temper == Temper::Territorial ? 2 : 1));
        if (now >= a.next_think) think(a, near);
        if (!a.alive) continue;
        move(a, dt);
    }
    rough_ = false;  // between steps the ground is looked at as it is
}

void Fauna::think(Animal& a, bool near) {
    const SpeciesDef& s = species_[a.species];
    a.next_think = now_ + (near ? 10 : 60) + rng_.below(8);
    const float age_days = (float)(now_ - a.born) / (float)kTicksPerDay;
    if (age_days > s.lifespan_days) {
        die(a, "老死");
        return;
    }
    if (a.hunger > 3.0f) {
        die(a, "饿死");
        return;
    }
    // A fish whose water is gone (drained, dug away, frozen through) does not last.
    if (s.aquatic && !swimmable(a.foot)) {
        Vec3i p = a.foot;
        if (find_spot(p, s) && p.dist2(a.foot) <= 4) {
            a.foot = a.next = a.goal = p;
            a.pos = Vec3f((float)p.x + 0.5f, (float)p.y + lift(s), (float)p.z + 0.5f);
        } else {
            die(a, "搁浅而死");
            return;
        }
    }
    const float grown = std::min(1.0f, 0.4f + 0.6f * age_days / kGrownDays);
    auto away_from = [&](const Vec3f& from, float dist) {
        Vec3f d = a.pos - from;
        d.y = 0;
        d = d.normalized();
        if (d.length() < 0.5f) d = Vec3f(std::cos(a.yaw), 0, std::sin(a.yaw));
        Vec3i g = (a.pos + d * dist).floor_i();
        if (!find_spot(g, s)) g = a.foot;
        return g;
    };
    // Continuing an action.
    if (a.state == AnimalState::Eat && now_ < a.state_until) return;
    // Territorial beasts (bears) drive off anyone who comes close; defensive ones turn
    // on whoever struck them.
    if (near && (s.temper == Temper::Territorial || s.temper == Temper::Defensive || s.temper == Temper::Predator)) {
        const Character* c = nullptr;
        if (a.attacker)
            if (const Character* at = ctx_.agents->get(a.attacker))
                if (at->alive && at->pos.dist_sq(a.pos) < 14.0f * 14.0f && now_ < a.state_until + 200) c = at;
        // A bear guards its ground in the wild, not among the houses (it keeps away from them),
        // and having driven someone off with a swipe it lets them go for a good while.
        if (!c && s.temper == Temper::Territorial && s.guard > 0 && (a.last_bite == 0 || now_ - a.last_bite > kTicksPerHour * 4)) {
            c = nearest_person(a.pos, s.guard);
            bool settled = false;
            if (c)
                for (const Polity& pol : ctx_.society->polities())
                    if (pol.alive)
                        if (const Building* seat = ctx_.buildings->get(pol.seat); seat && seat->entrance.dist2(c->foot) < 45 * 45)
                            settled = true;
            if (settled) c = nullptr;
        }
        // Starving wolves go for someone alone at night, away from the fires and houses.
        if (!c && s.temper == Temper::Predator && a.hunger > 2.2f && is_night(now_)) {
            const Character* p = nearest_person(a.pos, 20.0f);
            bool settled = false;
            if (p)
                for (const Building& b : ctx_.buildings->all())
                    if (b.alive && b.entrance.dist2(p->foot) < 40 * 40) {
                        settled = true;
                        break;
                    }
            if (p && !p->is_girl() && !settled) {
                bool alone = true;
                for (const auto& cp : ctx_.agents->all())
                    if (cp && cp.get() != p && cp->alive && !cp->departed && cp->pos.dist_sq(p->pos) < 64.0f) alone = false;
                if (alone) c = p;
            }
        }
        if (c) {
            if (a.state != AnimalState::Attack || !a.target_char || a.target != c->id) a.state_until = now_ + 240;
            a.state = AnimalState::Attack;
            a.target = c->id;
            a.target_char = true;
            a.goal = c->foot;
            return;
        }
    }
    if (a.state == AnimalState::Attack && now_ >= a.state_until) {
        a.state = AnimalState::Idle;
        a.attacker = kNoEntity;
    }
    if (a.state == AnimalState::Attack && a.target_char) {
        const Character* c = ctx_.agents->get(a.target);
        if (c && c->alive) {
            a.goal = c->foot;
            return;
        }
        a.state = AnimalState::Idle;
    }
    // The shy flee from people and from predators on the hunt.
    if (s.temper == Temper::Shy || s.temper == Temper::Defensive) {
        float flee = s.flee * (s.temper == Temper::Defensive ? 0.7f : 1.0f);
        const Character* c = near ? nearest_person(a.pos, flee) : nullptr;
        if (c) {
            a.state = AnimalState::Flee;
            a.threat = c->id;
            a.threat_char = true;
            a.state_until = now_ + 150;
            a.goal = away_from(c->pos, 16.0f);
            return;
        }
        for (u32 pid : chasers_) {
            const Animal* pp = get(pid);
            if (!pp) continue;
            const Animal& p = *pp;
            if (!p.alive || p.state != AnimalState::Chase || p.target != a.id || p.target_char) continue;
            if (p.pos.dist_sq(a.pos) > flee * flee * 1.5f) continue;
            a.state = AnimalState::Flee;
            a.threat = p.id;
            a.threat_char = false;
            a.state_until = now_ + 150;
            a.goal = away_from(p.pos, 16.0f);
            return;
        }
        if (a.state == AnimalState::Flee && now_ < a.state_until) return;
    }
    // Predators hunt when hungry.
    if (s.temper == Temper::Predator || s.temper == Temper::Territorial) {
        if (a.state == AnimalState::Chase && !a.target_char) {
            const Animal* prey = get(a.target);
            if (prey && prey->alive && prey->pos.dist_sq(a.pos) < 60.0f * 60.0f && now_ < a.state_until) {
                a.goal = prey->foot;
                return;
            }
            a.state = AnimalState::Idle;
        }
        if (a.hunger > 0.5f && grown >= 1.0f) {
            const Animal* best = nullptr;
            float bd = 40.0f * 40.0f;
            for (const Animal& p : animals_) {
                if (!p.alive || std::find(s.prey.begin(), s.prey.end(), p.species) == s.prey.end()) continue;
                const float d = p.pos.dist_sq(a.pos);
                if (d < bd) {
                    bd = d;
                    best = &p;
                }
            }
            if (best) {
                a.state = AnimalState::Chase;
                a.target = best->id;
                a.target_char = false;
                a.goal = best->foot;
                a.state_until = now_ + 900;
                return;
            }
            // Nothing to hunt here: a hungry pack moves on to new grounds.
            if (a.hunger > 1.0f && a.herd == a.id) {
                Vec3i g = a.foot + Vec3i{rng_.range(-70, 70), 0, rng_.range(-70, 70)};
                if (find_spot(g, s)) {
                    a.home = g;
                    a.goal = g;
                    a.state = AnimalState::Wander;
                    return;
                }
            }
        }
    }
    // Herd life: the leader wanders about its range, the others keep near it.
    const Animal* leader = a.herd != a.id ? get(a.herd) : nullptr;
    if (a.herd != a.id && (!leader || !leader->alive)) {
        a.herd = a.id;  // leads what is left
        leader = nullptr;
    }
    const float r = rng_.unit();
    if (leader) {
        if (leader->pos.dist_sq(a.pos) > 5.0f * 5.0f || r < 0.3f) {
            Vec3i g = leader->foot + Vec3i{rng_.range(-3, 3), 0, rng_.range(-3, 3)};
            a.goal = find_spot(g, s) ? g : leader->foot;
            a.state = AnimalState::Wander;
        } else {
            a.state = r < 0.7f ? AnimalState::Graze : AnimalState::Idle;
            a.goal = a.foot;
        }
        return;
    }
    if (r < 0.35f) {
        Vec3i g = a.home + Vec3i{rng_.range(-14, 14), 0, rng_.range(-14, 14)};
        if (find_spot(g, s)) {
            a.goal = g;
            a.state = AnimalState::Wander;
            return;
        }
    }
    a.state = r < 0.75f ? AnimalState::Graze : AnimalState::Idle;
    a.goal = a.foot;
}

Vec3i Fauna::pick_next(const Animal& a) const {
    static const int dirs[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const SpeciesDef& s = species_[a.species];
    Vec3i best = a.foot;
    auto score = [&](const Vec3i& p) {
        const i64 dx = p.x - a.goal.x, dz = p.z - a.goal.z, dy = s.aquatic ? p.y - a.goal.y : 0;
        return dx * dx + dz * dz + dy * dy;
    };
    i64 bd = score(a.foot);
    const int start = (int)((a.id * 3 + (u32)(now_ / 16)) % 8);
    for (int k = 0; k < 8; ++k) {
        const int* d = dirs[(start + k) % 8];
        for (int dy : {0, 1, -1, -2}) {
            const Vec3i c = a.foot + Vec3i{d[0], dy, d[1]};
            if (!fits(s, c)) continue;
            const i64 sc = score(c);
            if (sc < bd) {
                bd = sc;
                best = c;
            }
            break;
        }
    }
    return best;
}

void Fauna::move(Animal& a, float dt) {
    const SpeciesDef& s = species_[a.species];
    const bool running = a.state == AnimalState::Flee || a.state == AnimalState::Chase || a.state == AnimalState::Attack;
    float speed = 0.0f;
    if (running) speed = std::max(s.speed, s.run * (1.0f - 0.6f * a.tired));
    else if (a.state == AnimalState::Wander) speed = s.speed;
    // Running tires; walking and grazing rest.
    if (running) a.tired = std::min(1.0f, a.tired + dt / (float)std::max(1, s.stamina));
    else a.tired = std::max(0.0f, a.tired - dt / (float)std::max(1, s.stamina * 2));
    // Bites and blows at close quarters.
    if (a.state == AnimalState::Attack || a.state == AnimalState::Chase) {
        Vec3f tp;
        bool have = false;
        if (a.target_char) {
            if (Character* c = ctx_.agents->get(a.target); c && c->alive) {
                tp = c->pos;
                have = true;
                if (tp.dist_sq(a.pos) < 1.7f * 1.7f && now_ - a.last_bite >= 30) {
                    a.last_bite = now_;
                    Event e;
                    e.type = EventType::Injury;
                    e.severity = 3;
                    e.pos = c->foot;
                    e.actor = c->id;
                    e.polity = c->polity;
                    e.text = strfmt("%s遭到%s袭击", c->name.c_str(), s.name.c_str());
                    const EventId ev = ctx_.chron->emit(std::move(e));
                    ctx_.agents->damage(*c, s.attack, -1, "被" + s.name + "咬伤", ev);
                    // Having struck back, a bear or boar lets the intruder go (unless struck
                    // again); a wolf keeps at it a while.
                    if (s.temper == Temper::Territorial || s.temper == Temper::Defensive) {
                        a.state = AnimalState::Idle;
                        a.state_until = now_;
                        a.target = 0;
                        a.target_char = false;
                        a.attacker = kNoEntity;
                        a.goal = a.foot;
                    }
                    if (!c->alive && s.temper == Temper::Predator) {
                        a.state = AnimalState::Eat;
                        a.hunger = 0.0f;
                        a.state_until = now_ + 600;
                        a.goal = a.foot;
                    }
                }
            }
        } else if (Animal* p = get(a.target); p && p->alive) {
            tp = p->pos;
            have = true;
            // Far from people both move in big steps: the catch is judged as loosely.
            const float reach = 1.6f + (dt > 1.0f ? s.run * dt * 0.4f : 0.0f);
            if (tp.dist_sq(a.pos) < reach * reach && now_ - a.last_bite >= 20) {
                a.last_bite = now_;
                p->hp -= 0.35f;
                if (p->hp <= 0.0f) {
                    die(*p, "被" + s.name + "捕食");
                    // The pack shares the kill.
                    const Vec3f at = p->pos;
                    for (Animal& o : animals_) {
                        if (!o.alive || o.herd != a.herd || o.pos.dist_sq(at) > 24.0f * 24.0f) continue;
                        o.state = AnimalState::Eat;
                        o.hunger = 0.0f;
                        o.state_until = now_ + 500;
                        o.goal = o.foot;
                        o.target = 0;
                    }
                }
            }
        }
        if (have && a.state != AnimalState::Eat) a.goal = tp.floor_i();
    }
    if (speed <= 0.0f || (a.foot == a.goal && a.next == a.foot)) {
        a.moving = false;
        return;
    }
    float budget = speed * dt;
    Vec3f before = a.pos;
    for (int guard = 0; guard < 64 && budget > 1e-4f; ++guard) {
        if (a.next == a.foot) {
            if (a.foot.x == a.goal.x && a.foot.z == a.goal.z) break;
            a.next = pick_next(a);
            if (a.next == a.foot) {
                a.goal = a.foot;  // blocked: think again
                break;
            }
        }
        const Vec3f t((float)a.next.x + 0.5f, (float)a.next.y + lift(s), (float)a.next.z + 0.5f);
        Vec3f d = t - a.pos;
        const float len = d.length();
        if (len <= budget) {
            a.pos = t;
            a.foot = a.next;
            budget -= len;
        } else {
            a.pos += d * (budget / len);
            budget = 0.0f;
        }
    }
    Vec3f moved = a.pos - before;
    const float m = moved.length_xz();
    a.moving = m > 1e-3f;
    if (a.moving) {
        a.yaw = std::atan2(moved.x, moved.z);
        a.phase += m * 2.2f;
    }
}

void Fauna::die(Animal& a, const std::string& cause) {
    deaths[species_[a.species].key + ":" + cause]++;
    a.alive = false;
    a.died = now_;
    a.state = AnimalState::Dead;
    a.moving = false;
    a.death_cause = cause;
}

bool Fauna::strike(Animal& a, float power, EntityId by, EventId cause) {
    (void)cause;
    if (!a.alive) return false;
    now_ = ctx_.now;  // called from the agents' step: the clock of this tick, not the last fauna step
    const SpeciesDef& s = species_[a.species];
    a.hp -= power / s.hp;
    a.attacker = by;
    a.hunted_by = by;
    if (a.hp <= 0.0f) {
        const Character* c = ctx_.agents ? ctx_.agents->get(by) : nullptr;
        die(a, c ? "被" + c->name + "猎杀" : std::string("被猎杀"));
        return true;
    }
    const Character* c = ctx_.agents ? ctx_.agents->get(by) : nullptr;
    if (s.temper == Temper::Shy && c) {
        a.state = AnimalState::Flee;
        a.threat = by;
        a.threat_char = true;
        a.state_until = now_ + 300;
        Vec3f d = a.pos - c->pos;
        d.y = 0;
        d = d.normalized();
        Vec3i g = (a.pos + d * 18.0f).floor_i();
        a.goal = find_spot(g, s) ? g : a.foot;
    } else if (c) {
        a.state = AnimalState::Attack;
        a.target = by;
        a.target_char = true;
        a.state_until = now_ + 300;
        a.goal = c->foot;
    }
    a.next_think = now_ + 20;
    return false;
}

int Fauna::butcher(Animal& a, StoreId into) {
    if (a.alive || a.butchered) return 0;
    a.butchered = true;
    int n = 0;
    for (auto& [it, count] : species_[a.species].yield) n += ctx_.econ->add(into, it, count, "butcher");
    return n;
}

u32 Fauna::find_fish(const Vec3i& from, int radius) const {
    const Animal* best = nullptr;
    i64 bd = (i64)radius * radius;
    for (const Animal& a : animals_) {
        if (!a.alive || a.hunted_by != kNoEntity || !species_[a.species].aquatic) continue;
        const i64 d = a.foot.dist2(from);
        if (d < bd) {
            bd = d;
            best = &a;
        }
    }
    return best ? best->id : 0;
}

int Fauna::catch_fish(Animal& a, EntityId by, StoreId into) {
    if (!a.alive) return 0;
    now_ = ctx_.now;
    const Character* c = ctx_.agents ? ctx_.agents->get(by) : nullptr;
    die(a, c ? "被" + c->name + "捕获" : std::string("被捕获"));
    return butcher(a, into);
}

u32 Fauna::find_prey(const Vec3i& from, int radius, bool dangerous_too) const {
    const Animal* best = nullptr;
    i64 bd = (i64)radius * radius;
    for (const Animal& a : animals_) {
        if (!a.alive || a.hunted_by != kNoEntity) continue;
        const SpeciesDef& s = species_[a.species];
        if (s.aquatic) continue;  // (fish are caught from the shore)
        if (!dangerous_too && s.temper != Temper::Shy) continue;
        const i64 d = a.foot.dist2(from);
        if (d < bd) {
            bd = d;
            best = &a;
        }
    }
    return best ? best->id : 0;
}

float Fauna::threat_at(const Vec3f& p) const {
    // The few animals that threaten anyone, listed once per tick.
    if (threats_at_ != ctx_.now) {
        threats_at_ = ctx_.now;
        threats_.clear();
        for (const Animal& a : animals_) {
            if (!a.alive) continue;
            const SpeciesDef& s = species_[a.species];
            float radius = 0.0f;
            if (a.state == AnimalState::Attack) radius = 10.0f;
            else if (s.temper == Temper::Territorial) radius = s.guard + 3.0f;
            else if (s.temper == Temper::Predator && a.state == AnimalState::Chase && a.target_char) radius = 14.0f;
            if (radius > 0.0f) threats_.push_back({a.pos, radius});
        }
    }
    float worst = 0.0f;
    for (const auto& [at, radius] : threats_) {
        const float d2 = at.dist_sq(p);
        if (d2 >= radius * radius) continue;
        worst = std::max(worst, 1.0f - std::sqrt(d2) / radius);
    }
    return worst;
}

u32 Fauna::nearest_threat(const Vec3f& p, float radius) const {
    u32 best = 0;
    float bd = radius * radius;
    for (const Animal& a : animals_) {
        if (!a.alive) continue;
        const SpeciesDef& s = species_[a.species];
        if (a.state != AnimalState::Attack && s.temper != Temper::Territorial &&
            !(s.temper == Temper::Predator && a.state == AnimalState::Chase))
            continue;
        const float d = a.pos.dist_sq(p);
        if (d < bd) {
            bd = d;
            best = a.id;
        }
    }
    return best;
}

void Fauna::daily() {
    // Carcasses rot or are taken; the long dead leave the list.
    animals_.erase(std::remove_if(animals_.begin(), animals_.end(),
                                  [&](const Animal& a) {
                                      return !a.alive && (a.butchered || now_ - a.died > kCarcassTicks);
                                  }),
                   animals_.end());
    // Births, up to what the land carried at the start.
    std::vector<int> alive(species_.size(), 0);
    for (const Animal& a : animals_)
        if (a.alive) alive[a.species]++;
    const size_t n = animals_.size();
    for (size_t i = 0; i < n; ++i) {
        Animal& a = animals_[i];
        if (!a.alive || !a.female) continue;
        const SpeciesDef& s = species_[a.species];
        if (alive[s.id] >= capacity_[s.id]) continue;
        if ((float)(now_ - a.born) / (float)kTicksPerDay < kGrownDays * 1.5f) continue;
        if (a.hunger > 0.8f || !rng_.chance(1.0f / s.breed_days)) continue;
        const int litter = rng_.range(s.litter_min, s.litter_max);
        const Vec3i at = a.foot;
        rough_ = !people_near(at, kNearPeople);
        const u32 herd = a.herd;
        for (int k = 0; k < litter && alive[s.id] < capacity_[s.id]; ++k) {
            Vec3i p = at + Vec3i{rng_.range(-1, 1), 0, rng_.range(-1, 1)};
            if (!find_spot(p, s)) continue;
            spawn(s.id, p, herd, now_);
            alive[s.id]++;
        }
    }
}

// ------------------------------------------------------------------------ persistence

void Fauna::save(BinWriter& w) const {
    size_t sec = w.begin_section("FAUN");
    w.u64v(rng_.state());
    w.u64v(rng_.inc());
    w.u32v(next_id_);
    w.varu(capacity_.size());
    for (int c : capacity_) w.vari(c);
    w.varu(animals_.size());
    for (const Animal& a : animals_) {
        w.u32v(a.id);
        w.str(species_[a.species].key);
        w.boolean(a.alive);
        w.boolean(a.female);
        w.boolean(a.butchered);
        w.vec3f(a.pos);
        w.vec3i(a.foot);
        w.vec3i(a.next);
        w.f32(a.yaw);
        w.f32(a.hp);
        w.f32(a.hunger);
        w.f32(a.tired);
        w.f32(a.phase);
        w.boolean(a.moving);
        w.u8v((u8)a.state);
        w.u32v(a.herd);
        w.vec3i(a.home);
        w.vec3i(a.goal);
        w.u32v(a.target);
        w.boolean(a.target_char);
        w.u32v(a.threat);
        w.boolean(a.threat_char);
        w.u32v(a.hunted_by);
        w.u32v(a.attacker);
        w.u64v(a.born);
        w.u64v(a.died);
        w.u64v(a.next_think);
        w.u64v(a.state_until);
        w.u64v(a.last_bite);
        w.str(a.death_cause);
    }
    w.end_section(sec);
}

void Fauna::load(BinReader& outer) {
    BinReader r = outer.section("FAUN");
    const u64 st = r.u64v(), inc = r.u64v();
    rng_.set_raw(st, inc);
    next_id_ = r.u32v();
    const u64 nc = r.varu();
    std::vector<int> cap;
    for (u64 i = 0; i < nc; ++i) cap.push_back((int)r.vari());
    capacity_.assign(species_.size(), 0);
    for (size_t i = 0; i < cap.size() && i < capacity_.size(); ++i) capacity_[i] = cap[i];
    animals_.clear();
    const u64 n = r.varu();
    for (u64 i = 0; i < n; ++i) {
        Animal a;
        a.id = r.u32v();
        const int s = species_id(r.str());
        a.alive = r.boolean();
        a.female = r.boolean();
        a.butchered = r.boolean();
        a.pos = r.vec3f();
        a.foot = r.vec3i();
        a.next = r.vec3i();
        a.yaw = r.f32();
        a.hp = r.f32();
        a.hunger = r.f32();
        a.tired = r.f32();
        a.phase = r.f32();
        a.moving = r.boolean();
        a.state = (AnimalState)r.u8v();
        a.herd = r.u32v();
        a.home = r.vec3i();
        a.goal = r.vec3i();
        a.target = r.u32v();
        a.target_char = r.boolean();
        a.threat = r.u32v();
        a.threat_char = r.boolean();
        a.hunted_by = r.u32v();
        a.attacker = r.u32v();
        a.born = r.u64v();
        a.died = r.u64v();
        a.next_think = r.u64v();
        a.state_until = r.u64v();
        a.last_bite = r.u64v();
        a.death_cause = r.str();
        if (s < 0) continue;  // a species the rules no longer have
        a.species = (u16)s;
        animals_.push_back(std::move(a));
    }
    people_.clear();
    people_at_ = ~0ull;
    rough_ = false;
}

u64 Fauna::hash() const {
    u64 h = hash_combine(rng_.state(), next_id_);
    for (const Animal& a : animals_) {
        h = hash_combine(h, ((u64)a.id << 16) ^ ((u64)a.alive << 8) ^ (u64)a.state);
        h = fnv1a64(&a.pos, sizeof(a.pos), h);
        h = hash_combine(h, (u64)(u32)std::lround(a.hp * 1000.0f));
    }
    return h;
}

}  // namespace icarus
