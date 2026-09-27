// Three realms on the continent: strategy, long marches, peace terms, alliances.
#include "icarus/decision/decisions.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig realms(u64 seed = 3) {
    GameConfig c;
    c.world = WorldConfig::for_layout(WorldLayout::Continent, seed);
    c.scenario = "three_realms";
    return c;
}
std::vector<u16> alive_polities(Simulation& sim) {
    std::vector<u16> out;
    for (const Polity& p : sim.society().polities())
        if (p.alive) out.push_back(p.id);
    return out;
}
}  // namespace

TEST("realms: three civilisations start apart, hostile, and busy building up") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    const auto ids = alive_polities(sim);
    REQUIRE(ids.size() == 3);
    Society& soc = sim.society();
    for (u16 a : ids)
        for (u16 b : ids) {
            if (a == b) continue;
            CHECK(soc.polity(a)->attitude_to(b) < -0.2f);
            const Society::Assessment as = soc.assess(a, b);
            CHECK(!as.settled);  // the first days belong to building up
            CHECK(as.ours > 5.0f);
        }
    // Nobody may go to war in the first days.
    sim.run(kTicksPerDay * 2);
    for (u16 a : ids) CHECK(soc.polity(a)->wars.empty());
    // A wrathful ruler is done building up sooner than a hopeful one.
    Character* r = sim.agents().get(soc.polity(ids[0])->ruler);
    REQUIRE(r && r->girl);
    r->girl->drive = "wrath";
    const float wrath = soc.settle_days(ids[0]);
    r->girl->drive = "hope";
    const float hope = soc.settle_days(ids[0]);
    CHECK(wrath >= 10.0f);
    CHECK(hope > wrath + 5.0f);
}

TEST("realms: an army marches across the continent and fights at the enemy's hall") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    Society& soc = sim.society();
    const auto ids = alive_polities(sim);
    REQUIRE(ids.size() == 3);
    const u16 a = ids[0], b = ids[1];
    // The rulers hold their decisions (waiting for a remote answer that never comes): only
    // the march and the fighting are tested here.
    sim.decisions().mode = "remote";
    sim.decisions().remote_budget_per_day = 1 << 20;
    sim.decisions().remote_deadline = kTicksPerDay * 100;
    const EventId ev = soc.declare_war(a, b, "conquest", 0, 0);
    REQUIRE(ev != 0);
    soc.draft(a, 8, ev);
    soc.polity(a)->op.party = soc.soldiers(a);
    bool arrived = false, fought = false;
    for (int h = 0; h < 36 && !(arrived && fought); ++h) {
        sim.run(kTicksPerHour);
        const Polity* pa = soc.polity(a);
        if (!pa) break;
        if (pa->op.active && pa->op.phase == 2) arrived = true;
        for (const Event& e : sim.chronicle().events())
            if (e.type == EventType::Battle && e.polity == a && e.text.find("交战") != std::string::npos) fought = true;
        if (!soc.polity(b)) arrived = fought = true;  // conquered outright
    }
    CHECK(arrived);
    CHECK(fought);
}

TEST("realms: a truce follows peace, and allies are called to a war") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    Society& soc = sim.society();
    const auto ids = alive_polities(sim);
    const u16 a = ids[0], b = ids[1], c = ids[2];
    soc.make_alliance(b, c, 0);
    CHECK(soc.polity(b)->allied_with(c));
    CHECK(soc.polity(c)->allied_with(b));
    const EventId ev = soc.declare_war(a, b, "raid", 0, 0);
    REQUIRE(ev != 0);
    sim.decisions().call_allies(b, a, ev);
    bool asked = false;
    for (const Decision& d : sim.decisions().all())
        if (d.kind == "ally_call" && d.polity == c) asked = true;
    CHECK(asked);
    soc.make_peace(a, b, "议和停战", 0);
    CHECK(!soc.at_war(a, b));
    const Society::Assessment as = soc.assess(a, b);
    CHECK(as.truce);
    // Tribute travels as real food on carriers.
    const i32 sent = soc.send_tribute(b, a, 12, "赔粮", 0);
    CHECK(sent > 0);
    int jobs = 0;
    for (const Job& j : sim.jobs().all())
        if (j.alive && j.type == JobType::Trade && j.plot == 2 && j.polity == b) ++jobs;
    CHECK(jobs > 0);
    soc.make_vassal(b, a, 0);
    CHECK_EQ(soc.polity(b)->overlord, a);
}
