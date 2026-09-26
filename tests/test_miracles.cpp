// Miracles: the god's powers over people and minds act through the shared systems
// (ledger, bodies, memories, research, decisions) and are part of the causal record.
#include <algorithm>
#include <cmath>

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

Json vec_json(const Vec3i& v) {
    Json a = Json::array();
    a.push(v.x);
    a.push(v.y);
    a.push(v.z);
    return a;
}

void miracle(Simulation& sim, const std::string& type, Json params) {
    AdminCommand cmd;
    cmd.type = type;
    cmd.params = std::move(params);
    sim.queue_admin(cmd);
}

const Event* last_admin(const Simulation& sim, const std::string& action) {
    const Event* found = nullptr;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::AdminAction && e.data.str("action") == action) found = &e;
    return found;
}

Character* first_resident(Simulation& sim) {
    for (const auto& cp : sim.agents().all())
        if (cp && cp->alive && !cp->is_girl()) return cp.get();
    return nullptr;
}

Character* ruler(Simulation& sim) {
    const Polity* p = sim.society().polity(1);
    return p ? sim.agents().get(p->ruler) : nullptr;
}
}  // namespace

TEST("miracles: blessed food is real items in the ledger, and residents store it") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    sim.run(kTicksPerHour);
    const ItemId grain = sim.reg().item_id("grain");
    const i64 before = sim.economy().total(grain);
    const i64 produced = sim.economy().ledger(grain).produced;
    const i64 consumed = sim.economy().ledger(grain).consumed;
    Json p = Json::object();
    p.set("pos", vec_json(sim.world().gen().features().village + Vec3i{4, 0, 4}));
    p.set("amount", 50);
    miracle(sim, "bless_food", p);
    sim.run(1);
    // Conservation: what exists is what was there, plus the gift, minus what was eaten.
    const i64 eaten = sim.economy().ledger(grain).consumed - consumed;
    CHECK_EQ(sim.economy().ledger(grain).produced, produced + 50);
    CHECK_EQ(sim.economy().total(grain), before + 50 - eaten);
    REQUIRE(last_admin(sim, "bless_food") != nullptr);
    // Residents haul the gift into their stores within the day.
    sim.run(kTicksPerDay / 2);
    i64 on_ground = 0;
    for (const Store& s : sim.economy().stores())
        if (s.alive && s.kind == StoreKind::Pile)
            for (const ItemStack& st : s.items)
                if (st.item == grain) on_ground += st.count;
    CHECK(on_ground < 50);
}

TEST("miracles: healing regrows lost limbs, smiting wounds, terror sends people running") {
    Simulation sim(test_registry());
    sim.new_game(village(2));
    sim.run(kTicksPerHour);
    Character* c = first_resident(sim);
    REQUIRE(c != nullptr);
    // Take an arm off.
    BodyPart& arm = c->body.parts[kArmR];
    std::fill(arm.vox.begin(), arm.vox.end(), (u8)0);
    arm.alive = 0;
    arm.severed = true;
    CHECK(!c->body.can_hold() || c->body.total_alive() < c->body.total_voxels());
    Json h = Json::object();
    h.set("pos", vec_json(c->foot));
    h.set("radius", 1.5);
    miracle(sim, "heal", h);
    sim.run(1);
    CHECK(!c->body.parts[kArmR].severed);
    CHECK_EQ(c->body.total_alive(), c->body.total_voxels());
    bool remembers = false;
    for (const Memory& m : c->memories)
        if (m.kind == MemoryKind::Healed) remembers = true;
    CHECK(remembers);

    Character* victim = first_resident(sim);
    const int alive_before = victim->body.total_alive();
    Json s = Json::object();
    s.set("pos", vec_json(victim->foot));
    s.set("radius", 1.0);
    miracle(sim, "smite", s);
    sim.run(1);
    CHECK(!victim->alive || victim->body.total_alive() < alive_before);
    const Event* smite = last_admin(sim, "smite");
    REQUIRE(smite != nullptr);
    CHECK_EQ(smite->severity, 4);

    // Terror at the village: those caught in it are afraid and run.
    const Vec3i v = sim.world().gen().features().village;
    Json t = Json::object();
    t.set("pos", vec_json(v));
    t.set("radius", 12.0);
    miracle(sim, "terrify", t);
    sim.run(1);
    int afraid = 0;
    for (const auto& cp : sim.agents().all())
        if (cp && cp->alive && cp->fear > 0.9f) ++afraid;
    CHECK(afraid > 0);
    int fled = 0;
    for (int i = 0; i < 60; ++i) {
        sim.run(5);
        for (const auto& cp : sim.agents().all())
            if (cp && cp->alive && cp->task.type == TaskType::Flee) ++fled;
    }
    CHECK(fled > 0);
}

TEST("miracles: revelation advances research, empowering levels a girl") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    sim.run(kTicksPerHour);
    const size_t techs = sim.society().polity(1)->techs.size();
    Json e = Json::object();
    e.set("polity", 1);
    e.set("points", 500);
    miracle(sim, "enlighten", e);
    sim.run(1);
    CHECK_EQ(sim.society().polity(1)->techs.size(), techs + 1);
    const Event* rev = last_admin(sim, "enlighten");
    REQUIRE(rev != nullptr);
    bool traced = false;
    for (const Event& ev : sim.chronicle().events())
        if (ev.type == EventType::TechDiscovered && ev.causes[0] == rev->id) traced = true;
    CHECK(traced);

    Character* g = ruler(sim);
    REQUIRE(g != nullptr);
    const int level = g->girl->level;
    Json m = Json::object();
    m.set("girl", (double)g->id);
    miracle(sim, "empower", m);
    sim.run(1);
    CHECK_EQ(g->girl->level, level + 1);
    CHECK(g->girl->mana > 0.99f);
}

