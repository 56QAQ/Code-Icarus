#include "icarus/sim/ecology.h"

#include <algorithm>

#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"

namespace icarus {

namespace {
constexpr Tick kGrowTicks = kTicksPerDay * 3;     // sapling -> tree
constexpr Tick kGiveUpTicks = kTicksPerDay * 12;  // a sapling that never found room dies
}  // namespace

void Ecology::reset(u64 seed) {
    rng_.seed(seed, 0xEC0);
    saplings_.clear();
    regrown_.clear();
    picked_.clear();
}

bool Ecology::open_grass(const Vec3i& p, const Buildings& buildings) {
    const CoreMats& M = reg_->m();
    auto mat = [&](const Vec3i& q) { return vmat(w_.peek(q)); };
    if (!w_.in_bounds(p) || !w_.in_bounds(p + Vec3i{0, 6, 0})) return false;
    const MatId ground = mat(p + Vec3i{0, -1, 0});
    const bool continent = w_.config().layout != WorldLayout::Classic;
    if (ground != M.grass &&
        !(continent && ground != M.air && (ground == M.dry_grass || ground == M.snow || ground == M.mud)))
        return false;
    if (mat(p) != M.air || mat(p + Vec3i{0, 1, 0}) != M.air) return false;
    // Not at the lip of a pit, cut or cliff, where the crown would hang over the way down.
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx)
            if (!reg_->mat(mat(p + Vec3i{dx, -1, dz})).solid && !reg_->mat(mat(p + Vec3i{dx, -2, dz})).solid) return false;
    // Not against houses, halls or storehouses (doors and walls stay clear).
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx)
            if (buildings.at(p + Vec3i{dx, 0, dz}) || buildings.at(p + Vec3i{dx, 1, dz})) return false;
    return true;
}

void Ecology::grow_tree(const Vec3i& base) {
    const CoreMats& M = reg_->m();
    auto pick = [&](MatId m, MatId fallback) { return m != M.air ? m : fallback; };
    MatId trunk = M.log, leaf = M.leaves, fruit = M.leaves;
    int height = 4 + rng_.range(0, 2);
    bool conifer = false, flat = false;
    if (w_.config().layout != WorldLayout::Classic) {
        // The species of the place.
        const Biome b = w_.gen().column(base.x, base.z).biome;
        if (b == Biome::Taiga || b == Biome::Snowfield || b == Biome::Highland) {
            trunk = pick(M.pine_log, M.log);
            leaf = pick(M.pine_leaves, M.leaves);
            height = 6 + rng_.range(0, 2);
            conifer = true;
        } else if (b == Biome::Savanna) {
            flat = true;
        } else if (b == Biome::Forest) {
            const int r = rng_.range(0, 9);
            if (r >= 8) {
                fruit = pick(M.fruit_leaves, M.leaves);
                height = 3;
            } else if (r >= 5) {
                trunk = pick(M.birch_log, M.log);
                leaf = pick(M.birch_leaves, M.leaves);
                height = 5 + rng_.range(0, 2);
            }
        }
    }
    for (int y = 0; y < height; ++y) w_.set(base + Vec3i{0, y, 0}, make_voxel(trunk), 0);
    const Vec3i top = base + Vec3i{0, height - 1, 0};
    auto put_leaf = [&](const Vec3i& q, MatId m) {
        if (w_.in_bounds(q) && vmat(w_.peek(q)) == M.air) w_.set(q, make_voxel(m), 0);
    };
    if (conifer) {
        put_leaf(top + Vec3i{0, 1, 0}, leaf);
        for (int dy = -3; dy <= 0; ++dy) {
            const int r = dy <= -2 ? 2 : 1;
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx)
                    if ((dx || dz) && dx * dx + dz * dz <= r * r + 1) put_leaf(top + Vec3i{dx, dy, dz}, leaf);
        }
    } else if (flat) {
        for (int dz = -3; dz <= 3; ++dz)
            for (int dx = -3; dx <= 3; ++dx)
                if (dx * dx + dz * dz <= 10) put_leaf(top + Vec3i{dx, 1, dz}, leaf);
    } else {
        for (int dy = -1; dy <= 2; ++dy)
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int d2 = dx * dx + dz * dz + dy * dy * 2;
                    if (d2 > 6 || (dx == 0 && dz == 0 && dy <= 0)) continue;
                    put_leaf(top + Vec3i{dx, dy, dz}, ((dx + dz + dy) & 1) ? fruit : leaf);
                }
    }
    regrown_.push_back(base);
}

