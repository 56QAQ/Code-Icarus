// Civilisation outcomes: the island's forest, and unification ending a round.
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "icarus/sim/simulation.h"
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

TEST("outcomes: the forest tally counts generated trees and notices felled ones") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    auto t0 = std::chrono::steady_clock::now();
    const ForestStats f0 = sim.world().forest();
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  forest: %d trees, first tally %.1f ms\n", f0.initial, ms);
    CHECK(f0.initial > 100);
    CHECK_EQ(f0.standing, f0.initial);
    // Every enumerated tree really is a trunk in the generated world.
    std::vector<Vec3i> trees = sim.world().gen().tree_bases();
    const Vec3i v = sim.world().gen().features().village;
    std::sort(trees.begin(), trees.end(), [&](const Vec3i& a, const Vec3i& b) { return a.dist2(v) < b.dist2(v); });
    for (size_t i = 0; i < 20 && i < trees.size(); ++i)
        CHECK(vmat(sim.world().get(trees[i])) == sim.reg().m().log);
    // Fell three of them.
    for (int i = 0; i < 3; ++i) {
        AdminCommand cmd;
        cmd.type = "dig";
        cmd.params = Json::object();
        Json pos = Json::array();
        pos.push(trees[(size_t)i].x);
        pos.push(trees[(size_t)i].y);
        pos.push(trees[(size_t)i].z);
        cmd.params.set("pos", pos);
        cmd.params.set("radius", 0.5);
        sim.apply_admin(cmd);
    }
    t0 = std::chrono::steady_clock::now();
    const ForestStats f1 = sim.world().forest();
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  forest: tally after edits %.1f ms\n", ms);
    CHECK_EQ(f1.initial, f0.initial);
    CHECK_EQ(f1.standing, f0.initial - 3);
}

TEST("outcomes: one polity holding the island after several ends the round, once") {
    Simulation sim(test_registry());
    sim.new_game(village(2));
    sim.run(kTicksPerHour);
    // A splinter with no magical girl cannot hold together and rejoins its parent.
    const u16 nid = sim.society().create_polity("残部", 0x8899AA, 1);
    int moved = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && moved < 3) {
            cp->polity = nid;
            ++moved;
        }
    sim.run(kTicksPerHour * 8);
    int alive = 0;
    for (const Polity& p : sim.society().polities())
        if (p.alive) ++alive;
    CHECK_EQ(alive, 1);
    const EventId u = sim.society().unification_event();
    REQUIRE(u != 0);
    const Event* e = sim.chronicle().get(u);
    REQUIRE(e != nullptr);
    CHECK(e->type == EventType::Unification);
    CHECK(e->causes[0] != 0);  // the merger that did it
    // Only once, and it survives a save.
    sim.run(kTicksPerHour * 3);
    int count = 0;
    for (const Event& x : sim.chronicle().events())
        if (x.type == EventType::Unification) ++count;
    CHECK_EQ(count, 1);
    std::vector<u8> bytes = sim.save();
    Simulation loaded(test_registry());
    loaded.load(bytes);
    CHECK_EQ(loaded.society().unification_event(), u);
}

