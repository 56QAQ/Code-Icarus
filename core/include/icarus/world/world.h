// World storage: cells (地格) of 32^3 cubes with a three-tier lifecycle.
//
//   Ungenerated : only seed + macro information exist. No cube data.
//   Dormant     : cube data exists but is compressed; not simulated, not rendered
//                 with collision. Keeps a summary, timestamps and pending events.
//   Active      : cube data is decompressed and eligible for simulation. Even here
//                 only changed cubes are processed by the physics systems.
//
// Simulation access (get/set) activates cells; renderer access (peek) never changes
// the simulation-relevant state, so camera movement cannot change history.
#pragma once

#include <algorithm>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/util/binio.h"
#include "icarus/world/voxel.h"
#include "icarus/world/worldgen.h"

namespace icarus {

enum class CellState : u8 { Ungenerated = 0, Dormant = 1, Active = 2 };

// A deferred effect addressed to a cell that may be dormant (e.g. a slow process).
struct PendingEvent {
    u16 type = 0;
    Tick due = 0;
    Vec3i pos;
    i32 a = 0, b = 0;
    u32 cause = 0;
};

struct Cell {
    Vec3i coord;
    CellState state = CellState::Ungenerated;
    bool pristine = true;  // identical to generator output -> not stored in saves
    bool uniform = true;
    Voxel uniform_value = 0;
    std::vector<Voxel> vox;    // Active, non-uniform
    std::vector<u8> packed;    // Dormant, non-uniform (RLE)
    Tick last_sim_tick = 0;    // last tick the simulation processed this cell
    Tick last_touch_tick = 0;  // last simulation access
    Tick activated_tick = 0;
    u32 version = 0;           // bumps on every content change (renderer cache key)
    Biome biome = Biome::Sky;
    u16 owner = 0;             // polity id (0 = unclaimed)
    float fertility = 0.5f;
    float moisture = 0.5f;
    float pollution = 0.0f;
    u32 hold = 0;              // >0: something (physics, characters) keeps it active
    std::vector<PendingEvent> pending;

    bool has_data() const { return state != CellState::Ungenerated; }
};

struct VoxelChange {
    Vec3i p;
    Voxel before = 0, after = 0;
    u32 cause = 0;
};

// How much of the islands' forest still stands (trees counted by their trunk base).
struct ForestStats {
    int initial = 0, standing = 0;
    int regrown = 0;  // trees that grew back since (see Ecology)
    float ratio() const {
        return initial > 0 ? std::min(1.0f, (float)(standing + regrown) / (float)initial) : 1.0f;
    }
};

struct WorldStats {
    int ungenerated = 0, dormant = 0, active = 0;
    int non_uniform = 0;
    size_t bytes = 0;
};

class World {
public:
    explicit World(const Registry& reg);

    void init(const WorldConfig& cfg);
    const Registry& reg() const { return *reg_; }
    const WorldGen& gen() const { return gen_; }
    const WorldConfig& config() const { return gen_.config(); }

    int size_x() const { return cells_x_ * kCellSize; }
    int size_y() const { return cells_y_ * kCellSize; }
    int size_z() const { return cells_z_ * kCellSize; }
    int cells_x() const { return cells_x_; }
    int cells_y() const { return cells_y_; }
    int cells_z() const { return cells_z_; }
    bool in_bounds(const Vec3i& p) const {
        return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < size_x() && p.y < size_y() && p.z < size_z();
    }
    bool cell_in_bounds(const Vec3i& c) const {
        return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < cells_x_ && c.y < cells_y_ && c.z < cells_z_;
    }

    // ---- simulation access (activates cells) ----
    Voxel get(const Vec3i& p);
    MatId mat(const Vec3i& p) { return vmat(get(p)); }
    const Material& material(const Vec3i& p) { return reg_->mat(vmat(get(p))); }
    bool solid(const Vec3i& p) { return reg_->mat(vmat(get(p))).solid; }
    void set(const Vec3i& p, Voxel v, u32 cause = 0);
    // Writes without recording a change (used by bulk systems that emit their own events).
    void set_silent(const Vec3i& p, Voxel v);

    // ---- renderer / analysis access (never changes simulation state) ----
    Voxel peek(const Vec3i& p) const;
    // Copies a cell's cubes into out (kCellVol). Generates pristine data if needed.
    void peek_cell(const Vec3i& cc, Voxel* out) const;

    // ---- cells ----
    Cell* cell(const Vec3i& cc) { return cell_in_bounds(cc) ? &cells_[cell_index(cc)] : nullptr; }
    const Cell* cell(const Vec3i& cc) const { return cell_in_bounds(cc) ? &cells_[cell_index(cc)] : nullptr; }
    Cell& ensure_active(const Vec3i& cc);
    void touch(const Vec3i& p);  // mark cell as used by the simulation now
    void hold_cell(const Vec3i& cc, bool hold);
    u32 cell_version(const Vec3i& cc) const;
    Biome cell_biome(const Vec3i& cc) const;

    // Lifecycle: demote idle active cells to dormant. Returns number demoted.
    int update_lifecycle(Tick now, Tick idle_ticks);
    void set_now(Tick t) { now_ = t; }
    Tick now() const { return now_; }
    // Hook called when a dormant cell is re-activated: (cell, last_sim_tick, now).
    std::function<void(Cell&, Tick, Tick)> on_wake;

    // ---- change journal (drained by physics/other systems) ----
    std::vector<VoxelChange>& changes() { return changes_; }
    // Cells whose content changed since last call (for the renderer).
    std::vector<Vec3i> take_dirty_cells();

    // Column queries.
    int surface_y(int x, int z);  // topmost non-air, non-fluid solid cube y (-1 if none)
    int surface_y_peek(int x, int z) const;

    WorldStats stats() const;
    // Cheap: cells never touched still hold their generated trees; only modified cells
    // are looked at.
    ForestStats forest() const;
    // The generator's trees (trunk bases), sorted by cell.
    const std::vector<Vec3i>& generated_trees() const;

    // ---- persistence ----
    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 state_hash() const;

    std::vector<std::pair<Vec3i, std::vector<PendingEvent>>> all_pending() const;

private:
    size_t cell_index(const Vec3i& cc) const { return ((size_t)cc.y * cells_z_ + (size_t)cc.z) * cells_x_ + (size_t)cc.x; }
    void generate(Cell& c) const;
    void activate(Cell& c);
    void compress(Cell& c);
    void unpack_into(const Cell& c, Voxel* out) const;
    void mark_dirty(Cell& c);

    const Registry* reg_;
    WorldGen gen_;
    int cells_x_ = 0, cells_y_ = 0, cells_z_ = 0;
    mutable std::vector<Vec3i> tree_bases_;  // lazily enumerated, sorted by cell
    mutable bool trees_known_ = false;
    mutable std::vector<Cell> cells_;
    std::vector<VoxelChange> changes_;
    std::vector<u8> dirty_flag_;
    std::vector<Vec3i> dirty_list_;
    Tick now_ = 0;
    struct PeekCacheEntry {
        size_t cell = (size_t)-1;
        u32 version = 0;
        std::vector<Voxel> data;
    };
    mutable std::array<PeekCacheEntry, 8> peek_cache_;
    mutable size_t peek_cache_next_ = 0;
};

}  // namespace icarus
