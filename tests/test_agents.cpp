#include <map>

#include "icarus/sim/simulation.h"
#include "icarus/util/log.h"
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

TEST("village: setup creates polity, girls, residents, buildings, fields") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    const Polity* p = sim.society().polity(1);
    REQUIRE(p != nullptr);
    CHECK(p->ruler != kNoEntity);
    int girls = 0, residents = 0;
    for (auto& c : sim.agents().all())
        if (c) (c->is_girl() ? girls : residents)++;
    CHECK_EQ(girls, 3);
    CHECK_EQ(residents, 18);
    int huts = 0;
    bool bridge = false;
    for (auto& b : sim.buildings().all()) {
        if (!b.alive) continue;
        if (b.def == "hut") ++huts;
        if (b.is_bridge) bridge = b.functional;
    }
    CHECK(huts >= 6);
    CHECK(bridge);
    CHECK(sim.farming().stats_polity(1).plots > 40);
    CHECK(sim.society().title(1).find("的文明，") != std::string::npos);
}

TEST("village: residents survive and work for two days") {
    Simulation sim(test_registry());
    sim.new_game(village(2));
    int harvested = 0, drinks = 0;
    for (int i = 0; i < 2 * (int)kTicksPerDay; ++i) {
        sim.step();
        if (sim.now() % kTicksPerDay == kTicksPerDay - 1) {
            harvested += sim.agents().day.harvested;
            drinks += sim.agents().day.drinks;
        }
    }
    CHECK_EQ(sim.agents().count_alive(1), 21);
    CHECK(harvested > 40);
    CHECK(drinks > 40);
    // Nobody is permanently stuck: path failures stay rare.
    CHECK(sim.agents().debug_path_failures.size() < 60);
}

TEST("ledger: items are conserved (stock == produced - consumed)") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    sim.run(kTicksPerDay);
    const Registry& reg = test_registry();
    for (size_t i = 0; i < reg.item_count(); ++i) {
        ItemId it = (ItemId)i;
        const LedgerLine& l = sim.economy().ledger(it);
        i64 stock = sim.economy().total(it);
        if (stock != l.produced - l.consumed)
            std::printf("  item %s stock %lld produced %lld consumed %lld\n", reg.item(it).key.c_str(), (long long)stock,
                        (long long)l.produced, (long long)l.consumed);
        CHECK_EQ(stock, l.produced - l.consumed);
    }
}

TEST("village: save/load mid-run continues identically") {
    const Registry& reg = test_registry();
    Simulation a(reg), b(reg);
    a.new_game(village(4));
    b.new_game(village(4));
    a.run(900);
    b.run(450);
    std::vector<u8> blob = b.save();
    Simulation c(reg);
    c.load(blob);
    CHECK_EQ(c.state_hash(), b.state_hash());
    b.run(450);
    c.run(450);
    CHECK_EQ(a.state_hash(), b.state_hash());
    CHECK_EQ(b.state_hash(), c.state_hash());
}