TEST("war: after an operation ends, rulers can launch new ones; launching drafts and marches") {
    Simulation sim(test_registry());
    sim.new_game(village(4));
    sim.run(kTicksPerHour);
    // A rival polity led by one of the girls, holding half the residents.
    Polity* home = sim.society().polity(1);
    REQUIRE(home != nullptr);
    Character* rival = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->is_girl() && cp->id != home->ruler) rival = cp.get();
    REQUIRE(rival != nullptr);
    const u16 nid = sim.society().create_polity("对岸", 0x7799CC, 1);
    rival->polity = nid;
    sim.society().set_ruler(nid, rival->id, "secession", 0);
    int n = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && (n++ % 2 == 1)) cp->polity = nid;
    const EventId war = sim.society().declare_war(1, nid, "raid", home->ruler, 0);
    REQUIRE(war != 0);
    sim.society().draft(1, 3, war);
    CHECK(sim.society().soldiers(1) >= 3);
    // Run until the first operation is over.
    for (int h = 0; h < 48 && sim.society().polity(1) && sim.society().polity(1)->op.active; ++h) sim.run(kTicksPerHour);
    REQUIRE(sim.society().polity(1) != nullptr);
    CHECK(!sim.society().polity(1)->op.active);
    // How the raid went is on record, traced to the declaration.
    bool reported = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Battle && e.data.has("loot") && e.causes[0] == war) reported = true;
    CHECK(reported);
    // The next war decision offers to fight on, not only to wait.
    const Decision* next = nullptr;
    for (int h = 0; h < 30 && !next; ++h) {
        sim.run(kTicksPerHour);
        for (const Decision& d : sim.decisions().all())
            if (d.id && d.kind == "war" && d.polity == 1 && d.created > sim.now() - kTicksPerHour * 2) next = &d;
    }
    REQUIRE(next != nullptr);
    bool raid = false, hold = false, peace = false;
    for (const DecisionOption& o : next->options) {
        raid |= o.key == "raid_again";
        hold |= o.key == "hold";
        peace |= o.key == "offer_peace";
    }
    CHECK(raid);
    CHECK(hold);
    CHECK(peace);
    // Launching drafts soldiers and starts an operation.
    if (sim.society().polity(1) && !sim.society().polity(1)->op.active && sim.society().at_war(1, nid)) {
        sim.society().draft(1, 3, 0);
        sim.society().start_operation(1, nid, "raid", 0);
        CHECK(sim.society().polity(1)->op.active);
        CHECK(sim.society().soldiers(1) >= 3);
    }
}

TEST("migration: hungry, resentful residents walk over to a neighbour that feeds its people") {
    const Registry& reg = test_registry();
    Simulation sim(reg);
    sim.new_game(village(5));
    sim.run(kTicksPerHour);
    Polity* home = sim.society().polity(1);
    REQUIRE(home != nullptr);
    Character* rival = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->is_girl() && cp->id != home->ruler) rival = cp.get();
    REQUIRE(rival != nullptr);
    const u16 nid = sim.society().create_polity("丰饶", 0x88AA66, 1);
    rival->polity = nid;
    sim.society().set_ruler(nid, rival->id, "secession", 0);
    // The rival's seat: the old hall serves both for this test; its granary is full.
    sim.society().polity(nid)->seat = home->seat;
    if (const Building* hall = sim.buildings().get(home->seat)) {
        const StoreId granary = sim.economy().create_store(StoreKind::Stockpile, hall->entrance + Vec3i{2, 0, 0}, nid, kNoEntity, 2000.0f);
        sim.economy().add(granary, reg.find_item("grain"), 300, "test");
    }
    int n = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && (n++ % 3 == 0)) cp->polity = nid;
    // At home: hungry and resentful of the ruler.
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && cp->polity == 1) {
            cp->needs.food = 0.15f;
            cp->support_ref(home->ruler) = -0.6f;
            cp->support_ref(rival->id) = 0.4f;
        }
    const int before = sim.agents().count_alive(1);
    int moved = 0;
    for (int h = 0; h < 24 && moved == 0; ++h) {
        sim.run(kTicksPerHour);
        moved = 0;
        for (const Event& e : sim.chronicle().events())
            if (e.type == EventType::Migration) ++moved;
    }
    CHECK(moved > 0);
    CHECK(sim.agents().count_alive(1) < before);
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Migration) {
            const Character* m = sim.agents().get(e.actor);
            REQUIRE(m != nullptr);
            CHECK_EQ(m->polity, nid);
            bool remembers = false;
            for (const Memory& mem : m->memories) remembers |= mem.kind == MemoryKind::Migrated;
            CHECK(remembers);
        }
    // Nothing is created or lost by moving.
    for (size_t i = 0; i < reg.item_count(); ++i) {
        const LedgerLine& l = sim.economy().ledger((ItemId)i);
        CHECK_EQ(sim.economy().total((ItemId)i), l.produced - l.consumed);
    }
}

