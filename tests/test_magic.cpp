// Magic on the battlefield and at home: war cries, frenzy, discord, withering and
// devouring, their cooldowns, and the feed that makes every cast visible.
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
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
Character* girl_of(Simulation& sim, u16 polity) {
    for (auto& c : sim.agents().all())
        if (c && c->alive && c->is_girl() && c->polity == polity) return c.get();
    return nullptr;
}
// Make her a magical girl of the given drive, strong enough for all its spells.
void make_drive(Character& g, const std::string& drive) {
    g.girl->drive = drive;
    g.girl->level = 6;
    g.girl->mana = 1.0f;
}
const Json* spell(Simulation& sim, const std::string& drive, const std::string& effect) {
    for (const Json& d : sim.reg().doc("drives")["drives"].items())
        if (d.str("key") == drive)
            for (const Json& sp : d["spells"].items())
                if (sp.str("effect") == effect) return &sp;
    return nullptr;
}
// Hold every decision (the rulers wait for a remote answer that never comes).
void hold_decisions(Simulation& sim) {
    sim.decisions().mode = "remote";
    sim.decisions().remote_budget_per_day = 1 << 20;
    sim.decisions().remote_deadline = kTicksPerDay * 100;
}
}  // namespace

TEST("magic: a war cry steels the fighters around her, and every cast is seen") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    hold_decisions(sim);
    Society& soc = sim.society();
    Agents& ag = sim.agents();
    const auto ids = alive_polities(sim);
    REQUIRE(ids.size() == 3);
    const u16 a = ids[0], b = ids[1];
    REQUIRE(soc.declare_war(a, b, "raid", 0, 0) != 0);
    soc.draft(a, 6, 0);
    soc.draft(b, 3, 0);
    Character* g = girl_of(sim, a);
    REQUIRE(g != nullptr);
    make_drive(*g, "courage");
    // Her soldiers around her, an enemy soldier coming at them.
    int n = 0;
    Character* foe = nullptr;
    std::vector<EntityId> ranks;
    for (auto& c : ag.all()) {
        if (!c || !c->alive || !c->drafted) continue;
        if (c->polity == a && n < 4) {
            ranks.push_back(c->id);
            c->pos = g->pos + Vec3f((float)(n % 2) * 2.0f - 1.0f, 0.0f, (float)(n / 2) * 2.0f - 1.0f);
            c->foot = g->foot;
            c->fear = 0.8f;
            c->task = Task{};  // standing by her
            c->path.clear();
            c->next_think = sim.now() + 1000;
            ++n;
        } else if (c->polity == b && !foe) {
            foe = c.get();
            foe->pos = g->pos + Vec3f(8.0f, 0.0f, 0.0f);
            foe->foot = g->foot + Vec3i{8, 0, 0};
        }
    }
    REQUIRE(n >= 3);
    REQUIRE(foe != nullptr);
    Agents::SpellPick pick;
    std::string why;
    CHECK(ag.pick_spell(*g, pick, why) > 0.0f);
    CHECK_EQ(pick.name, std::string("战吼"));
    // What it does: the fighters around her lose their fear and strike harder.
    {
        Event e;
        ag.cast_ritual(*g, pick.effect, pick.name, spell(sim, "courage", "rally"), g->foot, nullptr, e);
        int steeled = 0;
        for (EntityId id : ranks)
            if (const Character* c = ag.get(id); c && ag.empowerment(*c) > 1.2f) {
                ++steeled;
                CHECK(c->fear < 0.5f);
            }
        CHECK(steeled >= 3);
        for (auto& c : ag.all())
            if (c) c->empowered_until = 0;
    }
    // Cast as she would on her own (kept from re-thinking meanwhile): told, and seen.
    ag.take_spells();
    g->task = Task{};
    g->task.type = TaskType::Cast;
    g->task.count = pick.effect;
    g->task.target = pick.pos;
    g->task.other = pick.who;
    g->task.label = pick.name;
    g->task.started = sim.now();
    g->next_think = sim.now() + 1000;
    sim.run(60);
    bool told = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::SpellCast && e.actor == g->id && e.text.find("战吼") != std::string::npos) {
            told = true;
        }
    CHECK(told);
    const auto fx = ag.take_spells();
    REQUIRE(!fx.empty());
    CHECK_EQ(fx.back().effect, std::string("rally"));
    CHECK_EQ(fx.back().caster, g->id);
    CHECK_EQ(fx.back().drive, std::string("courage"));
    CHECK(ag.take_spells().empty());
    // A war cry wants its rest: not again at once, however much mana she has.
    g->girl->mana = 1.0f;
    for (auto& c : ag.all())
        if (c) c->empowered_until = 0;
    Agents::SpellPick again;
    ag.pick_spell(*g, again, why);
    CHECK(again.name != std::string("战吼"));
    // The strength fades after a couple of hours.
    sim.run(kTicksPerHour * 3);
    for (auto& c : ag.all())
        if (c && c->alive && c->polity == a) CHECK(ag.empowerment(*c) == 1.0f);
}

