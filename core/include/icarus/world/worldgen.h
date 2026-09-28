// Deterministic world generator for the floating-island continent.
// All functions are pure in (seed, coordinates): a cell generates identically no matter
// when, or in which order, it is requested. This is what allows lazy generation.
#pragma once

#include <array>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/world/voxel.h"

namespace icarus {

enum class Biome : u8 {
    Sky = 0, Grassland, Forest, Highland, Lakeshore, Ravine, Underside, Islet,
    // Version 2 (continent layout).
    Taiga, Snowfield, Desert, Savanna, Wetland,
    // Version 3 (random layout): the sea around the island and its beaches.
    Ocean, Beach,
    Count
};
const char* biome_key(Biome b);
const char* biome_name_zh(Biome b);

// Classic: the first version's small island (village plateau, ravine and bridge).
// Continent: a much larger island with a mountain spine, lakes and climate-driven
// biomes, laid out for up to three civilisations.
// Random: an island generated from the seed and the new-game options (terrain, rivers,
// lakes, an optional sea around it, resources, up to four sites).
enum class WorldLayout : u8 { Classic = 0, Continent = 1, Random = 2 };
const char* layout_key(WorldLayout l);
WorldLayout layout_from_key(const std::string& k);

struct WorldConfig {
    WorldLayout layout = WorldLayout::Classic;
    u64 seed = 1;
    int cells_x = 32, cells_y = 8, cells_z = 32;  // world extent in cells (地格)
    float island_radius = 112.0f;                  // main island radius in cubes
    int base_height = 160;                         // main island surface height
    int islet_count = 4;
    // Random layout options (新游戏选项).
    bool sea = true;      // a sea around the island, held in by an invisible wall at its rim
    int richness = 1;     // 资源丰富度: 0 poor, 1 normal, 2 rich
    int relief = 1;       // 地形: 0 gentle, 1 rolling, 2 rugged
    int climate = 0;      // 气候: 0 varied, 1 temperate, 2 cold, 3 hot and dry, 4 wet
    int size = 1;         // 大小: 0 medium, 1 large
    int sea_level = 140;  // y of the topmost sea cube

    // Defaults for a layout (the classic values above, or the continent's). For the
    // random layout the extent follows `size` (see sized()).
    static WorldConfig for_layout(WorldLayout l, u64 seed);
    // The random layout's world extent and island radius for the current options.
    void sized();
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
    bool frozen = false;    // water surface is ice
    bool sea = false;       // the water here is the sea's: salt, not for drinking or fields
    bool weir = false;      // a stone step holding a river reach (walkable ford)
    u8 temp = 128;          // climate (continent): 0 cold .. 255 hot
    u8 moist = 128;         // 0 dry .. 255 wet
};

// A place laid out for a civilisation (continent layout): a flat plateau for the
// settlement, farmland by its lake.
struct Site {
    Vec3i center;       // surface at the plateau centre
    Vec3i farms;        // flat, fertile ground between the plateau and the water
    Vec3i water;        // the site's lake (surface water)
    Biome biome = Biome::Grassland;
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
    // Version 2: every settlement site, spring and still water surface. The classic
    // layout lists its one village, spring, lake and pond here too.
    std::vector<Site> sites;
    std::vector<Vec3i> springs;
    std::vector<Vec3i> waters;
};

class WorldGen {
public:
    void init(const WorldConfig& cfg, const Registry& reg);

    const WorldConfig& config() const { return cfg_; }
    const std::vector<IslandDef>& islands() const { return islands_; }
    const IslandFeatures& features() const { return feat_; }

    // Fill kCellVol voxels for cell coordinate cc.
    void generate_cell(const Vec3i& cc, Voxel* out) const;
    // Every tree the generator plants on the islands (the lowest trunk cube of each),
    // without generating any cell.
    std::vector<Vec3i> tree_bases() const;
    ColumnInfo column(int x, int z) const;
    // The same, without filling the per-cell column cache (for sparse sampling).
    ColumnInfo column_uncached(int x, int z) const { return compute_column(x, z); }
    // Macro summary of a cell without generating it.
    Biome cell_biome(const Vec3i& cc) const;
    bool cell_maybe_nonempty(const Vec3i& cc) const;

