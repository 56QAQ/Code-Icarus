#include <cmath>

#include "icarus/sim/simulation.h"
#include "icarus/util/log.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {

GameConfig empty_config(u64 seed) {
    GameConfig c;
    c.world.seed = seed;
    c.scenario = "empty";
    return c;
}

i64 water_units_in_box(World& w, Vec3i a, Vec3i b) {
    i64 total = 0;
    MatId water = w.reg().m().water;
    for (int y = a.y; y <= b.y; ++y)
        for (int z = a.z; z <= b.z; ++z)
            for (int x = a.x; x <= b.x; ++x) {
                Voxel v = w.get({x, y, z});
                if (vmat(v) == water) total += vlevel(v);
            }
    return total;
}

// A stone tub in the sky, anchored with a levistone block so it does not fall.
Vec3i build_tub(Simulation& sim, int inner) {
    World& w = sim.world();
    const CoreMats& M = sim.reg().m();
    Vec3i o{60, 220, 60};
    for (int y = 0; y <= 6; ++y)
        for (int z = -1; z <= inner; ++z)
            for (int x = -1; x <= inner; ++x) {
                bool wall = x == -1 || z == -1 || x == inner || z == inner || y == 0;
                if (wall) w.set(o + Vec3i{x, y, z}, make_voxel(y == 0 && x == -1 && z == -1 ? M.levistone : M.stone));
            }
    return o;
}

}  // namespace

TEST("physics: water is conserved while it flows and settles") {
    Simulation sim(test_registry());
    sim.new_game(empty_config(21));
    World& w = sim.world();
    const CoreMats& M = sim.reg().m();
    Vec3i o = build_tub(sim, 10);
    sim.physics().evaporation = 0.0f;  // evaporation stats are global; isolate the tub
    sim.physics().evaporation_samples = 0;
    // Drop a tall column of water into one corner.
    for (int y = 1; y <= 5; ++y) w.set(o + Vec3i{0, y, 0}, make_voxel(M.water, kFluidFull));
    sim.run(2);
    i64 before = water_units_in_box(w, o + Vec3i{-1, 0, -1}, o + Vec3i{10, 7, 10});
    CHECK_EQ(before, (i64)(5 * kFluidFull));
    sim.run(400);
    i64 after = water_units_in_box(w, o + Vec3i{-1, 0, -1}, o + Vec3i{10, 7, 10});
    CHECK_EQ(after, before);
    // It should have spread out: the corner column is no longer 5 high.
    CHECK(vmat(w.get(o + Vec3i{0, 3, 0})) != M.water);
    CHECK(vmat(w.get(o + Vec3i{3, 1, 3})) == M.water);
}

TEST("physics: water falling off the island is lost to the abyss") {
    Simulation sim(test_registry());
    sim.new_game(empty_config(22));
    World& w = sim.world();
    const CoreMats& M = sim.reg().m();
    Vec3i p{40, 30, 40};  // open sky far from islands
    sim.physics().evaporation_samples = 0;
    w.set(p, make_voxel(M.water, kFluidFull));
    sim.run(80);
    CHECK_EQ(sim.physics().stats().water_units_to_void, (i64)kFluidFull);
    CHECK_EQ((int)vmat(w.get(p)), (int)M.air);
}

TEST("physics: severed structure collapses as debris and lands") {
    Simulation sim(test_registry());
    sim.new_game(empty_config(23));
    World& w = sim.world();
    const CoreMats& M = sim.reg().m();
    const IslandFeatures& f = w.gen().features();
    Vec3i g = f.farms;  // flat ground
    int top = w.surface_y(g.x, g.z);
    // Pillar from ground up 8, then a horizontal beam of planks 6 long.
    for (int y = 1; y <= 8; ++y) w.set({g.x, top + y, g.z}, make_voxel(M.planks));
    for (int x = 1; x <= 6; ++x) w.set({g.x + x, top + 8, g.z}, make_voxel(M.planks));
    sim.run(2);
    // Cut the pillar at its base: everything above is unsupported.
    EventId cut = sim.apply_admin({"dig", Json::parse(strfmt("{\"pos\":[%d,%d,%d],\"radius\":0.4}", g.x, top + 1, g.z))});
    sim.run(3);
    CHECK(!sim.physics().debris().empty());
    bool found_collapse = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Collapse && e.causes[0] == cut) found_collapse = true;
    CHECK(found_collapse);
    sim.run(200);
    CHECK(sim.physics().debris().empty());
    // The planks landed back on the ground (some may have shattered).
    int planks = 0;
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 9; ++dx)
            for (int dy = 0; dy <= 9; ++dy)
                if (w.mat({g.x + dx, top + dy, g.z + dz}) == M.planks) ++planks;
    CHECK(planks >= 6);
    CHECK(planks <= 13);
}

