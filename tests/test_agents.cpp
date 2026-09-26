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

namespace {
GameConfig med_village(u64 seed) {
    GameConfig c;
    c.world.seed = seed;
    c.scenario = "village";
    return c;
}
Character* wound_someone(Simulation& sim, float frac) {
    for (auto& cp : sim.agents().all())
        if (cp && cp->alive && !cp->is_girl()) {
            sim.agents().damage(*cp, frac, kTorso, "测试", 0);
            if (cp->alive) {
                cp->body.bleeding = 0.4f;
                return cp.get();
            }
        }
    return nullptr;
}
}  // namespace

TEST("medicine: the wounded fetch herbs and get their wounds dressed; it heals them faster") {
    auto run = [](bool herbalism, int& missing_after, bool& treated, i64& herbs_used) {
        Simulation sim(test_registry());
        sim.new_game(med_village(6));
        sim.run(kTicksPerHour);
        Polity* p = sim.society().polity(1);
        const ItemId herbs = sim.reg().item_id("herbs");
        if (herbalism) sim.society().discover(*p, "herbalism", kNoEntity, 0);
        StoreId store = sim.society().public_stores(1).front();
        sim.economy().add(store, herbs, 6, "test");
        const i64 consumed = sim.economy().ledger(herbs).consumed;
        Character* c = wound_someone(sim, 0.12f);
        REQUIRE(c != nullptr);
        const EntityId id = c->id;
        sim.run(kTicksPerHour * 12);
        const Character* w = sim.agents().get(id);
        REQUIRE(w != nullptr);
        missing_after = w->body.total_voxels() - w->body.total_alive();
        treated = w->treated_until > 0;
        herbs_used = sim.economy().ledger(herbs).consumed - consumed;
    };
    int miss_a = 0, miss_b = 0;
    bool treated_a = false, treated_b = false;
    i64 used_a = 0, used_b = 0;
    run(true, miss_a, treated_a, used_a);
    run(false, miss_b, treated_b, used_b);
    CHECK(treated_a);
    CHECK(used_a >= 1);   // a real bundle of herbs was used up
    CHECK(!treated_b);
    CHECK_EQ(used_b, (i64)0);
    CHECK(miss_a < miss_b);  // dressed wounds close faster
}

TEST("carts: a resident with a cart can carry more, and keeps it when unloading") {
    Simulation sim(test_registry());
    sim.new_game(med_village(6));
    sim.run(kTicksPerHour);
    Character* c = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->alive && !cp->is_girl()) c = cp.get();
    REQUIRE(c != nullptr);
    const float base = sim.agents().carry_capacity(*c);
    const ItemId cart = sim.reg().item_id("cart");
    StoreId store = sim.society().public_stores(1).front();
    sim.economy().add(store, cart, 1, "test");
    REQUIRE(sim.economy().transfer(store, c->inv, cart, 1) == 1);
    c->cart = cart;
    CHECK(sim.agents().carry_capacity(*c) > base + 10.0f);
    // A few hours of ordinary work, unloading included: the cart stays with them.
    sim.run(kTicksPerHour * 6);
    CHECK_EQ(c->cart, cart);
    CHECK_EQ(sim.economy().store(c->inv)->count(cart), 1);
    CHECK(sim.economy().store(c->inv)->capacity > base + 10.0f);
    // It survives a save and load.
    std::vector<u8> bytes = sim.save();
    Simulation loaded(test_registry());
    loaded.load(bytes);
    const Character* lc = loaded.agents().get(c->id);
    REQUIRE(lc != nullptr);
    CHECK_EQ(lc->cart, cart);
}

TEST("governance: a polity that knows how to build something it lacks is offered to build it") {
    Simulation sim(test_registry());
    sim.new_game(med_village(2));
    sim.run(kTicksPerHour);
    sim.society().discover(*sim.society().polity(1), "carpentry", kNoEntity, 0);
    bool offered = false;
    for (int h = 0; h < 60 && !offered; ++h) {
        sim.run(kTicksPerHour);
        for (const Decision& d : sim.decisions().all())
            for (const DecisionOption& o : d.options)
                if (o.key == "build_workshop" || o.key == "build_longhouse") offered = true;
    }
    CHECK(offered);
}
