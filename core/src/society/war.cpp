// Society: war and peace. Wars are declared by rulers through decisions; armies are
// real drafted residents who arm themselves from the storehouses and follow one
// operation at a time (muster at home, march on the objective, raid or take it, come
// back). Nothing is abstract: soldiers walk, fight with voxel bodies and die, and what
// a raid carries off is really hauled away.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

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
    War wa;
    wa.enemy = defender;
    wa.attacker = true;
    wa.aim = aim;
    wa.since = ctx_.now;
    wa.event = ev;
    a->wars.push_back(wa);
    War wd;
    wd.enemy = attacker;
    wd.attacker = false;
    wd.aim = "defend";
    wd.since = ctx_.now;
    wd.event = ev;
    d->wars.push_back(wd);
    a->attitude_ref(defender) = std::min(a->attitude_to(defender), -0.6f);
    d->attitude_ref(attacker) = std::min(d->attitude_to(attacker), -0.8f);
    // The attackers march on the enemy seat (a raid aims at its storehouse).
    const Building* home = ctx_.buildings->get(a->seat);
    const Building* target = ctx_.buildings->get(d->seat);
    Vec3i objective = target ? target->entrance : Vec3i{};
    if (aim == "raid") {
        float bd = 1e30f;
        for (StoreId sid : public_stores(defender)) {
            const Store* s = ctx_.econ->store(sid);
            if (!s || s->kind != StoreKind::Stockpile) continue;
            float dd = home ? (float)s->pos.dist2(home->entrance) : 0.0f;
            if (dd < bd) {
                bd = dd;
                objective = s->pos;
            }
        }
    }
    a->op = Operation{};
    a->op.active = true;
    a->op.enemy = defender;
    a->op.aim = aim;
    a->op.rally = home ? home->entrance : objective;
    a->op.objective = objective;
    a->op.phase = 0;
    a->op.since = ctx_.now;
    a->op.event = ev;
    return ev;
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

static bool fit_to_serve(const Character& c, u16 polity) {
    return c.alive && !c.departed && !c.is_girl() && c.polity == polity && !c.drafted && c.body.can_hold() &&
           c.body.mobility() >= 0.6f;
}

int Society::draftable(u16 id) const {
    int n = 0;
    for (const auto& cp : ctx_.agents->all())
        if (cp && fit_to_serve(*cp, id)) ++n;
    return n;
}

int Society::draft(u16 id, int n, EventId cause) {
    Polity* p = polity(id);
    if (!p) return 0;
    std::vector<std::pair<float, Character*>> pool;
    for (const auto& cp : ctx_.agents->all()) {
        Character* c = cp.get();
        if (!c || !fit_to_serve(*c, id)) continue;
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
    if (w->war_with(loser)) make_peace(winner, loser, "的战争以征服告终", cause);
    Event e;
    e.type = EventType::Coup;  // a change of sovereignty
    e.severity = 5;
    e.polity = winner;
    e.causes[0] = cause;
    e.text = how.empty() ? strfmt("「%s」征服了「%s」，其人民与土地并入", title(winner).c_str(), old_title.c_str())
                         : strfmt("「%s」%s「%s」", old_title.c_str(), how.c_str(), title(winner).c_str());
    ctx_.chron->emit(std::move(e));
    l->alive = false;
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
    // Broken army: too many lost, or nobody left.
    if (serving == 0 || (op.party > 0 && op.lost * 2 >= op.party + 1)) {
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
            ctx_.chron->emit(std::move(e));
        }
    }
    // Muster → march once most soldiers have gathered (or after two hours).
    if (op.phase == 0) {
        int gathered = 0;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && cp->polity == p.id && cp->drafted && cp->foot.dist2(op.rally) < 10 * 10) ++gathered;
        if ((serving > 0 && gathered * 3 >= serving * 2) || ctx_.now - op.since > kTicksPerHour * 2) {
            op.phase = 1;
            op.party = serving;
            op.lost = 0;
            op.since = ctx_.now;
        }
    }
    // Arrival at the objective.
    if (op.phase == 1) {
        int there = 0;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && cp->polity == p.id && cp->drafted && cp->foot.dist2(op.objective) < 8 * 8) ++there;
        if (serving > 0 && there * 2 >= serving) {
            op.phase = 2;
            op.since = ctx_.now;
        } else if (ctx_.now - op.since > kTicksPerDay / 2) {
            op.phase = 3;  // could not get there
            op.since = ctx_.now;
        }
    }
    if (op.phase == 2) {
        if (op.aim == "raid" && ctx_.now - op.since > kTicksPerHour) {
            op.phase = 3;
            op.since = ctx_.now;
        } else if (op.aim == "conquest") {
            // Taken when no defender stands near the seat any more.
            int defenders = 0;
            for (const auto& cp : ctx_.agents->all())
                if (cp && cp->alive && cp->polity == op.enemy && (cp->drafted || cp->is_girl()) &&
                    cp->foot.dist2(op.objective) < 20 * 20)
                    ++defenders;
            if (defenders == 0) {
                annex(p.id, op.enemy, op.event);
                op.active = false;
                discharge(p.id);
                return;
            }
        }
    }
    // Back home: the operation ends and soldiers return to work.
    if (op.phase == 3) {
        int home = 0;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && cp->polity == p.id && cp->drafted && cp->foot.dist2(op.rally) < 12 * 12) ++home;
        if (home * 3 >= serving * 2 || ctx_.now - op.since > kTicksPerDay / 2) {
            op.active = false;
            discharge(p.id);
        }
    }
}

}  // namespace icarus
