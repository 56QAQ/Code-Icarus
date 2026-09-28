// Navigation over cube positions. A position p is standable if the three cubes
// p, p+1, p+2 are free (air, water, plants, doors) and p-1 is solid ground.
// A* with 8-way horizontal moves, 1-cube step up, up to 3-cube drops.
#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "icarus/agents/region_map.h"

#include "icarus/world/world.h"

namespace icarus {

struct Path {
    std::vector<Vec3i> nodes;  // from start (exclusive) to goal (inclusive)
    size_t next = 0;
    bool valid() const { return next < nodes.size(); }
    void clear() {
        nodes.clear();
        next = 0;
    }
};

struct NavStats {
    u64 searches = 0, failures = 0, expansions = 0;
    u64 flood_nodes = 0;  // positions labelled by region floods
};

class Nav {
public:
    explicit Nav(World& w);

    bool passable(const Vec3i& p);   // body can occupy this cube
    bool standable(const Vec3i& p);  // feet can be here
    float step_cost(const Vec3i& p);
    // Finds the nearest standable position at or below/above p within dy range.
    bool find_standable_near(const Vec3i& p, Vec3i& out, int radius = 2);

    // A* from start to goal. If adjacent_ok, any standable cube within reach_xz (xz), at
    // most 2 above the goal and at most reach_up below it counts as arrival (for working
    // on a cube; builders reach further, as if from a ladder).
    bool find_path(const Vec3i& start, const Vec3i& goal, bool adjacent_ok, Path& out, int max_expansions = 40000,
                   int reach_up = 3, int reach_xz = 1);

    // Walkable neighbours of a standable position with their move costs (max 8).
    int neighbors(const Vec3i& p, Vec3i* out, float* cost);
    // Breadth-first flood over walkable moves from seed, within an xz radius; labels every
    // reached position with id (positions already labelled are not revisited).
    // `open` is set when the flood was cut short (radius or node limit): places beyond it
    // may still be reachable.
    int flood(const Vec3i& seed, int radius, int max_nodes, RegionMap& label, u16 id, bool* open = nullptr);

    // Called with the world change journal: flags changes that alter walkability
    // (solid/passable) so cached reachability can be reused while nothing changed.
    // Major = caused by an event (construction, collapse, admin); minor = settling matter.
    void on_changes(const std::vector<VoxelChange>& changes);
    bool major_dirty = true, minor_dirty = true;

    // Cubes where someone lies asleep: still passable, but a path goes round them when it
    // reasonably can (set every tick by the residents; never blocks a way).
    void set_soft(std::vector<Vec3i> cubes);
    float soft_cost(const Vec3i& p) const;

    NavStats stats;

private:
    struct Node {
        u64 key = 0;
        u32 gen = 0;
        float g = 0;
        u64 parent = 0;
        bool closed = false;
    };
    Node* slot(u64 key, bool create);
    static u64 pack(const Vec3i& p) {
        return ((u64)(u32)(p.x & 0xFFFFF) << 40) | ((u64)(u32)(p.y & 0xFFFFF) << 20) | (u64)(u32)(p.z & 0xFFFFF);
    }
    static Vec3i unpack(u64 k) {
        auto sx = [](u64 v) { return (i32)(v & 0xFFFFF) - ((v & 0x80000) ? 0x100000 : 0); };
        return {sx(k >> 40), sx(k >> 20), sx(k)};
    }

    World& w_;
    std::vector<Node> table_;
    u32 gen_ = 1;
    std::vector<u64> soft_;              // packed positions, sorted
    std::array<u64, 16> soft_bits_{};    // quick filter by a hash of the position
};

}  // namespace icarus
