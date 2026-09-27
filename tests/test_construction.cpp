// Where villages build and whether what they start gets finished.
#include <algorithm>

#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig tribal(u64 seed) {
    GameConfig c;
    c.world = WorldConfig::for_layout(WorldLayout::Continent, seed);
    c.scenario = "wild";
    c.era = "tribal";
    return c;
}
struct Box {
    int x0, z0, x1, z1;
};
Box box_of(const Building& b) {
    Box bx{b.plan_pos[0].x, b.plan_pos[0].z, b.plan_pos[0].x, b.plan_pos[0].z};
    for (const Vec3i& p : b.plan_pos) {
        bx.x0 = std::min(bx.x0, p.x);
        bx.z0 = std::min(bx.z0, p.z);
        bx.x1 = std::max(bx.x1, p.x);
        bx.z1 = std::max(bx.z1, p.z);
    }
    return bx;
}
bool inside(const Box& b, const Vec3i& p) { return p.x >= b.x0 && p.x <= b.x1 && p.z >= b.z0 && p.z <= b.z1; }
u32 start(Simulation& sim, u16 polity, const std::string& def, const Vec3i& near) {
    Vec3i origin;
    u8 rot = 0;
    if (!sim.buildings().find_site(def, near, 40, origin, rot)) return 0;
    Project pr;
    pr.polity = polity;
    pr.kind = "construct";
    pr.title = "兴建" + def;
    pr.priority = 2.0f;
    pr.target = origin;
    const u32 pid = sim.society().add_project(pr);
    const u32 bid = sim.buildings().start_site(def, origin, rot, polity, pid);
    if (Project* p = sim.society().project(pid)) p->building = bid;
    return bid;
}
}  // namespace

TEST("construction: new buildings keep their distance and an open way to every door") {
    Simulation sim(test_registry());
    sim.new_game(tribal(6));
    sim.run(5);
    const Polity& p = *sim.society().polity(1);
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    const Vec3i near = seat->entrance;
    for (const char* def : {"hut", "hut", "storehouse", "hut", "longhouse", "hut"}) start(sim, p.id, def, near);
    std::vector<const Building*> all;
    for (const Building& b : sim.buildings().all())
        if (b.alive && !b.is_bridge && !b.plan_pos.empty()) all.push_back(&b);
    REQUIRE(all.size() >= 6);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t k = i + 1; k < all.size(); ++k) {
            const Building& a = *all[i];
            const Building& b = *all[k];
            if (a.project == 0 && b.project == 0) continue;  // the camp as it was founded
            const Box ba = box_of(a), bb = box_of(b);
            // At least two free cubes between them.
            const bool apart = ba.x1 + 2 < bb.x0 || bb.x1 + 2 < ba.x0 || ba.z1 + 2 < bb.z0 || bb.z1 + 2 < ba.z0;
            CHECK(apart);
            // Nobody's doorstep lies under another building.
            CHECK(!inside(bb, a.entrance));
            CHECK(!inside(ba, b.entrance));
        }
}

TEST("construction: what was begun first is finished first") {
    Simulation sim(test_registry());
    sim.new_game(tribal(6));
    sim.run(5);
    Polity& p = *sim.society().polity(1);
    for (const char* k : {"thatching", "carpentry"}) p.techs.push_back(k);
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    const Vec3i near = seat->entrance;
    // A longhouse first, then a hut (cheaper, so nearer completion once begun).
    const u32 first = start(sim, p.id, "longhouse", near);
    const u32 second = start(sim, p.id, "hut", near);
    REQUIRE(first && second);
    // Materials for the longhouse only.
    Economy& econ = sim.economy();
    for (auto [it, n] : sim.buildings().def("longhouse")->cost) econ.add(seat->store, it, n, "test");
    bool done = false;
    for (int h = 0; h < 24 * 6 && !done; ++h) {
        sim.run(kTicksPerHour);
        done = sim.buildings().get(first)->functional;
    }
    CHECK(done);
    CHECK(!sim.buildings().get(second)->functional);
}
