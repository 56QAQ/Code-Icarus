// Version 2 world: the continent layout (large island, climate biomes, wild plants).
#include <set>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/util/binio.h"
#include "icarus/world/world.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
WorldConfig continent(u64 seed) { return WorldConfig::for_layout(WorldLayout::Continent, seed); }
}  // namespace

TEST("continent: deterministic, order independent, larger than classic") {
    const Registry& reg = test_registry();
    WorldGen g1, g2;
    g1.init(continent(5), reg);
    g2.init(continent(5), reg);
    const Vec3i cc = cell_of(g1.features().sites[1].center);
    std::vector<Voxel> a(kCellVol), b(kCellVol);
    g1.generate_cell(cc, a.data());
    g2.generate_cell(cc + Vec3i{1, 0, -1}, b.data());
    g2.generate_cell(cc, b.data());
    CHECK(a == b);
    CHECK(g1.tree_bases() == g2.tree_bases());
    WorldGen classic;
    classic.init(WorldConfig{}, reg);
    CHECK(g1.islands()[0].radius > 2.5f * classic.islands()[0].radius);
}

TEST("continent: three sites in different biomes, flat and far apart") {
    const Registry& reg = test_registry();
    for (u64 seed : {1ull, 2ull, 9ull}) {
        WorldGen g;
        g.init(continent(seed), reg);
        const auto& sites = g.features().sites;
        CHECK_EQ((int)sites.size(), 3);
        std::set<int> biomes;
        for (size_t i = 0; i < sites.size(); ++i) {
            const Site& s = sites[i];
            biomes.insert((int)s.biome);
            const ColumnInfo c = g.column(s.center.x, s.center.z);
            CHECK(c.land);
            CHECK(c.water_top < 0);
            // The plateau is level around the centre.
            int lo = 9999, hi = -9999;
            for (int dz = -10; dz <= 10; dz += 2)
                for (int dx = -10; dx <= 10; dx += 2) {
                    const ColumnInfo q = g.column(s.center.x + dx, s.center.z + dz);
                    lo = std::min<int>(lo, q.top);
                    hi = std::max<int>(hi, q.top);
                }
            CHECK(hi - lo <= 1);
            // Its lake holds water.
            CHECK(g.column(s.water.x, s.water.z).water_top >= 0);
            for (size_t j = i + 1; j < sites.size(); ++j) CHECK(s.center.dist2(sites[j].center) > 200 * 200);
        }
        CHECK_EQ((int)biomes.size(), 3);
    }
}

TEST("continent: every climate biome appears") {
    const Registry& reg = test_registry();
    WorldGen g;
    g.init(continent(1), reg);
    int counts[(int)Biome::Count] = {0};
    int land = 0;
    const int W = g.config().cells_x * kCellSize, D = g.config().cells_z * kCellSize;
    for (int z = 0; z < D; z += 4)
        for (int x = 0; x < W; x += 4) {
            const ColumnInfo c = g.column(x, z);
            if (!c.land) continue;
            ++land;
            counts[(int)c.biome]++;
        }
    CHECK(land > 15000);  // ~ 9x the classic island's area (sampled 1 in 16)
    for (Biome b : {Biome::Grassland, Biome::Forest, Biome::Highland, Biome::Taiga, Biome::Snowfield, Biome::Desert,
                    Biome::Savanna, Biome::Wetland})
        CHECK(counts[(int)b] > land / 100);
}

TEST("continent: still water never spills over its banks") {
    const Registry& reg = test_registry();
    for (u64 seed : {1ull, 4ull}) {
        WorldGen g;
        g.init(continent(seed), reg);
        const int W = g.config().cells_x * kCellSize, D = g.config().cells_z * kCellSize;
        int water = 0, leaks = 0;
        for (int z = 1; z < D - 1; ++z)
            for (int x = 1; x < W - 1; ++x) {
                const ColumnInfo c = g.column(x, z);
                if (!c.land || c.water_top < 0) continue;
                ++water;
                for (int k = 0; k < 4; ++k) {
                    const ColumnInfo n = g.column(x + kDir4H[k].x, z + kDir4H[k].z);
                    if (!n.land || (n.water_top < c.water_top && n.top < c.water_top)) ++leaks;
                }
            }
        CHECK(water > 1000);
        CHECK_EQ(leaks, 0);
    }
}

TEST("continent: wild resources are generated") {
    const Registry& reg = test_registry();
    const CoreMats& M = reg.m();
    World w(reg);
    w.init(continent(1));
    std::set<MatId> seen;
    // Sample the surface around each site and along the island.
    const auto& sites = w.gen().features().sites;
    for (const Site& s : sites)
        for (int dz = -90; dz <= 90; dz += 1)
            for (int dx = -90; dx <= 90; dx += 3) {
                const int x = s.center.x + dx, z = s.center.z + dz;
                const ColumnInfo c = w.gen().column(x, z);
                if (!c.land) continue;
                for (int y = c.top - 1; y <= c.top + 14; ++y) seen.insert(vmat(w.peek({x, y, z})));
            }
    for (MatId m : {M.wild_grain, M.berry_bush, M.fruit_leaves, M.birch_log, M.pine_log, M.pine_leaves, M.dry_grass,
                    M.mushroom})
        CHECK(seen.count(m) == 1);
    // Every generated tree stands on a trunk cube.
    int checked = 0;
    for (const Vec3i& t : w.generated_trees()) {
        if (checked++ > 400) break;
        CHECK(reg.mat(vmat(w.peek(t))).trunk);
    }
}

TEST("continent: layout survives save and load") {
    const Registry& reg = test_registry();
    World w(reg);
    w.init(continent(3));
    const Vec3i p = w.gen().features().sites[2].center + Vec3i{0, 1, 0};
    w.set(p, make_voxel(reg.m().stone));
    BinWriter out;
    w.save(out);
    World w2(reg);
    BinReader in(out.data());
    w2.load(in);
    CHECK(w2.config().layout == WorldLayout::Continent);
    CHECK_EQ(w2.config().cells_y, 10);
    CHECK_EQ((int)w2.mat(p), (int)reg.m().stone);
    CHECK(w2.gen().features().sites[2].center == w.gen().features().sites[2].center);
}