    // The sea (random layout with a sea): its level, and the invisible wall at its rim
    // that water cannot pass. Always false/-1 for the other layouts.
    int sea_level() const { return has_sea_ ? cfg_.sea_level : -1; }
    // Water at (x, z) is the sea's (salt: not for drinking or watering fields).
    bool salt(int x, int z) const { return has_sea_ && column(x, z).sea; }
    bool sea_wall(int x, int z) const {
        if (!has_sea_) return false;
        const float dx = (float)x + 0.5f - sea_cx_, dz = (float)z + 0.5f - sea_cz_;
        return dx * dx + dz * dz > sea_r2_;
    }
    // A top-down picture of the island from column information alone (no cubes are
    // generated): `px` pixels on a side, RGB 0xRRGGBB, sites marked. For previews.
    std::vector<u32> preview(int px) const;

private:
    struct Lake {
        float x = 0, z = 0, r = 0;
        int wt = 0;  // water top
        u64 salt = 0;
        bool frozen = false;
    };
    struct Stream {
        Vec3i spring, mouth;
        int mouth_wt = 0;
        // Outflow: from a lake (spring = lake centre at its water top) to the rim. The
        // bed stays at the lake's top until `shore`, so only overflow leaves.
        bool outflow = false;
        float shore = 0.0f;
    };
    struct Peak {
        float x = 0, z = 0, h = 0, w = 0;
    };
    struct ColumnBlock {
        std::array<ColumnInfo, kCellSize * kCellSize> cols;
    };
    ColumnInfo compute_column(int x, int z) const;
    const ColumnBlock& column_block(int cx, int cz) const;
    void to_local(float x, float z, float& lu, float& lv) const;
    float ravine_center_u(float lv) const;
    void place_trees(const Vec3i& cc, Voxel* out) const;
    void place_ores(const Vec3i& cc, Voxel* out) const;
    // Continent layout (worldgen_continent.cpp).
    void init_continent();
    ColumnInfo compute_column_continent(int x, int z) const;
    void generate_cell_continent(const Vec3i& cc, Voxel* out) const;
    struct TreeSpec {
        int x = 0, z = 0, ground = 0;
        u8 kind = 0;  // 0 broadleaf, 1 birch, 2 fruit, 3 pine, 4 acacia
        u64 h = 0;
    };
    bool continent_tree(int sx, int sz, TreeSpec& t) const;
    void place_trees_continent(const Vec3i& cc, Voxel* out) const;
    void place_plants_continent(const Vec3i& cc, Voxel* out) const;
    // Random layout (worldgen_random.cpp).
    struct RandomIsland;
    void init_random();
    ColumnInfo compute_column_random(int x, int z) const;
    struct RawBlock;
    ColumnInfo column_random(int x, int z, RawBlock* rb) const;
    void column_block_random(int cx, int cz, ColumnBlock& out) const;
    void generate_cell_random(const Vec3i& cc, Voxel* out) const;
    std::shared_ptr<const RandomIsland> rnd_;
    // Resource richness (random layout; 1 elsewhere): trees, wild plants, ore bodies.
    float rich_trees_ = 1.0f, rich_plants_ = 1.0f, rich_ores_ = 1.0f;
    bool has_sea_ = false;
    float sea_cx_ = 0, sea_cz_ = 0, sea_r2_ = 0;

    WorldConfig cfg_;
    const Registry* reg_ = nullptr;
    std::vector<IslandDef> islands_;
    IslandFeatures feat_;
    float cos0_ = 1, sin0_ = 0;
    std::vector<Lake> lakes_;
    std::vector<Stream> streams_;
    std::vector<Peak> peaks_;
    std::vector<std::pair<Vec3i, Biome>> site_climate_;  // site centre -> intended biome
    mutable std::unordered_map<i64, std::unique_ptr<ColumnBlock>> col_cache_;
};

}  // namespace icarus
