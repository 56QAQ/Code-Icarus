// The course of a life: ages, partnerships, births, growing up, old age, and magical
// girls awakening among the people.
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
std::vector<Character*> residents(Simulation& sim, u16 polity = 0) {
    std::vector<Character*> out;
    for (auto& c : sim.agents().all())
        if (c && c->alive && !c->departed && !c->is_girl() && (!polity || c->polity == polity)) out.push_back(c.get());
    return out;
}
// Two grown residents of one home made fond of each other.
std::pair<Character*, Character*> sweethearts(Simulation& sim) {
    for (Character* a : residents(sim))
        for (Character* b : residents(sim))
            if (a->id < b->id && a->home && a->home == b->home) {
                a->affinity_ref(b->id) = 0.9f;
                b->affinity_ref(a->id) = 0.9f;
                return {a, b};
            }
    return {nullptr, nullptr};
}
}  // namespace

TEST("life: the first generation arrives grown and ages with the days") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    Agents& ag = sim.agents();
    float lo = 1e9f, hi = -1e9f;
    for (Character* c : residents(sim)) {
        const float a = ag.age_years(*c);
        lo = std::min(lo, a);
        hi = std::max(hi, a);
        CHECK(!ag.is_child(*c));
    }
    CHECK(lo >= 16.0f);
    CHECK(hi <= 44.0f);
    CHECK(hi - lo > 8.0f);  // not all the same age
    for (auto& c : ag.all())
        if (c && c->is_girl()) CHECK(ag.age_years(*c) < 20.0f);
    Character* r = residents(sim).front();
    const float before = ag.age_years(*r);
    sim.run(kTicksPerDay);
    CHECK(std::fabs(ag.age_years(*r) - before - 0.75f) < 0.01f);
}

TEST("life: fond housemates pair off and have a child who plays, then grows up to work") {
    Simulation sim(test_registry());
    sim.new_game(village(4));
    Agents& ag = sim.agents();
    auto [a, b] = sweethearts(sim);
    REQUIRE(a != nullptr);
    const EntityId ia = a->id, ib = b->id;
    ag.daily_life();
    CHECK_EQ(ag.get(ia)->partner, ib);
    CHECK_EQ(ag.get(ib)->partner, ia);
    bool paired = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Life && e.text.find("结为伴侣") != std::string::npos) paired = true;
    CHECK(paired);
    // A well-fed, content household: a child comes sooner or later.
    for (int i = 0; i < 40 && ag.children_of(ia).empty(); ++i) {
        for (EntityId id : {ia, ib}) ag.get(id)->mood = 0.9f;
        ag.daily_life();
    }
    REQUIRE(!ag.children_of(ia).empty());
    Character* kid = ag.get(ag.children_of(ia).front());
    CHECK(ag.is_child(*kid));
    CHECK(kid->parents[0] == ia || kid->parents[1] == ia);
    CHECK_EQ(kid->polity, ag.get(ia)->polity);
    CHECK(ag.children_of(ia).size() == 1);
    // Children play; they do not take jobs.
    for (int h = 0; h < 6; ++h) {
        sim.run(kTicksPerHour);
        CHECK(kid->task.type != TaskType::Work);
    }
    // Grown up, they work like anyone else.
    kid->age0 = 13.9f;
    kid->born = sim.now();
    sim.run(kTicksPerDay);
    CHECK(!ag.is_child(*kid));
    CHECK(!kid->occupation.empty());
    bool worked = false;
    for (int h = 0; h < 36 && !worked; ++h) {
        sim.run(kTicksPerHour / 2);
        worked = kid->task.type == TaskType::Work;
    }
    CHECK(worked);
}

TEST("life: the old die of old age, and their partners mourn") {
    Simulation sim(test_registry());
    sim.new_game(village(5));
    Agents& ag = sim.agents();
    auto [a, b] = sweethearts(sim);
    REQUIRE(a != nullptr);
    ag.daily_life();
    REQUIRE(a->partner == b->id);
    a->age0 = 95.0f;
    a->born = sim.now();
    CHECK(ag.is_elder(*a));
    ag.daily_life();
    CHECK(!a->alive);
    CHECK_EQ(a->death_cause, std::string("年老"));
    CHECK(b->partner != a->id);
    bool mourned = false;
    for (const Memory& m : b->memories)
        if (m.kind == MemoryKind::Bereaved && m.subject == a->id) mourned = true;
    CHECK(mourned);
    const Event* e = sim.chronicle().get(a->death_event);
    REQUIRE(e != nullptr);
    CHECK(e->text.find("安详离世") != std::string::npos);
}

TEST("life: a people left without magical girls sees one awaken, and she takes the lead") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    Agents& ag = sim.agents();
    const u16 pid = residents(sim).front()->polity;
    sim.run(10);
    for (auto& c : ag.all())
        if (c && c->is_girl()) ag.kill(*c, "测试", 0);
    ag.daily_life();
    Character* girl = nullptr;
    for (auto& c : ag.all())
        if (c && c->alive && c->is_girl() && c->polity == pid) girl = c.get();
    REQUIRE(girl != nullptr);
    CHECK(girl->girl->awakened > 0);
    CHECK(!girl->girl->drive.empty());
    CHECK_EQ(girl->girl->level, 1);
    bool told = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Awakening && e.actor == girl->id) told = true;
    CHECK(told);
    sim.run(kTicksPerHour * 3);
    CHECK_EQ(sim.society().polity(pid)->ruler, girl->id);
}

TEST("life: families and ages survive save and load, and the run continues identically") {
    Simulation sim(test_registry());
    sim.new_game(village(7));
    Agents& ag = sim.agents();
    auto [a, b] = sweethearts(sim);
    REQUIRE(a != nullptr);
    ag.daily_life();
    for (int i = 0; i < 40 && ag.children_of(a->id).empty(); ++i) ag.daily_life();
    REQUIRE(!ag.children_of(a->id).empty());
    sim.run(500);
    const auto blob = sim.save();
    Simulation other(test_registry());
    other.load(blob);
    const Character* kid2 = other.agents().all().back().get();
    const Character* kid = ag.all().back().get();
    CHECK_EQ(kid2->parents[0], kid->parents[0]);
    CHECK_EQ(kid2->parents[1], kid->parents[1]);
    CHECK(std::fabs(other.agents().age_years(*kid2) - ag.age_years(*kid)) < 1e-3f);  // within a tick
    CHECK_EQ(other.agents().get(a->id)->partner, b->id);
    sim.run(kTicksPerDay + 300);
    other.run(kTicksPerDay + 300);
    CHECK_EQ(sim.state_hash(), other.state_hash());
}
