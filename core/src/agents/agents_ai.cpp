// Agents: utility-based decision making. Every option is scored from needs, personality,
// policy and circumstance; the breakdown is kept so the UI can explain the choice.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
float need_curve(float x) {
    x = clampv(x, 0.0f, 1.0f);
    return x * x * 2.0f + (x > 0.75f ? (x - 0.75f) * 6.0f : 0.0f);
}
const char* pct(float v) {
    static thread_local char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", v * 100.0f);
    return buf;
}
}  // namespace

float Agents::work_score(Character& c, const Job& j, std::string& why) {
    const Polity* p = ctx_.society->polity(c.polity);
    const Policies pol = p ? p->policies : Policies{};
    std::string cat = job_category(j.type);
    float cat_w = 1.0f;
    if (cat == "food") {
        cat_w = pol.pri_food;
        // An emptying larder makes food work more urgent, whatever the ruler has ordered.
        if (p && p->stats.food_days < 1.5f) cat_w *= 1.0f + 0.8f * (1.5f - std::max(0.0f, p->stats.food_days)) / 1.5f;
    }
    else if (cat == "build") cat_w = pol.pri_build;
    else if (cat == "gather") cat_w = pol.pri_gather;
    else if (cat == "research") cat_w = pol.pri_research;
    else if (cat == "military") cat_w = pol.pri_military;
    float ruler_support = p ? c.support_for(p->ruler) : 0.0f;
    float motivation = 0.35f + 0.45f * c.pers.diligence + 0.3f * c.fear * c.pers.conformity +
                       0.25f * c.pers.ambition * pol.wage + 0.2f * std::max(0.0f, ruler_support);
    // Food work matters more to altruists when people are going hungry.
    if (cat == "food" && p && p->stats.food_days < 3.0f) motivation += 0.2f * c.pers.altruism;
    motivation *= 0.45f + 0.55f * c.mood;
    motivation *= 0.5f + 0.5f * c.needs.rest;
    float skill = c.skills[job_skill(j.type)];
    float skill_f = 0.8f + 0.4f * skill;
    // (The way to a job counts against taking it up, not against going on with it — nor
    // against a caravan's errand she carries on from her own pack.)
    const bool doing = c.task.type == TaskType::Work && c.task.job == j.id;
    float dist = doing || j.from == c.inv ? 0.0f : std::sqrt((float)c.foot.dist2(j.pos));
    // (An expedition for seed is worth the long walk.)
    const bool expedition = j.type == JobType::Forage && j.plot == 2;
    float score = motivation * j.priority * cat_w * skill_f - dist / (expedition ? 1200.0f : 260.0f);
    // A caravan under way (the goods in hand, or picked up again from her own pack after
    // a rest) is seen through: only a pressing need interrupts it.
    if (j.type == JobType::Trade && ((doing && c.task.step >= 2) || j.from == c.inv)) score += 0.5f;
    // One's own trade (the steward shares the hands out, see agents_plan.cpp) comes first
    // in working hours; other work is taken up when there is none of one's own.
    if (!c.occupation.empty() && c.occupation == cat) score += is_work_time(c) ? 0.35f : 0.08f;
    // Scholars are there to study in working hours (unless the larder is emptying).
    if (cat == "research" && c.occupation == "research" && is_work_time(c) &&
        !(p && p->stats.food_days < 1.0f && p->stats.food_access < 0.9f))
        score += 0.6f;
    // The right tool in hand makes a job more attractive (and bare hands less).
    {
        std::string kind;
        switch (j.type) {
            case JobType::Chop: kind = "axe"; break;
            case JobType::Mine: kind = "pick"; break;
            case JobType::Till: kind = "hoe"; break;
            case JobType::Harvest: kind = "sickle"; break;
            case JobType::Build: kind = "hammer"; break;
            default: break;
        }
        if (!kind.empty()) score += 0.06f * (std::min(1.4f, tool_factor(c, kind)) - 0.7f);
    }
    // Late in the working day nobody sets out for work far from home.
    if (p && dist > 40.0f) {
        const float left = 8.0f + p->policies.work_hours - hour_of(now_);
        if (left < 2.0f && left > -1.0f) score -= (2.0f - left) * 0.25f * std::min(1.0f, dist / 100.0f);
    }
    if (c.work_debt > 2.0f && pol.punishment > 0) score += pol.punishment * c.pers.conformity * 0.3f;
    if (!is_work_time(c)) score *= 0.25f + 0.4f * c.pers.diligence;
    why = strfmt("%s（优先级 %.1f，技能 %s，距离 %.0f）", job_name_zh(j.type), j.priority * cat_w, pct(skill), dist);
    return score;
}