TEST("magic: frenzy doubles her strength for a while, and it survives save and load") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    hold_decisions(sim);
    const auto ids = alive_polities(sim);
    Character* g = girl_of(sim, ids[0]);
    REQUIRE(g != nullptr);
    make_drive(*g, "wrath");
    Event e;
    sim.agents().cast_ritual(*g, 10, "狂暴", spell(sim, "wrath", "berserk"), g->foot, nullptr, e);
    CHECK(sim.agents().empowerment(*g) > 1.9f);
    CHECK(e.text.find("力量倍增") != std::string::npos);
    const auto blob = sim.save();
    Simulation other(test_registry());
    other.load(blob);
    CHECK(other.agents().empowerment(*other.agents().get(g->id)) > 1.9f);
    sim.run(kTicksPerHour * 2);
    CHECK(sim.agents().empowerment(*g) == 1.0f);
}

TEST("magic: withering kills the enemy's crops; devouring eats their walls for mana") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    hold_decisions(sim);
    Society& soc = sim.society();
    const auto ids = alive_polities(sim);
    const u16 a = ids[0], b = ids[1];
    Character* g = girl_of(sim, a);
    REQUIRE(g != nullptr);
    make_drive(*g, "despair");
    // Their field in full growth.
    const MatId crop = sim.reg().m().crop, air = sim.reg().m().air;
    Farm* field = nullptr;
    for (Farm& f : sim.farming().all_mut())
        if (f.alive && f.polity == b) field = &f;
    REQUIRE(field != nullptr);
    for (const Plot& pl : field->plots)
        if (sim.world().mat(pl.ground + Vec3i{0, 1, 0}) == air) sim.world().set(pl.ground + Vec3i{0, 1, 0}, make_voxel(crop, 4));
    auto crops = [&] {
        int n = 0;
        for (const Plot& pl : field->plots) n += sim.world().mat(pl.ground + Vec3i{0, 1, 0}) == crop;
        return n;
    };
    const int before = crops();
    REQUIRE(before > 5);
    const Vec3i at = field->plots.front().ground;
    Event e;
    sim.agents().cast_ritual(*g, 12, "枯萎", spell(sim, "despair", "wither"), at, nullptr, e);
    CHECK(crops() < before);
    CHECK(e.text.find("枯死") != std::string::npos);
    CHECK(soc.polity(b)->diplo_of(a) && soc.polity(b)->diplo_of(a)->grievance > 0.0f);
    // Devouring: at war, at their hall.
    REQUIRE(soc.declare_war(a, b, "conquest", 0, 0) != 0);
    Polity* pa = soc.polity(a);
    pa->op.active = true;
    pa->op.enemy = b;
    const Building* hall = sim.buildings().get(soc.polity(b)->seat);
    REQUIRE(hall != nullptr);
    make_drive(*g, "gluttony");
    g->girl->mana = 0.2f;
    int walls = 0;
    auto count_walls = [&] {
        int n = 0;
        for (int dy = -1; dy <= 3; ++dy)
            for (int dz = -4; dz <= 4; ++dz)
                for (int dx = -4; dx <= 4; ++dx) {
                    const Vec3i q = hall->entrance + Vec3i{dx, dy, dz};
                    if (sim.buildings().at(q) == hall->id && sim.world().material(q).solid) ++n;
                }
        return n;
    };
    walls = count_walls();
    REQUIRE(walls > 0);
    Event e2;
    sim.agents().cast_ritual(*g, 13, "吞噬", spell(sim, "gluttony", "devour"), hall->entrance, nullptr, e2);
    CHECK(count_walls() < walls);
    CHECK(g->girl->mana > 0.2f);
    CHECK(pa->war_with(b) && pa->war_with(b)->razed > 0);
}

TEST("magic: discord whispered at home turns listeners against the ruler") {
    Simulation sim(test_registry());
    sim.new_game(realms());
    hold_decisions(sim);
    const auto ids = alive_polities(sim);
    const u16 a = ids[0];
    const Polity* p = sim.society().polity(a);
    Character* g = nullptr;
    for (auto& c : sim.agents().all())
        if (c && c->alive && c->is_girl() && c->polity == a && c->id != p->ruler) g = c.get();
    REQUIRE(g != nullptr);
    make_drive(*g, "envy");
    // A few of the ruler's supporters gathered around her.
    std::vector<Character*> near;
    for (auto& c : sim.agents().all())
        if (c && c->alive && !c->is_girl() && c->polity == a && near.size() < 4) {
            c->pos = g->pos + Vec3f((float)near.size(), 0.0f, 1.0f);
            c->foot = g->foot + Vec3i{(int)near.size(), 0, 1};
            c->support_ref(p->ruler) = 0.6f;
            near.push_back(c.get());
        }
    REQUIRE(near.size() >= 3);
    Event e;
    sim.agents().cast_ritual(*g, 11, "离间", spell(sim, "envy", "discord"), g->foot, nullptr, e);
    for (Character* c : near) {
        CHECK(c->support_for(p->ruler) < 0.6f);
        CHECK(c->support_for(g->id) > 0.0f);
    }
    CHECK(e.text.find("疑心") != std::string::npos);
}
