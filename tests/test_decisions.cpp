// Jev decision pipeline: options, providers (local / remote / replay), validation,
// politics and persistence.
#include <map>
#include <set>

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

void break_bridge(Simulation& sim) {
    const IslandFeatures& f = sim.world().gen().features();
    AdminCommand cmd;
    cmd.type = "dig";
    cmd.params = Json::object();
    Json pos = Json::array();
    pos.push((f.bridge_a.x + f.bridge_b.x) / 2);
    pos.push((f.bridge_a.y + f.bridge_b.y) / 2);
    pos.push((f.bridge_a.z + f.bridge_b.z) / 2);
    cmd.params.set("pos", pos);
    cmd.params.set("radius", 4.5);
    sim.queue_admin(cmd);
}

const Decision* find_decision(const Simulation& sim, const std::string& kind, CrisisKind crisis) {
    for (const Decision& d : sim.decisions().all())
        if (d.id && d.kind == kind && (crisis == CrisisKind::None || d.crisis == crisis)) return &d;
    return nullptr;
}

std::vector<std::string> choices(const Simulation& sim) {
    std::vector<std::string> out;
    for (const Decision& d : sim.decisions().all())
        if (d.id) out.push_back(d.chosen >= 0 ? d.options[(size_t)d.chosen].key : "-");
    return out;
}
}  // namespace

TEST("decisions: broken bridge leads to a logistics decision with computed options and proposals") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    sim.run(kTicksPerHour * 6);
    break_bridge(sim);
    sim.run(kTicksPerHour * 3);
    const Decision* d = find_decision(sim, "crisis", CrisisKind::Logistics);
    REQUIRE(d != nullptr);
    CHECK(d->status == DecisionStatus::Executed);
    bool has_repair = false;
    for (const DecisionOption& o : d->options) {
        if (o.key == "rebuild_bridge") has_repair = true;
        CHECK(!o.desc.empty());
    }
    CHECK(has_repair);
    CHECK_EQ(d->proposals.size(), (size_t)2);  // the two other girls advise the ruler
    CHECK(!d->rationale.empty());
    CHECK(d->situation.find("桥") != std::string::npos);
    // The decision is part of the causal chain of the bridge collapse.
    const Event* made = sim.chronicle().get(d->decision_event);
    REQUIRE(made != nullptr);
    CHECK(made->type == EventType::DecisionMade);
    std::vector<EventId> chain = sim.chronicle().causes_of(made->id);
    bool reaches_admin = false;
    for (EventId id : chain)
        if (const Event* e = sim.chronicle().get(id))
            if (e->type == EventType::AdminAction) reaches_admin = true;
    CHECK(reaches_admin);
}

TEST("decisions: remote mode waits for the host, validates answers and falls back on timeout") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    sim.decisions().mode = "remote";
    sim.decisions().remote_deadline = kTicksPerHour;
    sim.run(kTicksPerHour * 6);
    break_bridge(sim);
    sim.run(kTicksPerHour);
    std::vector<u32> waiting = sim.decisions().awaiting_remote();
    REQUIRE(!waiting.empty());
    u32 id = waiting.front();
    CHECK(!sim.decisions().remote_system_prompt(id).empty());
    CHECK(sim.decisions().remote_user_prompt(id).find("rebuild_bridge") != std::string::npos);
    CHECK(sim.decisions().remote_schema(id).has("properties"));
    // An invalid option is rejected; the local persona model takes over.
    std::string err;
    CHECK(!sim.decisions().submit(id, "no_such_option", "?", "remote", err));
    CHECK(!err.empty());
    const Decision* d = sim.decisions().get(id);
    CHECK(d->status == DecisionStatus::Executed);
    CHECK_EQ(d->source, std::string("fallback"));
    // Timeouts fall back too.
    sim.run(kTicksPerHour * 30);
    for (const Decision& x : sim.decisions().all())
        if (x.id && x.created + kTicksPerHour * 2 < sim.now()) CHECK(x.status != DecisionStatus::AwaitingRemote);
}