// Which wild plant comes back at p (continent: by biome; classic: berry bushes).
MatId Ecology::wild_plant_for(const Vec3i& p) {
    const CoreMats& M = reg_->m();
    if (w_.config().layout == WorldLayout::Classic) return M.berry_bush;
    auto pick = [&](MatId m) { return m != M.air ? m : M.berry_bush; };
    const int r = rng_.range(0, 9);
    switch (w_.gen().column(p.x, p.z).biome) {
        case Biome::Grassland: return r < 6 ? pick(M.wild_grain) : (r < 9 ? M.berry_bush : pick(M.herb_plant));
        case Biome::Savanna: return pick(M.wild_grain);
        case Biome::Forest: return r < 5 ? M.berry_bush : (r < 8 ? pick(M.mushroom) : pick(M.herb_plant));
        case Biome::Taiga: return r < 6 ? M.berry_bush : pick(M.mushroom);
        case Biome::Wetland: return r < 6 ? pick(M.reeds) : (r < 8 ? pick(M.mushroom) : pick(M.herb_plant));
        case Biome::Lakeshore: return pick(M.reeds);
        default: return M.berry_bush;
    }
}

void Ecology::picked(const Vec3i& p, Tick now, MatId plant) { picked_.push_back({p, now, plant}); }

float Ecology::regrowing_food(const Vec3i& c, int r) const {
    float sum = 0;
    for (const Picked& f : picked_) {
        const i64 dx = f.pos.x - c.x, dz = f.pos.z - c.z;
        if (dx * dx + dz * dz > (i64)r * r) continue;
        const Material& m = reg_->mat(f.plant);
        if (m.forage_item == kNoItem) continue;
        const float n = reg_->item(m.forage_item).nutrition;
        if (n > 0.0f) sum += (float)std::max(1, m.forage_count) * n / (m.foliage ? 3.0f : 4.0f);
    }
    return sum;
}

void Ecology::daily(Tick now, const Buildings& buildings) {
    const CoreMats& M = reg_->m();
    // Looking never wakes a cell; only planting and growing write.
    auto mat = [&](const Vec3i& q) { return vmat(w_.peek(q)); };
    const MatId sapling = reg_->has_mat("sapling") ? reg_->mat_id("sapling") : M.air;
    if (sapling == M.air) return;
    // Saplings that have had their time become trees (if nothing took their place and
    // there is room to grow).
    std::vector<Sapling> keep;
    for (const Sapling& s : saplings_) {
        if (mat(s.pos) != sapling) continue;  // trampled, dug, burned or built over
        if (now - s.planted < kGrowTicks) {
            keep.push_back(s);
            continue;
        }
        bool room = true;
        for (int y = 1; y <= 6 && room; ++y) room = mat(s.pos + Vec3i{0, y, 0}) == M.air;
        if (room) {
            w_.set(s.pos, make_voxel(M.air), 0);
            grow_tree(s.pos);
        } else if (now - s.planted > kGiveUpTicks) {
            w_.set(s.pos, make_voxel(M.air), 0);
        } else {
            keep.push_back(s);
        }
    }
    saplings_ = std::move(keep);

    // Seeds fall near standing trees in places the world has already touched.
    std::vector<Vec3i> parents;
    for (const Vec3i& t : w_.generated_trees()) {
        const Cell* c = w_.cell(cell_of(t));
        if (!c || c->state == CellState::Ungenerated || c->pristine) continue;
        if (reg_->mat(mat(t)).trunk) parents.push_back(t);
    }
    for (const Vec3i& t : regrown_)
        if (reg_->mat(mat(t)).trunk) parents.push_back(t);
    // Gathered plants grow back: fruit ripens on the same branch, bushes and stalks
    // re-sprout where the ground is still open.
    if (!picked_.empty()) {
        std::vector<Picked> keep;
        for (const Picked& f : picked_) {
            const Material& pm = reg_->mat(f.plant);
            const bool fruit = pm.foliage;
            if (now - f.when < kTicksPerDay * (fruit ? 3 : 4)) {
                keep.push_back(f);
                continue;
            }
            const MatId here = mat(f.pos);
            if (fruit) {
                if (here == pm.forage_to) w_.set(f.pos, make_voxel(f.plant), 0);
            } else if (here == M.air && reg_->mat(mat(f.pos + Vec3i{0, -1, 0})).fertile && !buildings.at(f.pos)) {
                w_.set(f.pos, make_voxel(f.plant), 0);
            }
        }
        picked_ = std::move(keep);
    }
    if (parents.empty()) return;
    const int seeds = std::clamp((int)parents.size() / 12, 1, 6);
    for (int i = 0; i < seeds; ++i) {
        const Vec3i& parent = parents[(size_t)rng_.range(0, (int)parents.size() - 1)];
        const int dx = rng_.range(-6, 6), dz = rng_.range(-6, 6);
        if (dx * dx + dz * dz < 5) continue;
        for (int dy = 3; dy >= -4; --dy) {
            const Vec3i p = parent + Vec3i{dx, dy, dz};
            if (!w_.in_bounds(p) || mat(p + Vec3i{0, -1, 0}) == M.air) continue;
            if (open_grass(p, buildings)) {
                w_.set(p, make_voxel(sapling), 0);
                saplings_.push_back({p, now});
            }
            break;
        }
    }
    // Berry bushes come back at the edge of the woods.
    const int bushes = std::clamp((int)parents.size() / 40, 1, 3);
    for (int i = 0; i < bushes; ++i) {
        const Vec3i& parent = parents[(size_t)rng_.range(0, (int)parents.size() - 1)];
        const int dx = rng_.range(-8, 8), dz = rng_.range(-8, 8);
        if (dx * dx + dz * dz < 9) continue;
        for (int dy = 3; dy >= -4; --dy) {
            const Vec3i p = parent + Vec3i{dx, dy, dz};
            if (!w_.in_bounds(p) || mat(p + Vec3i{0, -1, 0}) == M.air) continue;
            if (open_grass(p, buildings)) w_.set(p, make_voxel(wild_plant_for(p)), 0);
            break;
        }
    }
}

