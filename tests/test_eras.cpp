// Starting eras (开局时代): a wild band around a campfire, a tribal camp, and how a band
// lives by foraging and learns by doing.
#include "icarus/decision/decisions.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig start(const char* era, u64 seed = 3) {
    GameConfig c;
    c.world = WorldConfig::for_layout(WorldLayout::Continent, seed);
    c.scenario = "village";
    c.era = era;
    return c;
}
Polity& band(Simulation& sim) {
    for (const Polity& p : sim.society().polities())
        if (p.alive) return *sim.society().polity(p.id);
    return *sim.society().polity(1);
}
int alive_in(Simulation& sim, u16 polity) {
    int n = 0;
    for (const auto& c : sim.agents().all())
        if (c && c->alive && !c->departed && c->polity == polity) ++n;
    return n;
}
}  // namespace

TEST("eras: a wild band has only a campfire, leaf wraps and bare hands") {
    Simulation sim(test_registry());
    GameConfig cfg = start("wild");
    cfg.scenario = "wild";
    sim.new_game(cfg);
    sim.run(5);
    const Registry& reg = test_registry();
    const Polity& p = band(sim);
    CHECK(p.has_tech("gathering"));
    CHECK(p.has_tech("fire"));
    CHECK(!p.has_tech("stone_tools"));
    CHECK(!p.has_tech("farming"));
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    CHECK_EQ(seat->def, std::string("campfire"));
    CHECK(seat->functional);  // the clearing around it must not trample the fire
    int homes = 0, clothed = 0, tools = 0, people = 0;
    for (const Building& b : sim.buildings().all())
        if (b.alive && b.beds > 0) ++homes;
    for (const auto& c : sim.agents().all()) {
        if (!c || c->is_girl()) continue;
        ++people;
        if (c->clothes != kNoItem && reg.item(c->clothes).key == "leaf_wrap") ++clothed;
        if (c->tool != kNoItem) ++tools;
    }
    CHECK_EQ(homes, 0);
    CHECK(people > 8);
    CHECK_EQ(clothed, people);
    CHECK_EQ(tools, 0);
    // Everything they own is kept by the fire.
    CHECK(sim.society().public_stores(p.id).size() == 1);
}

TEST("eras: a tribal camp has lean-tos, stone tools and hides") {
    Simulation sim(test_registry());
    sim.new_game(start("tribal"));
    const Polity& p = band(sim);
    CHECK(p.has_tech("stone_tools"));
    CHECK(p.has_tech("hunting"));
    CHECK(!p.has_tech("farming"));
    int lean_tos = 0, beds = 0;
    for (const Building& b : sim.buildings().all())
        if (b.alive && b.def == "lean_to" && b.functional) {
            ++lean_tos;
            beds += b.beds;
        }
    CHECK(lean_tos >= 4);
    CHECK(beds >= 8);
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    CHECK_EQ(seat->def, std::string("campfire"));
    CHECK(sim.economy().store(seat->store)->count(test_registry().item_id("stone_axe")) > 0);
}

TEST("eras: a band feeds itself by gathering, and the hungry pick food themselves") {
    Simulation sim(test_registry());
    GameConfig cfg = start("wild", 5);
    cfg.scenario = "wild";
    sim.new_game(cfg);
    const u16 pid = band(sim).id;
    const int before = alive_in(sim, pid);
    sim.run(kTicksPerDay * 2);
    const Polity* p = sim.society().polity(pid);
    REQUIRE(p != nullptr);
    CHECK_EQ(alive_in(sim, pid), before);
    CHECK(p->stats.food_access > 0.8f);
    CHECK(sim.economy().reasons().count("+forage:berries") || sim.economy().reasons().count("+forage:mushroom"));
    // A band has no larder to ration.
    CHECK(p->policies.ration >= 0.99f);
    CHECK(sim.society().foraging_band(*p));
    // Empty the store: a hungry resident goes and picks something growing wild.
    const Building* seat = sim.buildings().get(p->seat);
    Store* st = sim.economy().store(seat->store);
    std::vector<ItemStack> items = st->items;
    for (const ItemStack& is : items)
        if (test_registry().item(is.item).nutrition > 0) sim.economy().remove(seat->store, is.item, is.count, "test");
    Character* hungry = nullptr;
    for (auto& c : sim.agents().all())
        if (c && c->alive && !c->is_girl() && c->polity == pid) hungry = c.get();
    REQUIRE(hungry != nullptr);
    Store* inv = sim.economy().store(hungry->inv);
    items = inv->items;
    for (const ItemStack& is : items)
        if (test_registry().item(is.item).nutrition > 0) sim.economy().remove(hungry->inv, is.item, is.count, "test");
    hungry->needs.food = 0.2f;
    const i64 picked = sim.economy().reasons().count("+forage:berries") ? sim.economy().reasons().at("+forage:berries") : 0;
    (void)picked;
    float best = hungry->needs.food;
    for (int i = 0; i < 12 && best < 0.5f; ++i) {
        sim.run(kTicksPerHour / 2);
        best = std::max(best, hungry->needs.food);
    }
    CHECK(best >= 0.5f);
}