u32 Agents::best_job(Character& c, float& best, std::string& why) {
    best = -1e9f;
    u32 pick = 0;
    for (const Job& j : ctx_.jobs->all()) {
        if (!j.alive || j.polity != c.polity) continue;
        if (j.claimed_by != kNoEntity && j.claimed_by != c.id) continue;
        if (j.suspended_until > now_) continue;
        // Study keeps working hours; the seats at a research building are for its scholars.
        if (j.type == JobType::Research && !is_work_time(c)) continue;
        if (j.type == JobType::Research && c.occupation != "research")
            if (const Building* b = ctx_.buildings->get(j.building))
                if (const BuildingDef* d = ctx_.buildings->def(b->def); d && d->scholars > 0) continue;
        if (blacklisted(c, j.pos)) continue;
        // Cheap straight-line cut-off before scoring (not for what she is already about:
        // a caravan far from the store it set out from).
        const bool mine = (c.task.type == TaskType::Work && c.task.job == j.id) || j.from == c.inv;
        if (!mine && c.foot.chebyshev(j.pos) > (j.type == JobType::Forage && j.plot == 2 ? 900 : 240)) continue;
        std::string w;
        float s = work_score(c, j, w);
        if (s > best) {
            best = s;
            pick = j.id;
            why = w;
        }
    }
    return pick;
}

void Agents::start_task(Character& c, TaskType t, float utility, const std::string& label) {
    c.task = Task{};
    c.task.type = t;
    c.task.started = now_;
    c.task.utility = utility;
    c.task.label = label;
    c.path.clear();
    c.sleeping = false;
    leave_furniture(c);
}

void Agents::end_task(Character& c, bool success) {
    if (c.task.job) {
        Job* j = ctx_.jobs->get(c.task.job);
        const Store* inv = ctx_.econ->store(c.inv);
        if (success) {
            ctx_.jobs->complete(c.task.job);
        } else if (j && j->type == JobType::Trade && c.task.step == 2 && inv && inv->count(j->item) > 0) {
            // A carrier stopping on the way (to eat, drink or sleep) keeps her load and the
            // errand: the goods are now picked up from her own pack when she goes on.
            j->from = c.inv;
            j->count = std::min(j->count, inv->count(j->item));
            ctx_.jobs->claim(j->id, c.id, now_ + kTicksPerHour * 16);
        } else {
            ctx_.jobs->release(c.task.job, c.id);
        }
    }
    ctx_.econ->release_agent(c.id);
    land(c);  // (an errand afloat that ends out on the water ends ashore)
    if (!success) c.task.fails++;
    TaskType prev = c.task.type;
    c.task = Task{};
    c.task.type = TaskType::None;
    c.sleeping = false;
    leave_furniture(c);
    c.path.clear();
    c.next_think = now_;  // choose something new right away
    (void)prev;
}

