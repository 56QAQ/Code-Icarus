// Matter rules: water, fire, granular materials, structural support, falling debris,
// meteors and springs. Only "active" cubes are processed; everything else sleeps.
#pragma once

#include <unordered_set>
#include <vector>

#include "icarus/sim/chronicle.h"
#include "icarus/util/rng.h"
#include "icarus/world/world.h"

namespace icarus {

// Insertion-ordered set of positions (deterministic iteration).
class PosQueue {
public:
    void push(const Vec3i& p) {
        if (members_.insert(p).second) items_.push_back(p);
    }
    std::vector<Vec3i> take() {
        std::vector<Vec3i> out;
        out.swap(items_);
        members_.clear();
        return out;
    }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    const std::vector<Vec3i>& items() const { return items_; }
    void clear() {
        items_.clear();
        members_.clear();
    }

private:
    std::vector<Vec3i> items_;
    std::unordered_set<Vec3i, Vec3iHash> members_;
};

struct DebrisVoxel {
    Vec3i off;
    Voxel v;
};

struct DebrisBody {
    u32 id = 0;
    std::vector<DebrisVoxel> voxels;
    Vec3f pos;  // world position of offset (0,0,0)
    Vec3f vel;
    Vec3i min_off, max_off;
    EventId cause = 0;
    u32 version = 1;
};

struct Meteor {
    u32 id = 0;
    Vec3f pos, vel;
    float radius = 4.0f;
    EventId cause = 0;
};

// Damage request for characters/buildings, consumed by other systems.
struct AreaDamage {
    Vec3f center;
    float radius = 1.0f;
    float amount = 0.1f;  // fraction of body voxels hit near centre
    u8 kind = 0;          // 0 blunt/blast, 1 fire, 2 crush
    EventId cause = 0;
};

struct PhysicsStats {
    size_t water_active = 0, fire_active = 0, granular_active = 0, support_checks = 0;
    size_t debris = 0, meteors = 0;
    i64 water_units_to_void = 0, water_units_spring = 0, water_units_evaporated = 0;
};

class Physics {
public:
    Physics(World& world, Chronicle& chronicle);

    void reset(u64 seed);
    void step(Tick now);
    void on_changes(const std::vector<VoxelChange>& changes);

    // Registration / control.
    void add_spring(const Vec3i& p) { springs_.push_back(p); }
    const std::vector<Vec3i>& springs() const { return springs_; }
    void ignite(const Vec3i& p, EventId cause);
    u32 spawn_meteor(const Vec3f& target, float radius, EventId cause);
    void explode(const Vec3f& center, float radius, EventId cause, bool meteor);

    const std::vector<DebrisBody>& debris() const { return debris_; }
    const std::vector<Meteor>& meteors() const { return meteors_; }
    std::vector<AreaDamage>& damage_queue() { return damage_; }
    const PhysicsStats& stats() const { return stats_; }

    // Parameters (tunable).
    int water_budget = 30000;
    int spring_interval = 4;     // ticks per emitted water unit
    float evaporation = 1.0f / 1500.0f;  // chance per tick for shallow puddles
    int support_max_nodes = 40000;

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    void step_springs(Tick now);
    void step_water();
    void step_fire();
    void step_granular();
    void step_support();
    void step_debris();
    void step_meteors();
    void wake_neighbors(const Vec3i& p);
    bool water_can_enter(Voxel v) const;
    void collapse_component(const std::vector<Vec3i>& comp, EventId cause);
    void land_debris(DebrisBody& b, float impact_speed);
    EventId cause_of_removal(const Vec3i& p) const;

    World& w_;
    Chronicle& chron_;
    Rng rng_;
    PosQueue water_, fire_, granular_, support_;
    std::vector<Vec3i> springs_;
    std::vector<DebrisBody> debris_;
    std::vector<Meteor> meteors_;
    std::vector<AreaDamage> damage_;
    u32 next_body_id_ = 1;
    Tick now_ = 0;
    PhysicsStats stats_;
    // cause attribution for support checks: position -> cause event of the removal
    std::vector<std::pair<Vec3i, EventId>> removal_causes_;
    EventId current_cause_ = 0;
};

}  // namespace icarus
