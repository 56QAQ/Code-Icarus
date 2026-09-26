// Civilisation outcomes: the island's forest, and unification ending a round.
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "icarus/sim/simulation.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig village(u64 seed) {
    GameConfig c;
    c.world.seed = seed;
    c.scenario = "village";
    return c;
}
}  // namespace

TEST("outcomes: the forest tally counts generated trees and notices felled ones") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    auto t0 = std::chrono::steady_clock::now();
    const ForestStats f0 = sim.world().forest();
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  forest: %d trees, first tally %.1f ms\n", f0.initial, ms);
    CHECK(f0.initial > 100);
    CHECK_EQ(f0.standing, f0.initial);
    // Every enumerated tree really is a trunk in the generated world.
    std::vector<Vec3i> trees = sim.world().gen().tree_bases();
    const Vec3i v = sim.world().gen().features().village;
    std::sort(trees.begin(), trees.end(), [&](const Vec3i& a, const Vec3i& b) { return a.dist2(v) < b.dist2(v); });
    for (size_t i = 0; i < 20 && i < trees.size(); ++i)
        CHECK(vmat(sim.world().get(trees[i])) == sim.reg().m().log);
    // Fell three of them.
    for (int i = 0; i < 3; ++i) {
        AdminCommand cmd;
        cmd.type = "dig";
        cmd.params = Json::object();
        Json pos = Json::array();
        pos.push(trees[(size_t)i].x);
        pos.push(trees[(size_t)i].y);
        pos.push(trees[(size_t)i].z);
        cmd.params.set("pos", pos);
        cmd.params.set("radius", 0.5);
        sim.apply_admin(cmd);
    }
    t0 = std::chrono::steady_clock::now();
    const ForestStats f1 = sim.world().forest();
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  forest: tally after edits %.1f ms\n", ms);
    CHECK_EQ(f1.initial, f0.initial);
    CHECK_EQ(f1.standing, f0.initial - 3);
}

TEST("outcomes: one polity holding the island after several ends the round, once") {
    Simulation sim(test_registry());
    sim.new_game(village(2));
    sim.run(kTicksPerHour);
    // A splinter with no magical girl cannot hold together and rejoins its parent.
    const u16 nid = sim.society().create_polity("残部", 0x8899AA, 1);
    int moved = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && moved < 3) {
            cp->polity = nid;
            ++moved;
        }
    sim.run(kTicksPerHour * 8);
    int alive = 0;
    for (const Polity& p : sim.society().polities())
        if (p.alive) ++alive;
    CHECK_EQ(alive, 1);
    const EventId u = sim.society().unification_event();
    REQUIRE(u != 0);
    const Event* e = sim.chronicle().get(u);
    REQUIRE(e != nullptr);
    CHECK(e->type == EventType::Unification);
    CHECK(e->causes[0] != 0);  // the merger that did it
    // Only once, and it survives a save.
    sim.run(kTicksPerHour * 3);
    int count = 0;
    for (const Event& x : sim.chronicle().events())
        if (x.type == EventType::Unification) ++count;
    CHECK_EQ(count, 1);
    std::vector<u8> bytes = sim.save();
    Simulation loaded(test_registry());
    loaded.load(bytes);
    CHECK_EQ(loaded.society().unification_event(), u);
}
