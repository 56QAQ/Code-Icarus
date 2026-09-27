// Agents: magic a magical girl casts on her own initiative. Strategic spells (feasts,
// terror, growth) are chosen through the decision pipeline; these are the immediate,
// local ones: mending the injured and putting out fires. Every cast spends mana and
// acts on real matter and real bodies.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/physics.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
// Her active spell with this effect (if she has reached its level), and its place in her
// drive's list (for the cooldown).
const Json* active_spell(const Registry& reg, const GirlData& g, const std::string& effect, int* index = nullptr) {
    for (const Json& d : reg.doc("drives")["drives"].items()) {
        if (d.str("key") != g.drive) continue;
        int i = 0;
        for (const Json& sp : d["spells"].items()) {
            if (sp.str("effect") == effect && sp.str("type") == "active" && g.level >= sp.integer("level", 1)) {
                if (index) *index = i;
                return &sp;
            }
            ++i;
        }
    }
    return nullptr;
}

// Effect codes of spells cast on her own initiative (Task::count while casting).
enum SpellCode : int {
    kHeal = 1, kQuench, kStrike, kRanged, kDrainStrike, kFirebomb, kTerrify, kRally, kDrainMana,
    kBerserk, kDiscord, kWither, kDevour, kSpellCodes
};
const char* kEffect[kSpellCodes] = {"",          "heal",  "quench",  "strike", "ranged",  "drain_strike", "firebomb",
                                    "terrify",   "rally", "drain_mana", "berserk", "discord", "wither",     "devour"};

