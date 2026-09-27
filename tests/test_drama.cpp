// The drama among magical girls: friends, mentors, nemeses; grief and kindness turning
// a drive; champions marching with the army and duelling.
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
GameConfig realms(u64 seed = 3) {
    GameConfig c;
    c.world = WorldConfig::for_layout(WorldLayout::Continent, seed);
    c.scenario = "three_realms";
    return c;
}
std::vector<Character*> girls_of(Simulation& sim, u16 polity) {
    std::vector<Character*> out;
    for (auto& c : sim.agents().all())
        if (c && c->alive && c->is_girl() && c->polity == polity) out.push_back(c.get());
    return out;
}
bool told(Simulation& sim, EventType type, const std::string& needle) {
    for (const Event& e : sim.chronicle().events())
        if (e.type == type && e.text.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST("drama: girls who get along become friends; losing a friend can turn a drive dark") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    Agents& ag = sim.agents();
    const u16 pid = girls_of(sim, 1).front()->polity;
    auto gs = girls_of(sim, pid);
    REQUIRE(gs.size() >= 2);
    Character& a = *gs[0];
    Character& b = *gs[1];
    a.girl->drive = "hope";
    a.affinity_ref(b.id) = 0.9f;
    b.affinity_ref(a.id) = 0.9f;
    ag.daily_drama();
    REQUIRE(a.girl->bond_with(b.id) != nullptr);
    CHECK(a.girl->bond_with(b.id)->kind == BondKind::Friend);
    CHECK(b.girl->bond_with(a.id)->kind == BondKind::Friend);
    CHECK(told(sim, EventType::Bond, "挚友"));
    // Her friend dies: it weighs on her.
    const float before = a.girl->trauma;
    ag.kill(b, "测试", 0);
    CHECK(a.girl->trauma > before + 0.5f);
    CHECK(std::find(a.girl->marks.begin(), a.girl->marks.end(), b.death_event) != a.girl->marks.end());
    CHECK(a.girl->bond_with(b.id) != nullptr);  // a friend lost is still part of her story
    // More grief than she can bear: sooner or later, hope turns to despair.
    const u32 accent = a.look.accent;
    ag.mark_girl(a.id, 1.0f, 0.0f, 0);
    for (int d = 0; d < 20 && a.girl->drive == "hope"; ++d) {
        ag.mark_girl(a.id, 0.1f, 0.0f, 0);
        ag.daily_drama();
    }
    CHECK_EQ(a.girl->drive, std::string("despair"));
    CHECK_EQ(a.girl->born_drive, std::string("hope"));
    CHECK(a.look.accent != accent);
    const Event* turned = nullptr;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::DriveChanged && e.actor == a.id) turned = &e;
    REQUIRE(turned != nullptr);
    CHECK(turned->text.find("绝望") != std::string::npos);
    bool from_death = false;
    for (EventId c : turned->causes) from_death |= c == b.death_event;
    CHECK(from_death);  // the chronicle says why
    // Kindness and triumph can bring her back.
    for (int d = 0; d < 30 && a.girl->drive == "despair"; ++d) {
        ag.mark_girl(a.id, 0.0f, 0.4f, 0);
        ag.daily_drama();
    }
    CHECK_EQ(a.girl->drive, std::string("hope"));
}

TEST("drama: a girl newly awakened is taken in hand by the most experienced one") {
    Simulation sim(test_registry());
    sim.new_game(village(4));
    auto gs = girls_of(sim, girls_of(sim, 1).front()->polity);
    REQUIRE(gs.size() >= 3);
    gs[0]->girl->level = 5;
    gs[1]->girl->level = 2;
    gs[2]->girl->level = 1;
    sim.agents().take_student(*gs[2], 0);
    REQUIRE(gs[2]->girl->bond_with(gs[0]->id) != nullptr);
    CHECK(gs[2]->girl->bond_with(gs[0]->id)->kind == BondKind::Student);
    CHECK(gs[0]->girl->bond_with(gs[2]->id)->kind == BondKind::Mentor);
    CHECK(told(sim, EventType::Bond, "收"));
}

TEST("drama: an enemy girl who fells a friend becomes a nemesis; bonds survive save and load") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    Society& soc = sim.society();
    std::vector<u16> ids;
    for (const Polity& p : soc.polities())
        if (p.alive) ids.push_back(p.id);
    REQUIRE(ids.size() == 3);
    auto ours = girls_of(sim, ids[0]);
    auto theirs = girls_of(sim, ids[1]);
    REQUIRE(ours.size() >= 2);
    REQUIRE(!theirs.empty());
    Character& friend_a = *ours[0];
    Character& friend_b = *ours[1];
    Character& foe = *theirs[0];
    sim.agents().set_bond(friend_a, friend_b, BondKind::Friend, BondKind::Friend, 0);
    REQUIRE(soc.declare_war(ids[1], ids[0], "raid", 0, 0) != 0);
    sim.agents().begin_duel(foe, friend_b, 0);
    CHECK(told(sim, EventType::Battle, "对决"));
    for (int i = 0; i < 40 && friend_b.alive; ++i) sim.agents().strike(foe, friend_b, 0.5f, "测试", 0);
    REQUIRE(!friend_b.alive);
    CHECK(told(sim, EventType::Battle, "击倒"));
    REQUIRE(friend_a.girl->bond_with(foe.id) != nullptr);
    CHECK(friend_a.girl->bond_with(foe.id)->kind == BondKind::Nemesis);
    CHECK(friend_a.girl->trauma > 0.5f);
    const auto blob = sim.save();
    Simulation other(test_registry());
    other.load(blob);
    const Character* a2 = other.agents().get(friend_a.id);
    REQUIRE(a2 != nullptr && a2->girl != nullptr);
    REQUIRE(a2->girl->bond_with(foe.id) != nullptr);
    CHECK(a2->girl->bond_with(foe.id)->kind == BondKind::Nemesis);
    CHECK(std::fabs(a2->girl->trauma - friend_a.girl->trauma) < 1e-6f);
    sim.run(kTicksPerDay);
    other.run(kTicksPerDay);
    CHECK_EQ(sim.state_hash(), other.state_hash());
}

TEST("war: magical girls with battle magic march with the army") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    Society& soc = sim.society();
    std::vector<u16> ids;
    for (const Polity& p : soc.polities())
        if (p.alive) ids.push_back(p.id);
    auto ours = girls_of(sim, ids[0]);
    REQUIRE(ours.size() >= 2);
    Character* champion = nullptr;
    for (Character* g : ours)
        if (g->id != soc.polity(ids[0])->ruler) champion = g;
    REQUIRE(champion != nullptr);
    champion->girl->drive = "courage";
    champion->girl->level = 4;
    const EventId ev = soc.declare_war(ids[0], ids[1], "raid", 0, 0);
    REQUIRE(ev != 0);
    soc.draft(ids[0], 4, ev);
    soc.start_operation(ids[0], ids[1], "raid", ev);
    CHECK(champion->drafted);
    CHECK(told(sim, EventType::Battle, "随军出征"));
    // Peace sends everyone home.
    soc.make_peace(ids[0], ids[1], "议和", 0);
    CHECK(!champion->drafted);
}
