// Society: war and peace. Wars are declared by rulers through decisions; armies are
// real drafted residents who arm themselves from the storehouses and follow one
// operation at a time (muster at home, march on the objective, raid or take it, come
// back). Nothing is abstract: soldiers walk, fight with voxel bodies and die, and what
// a raid carries off is really hauled away.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/agents/nav.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
constexpr int kSiegeHours = 4;   // the ground around the enemy's hall held this long takes it
constexpr int kAnnexBelow = 5;   // a people with no more grown folk than this is absorbed
}  // namespace

bool Society::at_war(u16 a, u16 b) const {
    const Polity* p = polity(a);
    return p && p->war_with(b) != nullptr;
}

EventId Society::declare_war(u16 attacker, u16 defender, const std::string& aim, EntityId by, EventId cause) {
    Polity* a = polity(attacker);
    Polity* d = polity(defender);
    if (!a || !d || a->war_with(defender)) return 0;
    Event e;
    e.type = EventType::WarDeclared;
    e.severity = 5;
    e.actor = by;
    e.polity = attacker;
    e.causes[0] = cause;
    e.text = strfmt("「%s」向「%s」宣战（%s）", title(attacker).c_str(), title(defender).c_str(),
                    aim == "conquest" ? "意在征服" : "意在劫掠");
    e.data.set("aim", aim);
    e.data.set("enemy", (int)defender);
    EventId ev = ctx_.chron->emit(std::move(e));
    if (a->pact_with(defender)) end_trade(attacker, defender, "战争断绝了商路", ev);
    War wa;
    wa.enemy = defender;
    wa.attacker = true;
    wa.aim = aim;
    wa.since = ctx_.now;
    wa.active_at = ctx_.now;
    wa.event = ev;
    a->wars.push_back(wa);
    War wd;
    wd.enemy = attacker;
    wd.attacker = false;
    wd.aim = "defend";
    wd.since = ctx_.now;
    wd.active_at = ctx_.now;
    wd.event = ev;
    d->wars.push_back(wd);
    a->attitude_ref(defender) = std::min(a->attitude_to(defender), -0.6f);
    d->attitude_ref(attacker) = std::min(d->attitude_to(attacker), -0.8f);
    add_grievance(defender, attacker, 0.5f);
    start_operation(attacker, defender, aim, ev);
    return ev;
}

void Society::start_operation(u16 id, u16 enemy, const std::string& aim, EventId cause) {
    Polity* a = polity(id);
    Polity* d = polity(enemy);
    if (!a || !d) return;
    // The army marches on the enemy seat; a raid aims at the store with the most food
    // (the nearer the better), wherever the enemy keeps it.
    const Building* home = ctx_.buildings->get(a->seat);
    const Building* target = ctx_.buildings->get(d->seat);
    Vec3i objective = target ? target->entrance : Vec3i{};
    if (aim == "raid") {
        float best = 0.0f;
        for (StoreId sid : public_stores(enemy)) {
            const Store* s = ctx_.econ->store(sid);
            const int food = food_in(sid);
            if (!s || food <= 0) continue;
            const float dist = home ? std::sqrt((float)s->pos.dist2(home->entrance)) : 0.0f;
            const float score = (float)std::min(food, 200) / (1.0f + dist / 150.0f);
            if (score > best) {
                best = score;
                objective = s->pos;
            }
        }
    }
    for (Polity* x : {a, d})
        if (War* w = x->war_with(x == a ? enemy : id)) w->active_at = ctx_.now;
    a->op = Operation{};
    a->op.active = true;
    a->op.enemy = enemy;
    a->op.aim = aim;
    a->op.rally = home ? home->entrance : objective;
    a->op.objective = objective;
    a->op.stage = stage_point(a->op.rally, objective);
    a->op.phase = 0;
    a->op.since = ctx_.now;
    a->op.event = cause;
    enlist_champions(id, cause);
}

int Society::food_in(StoreId sid) const {
    const Store* s = ctx_.econ->store(sid);
    if (!s) return 0;
    int n = 0;
    for (const ItemStack& st : s->items)
        if (ctx_.reg->item(st.item).nutrition > 0) n += st.count;
    return n;
}