int Ecology::regrown_standing() const {
    int n = 0;
    for (const Vec3i& t : regrown_)
        if (reg_->mat(vmat(w_.peek(t))).trunk) ++n;
    return n;
}

void Ecology::save(BinWriter& w) const {
    size_t s = w.begin_section("ECOL");
    w.u64v(rng_.state());
    w.u64v(rng_.inc());
    w.varu(saplings_.size());
    for (const Sapling& p : saplings_) {
        w.vec3i(p.pos);
        w.u64v(p.planted);
    }
    w.varu(regrown_.size());
    for (const Vec3i& p : regrown_) w.vec3i(p);
    w.varu(picked_.size());
    for (const Picked& p : picked_) {
        w.vec3i(p.pos);
        w.u64v(p.when);
        w.str(reg_->mat(p.plant).key);
    }
    w.end_section(s);
}

void Ecology::load(BinReader& outer) {
    BinReader r = outer.section("ECOL");
    const u64 st = r.u64v(), inc = r.u64v();
    rng_.set_raw(st, inc);
    saplings_.clear();
    const u64 ns = r.varu();
    for (u64 i = 0; i < ns; ++i) {
        Sapling s;
        s.pos = r.vec3i();
        s.planted = r.u64v();
        saplings_.push_back(s);
    }
    regrown_.clear();
    const u64 nr = r.varu();
    for (u64 i = 0; i < nr; ++i) regrown_.push_back(r.vec3i());
    picked_.clear();
    if (!r.at_end()) {
        const u64 np = r.varu();
        for (u64 i = 0; i < np; ++i) {
            Picked s;
            s.pos = r.vec3i();
            s.when = r.u64v();
            const std::string key = r.str();
            if (!reg_->has_mat(key)) continue;
            s.plant = reg_->mat_id(key);
            picked_.push_back(s);
        }
    }
}

u64 Ecology::hash() const {
    u64 h = hash_combine(rng_.state(), saplings_.size());
    h = hash_combine(h, picked_.size());
    for (const Vec3i& p : regrown_) h = hash_combine(h, ((u64)(u32)p.x << 32) ^ ((u64)(u32)p.z << 12) ^ (u64)(u32)p.y);
    return h;
}

}  // namespace icarus