TEST("eras: everyday work teaches techs, and practice unlocks what research has not") {
    Simulation sim(test_registry());
    GameConfig cfg = start("wild");
    cfg.scenario = "wild";
    sim.new_game(cfg);
    Society& soc = sim.society();
    const u16 pid = band(sim).id;
    const EntityId who = sim.agents().all().back()->id;
    // Felling trees and knapping stones: stone tools.
    for (int i = 0; i < 60 && !soc.polity(pid)->has_tech("stone_tools"); ++i) soc.practice(pid, "chop", 1.0f, who);
    CHECK(soc.polity(pid)->has_tech("stone_tools"));
    bool by_practice = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::TechDiscovered && e.data.str("tech") == "stone_tools")
            by_practice = e.data.boolean("practice", false);
    CHECK(by_practice);
    // Practice only teaches what is within reach: shelter needs stone tools first, and
    // hunting practice does nothing for a tech that nothing lists.
    const float before = [&] {
        for (auto& r : soc.polity(pid)->research)
            if (r.first == "masonry") return r.second;
        return 0.0f;
    }();
    soc.practice(pid, "hunt", 5.0f, who);
    float after = 0.0f;
    for (auto& r : soc.polity(pid)->research)
        if (r.first == "masonry") after = r.second;
    CHECK(after == before);
    // Gathered wild grain teaches farming (it needs stone tools, now known).
    for (int i = 0; i < 80 && !soc.polity(pid)->has_tech("farming"); ++i) soc.practice(pid, "forage_grain", 1.0f, who);
    CHECK(soc.polity(pid)->has_tech("farming"));
}

TEST("eras: the first fields are offered once farming is known and seed is at hand") {
    Simulation sim(test_registry());
    GameConfig cfg = start("wild");
    cfg.scenario = "wild";
    sim.new_game(cfg);
    Society& soc = sim.society();
    Polity* p = &band(sim);
    for (const char* t : {"stone_tools", "farming"}) soc.discover(*p, t, 0, 0);
    const Building* seat = sim.buildings().get(p->seat);
    sim.economy().add(seat->store, test_registry().item_id("grain"), 20, "test");
    // The ruler is nudged toward it; she governs until the option comes up.
    sim.decisions().whisper(p->ruler, "found_farm", 0);
    bool offered = false, chosen = false, founded = false;
    for (int h = 0; h < 24 * 4 && (!offered || (chosen && !founded)); ++h) {
        sim.run(kTicksPerHour);
        for (const Decision& d : sim.decisions().all()) {
            if (d.polity != p->id) continue;
            for (size_t k = 0; k < d.options.size(); ++k)
                if (d.options[k].key == "found_farm" && d.options[k].feasible) {
                    offered = true;
                    if (d.chosen == (int)k && d.status == DecisionStatus::Decided) chosen = true;
                }
        }
        for (const Farm& f : sim.farming().all())
            if (f.alive && f.polity == p->id && !f.plots.empty()) founded = true;
    }
    CHECK(offered);
    if (chosen) CHECK(founded);
}

TEST("eras: a new era is built on the old one — half of its techs first") {
    Simulation sim(test_registry());
    GameConfig cfg = start("wild");
    cfg.scenario = "wild";
    sim.new_game(cfg);
    sim.run(5);
    Society& soc = sim.society();
    Polity& p = band(sim);
    // Only gathering and fire: nothing of the farming era yet, not even pottery.
    CHECK(!soc.tech_available(p, "pottery"));
    CHECK(soc.tech_available(p, "stone_tools"));
    auto [known0, needed0] = soc.era_foundation(p, 1);
    CHECK_EQ(known0, 2);
    CHECK_EQ(needed0, 3);
    p.techs.push_back("hunting");
    CHECK(soc.tech_available(p, "pottery"));
    // Pottery alone does not open writing: the farming era must be half known.
    p.techs.push_back("pottery");
    CHECK(!soc.tech_available(p, "writing"));
    for (const char* k : {"weaving", "stone_tools", "farming", "carpentry"}) p.techs.push_back(k);
    CHECK(!soc.tech_available(p, "writing"));
    p.techs.push_back("herbalism");
    CHECK(soc.tech_available(p, "writing"));
    // Practice cannot stumble on a tech whose era is still closed.
    Polity& q = p;
    q.techs = {"gathering", "fire"};
    for (int i = 0; i < 200; ++i) soc.practice(q.id, "forage_grain", 1.0f, 0);
    CHECK(!q.has_tech("farming"));
    // A village start already stands on the farming era.
    Simulation v(test_registry());
    v.new_game(start("village"));
    v.run(5);
    auto [known2, needed2] = v.society().era_foundation(band(v), 2);
    CHECK(known2 >= needed2);
}

TEST("eras: seed grain kept for sowing does not send the hungry to an empty larder") {
    Simulation sim(test_registry());
    GameConfig cfg = start("wild", 5);
    cfg.scenario = "wild";
    sim.new_game(cfg);
    sim.run(5);
    Polity& p = band(sim);
    p.techs.push_back("stone_tools");
    p.techs.push_back("farming");
    const Building* seat = sim.buildings().get(p.seat);
    std::vector<ItemStack> items = sim.economy().store(seat->store)->items;
    for (const ItemStack& is : items)
        if (test_registry().item(is.item).nutrition > 0) sim.economy().remove(seat->store, is.item, is.count, "test");
    sim.economy().add(seat->store, test_registry().find_item("grain"), 20, "test");
    Character* c = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->alive && !cp->is_girl() && cp->polity == p.id) c = cp.get();
    REQUIRE(c != nullptr);
    // Hungry but not starving: the 20 grain are seed, so there is no meal in the store.
    c->needs.food = 0.3f;
    CHECK(sim.agents().seed_kept(*c));
    CHECK_EQ(sim.agents().find_food_store(*c, true, false), kNoStore);
    // Starving: seed or not, it is eaten.
    c->needs.food = 0.1f;
    CHECK(!sim.agents().seed_kept(*c));
    CHECK_EQ(sim.agents().find_food_store(*c, true, false), seat->store);
    // Enough seed put by: the rest is food again.
    c->needs.food = 0.3f;
    sim.economy().add(seat->store, test_registry().find_item("grain"), 30, "test");
    CHECK(!sim.agents().seed_kept(*c));
    CHECK_EQ(sim.agents().find_food_store(*c, true, false), seat->store);
}