Vec3i Society::stage_point(const Vec3i& from, const Vec3i& objective) {
    const float dx = (float)(from.x - objective.x), dz = (float)(from.z - objective.z);
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return objective;
    const float back = std::min(26.0f, len * 0.5f);
    const int cx = objective.x + (int)std::lround(dx / len * back);
    const int cz = objective.z + (int)std::lround(dz / len * back);
    // Open, dry ground near that point, searched in rings.
    for (int r = 0; r <= 8; ++r)
        for (int oz = -r; oz <= r; ++oz)
            for (int ox = -r; ox <= r; ++ox) {
                if (std::max(std::abs(ox), std::abs(oz)) != r) continue;
                const int x = cx + ox, z = cz + oz;
                const int y = ctx_.world->surface_y(x, z);
                if (y < 0 || std::abs(y + 1 - objective.y) > 12) continue;
                const Vec3i p{x, y + 1, z};
                if (ctx_.world->material(p).fluid || ctx_.world->material(p + Vec3i{0, -1, 0}).fluid) continue;
                if (!ctx_.nav->standable(p)) continue;
                return p;
            }
    return objective;
}

int Society::enlist_champions(u16 id, EventId cause) {
    Polity* p = polity(id);
    if (!p) return 0;
    // Battle magic: what a magical girl brings to a fight.
    static const char* kBattle[] = {"strike", "ranged", "firebomb", "drain_strike", "terrify", "rally", "berserk"};
    std::vector<std::pair<float, Character*>> pool;
    for (const auto& cp : ctx_.agents->all()) {
        Character* c = cp.get();
        if (!c || !c->alive || c->departed || !c->is_girl() || c->polity != id || c->drafted) continue;
        if (c->body.mobility() < 0.6f || !c->body.can_hold()) continue;
        const Json* drive = nullptr;
        for (const Json& d : ctx_.reg->doc("drives")["drives"].items())
            if (d.str("key") == c->girl->drive) drive = &d;
        if (!drive) continue;
        int battle = 0;
        for (const Json& sp : (*drive)["spells"].items())
            for (const char* k : kBattle)
                if (sp.str("effect") == k && sp.str("type") == "active" && c->girl->level >= sp.integer("level", 1)) ++battle;
        const bool warrior = drive->str("category") == "combat";
        // The ruler stays to govern unless war is her very nature.
        if (battle == 0 || (c->id == p->ruler && !warrior)) continue;
        const float fit = (float)battle + (warrior ? 1.0f : 0.0f) + 0.2f * (float)c->girl->level + c->pers.aggression +
                          (c->girl->role == "general" ? 2.0f : 0.0f) - c->pers.caution;
        pool.push_back({fit, c});
    }
    std::sort(pool.begin(), pool.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first > y.first : x.second->id < y.second->id;
    });
    int n = 0;
    for (const auto& [fit, c] : pool) {
        if (n >= 2) break;
        (void)fit;
        c->drafted = true;
        c->task = Task{};
        c->next_think = ctx_.now;
        Event e;
        e.type = EventType::Battle;
        e.severity = 3;
        e.actor = c->id;
        e.polity = id;
        e.causes[0] = cause;
        e.text = strfmt("魔法少女%s随军出征", c->name.c_str());
        ctx_.chron->emit(std::move(e));
        ++n;
    }
    return n;
}