TEST("migration: contented residents stay") {
    Simulation sim(test_registry());
    sim.new_game(village(5));
    sim.run(kTicksPerHour);
    Polity* home = sim.society().polity(1);
    Character* rival = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->is_girl() && cp->id != home->ruler) rival = cp.get();
    REQUIRE(rival != nullptr);
    const u16 nid = sim.society().create_polity("邻邦", 0x88AA66, 1);
    rival->polity = nid;
    sim.society().set_ruler(nid, rival->id, "secession", 0);
    sim.society().polity(nid)->seat = home->seat;
    sim.run(kTicksPerDay);
    int moved = 0;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Migration) ++moved;
    CHECK_EQ(moved, 0);
}

TEST("ecology: saplings take root near standing trees, grow into trees, and survive a save") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    size_t most_saplings = 0;
    int most_regrown = 0;
    for (int d = 0; d < 8; ++d) {
        for (int h = 0; h < 24; ++h) {
            sim.run(kTicksPerHour);
            most_regrown = std::max(most_regrown, sim.ecology().regrown_standing());
        }
        most_saplings = std::max(most_saplings, sim.ecology().saplings());
    }
    std::printf("  ecology: up to %zu saplings, up to %d trees regrown (%d standing)\n", most_saplings, most_regrown,
                sim.ecology().regrown_standing());
    CHECK(most_saplings > 0);
    // (Woodcutters and builders may take them again: young growth is felled last, not never.)
    CHECK(most_regrown > 0);
    CHECK_EQ(sim.forest().regrown, sim.ecology().regrown_standing());
    // Deterministic across a save.
    std::vector<u8> bytes = sim.save();
    Simulation loaded(test_registry());
    loaded.load(bytes);
    CHECK_EQ(loaded.state_hash(), sim.state_hash());
    sim.run(kTicksPerDay);
    loaded.run(kTicksPerDay);
    CHECK_EQ(loaded.state_hash(), sim.state_hash());
}

TEST("logistics: failed routes are judged over a rolling day, and survive a save") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    sim.run(kTicksPerHour * 6);
    const IslandFeatures& f = sim.world().gen().features();
    AdminCommand c;
    c.type = "dig";
    c.params = Json::object();
    Json p = Json::array();
    p.push((f.bridge_a.x + f.bridge_b.x) / 2);
    p.push((f.bridge_a.y + f.bridge_b.y) / 2);
    p.push((f.bridge_a.z + f.bridge_b.z) / 2);
    c.params.set("pos", p);
    c.params.set("radius", 4.5);
    sim.queue_admin(c);
    sim.run(kTicksPerHour * 8);
    const int window = sim.agents().path_failures_24h();
    std::vector<u8> bytes = sim.save();
    Simulation loaded(test_registry());
    loaded.load(bytes);
    CHECK_EQ(loaded.agents().path_failures_24h(), window);
    CHECK_EQ(loaded.agents().day.path_failures, sim.agents().day.path_failures);
    CHECK_EQ(loaded.agents().day.harvested, sim.agents().day.harvested);
    sim.run(kTicksPerHour * 6);
    loaded.run(kTicksPerHour * 6);
    CHECK_EQ(loaded.state_hash(), sim.state_hash());
}

TEST("farming: a plot whose ground is blown away gets no more work") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    sim.run(kTicksPerHour);
    const Farm* farm = nullptr;
    for (const Farm& f : sim.farming().all())
        if (f.alive && !f.plots.empty()) farm = &f;
    REQUIRE(farm != nullptr);
    const u32 fid = farm->id;
    const Vec3i ground = farm->plots[0].ground;
    // Blow the ground out from under the first plot.
    AdminCommand c;
    c.type = "dig";
    c.params = Json::object();
    Json p = Json::array();
    p.push(ground.x);
    p.push(ground.y - 1);
    p.push(ground.z);
    c.params.set("pos", p);
    c.params.set("radius", 1.5);
    sim.queue_admin(c);
    sim.run(120);
    CHECK(sim.farming().state(sim.farming().get(fid)->plots[0]) == PlotState::Lost);
    CHECK(sim.farming().stats(fid).lost >= 1);
    for (const Job& j : sim.jobs().all())
        if (j.alive && j.farm == fid && j.plot == 0 && j.claimed_by == kNoEntity) CHECK(false);
}
