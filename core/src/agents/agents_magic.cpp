// Agents: magic a magical girl casts on her own initiative. Strategic spells (feasts,
// terror, growth) are chosen through the decision pipeline; these are the immediate,
// local ones: mending the injured and putting out fires. Every cast spends mana and
// acts on real matter and real bodies.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/physics.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
const Json* active_spell(const Registry& reg, const GirlData& g, const std::string& effect) {
    for (const Json& d : reg.doc("drives")["drives"].items()) {
        if (d.str("key") != g.drive) continue;
        for (const Json& sp : d["spells"].items())
            if (sp.str("effect") == effect && sp.str("type") == "active" && g.level >= sp.integer("level", 1)) return &sp;
    }
    return nullptr;
}

bool has_passive(const Registry& reg, const GirlData& g, const std::string& effect) {
    for (const Json& d : reg.doc("drives")["drives"].items()) {
        if (d.str("key") != g.drive) continue;
        for (const Json& sp : d["spells"].items())
            if (sp.str("effect") == effect && sp.str("type") == "passive" && g.level >= sp.integer("level", 1)) return true;
    }
    return false;
}

float injury_of(const Character& c) {
    float missing = 1.0f - (float)c.body.total_alive() / (float)std::max(1, c.body.total_voxels());
    return missing + c.body.bleeding * 0.5f;
}
}  // namespace

float Agents::pick_spell(Character& c, SpellPick& out, std::string& why) {
    if (!c.is_girl() || c.body.fatal()) return 0;
    const GirlData& g = *c.girl;
    float best = 0;
    if (const Json* sp = active_spell(*ctx_.reg, g, "heal")) {
        float cost = sp->flt("mana", 0.35f);
        if (g.mana >= cost) {
            Character* target = nullptr;
            float worst = 0.06f;
            for (auto& op : chars_) {
                if (!op || !op->alive || op->departed || op->polity != c.polity) continue;
                if (op->foot.dist2(c.foot) > 45 * 45) continue;
                float inj = injury_of(*op);
                if (inj > worst) {
                    worst = inj;
                    target = op.get();
                }
            }
            if (target) {
                float s = (0.4f + 2.0f * worst) * (0.5f + c.pers.altruism);
                if (s > best) {
                    best = s;
                    out = {1, target->foot, target->id, cost, sp->str("name")};
                    why = strfmt("%s伤势 %.0f%%，可以用「%s」治疗", target->name.c_str(), worst * 100.0f, sp->str("name").c_str());
                }
            }
        }
    }
    if (const Json* sp = active_spell(*ctx_.reg, g, "quench")) {
        float cost = sp->flt("mana", 0.45f);
        if (g.mana >= cost && ctx_.physics->stats().fire_active > 0) {
            Vec3i fire;
            i64 bd = 50LL * 50;
            bool found = false;
            for (const Vec3i& f : ctx_.physics->fire_positions()) {
                i64 d = f.dist2(c.foot);
                if (d < bd) {
                    bd = d;
                    fire = f;
                    found = true;
                }
            }
            if (found) {
                float s = 1.1f + 0.4f * c.pers.altruism;
                if (s > best) {
                    best = s;
                    out = {2, fire, kNoEntity, cost, sp->str("name")};
                    why = strfmt("附近起火了，可以用「%s」扑灭", sp->str("name").c_str());
                }
            }
        }
    }
    return best;
}

bool Agents::task_cast(Character& c) {
    Task& t = c.task;
    if (!c.is_girl()) {
        end_task(c, false);
        return false;
    }
    Character* who = t.other ? get(t.other) : nullptr;
    if (t.count == 1 && (!who || !who->alive)) {
        end_task(c, false);
        return false;
    }
    Vec3i goal = who ? who->foot : t.target;
    if (t.step == 0) {
        say(c, "赶去施法：" + t.label);
        if (c.foot.dist2(goal) > 4 * 4) {
            Move m = move_to(c, goal, true);
            if (m == Move::Failed) {
                blacklist(c, goal, kTicksPerHour);
                end_task(c, false);
                return false;
            }
            if (m == Move::Moving && now_ - t.started < kTicksPerHour) return true;
            if (m == Move::Moving) {
                end_task(c, false);
                return false;
            }
        }
        t.step = 1;
        t.until = now_ + 40;
        c.path.clear();
    }
    if (t.step == 1) {
        say(c, "施展「" + t.label + "」");
        c.yaw = std::atan2((float)goal.x + 0.5f - c.pos.x, (float)goal.z + 0.5f - c.pos.z);
        if (now_ < t.until) return true;
        GirlData& g = *c.girl;
        const Json* sp = active_spell(*ctx_.reg, g, t.count == 1 ? "heal" : "quench");
        float cost = sp ? sp->flt("mana", 0.4f) : 0.4f;
        if (g.mana < cost) {
            end_task(c, false);
            return false;
        }
        g.mana -= cost;
        g.xp += 3.0f;
        Event e;
        e.type = EventType::SpellCast;
        e.actor = c.id;
        e.polity = c.polity;
        e.pos = goal;
        if (t.count == 1 && who) {
            // Mending takes from the patient's own strength (food); severed limbs regrow
            // only with 再生之种.
            bool limbs = has_passive(*ctx_.reg, g, "regrow_limbs");
            int missing = who->body.total_voxels() - who->body.total_alive();
            int grow = std::min(missing, (int)(who->body.total_voxels() * 0.25f));
            int done = who->body.regrow(grow, who->look, limbs);
            who->body.bleeding = 0;
            who->body.vitality = std::min(1.0f, who->body.vitality + 0.3f);
            who->needs.food = std::max(0.0f, who->needs.food - 0.0012f * (float)done * (limbs ? 1.6f : 1.0f));
            who->remember(now_, MemoryKind::Healed, c.id, 0.25f, 0);
            who->support_ref(c.id) = clampv(who->support_for(c.id) + 0.12f, -1.0f, 1.0f);
            who->affinity_ref(c.id) = clampv(who->affinity(c.id) + 0.2f, -1.0f, 1.0f);
            e.severity = 2;
            e.target = who->id;
            e.text = strfmt("%s以「%s」治愈了%s", c.name.c_str(), t.label.c_str(), who->name.c_str());
        } else {
            int n = 0;
            World& w = *ctx_.world;
            for (int dy = -3; dy <= 4; ++dy)
                for (int dz = -7; dz <= 7; ++dz)
                    for (int dx = -7; dx <= 7; ++dx) {
                        if (dx * dx + dz * dz + dy * dy > 49) continue;
                        Vec3i q = goal + Vec3i{dx, dy, dz};
                        Voxel v = w.get(q);
                        if (!vburning(v)) continue;
                        w.set(q, make_voxel(vmat(v), vlevel(v), vdamage(v), false));
                        ++n;
                    }
            e.severity = 3;
            e.text = strfmt("%s以「%s」扑灭了 %d 处火焰", c.name.c_str(), t.label.c_str(), n);
            for (auto& op : chars_)
                if (op && op->alive && op->polity == c.polity && op->foot.dist2(goal) < 40 * 40)
                    op->support_ref(c.id) = clampv(op->support_for(c.id) + 0.05f, -1.0f, 1.0f);
        }
        ctx_.chron->emit(std::move(e));
        end_task(c, true);
    }
    return true;
}

}  // namespace icarus