EventId Society::make_peace(u16 a, u16 b, const std::string& how, EventId cause) {
    Polity* pa = polity(a);
    Polity* pb = polity(b);
    if (!pa || !pb) return 0;
    auto drop = [](Polity& p, u16 other) {
        p.wars.erase(std::remove_if(p.wars.begin(), p.wars.end(), [&](const War& w) { return w.enemy == other; }),
                     p.wars.end());
        if (p.op.active && p.op.enemy == other) p.op.active = false;
    };
    drop(*pa, b);
    drop(*pb, a);
    discharge(a);
    discharge(b);
    pa->attitude_ref(b) = std::max(pa->attitude_to(b), -0.3f);
    pb->attitude_ref(a) = std::max(pb->attitude_to(a), -0.3f);
    // A truce: no new war between them for a while, and the worst of it is let go.
    set_truce(a, b, 8.0f);
    pa->diplo_ref(b).grievance *= 0.5f;
    pb->diplo_ref(a).grievance *= 0.5f;
    Event e;
    e.type = EventType::Peace;
    e.severity = 4;
    e.polity = a;
    e.causes[0] = cause;
    e.text = strfmt("「%s」与「%s」%s", title(a).c_str(), title(b).c_str(), how.c_str());
    return ctx_.chron->emit(std::move(e));
}

int Society::soldiers(u16 id) const {
    int n = 0;
    for (const auto& cp : ctx_.agents->all())
        if (cp && cp->alive && !cp->departed && cp->polity == id && cp->drafted) ++n;
    return n;
}

static bool fit_to_serve(const SimContext& ctx, const Character& c, u16 polity) {
    return c.alive && !c.departed && !c.is_girl() && c.polity == polity && !c.drafted && c.body.can_hold() &&
           c.body.mobility() >= 0.6f && !ctx.agents->is_child(c) && !ctx.agents->is_elder(c);
}

int Society::draftable(u16 id) const {
    int n = 0;
    for (const auto& cp : ctx_.agents->all())
        if (cp && fit_to_serve(ctx_, *cp, id)) ++n;
    return n;
}

int Society::draft(u16 id, int n, EventId cause) {
    Polity* p = polity(id);
    if (!p) return 0;
    std::vector<std::pair<float, Character*>> pool;
    for (const auto& cp : ctx_.agents->all()) {
        Character* c = cp.get();
        if (!c || !fit_to_serve(ctx_, *c, id)) continue;
        float fit = c->pers.aggression + c->skills[kCombat] + 0.3f * c->pers.conformity - 0.3f * c->pers.caution +
                    0.2f * c->body.vitality;
        pool.push_back({fit, c});
    }
    std::sort(pool.begin(), pool.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first > y.first : x.second->id < y.second->id;
    });
    int have = soldiers(id);
    int want = std::max(0, n - have);
    for (int i = 0; i < want && i < (int)pool.size(); ++i) {
        Character* c = pool[(size_t)i].second;
        c->drafted = true;
        c->task = Task{};
        c->next_think = ctx_.now;
        c->remember(ctx_.now, MemoryKind::LostJob, p->ruler, -0.05f, cause);
    }
    p->policies.army = std::max(p->policies.army, n);
    return soldiers(id);
}

void Society::discharge(u16 id) {
    Polity* p = polity(id);
    if (!p) return;
    for (const auto& cp : ctx_.agents->all())
        if (cp && cp->polity == id && cp->drafted) {
            cp->drafted = false;
            cp->next_think = ctx_.now;
        }
    p->policies.army = 0;
}

