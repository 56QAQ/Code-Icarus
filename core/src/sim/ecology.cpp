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
}

bool Ecology::open_grass(const Vec3i& p, const Buildings& buildings) {
    const CoreMats& M = reg_->m();
    auto mat = [&](const Vec3i& q) { return vmat(w_.peek(q)); };
    if (!w_.in_bounds(p) || !w_.in_bounds(p + Vec3i{0, 6, 0})) return false;
    if (mat(p + Vec3i{0, -1, 0}) != M.grass) return false;
    if (mat(p) != M.air || mat(p + Vec3i{0, 1, 0}) != M.air) return false;
    // Not against houses, halls or storehouses (doors and walls stay clear).
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx)
            if (buildings.at(p + Vec3i{dx, 0, dz}) || buildings.at(p + Vec3i{dx, 1, dz})) return false;
    return true;
}

void Ecology::grow_tree(const Vec3i& base) {
    const CoreMats& M = reg_->m();
    const int height = 4 + rng_.range(0, 2);
    for (int y = 0; y < height; ++y) w_.set(base + Vec3i{0, y, 0}, make_voxel(M.log), 0);
    const Vec3i top = base + Vec3i{0, height - 1, 0};
    for (int dy = -1; dy <= 2; ++dy)
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                const int d2 = dx * dx + dz * dz + dy * dy * 2;
                if (d2 > 6 || (dx == 0 && dz == 0 && dy <= 0)) continue;
                const Vec3i q = top + Vec3i{dx, dy, dz};
                if (w_.in_bounds(q) && vmat(w_.peek(q)) == M.air) w_.set(q, make_voxel(M.leaves), 0);
            }
    regrown_.push_back(base);
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
        if (mat(t) == M.log) parents.push_back(t);
    }
    for (const Vec3i& t : regrown_)
        if (mat(t) == M.log) parents.push_back(t);
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
            if (open_grass(p, buildings)) w_.set(p, make_voxel(M.berry_bush), 0);
            break;
        }
    }
}

int Ecology::regrown_standing() const {
    int n = 0;
    for (const Vec3i& t : regrown_)
        if (vmat(w_.peek(t)) == reg_->m().log) ++n;
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
}

u64 Ecology::hash() const {
    u64 h = hash_combine(rng_.state(), saplings_.size());
    for (const Vec3i& p : regrown_) h = hash_combine(h, ((u64)(u32)p.x << 32) ^ ((u64)(u32)p.z << 12) ^ (u64)(u32)p.y);
    return h;
}

}  // namespace icarus