TEST("decisions: a valid remote answer is executed with its rationale") {
    Simulation sim(test_registry());
    sim.new_game(village(2));
    sim.decisions().mode = "remote";
    sim.run(kTicksPerHour * 6);
    break_bridge(sim);
    sim.run(kTicksPerHour);
    std::vector<u32> waiting = sim.decisions().awaiting_remote();
    REQUIRE(!waiting.empty());
    const Decision* d = sim.decisions().get(waiting.front());
    std::string key;
    for (const DecisionOption& o : d->options)
        if (o.feasible && o.key != "wait") key = o.key;
    std::string err;
    CHECK(sim.decisions().submit(d->id, key, "测试理由", "remote", err));
    d = sim.decisions().get(waiting.front());
    CHECK_EQ(d->options[(size_t)d->chosen].key, key);
    CHECK_EQ(d->rationale, std::string("测试理由"));
    CHECK_EQ(d->source, std::string("remote"));
}

TEST("decisions: replaying a decision log reproduces the run exactly") {
    const Registry& reg = test_registry();
    Simulation a(reg);
    a.new_game(village(3));
    a.run(kTicksPerHour * 6);
    break_bridge(a);
    a.run(kTicksPerDay * 3);
    Json log = a.decisions().export_log();
    REQUIRE(log.size() > 2);

    Simulation b(reg);
    b.new_game(village(3));
    b.decisions().mode = "replay";
    b.decisions().load_replay(log);
    b.run(kTicksPerHour * 6);
    break_bridge(b);
    b.run(kTicksPerDay * 3);
    CHECK(choices(a) == choices(b));
    CHECK_EQ(a.state_hash(), b.state_hash());
}

TEST("decisions: save/load across open decisions continues identically") {
    const Registry& reg = test_registry();
    Simulation a(reg), b(reg);
    a.new_game(village(5));
    b.new_game(village(5));
    for (Simulation* s : {&a, &b}) {
        s->run(kTicksPerHour * 6);
        break_bridge(*s);
    }
    a.run(kTicksPerDay * 2);
    b.run(kTicksPerDay);
    Simulation c(reg);
    c.load(b.save());
    CHECK_EQ(c.state_hash(), b.state_hash());
    b.run(kTicksPerDay);
    c.run(kTicksPerDay);
    CHECK_EQ(a.state_hash(), b.state_hash());
    CHECK_EQ(b.state_hash(), c.state_hash());
    CHECK(choices(a) == choices(c));
}

TEST("politics: secession splits people and fields and conserves every item") {
    const Registry& reg = test_registry();
    Simulation sim(reg);
    sim.new_game(village(1));
    sim.run(kTicksPerHour * 2);
    Polity* p = sim.society().polity(1);
    REQUIRE(p != nullptr);
    // A disaffected, popular girl.
    Character* rebel = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->is_girl() && cp->id != p->ruler) rebel = cp.get();
    REQUIRE(rebel != nullptr);
    rebel->girl->loyalty = -0.8f;
    rebel->girl->last_decision = 0;
    int n = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && (n++ % 2 == 0)) {
            cp->support_ref(rebel->id) = 0.9f;
            cp->support_ref(p->ruler) = -0.2f;
        }
    sim.decisions().whisper(rebel->id, "secede", 0);
    // Girls reconsider their stance every two days.
    int polities = 1;
    for (int h = 0; h < 60 && polities < 2; ++h) {
        sim.run(kTicksPerHour);
        polities = 0;
        for (const Polity& x : sim.society().polities())
            if (x.alive) ++polities;
    }
    REQUIRE(polities == 2);
    u16 nid = rebel->polity;
    CHECK(nid != 1);
    CHECK(sim.society().polity(nid)->ruler == rebel->id);
    CHECK(sim.society().title(nid).find("的文明，") != std::string::npos);
    CHECK(sim.farming().stats_polity(1).plots > 0);
    CHECK(sim.farming().stats_polity(nid).plots > 0);
    int followers = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && cp->polity == nid) ++followers;
    CHECK(followers >= 4);
    // Goods are hauled, not teleported: the ledger balances throughout.
    sim.run(kTicksPerDay);
    for (size_t i = 0; i < reg.item_count(); ++i) {
        const LedgerLine& l = sim.economy().ledger((ItemId)i);
        CHECK_EQ(sim.economy().total((ItemId)i), l.produced - l.consumed);
    }
    CHECK(sim.society().public_food(nid) > 0.0f);
    bool secession_event = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Secession) secession_event = true;
    CHECK(secession_event);
}
