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
    // To just past midnight, when the day's index is taken (and nothing learnt since).
    sim.run(kTicksPerDay - sim.now() % kTicksPerDay + 1);
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

TEST("steward: fields that have filled the ground their water reaches are not 'expanded' again and again") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world.seed = 1;
    cfg.scenario = "village";
    sim.new_game(cfg);
    sim.run(kTicksPerHour);
    u32 fid = 0;
    for (const Farm& f : sim.farming().all())
        if (f.alive && f.polity == 1) fid = f.id;
    REQUIRE(fid != 0);
    // What `expansion` shows is what `expand` lays out, and looking changes nothing.
    const size_t before = sim.farming().get(fid)->plots.size();
    const std::vector<Vec3i> room = sim.farming().expansion(fid, 6);
    CHECK_EQ(sim.farming().get(fid)->plots.size(), before);
    CHECK_EQ(sim.farming().expand(fid, 6), (int)room.size());
    // Sow every patch of ground the water reaches, then a long stretch of rule.
    while (sim.farming().expand(fid, 64) > 0) {}
    CHECK(sim.farming().expansion(fid, 1).empty());
    const size_t seen = sim.decisions().all().size();
    sim.run(kTicksPerDay * 3);
    int offered = 0;
    for (size_t i = seen; i < sim.decisions().all().size(); ++i) {
        const Decision& d = sim.decisions().all()[i];
        if (d.polity != 1) continue;
        for (const DecisionOption& o : d.options)
            if (o.key == "expand_farms") {
                ++offered;
                CHECK(!o.feasible);
            }
        if (d.chosen >= 0) CHECK(d.options[(size_t)d.chosen].key != "expand_farms");
    }
    CHECK(offered > 0);
}

TEST("steward: emergency measures lapse after their days; building and full portions resume") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world.seed = 1;
    cfg.scenario = "village";
    sim.new_game(cfg);
    sim.run(kTicksPerHour);
    Polity* p = sim.society().polity(1);
    REQUIRE(p != nullptr);
    // "Everyone to the food": what a famine decision orders.
    p->policies.pri_food = 1.8f;
    p->policies.pri_build = 0.5f;
    p->policies.pri_gather = 0.5f;
    p->policies.ration = 0.7f;
    p->emergency_until = sim.now() + kTicksPerHour * 2;
    sim.run(kTicksPerHour);
    CHECK(p->policies.pri_build < 1.0f);  // still in force
    sim.run(kTicksPerHour * 2);
    CHECK(p->policies.pri_food <= 1.0f);
    CHECK(p->policies.pri_build == 1.0f);
    CHECK(p->policies.ration >= 1.0f);
    CHECK_EQ(p->emergency_until, (Tick)0);
    bool told = false;
    for (const Event& e : sim.chronicle().events())
        if (e.polity == 1 && e.text.find("应急措施到期") != std::string::npos) told = true;
    CHECK(told);
}

TEST("steward: a war leaves a people tired of fighting for a while, and the steward says so") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world = WorldConfig::for_layout(WorldLayout::Random, 3);
    cfg.world.sized();
    cfg.era = "wild";
    cfg.civs = 2;
    cfg.scenario = "wild";
    sim.new_game(cfg);
    sim.run(kTicksPerHour);
    Polity* a = sim.society().polity(1);
    Polity* b = sim.society().polity(2);
    REQUIRE(a != nullptr && b != nullptr);
    CHECK(a->weary == 0.0f);
    sim.society().declare_war(1, 2, "raid", a->ruler, 0);
    sim.run(kTicksPerDay * 2);
    sim.society().make_peace(1, 2, "议和停战", 0);
    CHECK(a->weary > 0.25f);
    CHECK(b->weary > 0.25f);
    const float w0 = a->weary;
    sim.run(kTicksPerHour * 3);  // (the plan is drawn up every two hours)
    CHECK(a->plan.backing("avoid_war") > 0.0f);
    sim.run(kTicksPerDay * 3);
    CHECK(a->weary < w0);
}

TEST("steward: rewards by work never starve children, the old, soldiers or the wounded") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world.seed = 1;
    cfg.scenario = "village";
    sim.new_game(cfg);
    sim.run(kTicksPerHour);
    Polity* p = sim.society().polity(1);
    REQUIRE(p != nullptr);
    p->policies.distribution = 1;  // rewards by work
    p->stats.food_days = 1.0f;     // (scarcity: shirkers go without)
    Character* idler = nullptr;
    Character* soldier = nullptr;
    for (auto& cp : sim.agents().all()) {
        if (!cp || !cp->alive || cp->is_girl() || cp->polity != 1 || sim.agents().is_child(*cp)) continue;
        if (!idler) idler = cp.get();
        else if (!soldier) soldier = cp.get();
    }
    REQUIRE(idler != nullptr && soldier != nullptr);
    idler->work_debt = 6.0f;
    soldier->work_debt = 6.0f;
    soldier->drafted = true;
    CHECK(sim.agents().find_food_store(*idler, true, false) == kNoStore);  // a shirker goes without
    CHECK(sim.agents().find_food_store(*soldier, true, false) != kNoStore);
    // A child playing all day owes no work in the first place.
    Character* child = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->alive && !cp->is_girl() && cp->polity == 1 && cp.get() != idler && cp.get() != soldier) child = cp.get();
    REQUIRE(child != nullptr);
    child->age0 = 5.0f;
    child->born = sim.now();
    child->work_debt = 0.0f;
    sim.run(kTicksPerHour * 4);
    CHECK(child->work_debt == 0.0f);
}
