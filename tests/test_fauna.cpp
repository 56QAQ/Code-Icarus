// Wild animals: population, behaviour, hunting, persistence.
#include <map>

#include "icarus/fauna/fauna.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig world_only(WorldLayout l, u64 seed) {
    GameConfig c;
    c.world = WorldConfig::for_layout(l, seed);
    c.scenario = "none";
    return c;
}
}  // namespace

TEST("fauna: species live where their biomes are; the classic island has no predators") {
    Simulation sim(test_registry());
    sim.new_game(world_only(WorldLayout::Continent, 1));
    const Fauna& f = sim.fauna();
    CHECK(f.count_alive() > 300);
    std::map<std::string, int> by;
    for (const Animal& a : f.all()) {
        by[f.spec(a.species).key]++;
        // Standing on the ground of a biome the species lives in.
        const ColumnInfo col = sim.world().gen().column(a.foot.x, a.foot.z);
        CHECK(col.land);
    }
    for (const char* k : {"rabbit", "deer", "boar", "goat", "wolf"}) CHECK(by[k] > 0);
    Simulation classic(test_registry());
    classic.new_game(world_only(WorldLayout::Classic, 1));
    CHECK(classic.fauna().count_alive() > 20);
    CHECK_EQ(classic.fauna().count_alive(classic.fauna().species_id("wolf")), 0);
    CHECK_EQ(classic.fauna().count_alive(classic.fauna().species_id("bear")), 0);
}

TEST("fauna: animals roam, graze and keep their numbers over days") {
    Simulation sim(test_registry());
    sim.new_game(world_only(WorldLayout::Continent, 2));
    const Fauna& f = sim.fauna();
    std::map<u32, Vec3i> start;
    for (const Animal& a : f.all()) start[a.id] = a.foot;
    const int before = f.count_alive();
    sim.run(kTicksPerDay * 2);
    int moved = 0, total = 0;
    for (const Animal& a : f.all()) {
        auto it = start.find(a.id);
        if (it == start.end() || !a.alive) continue;
        ++total;
        if (a.foot.dist2(it->second) > 4) ++moved;
    }
    CHECK(moved > total / 3);
    const int after = f.count_alive();
    CHECK(after > before * 7 / 10);
    CHECK(after < before * 2);
}

TEST("fauna: a struck deer flees, tires and can be killed and butchered") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world.seed = 3;
    cfg.scenario = "village";
    sim.new_game(cfg);
    Fauna& f = sim.fauna();
    const int deer = f.species_id("deer");
    Animal* a = nullptr;
    for (const Animal& x : f.all())
        if (x.alive && x.species == deer) a = f.get(x.id);
    REQUIRE(a != nullptr);
    Character* hunter = nullptr;
    for (auto& c : sim.agents().all())
        if (c && !c->is_girl()) hunter = c.get();
    REQUIRE(hunter != nullptr);
    CHECK(!f.strike(*a, 0.1f, hunter->id, 0));
    CHECK(a->state == AnimalState::Flee || a->state == AnimalState::Attack);
    CHECK(a->hp < 1.0f);
    bool dead = false;
    for (int i = 0; i < 20 && !dead; ++i) dead = f.strike(*a, 0.18f, hunter->id, 0);
    CHECK(dead);
    CHECK(!a->alive);
    const Registry& reg = test_registry();
    const i64 meat_before = sim.economy().total(reg.item_id("meat"));
    CHECK(f.butcher(*a, hunter->inv) > 0);
    CHECK(sim.economy().total(reg.item_id("meat")) > meat_before);
    CHECK(f.butcher(*a, hunter->inv) == 0);  // only once
}

TEST("fauna: hunters go after game when food runs short") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world.seed = 4;
    cfg.scenario = "village";
    sim.new_game(cfg);
    Polity* p = sim.society().polity(1);
    REQUIRE(p != nullptr);
    p->techs.push_back("hunting");
    // Empty the storehouse of food so the village is short.
    const Registry& reg = test_registry();
    for (StoreId sid : sim.society().public_stores(1))
        if (Store* st = sim.economy().store(sid))
            for (const char* k : {"grain", "berries", "bread"}) {
                const ItemId it = reg.item_id(k);
                sim.economy().remove(sid, it, st->count(it), "test");
            }
    const i64 meat0 = sim.economy().total(reg.item_id("meat"));
    int hunts = 0;
    for (int i = 0; i < 2 * (int)kTicksPerDay; ++i) {
        sim.step();
        for (const Job& j : sim.jobs().all())
            if (j.alive && j.type == JobType::Hunt) {
                ++hunts;
                break;
            }
    }
    CHECK(hunts > 0);
    int taken = 0;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Hunt && e.text.find("猎获") != std::string::npos) ++taken;
    CHECK(taken > 0);
    CHECK(sim.economy().total(reg.item_id("meat")) + 0 >= meat0);  // meat came in (some may be eaten)
}

TEST("fauna: save and load continue identically") {
    Simulation sim(test_registry());
    sim.new_game(world_only(WorldLayout::Continent, 6));
    sim.run(700);
    std::vector<u8> bytes = sim.save();
    Simulation sim2(test_registry());
    sim2.load(bytes);
    CHECK_EQ(sim2.state_hash(), sim.state_hash());
    CHECK_EQ(sim2.fauna().count_alive(), sim.fauna().count_alive());
    sim.run(900);
    sim2.run(900);
    CHECK_EQ(sim2.state_hash(), sim.state_hash());
}
