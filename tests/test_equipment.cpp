// Equipment: typed tools, fetching and swapping them, wear, clothes.
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
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
Character* first_resident(Simulation& sim) {
    for (auto& c : sim.agents().all())
        if (c && !c->is_girl()) return c.get();
    return nullptr;
}
}  // namespace

TEST("equipment: village folk carry the tool of their trade and wear linen") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    const Registry& reg = test_registry();
    int tooled = 0, dressed = 0, residents = 0;
    for (auto& c : sim.agents().all()) {
        if (!c || c->is_girl()) continue;
        ++residents;
        if (c->tool != kNoItem && reg.item(c->tool).tool_kind == Agents::occupation_tool(c->occupation)) ++tooled;
        if (c->clothes != kNoItem && reg.item(c->clothes).key == "linen_clothes") ++dressed;
        // The gear really is in their hands (the ledger holds it).
        if (c->tool != kNoItem) CHECK(sim.economy().store(c->inv)->count(c->tool) == 1);
    }
    CHECK_EQ(tooled, residents);
    CHECK_EQ(dressed, residents);
}

TEST("equipment: the right tool sets the pace; bare hands are slow") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    const Registry& reg = test_registry();
    Character* c = first_resident(sim);
    REQUIRE(c != nullptr);
    Economy& econ = sim.economy();
    auto hold = [&](const char* key) {
        if (c->tool != kNoItem) econ.remove(c->inv, c->tool, 1, "test");
        c->tool = kNoItem;
        if (key) {
            const ItemId it = reg.item_id(key);
            econ.add(c->inv, it, 1, "test");
            c->tool = it;
        }
    };
    const Agents& ag = sim.agents();
    hold(nullptr);
    const float bare = ag.tool_factor(*c, "axe");
    hold("stone_axe");
    const float stone = ag.tool_factor(*c, "axe");
    hold("iron_axe");
    const float iron = ag.tool_factor(*c, "axe");
    CHECK(bare < 0.5f);
    CHECK(stone > bare * 2.0f);
    CHECK(iron > stone);
    // An axe is no help at quarrying; an all-round kit helps with anything.
    CHECK(ag.tool_factor(*c, "pick") < 0.5f);
    hold("stone_tools");
    CHECK(ag.tool_factor(*c, "pick") > 0.9f);
    CHECK(ag.tool_factor(*c, "") == 1.0f);
}

TEST("equipment: workers take the right tool from a store, hand back theirs, and wear tools out") {
    Simulation sim(test_registry());
    sim.new_game(village(4));
    const Registry& reg = test_registry();
    Economy& econ = sim.economy();
    Agents& ag = sim.agents();
    Character* c = first_resident(sim);
    REQUIRE(c != nullptr);
    // Holding a hoe; the storehouse has picks.
    if (c->tool != kNoItem) econ.remove(c->inv, c->tool, 1, "test");
    const ItemId hoe = reg.item_id("stone_hoe"), pick = reg.item_id("stone_pick");
    econ.add(c->inv, hoe, 1, "test");
    c->tool = hoe;
    StoreId sid = ag.tool_store_for(*c, "pick", c->foot);
    REQUIRE(sid != kNoStore);
    const i32 picks_before = econ.store(sid)->count(pick), hoes_before = econ.store(sid)->count(hoe);
    CHECK(ag.swap_tool(*c, sid, "pick"));
    CHECK(c->tool == pick);
    CHECK_EQ(econ.store(sid)->count(pick), picks_before - 1);
    CHECK_EQ(econ.store(sid)->count(hoe), hoes_before + 1);
    // Nothing to fetch when the right tool is already in hand.
    CHECK(ag.tool_store_for(*c, "pick", c->foot) == kNoStore);
    // Used up after its durability.
    const std::string worn = "-worn_out:stone_pick";
    const i64 worn_before = econ.reasons().count(worn) ? econ.reasons().at(worn) : 0;
    for (int i = 0; i < reg.item(pick).durability; ++i) ag.wear_tool(*c);
    CHECK(c->tool == kNoItem);
    CHECK_EQ(econ.store(c->inv)->count(pick), 0);
    CHECK(econ.reasons().at(worn) > worn_before);
}

TEST("equipment: clothes keep out the cold of the night") {
    Simulation sim(test_registry());
    sim.new_game(village(5));
    const Registry& reg = test_registry();
    Character* c = first_resident(sim);
    REQUIRE(c != nullptr);
    // Outdoors at night, away from home.
    c->home = 0;
    while (!is_night(sim.now())) sim.step();
    sim.run(10);
    Economy& econ = sim.economy();
    auto wear = [&](const char* key) {
        if (c->clothes != kNoItem) econ.remove(c->inv, c->clothes, 1, "test");
        c->clothes = kNoItem;
        if (key) {
            c->clothes = reg.item_id(key);
            econ.add(c->inv, c->clothes, 1, "test");
        }
    };
    wear(nullptr);
    const float naked = sim.agents().exposure(*c);
    wear("leaf_wrap");
    const float leaves = sim.agents().exposure(*c);
    wear("fur_cloak");
    const float fur = sim.agents().exposure(*c);
    CHECK(naked > 0.1f);
    CHECK(leaves < naked);
    CHECK(fur <= leaves);
    CHECK(fur == 0.0f);
}

TEST("equipment: clothes and tool wear survive save and load") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    Character* c = first_resident(sim);
    REQUIRE(c != nullptr);
    c->tool_wear = 17;
    const EntityId id = c->id;
    const ItemId clothes = c->clothes;
    sim.run(300);
    const u64 h = sim.state_hash();
    std::vector<u8> bytes = sim.save();
    Simulation sim2(test_registry());
    sim2.load(bytes);
    CHECK_EQ(sim2.state_hash(), h);
    const Character* c2 = sim2.agents().get(id);
    CHECK(c2->clothes == clothes);
    CHECK_EQ((int)c2->tool_wear, (int)sim.agents().get(id)->tool_wear);
    sim.run(600);
    sim2.run(600);
    CHECK_EQ(sim2.state_hash(), sim.state_hash());
}
