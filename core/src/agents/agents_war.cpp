// Agents: soldiering. Drafted residents follow their polity's operation and fight the
// enemy's fighters (soldiers and magical girls; civilians are left alone) with whatever
// weapon they carry. Hits take voxels off real bodies; armour and skill matter; raiders
// carry off real food from the enemy's storehouse.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/physics.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

Character* Agents::nearest_enemy(const Character& c, float radius, bool fighters_only, bool soldiers_only) {
    const Polity* p = ctx_.society->polity(c.polity);
    if (!p || p->wars.empty()) return nullptr;
    Character* best = nullptr;
    float bd = radius * radius;
    for (auto& op : chars_) {
        Character* o = op.get();
        if (!o || !o->alive || o->departed || o->polity == c.polity || !p->war_with(o->polity)) continue;
        if (fighters_only && !o->drafted && !o->is_girl()) continue;
        if (soldiers_only && !o->drafted) continue;
        float d = o->pos.dist_sq(c.pos);
        if (d < bd || (d == bd && best && o->id < best->id)) {
            bd = d;
            best = o;
        }
    }
    return best;
}

void Agents::strike(Character& attacker, Character& target, float power, const std::string& how, EventId cause) {
    Polity* ap = ctx_.society->polity(attacker.polity);
    Polity* tp = ctx_.society->polity(target.polity);
    const bool was_alive = target.alive;
    damage(target, power, -1, how, cause);
    target.fear = std::min(1.0f, target.fear + 0.1f);
    target.affinity_ref(attacker.id) = clampv(target.affinity(attacker.id) - 0.2f, -1.0f, 1.0f);
    if (was_alive && !target.alive) {
        if (ap)
            if (War* w = ap->war_with(target.polity)) w->kills++;
        if (tp) {
            if (War* w = tp->war_with(attacker.polity)) w->losses++;
            if (tp->op.active && target.drafted) tp->op.lost++;
        }
        attacker.skills[kCombat] = std::min(1.0f, attacker.skills[kCombat] + 0.03f);
    }
}

bool Agents::task_fight(Character& c) {
    Task& t = c.task;
    Polity* p = ctx_.society->polity(c.polity);
    if (!p || !c.drafted || p->wars.empty()) {
        end_task(c, true);
        return true;
    }
    const Operation& op = p->op;
    const EventId cause = op.active ? op.event : (p->wars.empty() ? 0 : p->wars.front().event);
    const ItemDef* weapon = c.weapon != kNoItem ? &ctx_.reg->item(c.weapon) : nullptr;
    const float reach = weapon ? std::max(1.5f, weapon->range) : 1.5f;
    // Fight whoever of the enemy's fighters is close.
    // Raiders are after the food and fight only soldiers in their way; conquerors and
    // defenders also face the enemy's magical girls.
    const bool raiding = op.active && op.aim == "raid";
    Character* foe = nearest_enemy(c, op.active && op.phase == 3 ? 6.0f : 18.0f, true, raiding);
    if (foe) {
        if (!op.engaged && op.active) {
            Event e;
            e.type = EventType::Battle;
            e.severity = 4;
            e.polity = c.polity;
            e.pos = c.foot;
            e.causes[0] = cause;
            const Polity* fp = ctx_.society->polity(foe->polity);
            e.text = strfmt("「%s」与「%s」的军队交战", p->name.c_str(), fp ? fp->name.c_str() : "?");
            p->op.event = ctx_.chron->emit(std::move(e));
            p->op.engaged = true;
        }
        float d = std::sqrt(foe->pos.dist_sq(c.pos));
        if (d <= reach + 0.4f) {
            c.path.clear();
            c.moving = false;
            c.yaw = std::atan2(foe->pos.x - c.pos.x, foe->pos.z - c.pos.z);
            say(c, "与" + foe->name + "交战");
            if (now_ >= t.until) {
                t.until = now_ + (weapon && weapon->range > 3.0f ? 45 : 28);
                float skill = c.skills[kCombat] - foe->skills[kCombat];
                float hit = clampv(0.55f + 0.35f * skill + (c.fear > 0.6f ? -0.15f : 0.0f), 0.15f, 0.9f);
                if (rng_.chance(hit)) {
                    float power = weapon ? weapon->power : 0.03f;
                    power *= 0.8f + 0.4f * c.body.manipulation();
                    std::string how = weapon ? (weapon->range > 3.0f ? "中箭" : "被" + weapon->name + "所伤") : "被拳脚所伤";
                    strike(c, *foe, power, how, p->op.event ? p->op.event : cause);
                }
                c.skills[kCombat] = std::min(1.0f, c.skills[kCombat] + 0.002f);
            }
            return true;
        }
        // Close in (re-plan only when the foe has moved on).
        if (t.target2.dist2(foe->foot) > 4 || !c.path.valid()) t.target2 = foe->foot;
        Move m = move_to(c, t.target2, true);
        if (m == Move::Failed) blacklist(c, t.target2, 60);
        say(c, "冲向" + foe->name);
        return true;
    }
    // No foe at hand: follow the operation, or guard home.
    Vec3i goal = c.foot;
    std::string what = "戒备";
    const Building* seat = ctx_.buildings->get(p->seat);
    if (op.active) {
        if (op.phase == 0 || op.phase == 3) {
            goal = op.rally;
            what = op.phase == 0 ? "集结" : "撤回";
        } else {
            goal = op.objective;
            what = op.aim == "raid" ? "劫掠" : (op.aim == "conquest" ? "进攻" : "据守");
        }
        // Raiders at the enemy storehouse carry off food.
        if (op.phase == 2 && op.aim == "raid" && c.foot.dist2(op.objective) < 4 * 4) {
            for (StoreId sid : ctx_.society->public_stores(op.enemy)) {
                const Store* s = ctx_.econ->store(sid);
                if (!s || s->pos.dist2(op.objective) > 3 * 3) continue;
                std::vector<ItemStack> items = s->items;
                for (const ItemStack& st : items) {
                    if (ctx_.reg->item(st.item).nutrition <= 0) continue;
                    float unit = std::max(0.1f, ctx_.reg->item(st.item).weight);
                    i32 room = (i32)std::floor((tune.carry_capacity + 4.0f - carried_weight(c)) / unit);
                    if (room > 0) ctx_.econ->transfer(sid, c.inv, st.item, std::min(room, st.count));
                }
            }
            say(c, "搬走敌人的粮食");
        }
        // Back home with plunder: into our own stores.
        if (op.phase == 3 && c.foot.dist2(op.rally) < 6 * 6)
            if (StoreId home = nearest_storage(c.polity, c.foot, kNoItem)) {
                const Store* hs = ctx_.econ->store(home);
                if (hs && hs->pos.dist2(c.foot) < 5 * 5) deposit_all(c, home);
            }
    } else if (seat) {
        goal = seat->entrance;
    }
    if (c.foot.dist2(goal) > 3 * 3) {
        Move m = move_to(c, goal, true);
        if (m == Move::Failed) {
            blacklist(c, goal, kTicksPerHour);
            end_task(c, false);
            return false;
        }
        say(c, what);
    } else {
        c.moving = false;
        say(c, what);
    }
    return true;
}

}  // namespace icarus