// The long rituals (war cries, frenzies, curses on the land) wait out their cooldown;
// the quick battle spells are limited by mana alone.
bool ready(const GirlData& g, const Json& sp, int index, Tick now) {
    if (sp.flt("cooldown_h", 0.0f) < 3.0f) return true;
    return index < 0 || index >= (int)g.spell_cooldowns.size() || now >= (Tick)g.spell_cooldowns[(size_t)index];
}
void start_cooldown(GirlData& g, const Json& sp, int index, Tick now) {
    if (index < 0) return;
    if ((size_t)index >= g.spell_cooldowns.size()) g.spell_cooldowns.resize((size_t)index + 1, 0);
    g.spell_cooldowns[(size_t)index] = (u32)std::min<Tick>(0xffffffffu, now + (Tick)(sp.flt("cooldown_h", 0.0f) * kTicksPerHour));
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

void Agents::note_spell(const Character& caster, const std::string& effect, const std::string& name, const Vec3f& to,
                        EntityId target, float radius) {
    if (spell_fx_.size() >= 64) spell_fx_.erase(spell_fx_.begin());
    SpellFx f;
    f.effect = effect;
    f.name = name;
    f.drive = caster.girl ? caster.girl->drive : std::string();
    f.caster = caster.id;
    f.target = target;
    f.from = caster.pos + Vec3f(0.0f, 1.3f, 0.0f);
    f.to = to;
    f.radius = radius;
    spell_fx_.push_back(std::move(f));
}

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
    // War: combat magic against the enemy's fighters.
    if (Character* foe = nearest_enemy(c, 16.0f, true)) {
        struct Combat {
            const char* effect;
            int code;
            float base;
        };
        const Combat kinds[] = {{"firebomb", 6, 1.9f}, {"strike", 3, 1.8f}, {"drain_strike", 5, 1.75f},
                                {"ranged", 4, 1.7f},   {"terrify", 7, 1.5f}, {"drain_mana", 9, 1.4f}};
        for (const Combat& k : kinds) {
            const Json* sp = active_spell(*ctx_.reg, g, k.effect);
            if (!sp) continue;
            float cost = sp->flt("mana", 0.3f);
            if (g.mana < cost) continue;
            if (k.code == 9 && !foe->is_girl()) continue;
            float s = k.base + 0.3f * c.pers.aggression;
            if (s > best) {
                best = s;
                out = {k.code, foe->foot, foe->id, cost, sp->str("name")};
                why = strfmt("敌人%s近在眼前，施展「%s」", foe->name.c_str(), sp->str("name").c_str());
            }
        }
    }
    const Polity* pol = ctx_.society->polity(c.polity);
    const bool at_war = pol && !pol->wars.empty();
    auto consider = [&](const char* effect, float score, const Vec3i& pos, EntityId who, const std::string& reason) {
        int idx = -1;
        const Json* sp = active_spell(*ctx_.reg, g, effect, &idx);
        if (!sp || score <= best) return;
        const float cost = sp->flt("mana", 0.4f);
        if (g.mana < cost || !ready(g, *sp, idx, now_)) return;
        int code = 0;
        for (int k = 1; k < kSpellCodes; ++k)
            if (std::string(kEffect[k]) == effect) code = k;
        best = score;
        out = {code, pos, who, cost, sp->str("name")};
        why = strfmt(reason.c_str(), sp->str("name").c_str());
    };
    if (at_war) {
        if (Character* foe = nearest_enemy(c, 20.0f, true)) {
            // A war cry for the fighters around her.
            int allies = 0;
            for (auto& op : chars_)
                if (op && op->alive && op->polity == c.polity && op->drafted && op->foot.dist2(c.foot) < 14 * 14 &&
                    now_ >= op->empowered_until)
                    ++allies;
            if (allies >= 3) consider("rally", 2.1f + 0.2f * c.pers.aggression, c.foot, kNoEntity, "同伴们面对敌军，施展「%s」鼓舞士气");
            // Her own battle frenzy.
            if (now_ >= c.empowered_until && foe->foot.dist2(c.foot) < 12 * 12)
                consider("berserk", 2.05f + 0.2f * c.pers.aggression, c.foot, kNoEntity, "敌人近在眼前，施展「%s」");
            // Discord in the enemy's ranks.
            int ranks = 0;
            for (auto& op : chars_)
                if (op && op->alive && op->polity == foe->polity && op->drafted && op->foot.dist2(foe->foot) < 16 * 16) ++ranks;
            if (ranks >= 4) consider("discord", 1.65f, foe->foot, foe->id, "敌军成群而来，施展「%s」动摇其军心");
        }
        // At the enemy's village: devour its walls, wither its fields.
        if (pol->op.active && pol->op.phase == 2 && c.foot.dist2(pol->op.objective) < 40 * 40) {
            const u16 enemy = pol->op.enemy;
            Vec3i wall;
            i64 bd = 12LL * 12;
            bool found = false;
            for (const Building& b : ctx_.buildings->all()) {
                if (!b.alive || b.polity != enemy || b.entrance.dist2(c.foot) > 30 * 30) continue;
                const i64 d = b.entrance.dist2(c.foot);
                if (d < bd || !found) {
                    bd = d;
                    wall = b.entrance;
                    found = true;
                }
            }
            if (found) consider("devour", 1.6f, wall, kNoEntity, "敌人的房屋就在眼前，以「%s」吞噬其墙体");
            Vec3i field;
            bool crops = false;
            for (const Farm& f : ctx_.farming->all())
                if (f.alive && f.polity == enemy && f.center.dist2(c.foot) < 30 * 30)
                    for (const Plot& pl : f.plots)
                        if (ctx_.world->mat(pl.ground + Vec3i{0, 1, 0}) == ctx_.reg->m().crop) {
                            field = pl.ground;
                            crops = true;
                            break;
                        }
            if (crops) consider("wither", 1.5f + 0.2f * c.pers.aggression, field, kNoEntity, "敌人的田地就在附近，以「%s」令其枯死");
        }
    } else if (pol && pol->ruler != c.id && (g.stance == "defiant" || g.stance == "rebel")) {
        // Politics: a girl who has turned against the ruler whispers against her.
        int listeners = 0;
        for (auto& op : chars_)
            if (op && op->alive && !op->departed && op->polity == c.polity && !op->is_girl() &&
                op->foot.dist2(c.foot) < 16 * 16 && op->support_for(pol->ruler) > 0.2f)
                ++listeners;
        if (listeners >= 3)
            consider("discord", 0.5f + 0.8f * c.pers.ambition, c.foot, kNoEntity, "身边都是统治者的拥护者，暗中施展「%s」");
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
    const int code = std::clamp(t.count, 0, kSpellCodes - 1);
    const bool on_someone = code == kHeal || (code >= kStrike && code <= kTerrify) || code == kDrainMana ||
                            (code == kDiscord && t.other);
    if (on_someone && (!who || !who->alive)) {
        end_task(c, false);
        return false;
    }
    // Spells on herself and those around her are cast where she stands.
    if (code == kRally || code == kBerserk || (code == kDiscord && !t.other)) t.target = c.foot;
    Vec3i goal = who ? who->foot : t.target;
    i64 reach = 4;
    switch (code) {
        case kRanged: case kFirebomb: case kTerrify: case kDiscord: reach = 11; break;
        case kStrike: case kDrainStrike: case kDrainMana: reach = 2; break;
        case kWither: reach = 7; break;
        case kDevour: reach = 3; break;
        case kRally: case kBerserk: reach = 1000; break;
        default: break;
    }
    if (t.step == 0) {
        say(c, "赶去施法：" + t.label);
        if (c.foot.dist2(goal) > reach * reach) {
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
        int idx = -1;
        const Json* sp = active_spell(*ctx_.reg, g, kEffect[code], &idx);
        float cost = sp ? sp->flt("mana", 0.4f) : 0.4f;
        if (g.mana < cost || (sp && !ready(g, *sp, idx, now_))) {
            end_task(c, false);
            return false;
        }
        g.mana -= cost;
        g.xp += 3.0f;
        if (sp) start_cooldown(g, *sp, idx, now_);
        const float radius = sp ? sp->flt("radius", 0.0f) : 0.0f;
        note_spell(c, kEffect[code], t.label, who ? who->pos + Vec3f(0.0f, 1.0f, 0.0f) : Vec3f((float)goal.x + 0.5f, (float)goal.y + 0.5f, (float)goal.z + 0.5f),
                   who ? who->id : kNoEntity, radius);
        Event e;
        e.type = EventType::SpellCast;
        e.actor = c.id;
        e.polity = c.polity;
        e.pos = goal;
        if (code == kRally || code >= kBerserk) {
            cast_ritual(c, code, t.label, sp, goal, who, e);
        } else if (t.count >= 3 && who) {
            // Combat magic.
            const float amt = (sp ? sp->flt("amount", 0.3f) : 0.3f) * empowerment(c);
            const Polity* pp = ctx_.society->polity(c.polity);
            const EventId cause = pp && !pp->wars.empty() ? (pp->op.event ? pp->op.event : pp->wars.front().event) : 0;
            e.severity = 3;
            e.target = who->id;
            switch (t.count) {
                case 3:
                case 4:
                    strike(c, *who, amt, "被魔法「" + t.label + "」击中", cause);
                    e.text = strfmt("%s以「%s」击中了%s", c.name.c_str(), t.label.c_str(), who->name.c_str());
                    break;
                case 5:
                    strike(c, *who, amt, "被「" + t.label + "」撕咬", cause);
                    c.body.vitality = std::min(1.0f, c.body.vitality + 0.2f);
                    c.needs.food = std::min(1.0f, c.needs.food + 0.2f);
                    e.text = strfmt("%s以「%s」撕咬%s，吸取了体力", c.name.c_str(), t.label.c_str(), who->name.c_str());
                    break;
                case 6: {
                    // Real fire and blast: terrain, buildings and bodies alike.
                    Vec3f at = who->pos + Vec3f(0.0f, 1.0f, 0.0f);
                    e.text = strfmt("%s朝%s掷出「%s」", c.name.c_str(), who->name.c_str(), t.label.c_str());
                    EventId ev = ctx_.chron->emit(e);
                    ctx_.physics->explode(at, 2.5f, ev ? ev : cause, false);
                    e.text.clear();
                    break;
                }
                case 7: {
                    int n = 0;
                    for (auto& op : chars_)
                        if (op && op->alive && op->polity != c.polity && op->foot.dist2(who->foot) < 10 * 10) {
                            op->fear = std::min(1.0f, op->fear + 0.6f);
                            op->remember(now_, MemoryKind::Cursed, c.id, -0.2f, cause);
                            if (op->task.type == TaskType::Fight) {
                                op->task = Task{};
                                op->next_think = now_;
                            }
                            ++n;
                        }
                    e.text = strfmt("%s施展「%s」，%d 名敌人陷入恐惧", c.name.c_str(), t.label.c_str(), n);
                    break;
                }
                case 9:
                    if (who->girl) {
                        float took = std::min(who->girl->mana, 0.4f);
                        who->girl->mana -= took;
                        g.mana = std::min(1.0f, g.mana + took);
                    }
                    e.text = strfmt("%s以「%s」夺走了%s的魔力", c.name.c_str(), t.label.c_str(), who->name.c_str());
                    break;
                default: break;
            }
        } else if (t.count == 1 && who) {
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
        } else if (t.count == 2) {
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
        if (!e.text.empty()) ctx_.chron->emit(std::move(e));
        end_task(c, true);
    }
    return true;
}

void Agents::cast_ritual(Character& c, int code, const std::string& name, const Json* sp, const Vec3i& at, Character* who,
                         Event& e) {
    GirlData& g = *c.girl;
    const Polity* pol = ctx_.society->polity(c.polity);
    const EventId cause = pol && !pol->wars.empty() ? (pol->op.event ? pol->op.event : pol->wars.front().event) : 0;
    const float radius = sp ? sp->flt("radius", 8.0f) : 8.0f;
    const i64 r2 = (i64)(radius * radius);
    e.severity = 3;
    e.causes[0] = cause;
    switch (code) {
        case kRally: {
            // Fear gone, blows harder: for the fighters around her, a couple of hours.
            int n = 0;
            for (auto& op : chars_) {
                if (!op || !op->alive || op->polity != c.polity || op->foot.dist2(c.foot) > r2) continue;
                if (!op->drafted && !op->is_girl()) continue;
                op->fear = 0.0f;
                op->empowered_until = now_ + kTicksPerHour * 2;
                op->empowered = std::max(op->empowered_until > now_ ? op->empowered : 1.0f, 1.3f);
                if (op->task.type == TaskType::Flee) {
                    op->task = Task{};
                    op->next_think = now_;
                }
                ++n;
            }
            e.text = strfmt("%s发出「%s」，%d 名同伴重振士气", c.name.c_str(), name.c_str(), n);
            break;
        }
        case kBerserk:
            c.empowered_until = now_ + kTicksPerHour * 3 / 2;
            c.empowered = 2.0f;
            c.fear = 0.0f;
            e.text = strfmt("%s陷入「%s」，力量倍增", c.name.c_str(), name.c_str());
            break;
        case kDiscord: {
            int n = 0;
            if (who && who->polity != c.polity) {
                // In battle: the enemy's ranks lose heart and their trust in their rulers.
                const u16 enemy = who->polity;
                const Polity* ep = ctx_.society->polity(enemy);
                for (auto& op : chars_) {
                    if (!op || !op->alive || op->polity != enemy || op->is_girl() || op->foot.dist2(at) > r2) continue;
                    op->fear = std::min(1.0f, op->fear + 0.3f);
                    if (ep && ep->ruler) op->support_ref(ep->ruler) = clampv(op->support_for(ep->ruler) - 0.1f, -1.0f, 1.0f);
                    op->remember(now_, MemoryKind::Cursed, c.id, -0.1f, cause);
                    ++n;
                }
                e.target = who->id;
                e.text = strfmt("%s施展「%s」，%d 名敌兵军心动摇", c.name.c_str(), name.c_str(), n);
            } else if (pol) {
                // At home: doubts about the ruler, whispered among her supporters.
                for (auto& op : chars_) {
                    if (!op || !op->alive || op->departed || op->polity != c.polity || op->is_girl() || op->foot.dist2(at) > r2)
                        continue;
                    op->support_ref(pol->ruler) = clampv(op->support_for(pol->ruler) - 0.08f, -1.0f, 1.0f);
                    op->support_ref(c.id) = clampv(op->support_for(c.id) + 0.04f, -1.0f, 1.0f);
                    ++n;
                }
                e.severity = 2;
                e.target = pol->ruler;
                e.causes[0] = 0;
                e.text = strfmt("%s暗中施展「%s」，%d 人对统治者生出疑心", c.name.c_str(), name.c_str(), n);
            }
            break;
        }
        case kWither: {
            // Crops die where they stand (the land itself is not harmed).
            int n = 0;
            u16 owner = 0;
            const MatId crop = ctx_.reg->m().crop;
            for (Farm& f : ctx_.farming->all_mut()) {
                if (!f.alive || f.polity == c.polity || f.center.dist2(at) > (i64)(radius + 40) * (radius + 40)) continue;
                for (Plot& pl : f.plots) {
                    const Vec3i up = pl.ground + Vec3i{0, 1, 0};
                    if (up.dist2(at) > r2 || ctx_.world->mat(up) != crop) continue;
                    ctx_.world->set(up, make_voxel(ctx_.reg->m().air), cause);
                    pl.growth = 0;
                    owner = f.polity;
                    ++n;
                }
            }
            if (owner) ctx_.society->add_grievance(owner, c.polity, 0.1f);
            e.text = strfmt("%s施展「%s」，%d 株作物枯死", c.name.c_str(), name.c_str(), n);
            break;
        }
        case kDevour: {
            // The walls of the enemy's buildings, swallowed and turned to mana (gone for good).
            int n = 0;
            const u16 enemy = pol && pol->op.active ? pol->op.enemy : 0;
            const int R = (int)std::ceil(radius);
            for (int dy = -1; dy <= 3; ++dy)
                for (int dz = -R; dz <= R; ++dz)
                    for (int dx = -R; dx <= R; ++dx) {
                        const Vec3i q = at + Vec3i{dx, dy, dz};
                        if (q.dist2(at) > r2) continue;
                        const u32 bid = ctx_.buildings->at(q);
                        const Building* b = bid ? ctx_.buildings->get(bid) : nullptr;
                        if (!b || b->polity != enemy || !ctx_.world->material(q).solid) continue;
                        ctx_.world->set(q, make_voxel(ctx_.reg->m().air), cause);
                        ++n;
                    }
            g.mana = std::min(1.0f, g.mana + 0.03f * (float)n);
            if (pol)
                if (War* w = const_cast<Polity*>(pol)->war_with(enemy)) w->razed += n;
            e.text = strfmt("%s以「%s」吞噬了敌人房屋的 %d 块墙体", c.name.c_str(), name.c_str(), n);
            break;
        }
        default: break;
    }
}

}  // namespace icarus