TEST("miracles: a whisper shifts what a girl values, and joins the causal chain of her choices") {
    // Two identical worlds; in one the god whispers "care more about X" to the ruler.
    auto run = [](bool whisper, int feature, std::vector<const Decision*>* out, Simulation& sim) {
        sim.new_game(village(1));
        sim.run(kTicksPerHour * 6);
        const Tick t0 = sim.now();
        if (whisper) {
            Json p = Json::object();
            p.set("girl", (double)ruler(sim)->id);
            p.set("feature", feature_key(feature));
            p.set("dir", 1);
            miracle(sim, "whisper_value", p);
        }
        // A crisis to decide about.
        const IslandFeatures& f = sim.world().gen().features();
        Json d = Json::object();
        d.set("pos", vec_json(Vec3i{(f.bridge_a.x + f.bridge_b.x) / 2, (f.bridge_a.y + f.bridge_b.y) / 2,
                                    (f.bridge_a.z + f.bridge_b.z) / 2}));
        d.set("radius", 4.5);
        miracle(sim, "dig", d);
        sim.run(kTicksPerHour * 3);
        for (const Decision& x : sim.decisions().all())
            if (x.id && x.created >= t0 && x.girl == ruler(sim)->id && x.chosen >= 0) out->push_back(&x);
    };
    Simulation control(test_registry());
    std::vector<const Decision*> cd;
    run(false, 0, &cd, control);
    REQUIRE(!cd.empty());
    // Whisper the value on which the control choice leads its alternatives the most, so
    // the choice stands and the whisper should show up in its causes.
    const Decision& c0 = *cd.front();
    int feature = 0;
    float lead = -1e9f;
    for (int f = 0; f < kFeatureCount; ++f) {
        float best_other = -1e9f;
        for (size_t i = 0; i < c0.options.size(); ++i)
            if ((int)i != c0.chosen && c0.options[i].feasible) best_other = std::max(best_other, c0.options[i].f[f]);
        float l = c0.options[(size_t)c0.chosen].f[f] - best_other;
        if (l > lead) {
            lead = l;
            feature = f;
        }
    }
    Simulation whispered(test_registry());
    std::vector<const Decision*> wd;
    run(true, feature, &wd, whispered);
    REQUIRE(!wd.empty());
    const Decision& w0 = *wd.front();
    REQUIRE(w0.options.size() == c0.options.size());
    // Scores move by exactly the whisper's weight on that value.
    for (size_t i = 0; i < c0.options.size(); ++i) {
        if (!c0.options[i].feasible) continue;
        float expect = c0.local_scores[i] + 0.8f * c0.options[i].f[feature];
        CHECK(std::fabs(w0.local_scores[i] - expect) < 1e-3f);
    }
    CHECK_EQ(w0.chosen, c0.chosen);
    const Event* ev = whispered.chronicle().get(w0.decision_event);
    const Event* wev = last_admin(whispered, "whisper_value");
    REQUIRE(ev != nullptr);
    REQUIRE(wev != nullptr);
    if (c0.options[(size_t)c0.chosen].f[feature] * 0.8f > 0.05f) {
        CHECK_EQ(ev->causes[2], wev->id);
        CHECK(w0.rationale.find("莫名") != std::string::npos);
    }
    // The whisper survives a save and load.
    std::vector<u8> bytes = whispered.save();
    Simulation loaded(test_registry());
    loaded.load(bytes);
    whispered.run(kTicksPerDay);
    loaded.run(kTicksPerDay);
    CHECK_EQ(loaded.state_hash(), whispered.state_hash());
}

TEST("miracles: called rain falls for its hours, is on record, and puts out open fires") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    sim.run(kTicksPerHour);
    // A fire in the open, then rain.
    const Vec3i v = sim.world().gen().features().village;
    AdminCommand fire;
    fire.type = "ignite";
    fire.params = Json::object();
    fire.params.set("pos", vec_json(v + Vec3i{12, 1, 12}));
    fire.params.set("radius", 1.5);
    sim.apply_admin(fire);
    sim.run(20);
    auto burning_near = [&]() {
        int n = 0;
        const Vec3i c = v + Vec3i{12, 1, 12};
        for (int dy = -6; dy <= 10; ++dy)
            for (int dz = -16; dz <= 16; ++dz)
                for (int dx = -16; dx <= 16; ++dx)
                    if (vburning(sim.world().peek(c + Vec3i{dx, dy, dz}))) ++n;
        return n;
    };
    const int burning = burning_near();
    AdminCommand rain;
    rain.type = "rain";
    rain.params = Json::object();
    rain.params.set("hours", 3);
    const EventId ev = sim.apply_admin(rain);
    CHECK(ev != 0);
    CHECK(sim.physics().raining());
    const Event* e = sim.chronicle().get(ev);
    REQUIRE(e != nullptr);
    CHECK(e->type == EventType::AdminAction);
    sim.run(kTicksPerHour * 2);
    CHECK(sim.physics().raining());
    CHECK(burning_near() <= burning);
    sim.run(kTicksPerHour * 2);
    CHECK(!sim.physics().raining());
}
