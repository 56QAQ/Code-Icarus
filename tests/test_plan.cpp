// The steward's plan (内政官) and the civilisation index.
#include <cmath>
#include <set>
#include <string>

#include "icarus/sim/simulation.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

TEST("steward: the island's index counts what is known once, wherever it is known") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world = WorldConfig::for_layout(WorldLayout::Random, 3);
    cfg.world.sized();
    cfg.era = "wild";
    cfg.civs = 2;
    cfg.scenario = "wild";
    sim.new_game(cfg);
    sim.run(kTicksPerDay);
    const auto& isl = sim.society().island_history();
    REQUIRE(!isl.empty());
    float rest = 0.0f;
    std::set<std::string> known;
    int peoples = 0;
    for (const Polity& p : sim.society().polities()) {
        if (!p.alive) continue;
        REQUIRE(!p.civ_history.empty());
        rest += p.civ_history.back().total - kCivKnownWeight * p.civ_history.back().known;
        known.insert(p.techs.begin(), p.techs.end());
        ++peoples;
    }
    CHECK_EQ(peoples, 2);
    float k = 0.0f;
    for (const Json& t : sim.reg().doc("techs")["techs"].items())
        if (known.count(t.str("key"))) k += 1.0f + (float)t.integer("era", 0);
    // (Both peoples start with the same wild-era knowledge: summing it would count it twice.)
    CHECK(std::fabs(isl.back() - (rest + kCivKnownWeight * k)) < 0.01f);
}

TEST("steward: a people whose grown folk are ageing is told to have children to take their place") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world.seed = 1;
    cfg.scenario = "village";
    sim.new_game(cfg);
    sim.run(kTicksPerHour);
    // Everyone grown is fifty: old a generation from now, not yet too old for children.
    Character* a = nullptr;
    Character* b = nullptr;
    for (auto& cp : sim.agents().all()) {
        if (!cp || !cp->alive || cp->is_girl() || cp->polity != 1 || sim.agents().is_child(*cp)) continue;
        cp->age0 = 50.0f;
        cp->born = sim.now();
        cp->partner = kNoEntity;
        if (!a) a = cp.get();
        else if (!b) b = cp.get();
    }
    REQUIRE(a != nullptr && b != nullptr);
    a->partner = b->id;
    b->partner = a->id;
    sim.run(kTicksPerHour * 3);  // (the plan is drawn up every two hours)
    const PolityPlan& plan = sim.society().polity(1)->plan;
    CHECK(plan.renewing);
    CHECK(plan.birth >= 0.2f);
    CHECK(plan.birth_why.find("接替") != std::string::npos);
}