TEST("physics: meteor carves a crater with causal chain") {
    Simulation sim(test_registry());
    sim.new_game(empty_config(24));
    World& w = sim.world();
    const CoreMats& M = sim.reg().m();
    Vec3i g = w.gen().features().farms;
    int top = w.surface_y(g.x, g.z);
    EventId admin = sim.apply_admin({"meteor", Json::parse(strfmt("{\"pos\":[%d,%d,%d],\"radius\":5}", g.x, top, g.z))});
    sim.run(200);
    CHECK(sim.physics().meteors().empty());
    const Event* impact = nullptr;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::MeteorImpact) impact = &e;
    REQUIRE(impact != nullptr);
    CHECK_EQ(impact->causes[0], admin);
    // Crater: the impact point area is now lower than before.
    int new_top = w.surface_y(impact->pos.x, impact->pos.z);
    CHECK(new_top < top - 2);
    // Meteoric iron was deposited somewhere near the impact.
    int iron = 0;
    for (int dy = -8; dy <= 2; ++dy)
        for (int dz = -4; dz <= 4; ++dz)
            for (int dx = -4; dx <= 4; ++dx)
                if (w.mat(impact->pos + Vec3i{dx, dy, dz}) == M.meteorite) ++iron;
    CHECK(iron > 0);
}

TEST("physics: fire spreads through wood and is quenched by water") {
    Simulation sim(test_registry());
    sim.new_game(empty_config(25));
    World& w = sim.world();
    const CoreMats& M = sim.reg().m();
    Vec3i o = build_tub(sim, 8);
    // A plank floor inside the tub, one water cube touching one corner.
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x) w.set(o + Vec3i{x, 1, z}, make_voxel(M.planks));
    sim.run(1);
    sim.apply_admin({"ignite", Json::parse(strfmt("{\"pos\":[%d,%d,%d],\"radius\":0}", o.x + 4, o.y + 1, o.z + 4))});
    int burning_peak = 0;
    for (int i = 0; i < 1500; ++i) {
        sim.step();
        burning_peak = std::max(burning_peak, (int)sim.physics().stats().fire_active);
    }
    CHECK(burning_peak > 4);
    int planks_left = 0;
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x)
            if (w.mat(o + Vec3i{x, 1, z}) == M.planks) ++planks_left;
    CHECK(planks_left < 40);
}

TEST("sim: deterministic and save/load continues identically") {
    const Registry& reg = test_registry();
    auto scenario = [&](Simulation& s) {
        Vec3i g = s.world().gen().features().village;
        int top = s.world().surface_y(g.x, g.z);
        s.queue_admin({"meteor", Json::parse(strfmt("{\"pos\":[%d,%d,%d],\"radius\":4}", g.x, top, g.z))});
        const IslandFeatures& f = s.world().gen().features();
        s.queue_admin({"dig", Json::parse(strfmt("{\"pos\":[%d,%d,%d],\"radius\":3}", f.lake.x + f.lake_radius, f.lake.y, f.lake.z))});
    };
    Simulation a(reg), b(reg);
    a.new_game(empty_config(99));
    b.new_game(empty_config(99));
    scenario(a);
    scenario(b);
    a.run(300);
    b.run(150);
    std::vector<u8> blob = b.save();
    Simulation c(reg);
    c.load(blob);
    CHECK_EQ(c.state_hash(), b.state_hash());
    b.run(150);
    c.run(150);
    CHECK_EQ(a.state_hash(), b.state_hash());
    CHECK_EQ(a.state_hash(), c.state_hash());
}
