// Agents: migration. Residents are not bound to their ruler: one who is hungry or
// resents her, and sees a neighbour that feeds its people better or is ruled by someone
// they favour, may pack up and go over. They walk there (the road must exist), take
// what they carry, and are remembered as a loss by the polity they left.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

float Agents::migration_pull(const Character& c, const Polity& own, const Polity*& dest, std::string& why) const {
    dest = nullptr;
    if (c.is_girl() || c.drafted || !c.alive) return 0.0f;
    const float own_support = c.support_for(own.ruler);
    const float hunger = 1.0f - c.needs.food;
    const float grievance = std::max(0.0f, -own_support) * 0.8f + std::max(0.0f, hunger - 0.3f) * 1.2f +
                            std::max(0.0f, 0.5f - c.mood) * 0.8f;
    if (grievance < 0.15f) return 0.0f;
    float best = 0.0f;
    for (const Polity& q : ctx_.society->polities()) {
        if (!q.alive || q.id == own.id || !q.seat) continue;
        float friends = 0.0f;
        for (const Relation& r : c.relations)
            if (r.affinity > 0.3f)
                if (const Character* o = get(r.other); o && o->alive && o->polity == q.id) friends += r.affinity;
        float pull = 0.6f * std::max(0.0f, c.support_for(q.ruler) - own_support) +
                     0.9f * std::max(0.0f, q.stats.food_access - own.stats.food_access) +
                     0.5f * std::max(0.0f, q.stats.mood - own.stats.mood) + 0.3f * std::min(1.0f, friends);
        if (own.war_with(q.id)) pull -= 0.3f;  // going over to the enemy takes more
        if (pull > best) {
            best = pull;
            dest = &q;
        }
    }
    if (!dest) return 0.0f;
    const float score = grievance * best * (1.3f - c.pers.conformity) * (1.0f - 0.5f * c.pers.caution);
    why = strfmt("在「%s」%s，「%s」%s", own.name.c_str(), hunger > 0.5f ? "挨饿" : "心怀不满", dest->name.c_str(),
                 dest->stats.food_access > own.stats.food_access + 0.2f ? "的人吃得饱" : "更合心意");
    return score;
}

bool Agents::task_leave(Character& c) {
    Task& t = c.task;
    const Polity* q = ctx_.society->polity((u16)t.count);
    const Building* seat = q ? ctx_.buildings->get(q->seat) : nullptr;
    if (!q || !seat || q->id == c.polity) {
        end_task(c, false);
        return false;
    }
    t.target = seat->entrance;
    if (c.foot.dist2(t.target) <= 3 * 3) {
        // The latest bitter memory is why they left.
        EventId why = 0;
        for (auto it = c.memories.rbegin(); it != c.memories.rend() && !why; ++it)
            if (it->valence < -0.05f && it->event) why = it->event;
        defect(c, q->id, why);
        end_task(c, true);
        return true;
    }
    Move m = move_to(c, t.target, true);
    if (m == Move::Failed) {
        blacklist(c, t.target, kTicksPerDay);
        end_task(c, false);
        return false;
    }
    say(c, "前往「" + q->name + "」");
    return true;
}

void Agents::defect(Character& c, u16 to, EventId cause) {
    Polity* from = ctx_.society->polity(c.polity);
    Polity* dest = ctx_.society->polity(to);
    if (!dest || c.polity == to) return;
    const bool enemy = from && from->war_with(to);
    Event e;
    e.type = EventType::Migration;
    e.severity = 3;
    e.actor = c.id;
    e.polity = c.polity;
    e.pos = c.foot;
    e.causes[0] = cause;
    e.text = strfmt("%s离开「%s」，%s「%s」", c.name.c_str(), from ? from->name.c_str() : "?", enemy ? "投向敌国" : "投奔",
                    dest->name.c_str());
    e.data.set("to", (int)to);
    const EventId ev = ctx_.chron->emit(std::move(e));
    if (from) c.support_ref(from->ruler) = std::min(c.support_for(from->ruler), -0.3f);
    c.support_ref(dest->ruler) = std::max(c.support_for(dest->ruler), 0.2f);
    c.polity = to;
    c.drafted = false;
    c.home = 0;
    c.occupation.clear();
    if (Store* inv = ctx_.econ->store(c.inv)) inv->polity = to;
    c.remember(now_, MemoryKind::Migrated, dest->ruler, 0.15f, ev);
    c.next_think = now_;
}

}  // namespace icarus
