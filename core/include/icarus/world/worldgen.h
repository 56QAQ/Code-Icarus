// Deterministic world generator for the floating-island continent.
// All functions are pure in (seed, coordinates): a cell generates identically no matter
// when, or in which order, it is requested. This is what allows lazy generation.
#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/world/voxel.h"

namespace icarus {

enum class Biome : u8 { Sky = 0, Grassland, Forest, Highland, Lakeshore, Ravine, Underside, Islet, Count };
const char* biome_key(Biome b);
const char* biome_name_zh(Biome b);

struct WorldConfig {
    u64 seed = 1;
    int cells_x = 32, cells_y = 8, cells_z = 32;  // world extent in cells (地格)
    float island_radius = 112.0f;                  // main island radius in cubes
    int base_height = 160;                         // main island surface height
    int islet_count = 4;
};

struct IslandDef {
    float cx = 0, cz = 0;
    float radius = 0;
    float base_h = 0;
    float thickness = 0;
    u64 salt = 0;
    bool main = false;
};

// Per-column terrain description (cheap, computed without generating cubes).
struct ColumnInfo {
    bool land = false;
    i16 top = -1;       // y of the topmost solid terrain cube
    i16 bottom = 0;     // y of the lowest solid cube
    i16 water_top = -1; // y of the topmost water cube (lakes), -1 if none
    Biome biome = Biome::Sky;
    u8 island = 255;    // index into islands()
    bool ravine = false;
    bool reserved = false;  // keep clear of trees (village, farms, bridge)
};

// Named locations of the main island, used by the scenario builder.
struct IslandFeatures {
    Vec3i village;      // centre of the village plateau (surface)
    Vec3i farms;        // centre of the farm area
    Vec3i lake;         // lake centre (surface water)
    Vec3i pond;         // small pond near the village
    Vec3i spring;       // spring cube position
    Vec3i mountain;     // summit
    Vec3i bridge_a;     // ravine rim on the village side
    Vec3i bridge_b;     // ravine rim on the farm side
    Vec3i ravine_end;   // inner end of the ravine (detour route)
    float axis_u_x = 1, axis_u_z = 0;  // local frame: u crosses the ravine
    float axis_v_x = 0, axis_v_z = 1;  // v runs along the ravine
    int lake_radius = 13;
    int pond_radius = 6;
};

class WorldGen {
public:
    void init(const WorldConfig& cfg, const Registry& reg);

    const WorldConfig& config() const { return cfg_; }
    const std::vector<IslandDef>& islands() const { return islands_; }
    const IslandFeatures& features() const { return feat_; }

    // Fill kCellVol voxels for cell coordinate cc.
    void generate_cell(const Vec3i& cc, Voxel* out) const;
    ColumnInfo column(int x, int z) const;
    // Macro summary of a cell without generating it.
    Biome cell_biome(const Vec3i& cc) const;
    bool cell_maybe_nonempty(const Vec3i& cc) const;

private:
    struct ColumnBlock {
        std::array<ColumnInfo, kCellSize * kCellSize> cols;
    };
    ColumnInfo compute_column(int x, int z) const;
    const ColumnBlock& column_block(int cx, int cz) const;
    void to_local(float x, float z, float& lu, float& lv) const;
    float ravine_center_u(float lv) const;
    void place_trees(const Vec3i& cc, Voxel* out) const;
    void place_ores(const Vec3i& cc, Voxel* out) const;

    WorldConfig cfg_;
    const Registry* reg_ = nullptr;
    std::vector<IslandDef> islands_;
    IslandFeatures feat_;
    float cos0_ = 1, sin0_ = 0;
    mutable std::unordered_map<i64, std::unique_ptr<ColumnBlock>> col_cache_;
};

}  // namespace icarus
