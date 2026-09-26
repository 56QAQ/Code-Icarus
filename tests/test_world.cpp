#include <vector>

#include "icarus/data/registry.h"
#include "icarus/world/world.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

TEST("registry: loads game data") {
    const Registry& reg = test_registry();
    CHECK(reg.mat_count() > 20);
    CHECK_EQ((int)reg.m().air, 0);
    CHECK(reg.mat(reg.m().water).fluid);
    CHECK(reg.mat(reg.m().levistone).anchor);
    CHECK(reg.item_count() > 10);
    CHECK(reg.find_item("grain") != kNoItem);
}

TEST("worldgen: deterministic and order independent") {
    const Registry& reg = test_registry();
    WorldConfig cfg;
    cfg.seed = 7;
    WorldGen g1, g2;
    g1.init(cfg, reg);
    g2.init(cfg, reg);
    Vec3i island_center = cell_of(g1.features().village);
    std::vector<Voxel> a(kCellVol), b(kCellVol);
    // Generate in different orders; results must match.
    Vec3i other{island_center.x + 1, island_center.y, island_center.z + 1};
    g1.generate_cell(island_center, a.data());
    g2.generate_cell(other, b.data());
    g2.generate_cell(island_center, b.data());
    CHECK(a == b);
    // Different seed -> different world.
    WorldConfig cfg2 = cfg;
    cfg2.seed = 8;
    WorldGen g3;
    g3.init(cfg2, reg);
    std::vector<Voxel> c(kCellVol);
    g3.generate_cell(cell_of(g3.features().village), c.data());
    CHECK(!(a == c));
}

TEST("worldgen: island has key features") {
    const Registry& reg = test_registry();
    WorldConfig cfg;
    cfg.seed = 3;
    WorldGen g;
    g.init(cfg, reg);
    const IslandFeatures& f = g.features();
    ColumnInfo v = g.column(f.village.x, f.village.z);
    CHECK(v.land);
    CHECK(!v.ravine);
    ColumnInfo lake = g.column(f.lake.x, f.lake.z);
    CHECK(lake.water_top > lake.top);
    // Bridge endpoints are on either side of the ravine and roughly level.
    CHECK(g.column(f.bridge_a.x, f.bridge_a.z).land);
    CHECK(g.column(f.bridge_b.x, f.bridge_b.z).land);
    int mid_x = (f.bridge_a.x + f.bridge_b.x) / 2, mid_z = (f.bridge_a.z + f.bridge_b.z) / 2;
    ColumnInfo mid = g.column(mid_x, mid_z);
    CHECK(mid.ravine);
    CHECK(mid.top < f.bridge_a.y - 10);
    CHECK(f.mountain.y > f.village.y + 15);
    // Outside the islands is sky.
    CHECK(!g.column(2, 2).land);
}

TEST("world: lifecycle ungenerated -> active -> dormant -> active keeps edits") {
    const Registry& reg = test_registry();
    World w(reg);
    WorldConfig cfg;
    cfg.seed = 11;
    w.init(cfg);
    Vec3i p = w.gen().features().village;
    p.y += 3;
    Vec3i cc = cell_of(p);
    CHECK(w.cell(cc)->state == CellState::Ungenerated);
    int wakes = 0;
    w.on_wake = [&](Cell&, Tick, Tick) { ++wakes; };
    w.set_now(5);
    w.set(p, make_voxel(reg.m().planks));
    CHECK(w.cell(cc)->state == CellState::Active);
    CHECK(!w.cell(cc)->pristine);
    CHECK_EQ(w.changes().size(), (size_t)1);
    w.set_now(1000);
    int demoted = w.update_lifecycle(1000, 100);
    CHECK(demoted >= 1);
    CHECK(w.cell(cc)->state == CellState::Dormant);
    // Renderer access does not wake the cell.
    CHECK_EQ((int)vmat(w.peek(p)), (int)reg.m().planks);
    CHECK(w.cell(cc)->state == CellState::Dormant);
    int wakes_before = wakes;
    w.set_now(2000);
    CHECK_EQ((int)w.mat(p), (int)reg.m().planks);
    CHECK(w.cell(cc)->state == CellState::Active);
    CHECK_EQ(wakes, wakes_before + 1);
}

TEST("world: save/load preserves edits and hash") {
    const Registry& reg = test_registry();
    World w(reg);
    WorldConfig cfg;
    cfg.seed = 12;
    w.init(cfg);
    Vec3i p = w.gen().features().farms;
    for (int i = 0; i < 20; ++i) w.set({p.x + i, p.y, p.z}, make_voxel(reg.m().air));
    w.set({p.x, p.y + 5, p.z}, make_voxel(reg.m().brick));
    w.set_now(50);
    w.update_lifecycle(50, 10);
    u64 h1 = w.state_hash();
    BinWriter bw;
    w.save(bw);
    World w2(reg);
    BinReader br(bw.data());
    w2.load(br);
    CHECK_EQ(w2.state_hash(), h1);
    CHECK_EQ((int)w2.mat({p.x + 3, p.y, p.z}), (int)reg.m().air);
    CHECK_EQ((int)w2.mat({p.x, p.y + 5, p.z}), (int)reg.m().brick);
    // Untouched far cell regenerates identically.
    Vec3i q = w.gen().features().mountain;
    CHECK_EQ(w.peek(q), w2.peek(q));
}