void Society::annex(u16 winner, u16 loser, EventId cause, const std::string& how) {
    Polity* w = polity(winner);
    Polity* l = polity(loser);
    if (!w || !l) return;
    const std::string old_title = title(loser);
    for (const auto& cp : ctx_.agents->all()) {
        Character* c = cp.get();
        if (!c || c->polity != loser) continue;
        c->polity = winner;
        c->drafted = false;
        c->support_ref(w->ruler) = std::min(c->support_for(w->ruler), -0.2f);
        c->remember(ctx_.now, MemoryKind::HomeLost, w->ruler, -0.3f, cause);
        if (Store* inv = ctx_.econ->store(c->inv)) inv->polity = winner;
        if (c->is_girl()) {
            c->girl->role = "none";
            c->girl->loyalty = -0.4f;
            c->girl->grudge = w->ruler;
        }
    }
    for (const Store& s : ctx_.econ->stores())
        if (s.alive && s.polity == loser) ctx_.econ->store(s.id)->polity = winner;
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.polity == loser) ctx_.buildings->get(b.id)->polity = winner;
    for (const Farm& f : ctx_.farming->all())
        if (f.alive && f.polity == loser) ctx_.farming->get(f.id)->polity = winner;
    if (w->war_with(loser)) make_peace(winner, loser, how.empty() ? "的战争以征服告终" : "的战争随之结束", cause);
    Event e;
    e.type = EventType::Coup;  // a change of sovereignty
    e.severity = 5;
    e.polity = winner;
    e.causes[0] = cause;
    e.text = how.empty() ? strfmt("「%s」征服了「%s」，其人民与土地并入", title(winner).c_str(), old_title.c_str())
                         : strfmt("「%s」%s「%s」", old_title.c_str(), how.c_str(), title(winner).c_str());
    last_merge_ = ctx_.chron->emit(std::move(e));
    l->alive = false;
    // The magical girls of a conquered people bear it hardest.
    for (const auto& cp : ctx_.agents->all())
        if (cp && cp->alive && cp->girl && cp->polity == winner && cp->girl->grudge == w->ruler)
            ctx_.agents->mark_girl(cp->id, 0.6f, 0.0f, last_merge_);
    ctx_.agents->mark_girl(w->ruler, 0.0f, 0.4f, last_merge_);
}