void Agents::think(Character& c) {
    c.next_think = now_ + tune.think_interval + (Tick)(c.id % 7);
    // Someone already at the water drinks her fill (a sip only quenches half a thirst);
    // only danger pulls her away.
    if (c.task.type == TaskType::Drink && c.task.step == 2 && danger_at(c) < 0.3f && !nearest_enemy(c, 14.0f, true)) return;
    // Out on the water there is nothing to do but fish and row back.
    if (c.in_boat && c.task.type == TaskType::Work) return;
    const Polity* p = ctx_.society->polity(c.polity);
    const Policies pol = p ? p->policies : Policies{};
    const bool night = is_night(now_);
    const bool work_time = is_work_time(c);
    std::vector<Consideration> opts;
    auto add = [&](const std::string& label, float score, const std::string& why) {
        opts.push_back({label, score, why});
    };

    // Needs.
    float hunger = 1.0f - c.needs.food;
    float eat = need_curve(hunger);
    bool food_known = find_food_store(c, true, false) != kNoStore;
    const Store* inv = ctx_.econ->store(c.inv);
    bool carrying_food = false;
    if (inv)
        for (auto& st : inv->items)
            if (ctx_.reg->item(st.item).nutrition > 0) carrying_food = true;
    if (!food_known && !carrying_food) eat *= now_ < c.no_food_until ? 0.15f : 0.6f;
    add("吃饭", eat, strfmt("饥饿 %s%s", pct(hunger), (food_known || carrying_food) ? "" : "，却不知哪里有吃的"));

    float thirst = 1.0f - c.needs.water;
    add("喝水", need_curve(thirst) * 1.15f, strfmt("口渴 %s", pct(thirst)));

    float tired = 1.0f - c.needs.rest;
    float sleep = tired * tired * 2.2f;
    if (night) sleep += (c.needs.rest < 0.95f ? 0.9f + 0.6f * tired : 0.0f);
    else if (c.needs.rest > 0.3f) sleep *= 0.3f;
    if (c.task.type == TaskType::Sleep && night) sleep += 1.0f;  // stay in bed through the night
    // After the working day, those out at the far fields set off home to bed in good time
    // (rather than work on until they drop and sleep in the open).
    // (Not while the people are in a crisis: then the work goes on into the evening.)
    bool far_bed = false, crisis = false;
    if (p)
        for (const Crisis& cr : p->crises)
            if (cr.active && (cr.kind == CrisisKind::Water || cr.kind == CrisisKind::Food)) crisis = true;
    if (!work_time && !crisis && hour_of(now_) >= 12.0f)
        if (const Building* h = ctx_.buildings->get(c.home); h && h->functional && c.foot.chebyshev(h->inside) > 40) {
            sleep += 0.45f;
            far_bed = true;
        }
    add("睡觉", sleep, strfmt("疲劳 %s%s", pct(tired), night ? "，夜深了" : (far_bed ? "，收工回家" : "")));

    float lonely = 1.0f - c.needs.social;
    float social = lonely * (0.25f + 0.75f * c.pers.sociability) * 0.9f;
    if (work_time) social *= 0.5f;
    // (Just talked, or found nobody free to talk to: not again straight away — else the
    // lonely try every moment of the working day and never get to work.)
    if (now_ - c.last_social < kTicksPerHour) social *= 0.3f;
    add("交谈", social, strfmt("孤独 %s，社交性 %s", pct(lonely), pct(c.pers.sociability)));

    // Work (children play instead).
    const bool child = c.age0 < 14.0f && is_child(c);
    std::string job_why;
    float job_score = -1;
    u32 job = 0;
    if (c.body.can_hold() && !child) job = best_job(c, job_score, job_why);
    if (job) add("工作", job_score, job_why);
    if (child) add("玩耍", 0.3f + 0.2f * c.pers.sociability, strfmt("还是个孩子（%d 岁）", (int)age_years(c)));

    // Governing (magical girls holding office).
    if (c.is_girl() && p && (p->ruler == c.id || c.girl->role != "none") && work_time) {
        float g = p->ruler == c.id ? 0.75f : 0.45f;
        add("处理政务", g, p->ruler == c.id ? "身为统治者，需在议事厅坐镇" : "担任职务：" + c.girl->role);
    }

    // Danger.
    float danger = danger_at(c);
    if (danger > 0.05f) add("逃离危险", danger * (1.0f + c.pers.caution) * 2.5f, strfmt("危险 %s，谨慎 %s", pct(danger), pct(c.pers.caution)));

    // Grievance → protest.
    if (p && p->ruler != kNoEntity && p->ruler != c.id && work_time && !child) {
        float s = c.support_for(p->ruler);
        float grievance = std::max(0.0f, -s) * (1.0f - c.mood) * (0.6f + c.pers.aggression + 0.3f * c.pers.idealism) -
                          c.fear * c.pers.conformity * 0.6f - pol.punishment * 0.25f;
        const float calm = p->passive("calm");
        if (grievance > 0.18f + calm) add("抗议", grievance * 1.6f, strfmt("对统治者不满（支持度 %.2f），心情 %s", s, pct(c.mood)));
    }

    // Wounds: with herbalism known and herbs in store, the injured get them dressed.
    if (const float tn = treatment_need(c); tn > 0.0f) {
        const ItemId herbs = ctx_.reg->find_item("herbs");
        bool have = false;
        if (const Store* inv = ctx_.econ->store(c.inv)) have = inv->count(herbs) > 0;
        for (StoreId sid : ctx_.society->public_stores(c.polity))
            if (!have && ctx_.econ->available(sid, herbs, c.id) > 0) have = true;
        if (have) add("疗伤", 0.35f + tn * 1.2f, strfmt("伤势 %s，仓库里有草药", pct(std::min(1.0f, tn))));
    }

    // Going over to a neighbour that treats its people better.
    const Polity* migrate_to = nullptr;
    if (p && ctx_.society->polities().size() > 2 && !child) {
        std::string why;
        const float m = migration_pull(c, *p, migrate_to, why);
        if (migrate_to && m > 0.3f && !blacklisted(c, ctx_.buildings->get(migrate_to->seat)
                                                          ? ctx_.buildings->get(migrate_to->seat)->entrance
                                                          : c.foot))
            add("投奔他国", m * 1.6f, why);
        else
            migrate_to = nullptr;
    }

    // Desperate theft when public food is withheld.
    if (hunger > 0.6f && !carrying_food && !food_known && p && ctx_.society->public_food(c.polity) > 1.0f) {
        float steal = hunger * hunger * (0.4f + c.pers.aggression - 0.6f * c.pers.conformity - 0.5f * pol.punishment);
        if (steal > 0.05f) add("偷取食物", steal * 2.0f, strfmt("饥饿难耐，公共粮仓却不开放（服从 %s）", pct(c.pers.conformity)));
    }

    // War: soldiers serve (the badly hurt are sent home, whatever they are about);
    // civilians keep away from enemy fighters.
    discharge_if_hurt(c);
    if (p && !p->wars.empty()) {
        if (c.drafted) {
            // On campaign, and above all in the thick of it at the objective, the army's
            // business comes before anything that can wait.
            float duty = 1.25f + (p->op.active ? 0.35f : 0.0f) + (p->op.active && p->op.phase == 2 ? 0.7f : 0.0f);
            add("从军", duty, p->op.active ? "军令在身：" + p->op.aim : "战时戒备");
            // The badly wounded fall back out of the fight.
            const float hurt = c.hurt();
            if (hurt > 0.3f && nearest_enemy(c, 16.0f, true))
                add("逃离危险", 1.6f + 2.5f * hurt, strfmt("伤势 %s，撤下战场", pct(std::min(1.0f, hurt))));
        } else {
            // Girls with combat magic and mana stand their ground (see 施法); others flee.
            bool can_fight = false;
            if (c.is_girl()) {
                SpellPick sp;
                std::string w;
                can_fight = pick_spell(c, sp, w) > 0 && sp.effect >= 3;
            }
            if (Character* foe = can_fight ? nullptr : nearest_enemy(c, 14.0f, true)) {
                float threat = 1.0f - std::sqrt(foe->pos.dist_sq(c.pos)) / 14.0f;
                add("逃离危险", 0.8f + 1.8f * threat * (0.6f + c.pers.caution), "敌兵" + foe->name + "就在附近");
            }
        }
    }

    // (Not again right where getting out was just found impossible: then try the needs.)
    if (trapped(c) && !blacklisted(c, c.foot)) {
        // Needs cannot be met from here; getting out comes first unless dying of thirst
        // right next to water.
        float need = std::max(1.0f - c.needs.food, 1.0f - c.needs.water);
        add("设法脱困", 1.6f + 3.0f * need, "被困在无法走出的地方，只能设法出去");
    }

    SpellPick spell;
    if (c.is_girl()) {
        std::string why;
        float sc = pick_spell(c, spell, why);
        if (sc > 0) add("施法", sc, why);
    }

    add("闲逛", 0.1f + (1.0f - c.mood) * 0.12f, "无事可做");

    // Pick with hysteresis: the current activity gets a commitment bonus.
    auto label_for = [&](TaskType t) -> std::string {
        switch (t) {
            case TaskType::Eat: return "吃饭";
            case TaskType::Drink: return "喝水";
            case TaskType::Sleep: return "睡觉";
            case TaskType::Socialize: return "交谈";
            case TaskType::Work: return "工作";
            case TaskType::Govern: return "处理政务";
            case TaskType::Flee: return "逃离危险";
            case TaskType::Protest: return "抗议";
            case TaskType::Steal: return "偷取食物";
            case TaskType::Wander: return child ? "玩耍" : "闲逛";
            case TaskType::Cast: return "施法";
            case TaskType::Escape: return "设法脱困";
            case TaskType::Fight: return "从军";
            case TaskType::Leave: return "投奔他国";
            case TaskType::Heal: return "疗伤";
            default: return "";
        }
    };
    std::string current = label_for(c.task.type);
    for (auto& o : opts)
        if (!current.empty() && o.label == current) {
            o.score = o.score * 1.3f + 0.08f;
            o.why += "（正在进行）";
        }
    std::stable_sort(opts.begin(), opts.end(), [](const Consideration& a, const Consideration& b) { return a.score > b.score; });
    // A drink before bed: a night's sleep leaves a third of a thirst.
    if (opts.front().label == "睡觉" && c.task.type != TaskType::Sleep && c.needs.water < 0.6f)
        for (size_t i = 1; i < opts.size(); ++i)
            if (opts[i].label == "喝水") {
                Consideration d = opts[i];
                d.score = opts.front().score + 0.01f;
                d.why += "，睡前先喝点水";
                opts.erase(opts.begin() + (long)i);
                opts.insert(opts.begin(), d);
                break;
            }
    // Before setting out on work far from food and water: a meal and a drink first (a
    // long walk back half-way through the job would waste the day).
    if (opts.front().label == "工作" && c.task.type != TaskType::Work && job)
        if (const Job* jb = ctx_.jobs->get(job); jb && c.foot.dist2(jb->pos) > 70 * 70) {
            const char* first = c.needs.water < 0.6f ? "喝水" : ((c.needs.food < 0.6f && (food_known || carrying_food)) ? "吃饭" : nullptr);
            for (size_t i = 1; first && i < opts.size(); ++i)
                if (opts[i].label == first) {
                    Consideration d = opts[i];
                    d.score = opts.front().score + 0.01f;
                    d.why += "，出远门之前";
                    opts.erase(opts.begin() + (long)i);
                    opts.insert(opts.begin(), d);
                    break;
                }
        }
    c.trace.assign(opts.begin(), opts.begin() + (long)std::min<size_t>(5, opts.size()));
    const Consideration& best = opts.front();
    if (best.label == current && c.task.type != TaskType::None) return;

    // Switch.
    if (c.task.type != TaskType::None) end_task(c, false);
    c.next_think = now_ + tune.think_interval + (Tick)(c.id % 7);
    if (best.label == "吃饭") start_task(c, TaskType::Eat, best.score, best.why);
    else if (best.label == "喝水") start_task(c, TaskType::Drink, best.score, best.why);
    else if (best.label == "睡觉") start_task(c, TaskType::Sleep, best.score, best.why);
    else if (best.label == "交谈") start_task(c, TaskType::Socialize, best.score, best.why);
    else if (best.label == "处理政务") start_task(c, TaskType::Govern, best.score, best.why);
    else if (best.label == "逃离危险") start_task(c, TaskType::Flee, best.score, best.why);
    else if (best.label == "抗议") start_task(c, TaskType::Protest, best.score, best.why);
    else if (best.label == "偷取食物") start_task(c, TaskType::Steal, best.score, best.why);
    else if (best.label == "设法脱困") start_task(c, TaskType::Escape, best.score, best.why);
    else if (best.label == "从军") start_task(c, TaskType::Fight, best.score, best.why);
    else if (best.label == "疗伤") start_task(c, TaskType::Heal, best.score, best.why);
    else if (best.label == "投奔他国" && migrate_to) {
        start_task(c, TaskType::Leave, best.score, best.why);
        c.task.count = migrate_to->id;
    }
    else if (best.label == "施法") {
        start_task(c, TaskType::Cast, best.score, best.why);
        c.task.count = spell.effect;
        c.task.target = spell.pos;
        c.task.other = spell.who;
        c.task.label = spell.name;
    }
    else if (best.label == "工作" && job) {
        start_task(c, TaskType::Work, best.score, best.why);
        c.task.job = job;
        ctx_.jobs->claim(job, c.id, now_ + kTicksPerHour * 2);
    } else {
        start_task(c, TaskType::Wander, best.score, best.why);
    }
    // Needs tasks should not be re-evaluated too eagerly.
    if (c.task.type == TaskType::Eat || c.task.type == TaskType::Drink || c.task.type == TaskType::Sleep)
        c.next_think = now_ + tune.think_interval * 4;
}

}  // namespace icarus
