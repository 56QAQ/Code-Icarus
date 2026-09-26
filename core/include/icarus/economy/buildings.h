// Buildings are made of real cubes placed in the world. A building tracks its plan
// (which cube should be where) and derives integrity from the actual world state, so
// meteors, fire and digging degrade function naturally. Construction sites place the
// plan cube by cube using delivered materials.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "icarus/economy/economy.h"
#include "icarus/sim/chronicle.h"
#include "icarus/world/world.h"

namespace icarus {

class Physics;

struct BuildingDef {
    std::string key, name, category, description, tech, workstation;
    int w = 0, d = 0, h = 0;
    int beds = 0;
    float storage = 0;
    float spoil_factor = 1.0f;
    bool seat = false;
    struct PlanCell {
        Vec3i local;
        MatId mat;  // 0 = must be clear
    };
    std::vector<PlanCell> cells;
    std::map<ItemId, int> cost;
    Vec3i door_local{-1, -1, -1};
};

struct Building {
    u32 id = 0;
    bool alive = false;
    std::string def;
    std::string name;
    u16 polity = 0;
    Vec3i origin;
    u8 rot = 0;
    bool complete = false;
    bool functional = false;
    bool is_bridge = false;
    std::vector<Vec3i> plan_pos;
    std::vector<Voxel> plan_vox;   // target voxel (air = clear)
    i32 solid_total = 0;
    i32 solid_intact = 0;
    float integrity = 0;
    StoreId store = kNoStore;      // storage (stockpile / workshop)
    StoreId site = kNoStore;       // construction materials while being built or repaired
    std::vector<EntityId> residents;
    Vec3i entrance;                // standable spot outside the door
    Vec3i inside;                  // standable spot inside (storage access, beds)
    u32 project = 0;
    Tick completed_tick = 0;
    EventId last_event = 0;
    int beds = 0;
    // Bridge endpoints (standable positions at each end).
    Vec3i end_a, end_b;
};

MatId material_for_item_placement(const Registry& reg, ItemId item);
ItemId item_for_material(const Registry& reg, MatId mat);

class Buildings {
public:
    Buildings(World& w, Economy& e, Chronicle& c) : w_(w), econ_(e), chron_(c) {}

    void load_defs(const Registry& reg);
    void reset();
    void set_physics(Physics* p) { physics_ = p; }

    const BuildingDef* def(const std::string& key) const;
    const std::vector<BuildingDef>& defs() const { return defs_; }

    // Places a finished building immediately (scenario setup / admin gift).
    u32 place_complete(const std::string& def_key, const Vec3i& origin, u8 rot, u16 polity, EventId cause);
    // Starts a construction site; cubes are placed later by builders.
    u32 start_site(const std::string& def_key, const Vec3i& origin, u8 rot, u16 polity, u32 project);
    // Procedural plank bridge between two standable rim positions.
    u32 place_bridge(const Vec3i& a, const Vec3i& b, u16 polity, bool complete, EventId cause, u32 project = 0);

    Building* get(u32 id) { return (id > 0 && id < list_.size() && list_[id].alive) ? &list_[id] : nullptr; }
    const Building* get(u32 id) const { return (id > 0 && id < list_.size() && list_[id].alive) ? &list_[id] : nullptr; }
    const std::vector<Building>& all() const { return list_; }
    u32 at(const Vec3i& p) const;

    // Construction helpers.
    // Next plan index that can be built now (supported, needs material), or -1.
    int next_buildable(const Building& b, int start_hint = 0);
    bool place_cell(Building& b, int idx, EventId cause);
    std::map<ItemId, int> remaining_cost(const Building& b) const;
    bool site_done(const Building& b);
    void finish(Building& b, EventId cause);
    void demolish(u32 id);
    // Turns a damaged building back into a construction site (repair project).
    bool reopen(u32 id, u32 project);
    // Finds a flat, free spot for a building near a point (origin y = ground + 1).
    bool find_site(const std::string& def_key, const Vec3i& near, int radius, Vec3i& origin, u8& rot);

    void on_changes(const std::vector<VoxelChange>& changes);
    void recompute(Building& b, EventId cause);

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    Vec3i to_world(const BuildingDef& d, const Vec3i& origin, u8 rot, const Vec3i& local) const;
    void index_building(const Building& b);
    void bridge_check(Building& b, EventId cause);

    World& w_;
    Economy& econ_;
    Chronicle& chron_;
    Physics* physics_ = nullptr;
    const Registry* reg_ = nullptr;
    std::vector<BuildingDef> defs_;
    std::vector<Building> list_ = std::vector<Building>(1);
    std::unordered_map<Vec3i, u32, Vec3iHash> index_;
};

}  // namespace icarus