void Society::update_wars(Polity& p) {
    if (p.wars.empty()) return;
    // Losses and kills from the chronicle's deaths are tallied by the agents; here the
    // operation advances and morale is judged.
    Operation& op = p.op;
    for (War& w : p.wars) {
        Polity* enemy = polity(w.enemy);
        if (!enemy) {
            w.enemy = 0;
            continue;
        }
    }
    p.wars.erase(std::remove_if(p.wars.begin(), p.wars.end(), [](const War& w) { return w.enemy == 0; }), p.wars.end());
    // A war nobody fights any more fizzles out: after four quiet days both sides let it
    // lie (no army out on either side, nobody killed).
    for (const War& w : p.wars) {
        const Polity* en = polity(w.enemy);
        const War* back = en ? en->war_with(p.id) : nullptr;
        const Tick quiet = std::max(w.active_at, back ? back->active_at : (Tick)0);
        if (!p.op.active && en && !en->op.active && ctx_.now > quiet + kTicksPerDay * 4) {
            make_peace(p.id, w.enemy, "的战事渐渐平息，双方默契停战", w.event);
            return;  // the list changed
        }
    }
    // War-weariness at home: after the first day the peaceable tire of the war and of
    // the ruler who keeps it going, faster when there is hunger.
    Tick oldest = ctx_.now;
    for (const War& w : p.wars) oldest = std::min(oldest, w.since);
    if (!p.wars.empty() && ctx_.now - oldest > kTicksPerDay) {
        bool hungry = false;
        for (const Crisis& c : p.crises)
            if (c.active && c.kind == CrisisKind::Food) hungry = true;
        for (const auto& cp : ctx_.agents->all()) {
            Character* c = cp.get();
            if (!c || !c->alive || c->departed || c->is_girl() || c->polity != p.id) continue;
            const float drop = 0.0015f * (1.0f - c->pers.aggression) * (hungry ? 2.0f : 1.0f);
            c->support_ref(p.ruler) = clampv(c->support_for(p.ruler) - drop, -1.0f, 1.0f);
        }
    }
    if (!op.active) return;
    if (!polity(op.enemy) || !p.war_with(op.enemy)) {
        op.active = false;
        return;
    }
    int serving = soldiers(p.id);
    // Nobody could be called up (or everyone has already gone home): the operation
    // simply lapses.
    if (serving == 0 && op.lost == 0) {
        op.active = false;
        return;
    }
    // Broken army: a third fallen (the rest lose heart), or nobody left.
    if (serving == 0 || (op.party > 0 && op.lost * 3 >= op.party + 1)) {
        if (op.phase < 3) {
            op.phase = 3;
            op.since = ctx_.now;
            Event e;
            e.type = EventType::Battle;
            e.severity = 4;
            e.polity = p.id;
            e.causes[0] = op.event;
            e.text = op.aim == "defend"
                         ? strfmt("「%s」的守军伤亡惨重，溃散了（损失 %d 人）", title(p.id).c_str(), op.lost)
                         : strfmt("「%s」的军队伤亡惨重，撤退了（损失 %d 人）", title(p.id).c_str(), op.lost);
            const EventId ev = ctx_.chron->emit(std::move(e));
            // The magical girls who led them carry the defeat; those who broke them, the triumph.
            for (const auto& cp : ctx_.agents->all()) {
                if (!cp || !cp->alive || !cp->girl) continue;
                if (cp->drafted && cp->polity == p.id) ctx_.agents->mark_girl(cp->id, 0.25f, 0.0f, ev);
                else if (cp->polity == op.enemy && (cp->drafted || cp->id == (polity(op.enemy) ? polity(op.enemy)->ruler : 0)))
                    ctx_.agents->mark_girl(cp->id, 0.0f, 0.2f, ev);
            }
        }
    }
    // Muster → march once most soldiers have gathered (or after two hours).
    if (op.phase == 0) {
        int gathered = 0;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && cp->polity == p.id && cp->drafted && cp->foot.dist2(op.rally) < 10 * 10) ++gathered;
        // An army does not set out into the night (half of it would fall asleep by the road).
        if (!is_night(ctx_.now) && ((serving > 0 && gathered * 3 >= serving * 2) || ctx_.now - op.since > kTicksPerHour * 2)) {
            op.phase = 1;
            op.party = serving;
            op.lost = 0;
            op.since = ctx_.now;
        }
    }
    // The march ends outside the enemy's village: the army forms up there, and falls on
    // the objective together once most have come in (or the stragglers are given up on).
    if (op.phase == 1) {
        int formed = 0;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && cp->polity == p.id && cp->drafted &&
                (cp->foot.dist2(op.stage) < 14 * 14 || cp->foot.dist2(op.objective) < 16 * 16))
                ++formed;
        if (formed > 0 && op.staged_since == 0) op.staged_since = ctx_.now;
        // Stragglers are waited for, through the night if need be.
        const bool waited = op.staged_since && ctx_.now - op.staged_since > kTicksPerHour * 3 / 2 && !is_night(ctx_.now);
        if (serving > 0 && (formed * 3 >= serving * 2 || waited)) {
            op.phase = 2;
            op.since = ctx_.now;
            if (op.aim != "defend") {
                const Polity* en = polity(op.enemy);
                Event e;
                e.type = EventType::Battle;
                e.severity = 4;
                e.polity = p.id;
                e.pos = op.objective;
                e.causes[0] = op.event;
                e.text = op.aim == "raid"
                             ? strfmt("「%s」的劫掠队（%d 人）冲进了「%s」的村子", p.name.c_str(), formed, en ? en->name.c_str() : "?")
                             : strfmt("「%s」的大军（%d 人）兵临「%s」的议事厅", p.name.c_str(), formed, en ? en->name.c_str() : "?");
                op.event = ctx_.chron->emit(std::move(e));
            }
        } else if (ctx_.now - op.since > kTicksPerDay * 3 / 4) {
            op.phase = 3;  // could not get there
            op.since = ctx_.now;
        }
    }
    if (op.phase == 2) {
        // A raid lasts until the raiders are laden or nothing is left to take nearby
        // (after a first half hour), and at most two hours.
        bool done = false;
        if (op.aim == "raid" && ctx_.now - op.since > kTicksPerHour / 2) {
            int food_left = 0;
            for (StoreId sid : public_stores(op.enemy)) {
                const Store* s = ctx_.econ->store(sid);
                if (s && s->pos.dist2(op.objective) < 45 * 45) food_left += food_in(sid);
            }
            int present = 0, laden = 0;
            for (const auto& cp : ctx_.agents->all()) {
                const Character* c = cp.get();
                if (!c || !c->alive || c->polity != p.id || !c->drafted || c->is_girl() || c->foot.dist2(op.objective) > 50 * 50) continue;
                ++present;
                if (ctx_.agents->raider_laden(*c)) ++laden;
            }
            done = food_left == 0 || present == 0 || laden * 3 >= present * 2;
        }
        if (op.aim == "raid" && (done || ctx_.now - op.since >= kTicksPerHour * 2)) {
            op.phase = 3;
            op.since = ctx_.now;
        } else if (op.aim == "conquest") {
            // A siege: the hall falls once the attackers have held the ground around it,
            // with no defender standing there, for some hours.
            int defenders = 0;
            for (const auto& cp : ctx_.agents->all())
                if (cp && cp->alive && cp->polity == op.enemy && (cp->drafted || cp->is_girl()) &&
                    cp->foot.dist2(op.objective) < 20 * 20)
                    ++defenders;
            if (defenders > 0) op.held_since = 0;
            else if (op.held_since == 0) op.held_since = ctx_.now;
            if (op.held_since && ctx_.now - op.held_since >= kTicksPerHour * kSiegeHours) {
                // A people with the strength left to rebuild bows to the victor and pays
                // tribute; a broken one is absorbed.
                const u16 enemy = op.enemy;
                int adults = 0;
                bool led = false;
                for (const auto& cp : ctx_.agents->all()) {
                    const Character* c = cp.get();
                    if (!c || !c->alive || c->departed || c->polity != enemy) continue;
                    if (c->is_girl()) led = true;
                    else if (!ctx_.agents->is_child(*c)) ++adults;
                }
                const EventId siege = op.event;
                op.active = false;
                op.held_since = 0;
                discharge(p.id);
                if (adults <= kAnnexBelow || !led) {
                    annex(p.id, enemy, siege);
                } else {
                    const EventId ev = make_peace(p.id, enemy,
                                                  strfmt("订立城下之盟：「%s」兵临城下，「%s」被迫称臣纳贡",
                                                         p.name.c_str(), polity(enemy) ? polity(enemy)->name.c_str() : "?"),
                                                  siege);
                    make_vassal(enemy, p.id, ev);
                    send_tribute(enemy, p.id, 30, "战败赔款", ev);
                    add_grievance(enemy, p.id, 0.6f);  // a humiliation not soon forgotten
                }
                return;
            }
            // A siege that cannot be made to hold is given up after a day.
            if (ctx_.now - op.since > kTicksPerDay) {
                op.phase = 3;
                op.since = ctx_.now;
                op.held_since = 0;
            }
        }
    }
    // Back home: the operation ends and soldiers return to work.
    if (op.phase == 3) {
        int home = 0;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && cp->polity == p.id && cp->drafted && cp->foot.dist2(op.rally) < 12 * 12) ++home;
        if (home * 3 >= serving * 2 || ctx_.now - op.since > kTicksPerDay / 2) {
            // What the undertaking came to.
            const Polity* en = polity(op.enemy);
            const std::string enemy = en ? en->name : "?";
            Event e;
            e.type = EventType::Battle;
            e.severity = 3;
            e.polity = p.id;
            e.causes[0] = op.event;
            if (op.aim == "raid")
                e.text = op.loot > 0 ? strfmt("「%s」劫掠「%s」的队伍回来了：抢回 %d 份粮食（出动 %d 人，折损 %d 人）",
                                              p.name.c_str(), enemy.c_str(), op.loot, op.party, op.lost)
                                     : strfmt("「%s」劫掠「%s」的队伍空手而归（出动 %d 人，折损 %d 人）", p.name.c_str(),
                                              enemy.c_str(), op.party, op.lost);
            else if (op.aim == "conquest")
                e.text = strfmt("「%s」进攻「%s」未能得手，军队撤回（出动 %d 人，折损 %d 人）", p.name.c_str(), enemy.c_str(),
                                op.party, op.lost);
            else
                e.text = strfmt("「%s」的守军解散回家（折损 %d 人）", p.name.c_str(), op.lost);
            e.data.set("loot", op.loot);
            ctx_.chron->emit(std::move(e));
            op.active = false;
            discharge(p.id);
        }
    }
}

}  // namespace icarus
