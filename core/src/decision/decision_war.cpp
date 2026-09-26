// Jev pipeline: foreign policy and war. Rulers facing another polity choose between
// peace, reconciliation, raids and conquest; at war they press on, reinforce, withdraw
// or offer peace; the attacked call their people to arms or sue for peace. Offers of
// peace are decisions of the other ruler.
#include <algorithm>
#include <cmath>

#include "decision_util.h"
#include "icarus/agents/agents.h"
#include "icarus/decision/decisions.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

using decision_util::act;
using decision_util::make;

namespace {
int residents_of(SimContext& ctx, u16 polity) {
    int n = 0;
    for (auto& cp : ctx.agents->all())
        if (cp && cp->alive && !cp->departed && !cp->is_girl() && cp->polity == polity) ++n;
    return n;
}
int armed_stock(SimContext& ctx, u16 polity) {
    int n = 0;
    for (StoreId sid : ctx.society->public_stores(polity))
        if (const Store* s = ctx.econ->store(sid))
            for (auto& st : s->items)
                if (ctx.reg->item(st.item).has_tag("weapon")) n += st.count;
    return n;
}
// How tired of this war a polity is: its length, the dead, and hunger at home.
float war_weariness(const Polity& p, const War& w, Tick now) {
    const float days = (float)(now - w.since) / (float)kTicksPerDay;
    bool hungry = false;
    for (const Crisis& c : p.crises)
        if (c.active && c.kind == CrisisKind::Food) hungry = true;
    return std::min(0.6f, 0.08f * days) + std::min(0.5f, 0.08f * (float)w.losses) + (hungry ? 0.3f : 0.0f);
}
}  // namespace

void Decisions::build_diplomacy_options(Decision& d, Polity& p, Polity& other) {
    auto& O = d.options;
    const float att = p.attitude_to(other.id);
    const int ours = residents_of(ctx_, p.id), theirs = residents_of(ctx_, other.id);
    const int weapons = armed_stock(ctx_, p.id);
    d.petition = Json::object();
    d.petition.set("other", (int)other.id);
    O.push_back(make("keep_peace", "维持现状", strfmt("与「%s」相安无事（关系 %+.2f）。", other.name.c_str(), att),
                     {{kFrugality, 0.5f}, {kRisk, -0.4f}, {kOrder, 0.2f}}, act("wait")));
    {
        DecisionOption o = make("reconcile", "遣使修好", "派人携礼前往，缓和两国关系。",
                                {{kCooperation, 0.9f}, {kWelfare, 0.3f}, {kRisk, -0.3f}, {kMilitary, -0.3f}, {kSelfPower, -0.1f}},
                                act("reconcile"));
        o.action.set("other", (int)other.id);
        O.push_back(o);
    }
    const int raiders = std::max(3, ours / 4);
    {
        DecisionOption o = make("raid", strfmt("发动劫掠（出动 %d 人）", raiders),
                                strfmt("抢夺「%s」仓库里的粮食。我方可用武器 %d 件；对方居民 %d 人。", other.name.c_str(), weapons, theirs),
                                {{kFoodSecurity, 0.4f}, {kMilitary, 0.8f}, {kHarshness, 0.6f}, {kRisk, 0.6f}, {kSelfPower, 0.3f},
                                 {kWelfare, -0.3f}, {kCooperation, -0.8f}, {kFairness, -0.5f}},
                                act("declare_war"));
        o.action.set("other", (int)other.id);
        o.action.set("aim", "raid");
        o.action.set("soldiers", raiders);
        o.facts.set("weapons", weapons);
        if (att > -0.2f) {
            o.feasible = false;
            o.why_not = "两国并无积怨，师出无名";
        } else if (ours < 6) {
            o.feasible = false;
            o.why_not = "人手太少";
        }
        O.push_back(o);
    }
    const int army = std::max(4, ours / 3);
    {
        DecisionOption o = make("conquest", strfmt("发动征服战争（出动 %d 人）", army),
                                strfmt("攻取「%s」的议事厅，将其人民与土地并入。对方居民 %d 人，我方 %d 人。", other.name.c_str(),
                                       theirs, ours),
                                {{kSelfPower, 0.9f}, {kMilitary, 0.9f}, {kRisk, 0.8f}, {kGrowth, 0.5f}, {kHarshness, 0.7f},
                                 {kCooperation, -1.0f}, {kWelfare, -0.4f}},
                                act("declare_war"));
        o.action.set("other", (int)other.id);
        o.action.set("aim", "conquest");
        o.action.set("soldiers", army);
        if (att > -0.4f) {
            o.feasible = false;
            o.why_not = "仇恨还不足以发动征服";
        } else if (ours * 10 < theirs * 12) {
            o.feasible = false;
            o.why_not = "兵力不足以征服对方";
        }
        O.push_back(o);
    }
}

void Decisions::build_war_options(Decision& d, Polity& p, War& w) {
    auto& O = d.options;
    Polity* enemy = ctx_.society->polity(w.enemy);
    const std::string en = enemy ? enemy->name : "?";
    d.petition = Json::object();
    d.petition.set("other", (int)w.enemy);
    const int ours = residents_of(ctx_, p.id), theirs = enemy ? residents_of(ctx_, enemy->id) : 0;
    const int fit = ctx_.society->soldiers(p.id) + ctx_.society->draftable(p.id);
    const float days = (float)(now_ - w.since) / (float)kTicksPerDay;
    const float weary = war_weariness(p, w, now_);
    const std::string tally = strfmt("开战 %.1f 天，杀敌 %d，阵亡 %d。", days, w.kills, w.losses);
    if (p.op.active) {
        O.push_back(make("press_on", "继续作战", "军队正在出征。" + tally, {{kMilitary, 0.6f}, {kSelfPower, 0.4f}, {kRisk, 0.3f}},
                         act("wait")));
        DecisionOption o = make("reinforce", "增派兵力（再征召 3 人）", "更多人离开田地与工坊奔赴战场。",
                                {{kMilitary, 0.8f}, {kRisk, 0.4f}, {kFoodSecurity, -0.3f}, {kWelfare, -0.3f}}, act("reinforce"));
        o.action.set("n", 3);
        if (fit <= ctx_.society->soldiers(p.id)) {
            o.feasible = false;
            o.why_not = "已没有能再征召的人";
        }
        O.push_back(o);
        O.push_back(make("withdraw", "撤回军队", "结束这次出征，士兵回家。",
                         {{kWelfare, 0.3f}, {kRisk, -0.4f}, {kMilitary, -0.4f}, {kSelfPower, -0.2f}}, act("withdraw")));
    } else {
        // No army in the field: holding the war without fighting costs nothing today but
        // settles nothing either.
        O.push_back(make("hold", "按兵不动，维持战争状态", "不出兵也不议和。" + tally,
                         {{kMilitary, 0.2f}, {kSelfPower, 0.3f}, {kFrugality, 0.3f}, {kRisk, -0.2f}, {kCooperation, -0.3f}},
                         act("wait")));
        const int raiders = std::max(3, ours / 4);
        DecisionOption r = make("raid_again", strfmt("出兵劫掠「%s」（出动 %d 人）", en.c_str(), raiders),
                                "夺取对方仓库里的粮食，士兵会离开田地。",
                                {{kFoodSecurity, 0.4f}, {kMilitary, 0.7f}, {kSelfPower, 0.3f}, {kRisk, 0.5f}, {kHarshness, 0.4f},
                                 {kWelfare, -0.2f}, {kFairness, -0.4f}},
                                act("launch"));
        r.action.set("aim", "raid");
        r.action.set("soldiers", raiders);
        r.facts.set("fit_to_fight", fit);
        if (fit < 3) {
            r.feasible = false;
            r.why_not = "能上阵的人太少";
        }
        O.push_back(r);
        const int army = std::max(4, ours / 3);
        DecisionOption c = make("assault", strfmt("进攻「%s」的议事厅（出动 %d 人）", en.c_str(), army),
                                strfmt("攻下议事厅便能结束这场战争。对方居民 %d 人，我方 %d 人。", theirs, ours),
                                {{kSelfPower, 0.9f}, {kMilitary, 0.9f}, {kRisk, 0.8f}, {kGrowth, 0.4f}, {kHarshness, 0.5f},
                                 {kWelfare, -0.4f}},
                                act("launch"));
        c.action.set("aim", "conquest");
        c.action.set("soldiers", army);
        if (fit < 4) {
            c.feasible = false;
            c.why_not = "能上阵的人太少";
        } else if (ours * 10 < theirs * 12) {
            c.feasible = false;
            c.why_not = "兵力不足以攻下对方";
        }
        O.push_back(c);
    }
    DecisionOption o = make("offer_peace", "向「" + en + "」提出议和", "由对方的统治者决定是否接受。" + tally,
                            {{kCooperation, 0.8f}, {kWelfare, 0.5f}, {kRisk, -0.5f}, {kSelfPower, -0.3f}, {kMilitary, -0.5f}},
                            act("offer_peace"));
    o.action.set("other", (int)w.enemy);
    o.bias += weary;  // war-weariness
    o.facts.set("war_days", days);
    o.facts.set("losses", w.losses);
    O.push_back(o);
}

void Decisions::build_defense_options(Decision& d, Polity& p, const Crisis& c) {
    auto& O = d.options;
    (void)c;
    const War* w = p.wars.empty() ? nullptr : &p.wars.front();
    for (const War& x : p.wars)
        if (!x.attacker) w = &x;
    if (!w) return;
    Polity* enemy = ctx_.society->polity(w->enemy);
    const std::string en = enemy ? enemy->name : "?";
    const int ours = residents_of(ctx_, p.id);
    d.petition = Json::object();
    d.petition.set("other", (int)w->enemy);
    // Only the able-bodied can be called up; the maimed and the soldiers already
    // serving do not count.
    const int serving = ctx_.society->soldiers(p.id);
    const int fit = ctx_.society->draftable(p.id);
    const int n = std::min(std::max(4, ours / 3), serving + fit);
    {
        DecisionOption o = make("call_to_arms", strfmt("全民应战（征召 %d 人守卫家园）", n),
                                "拿起武器守在议事厅周围，击退来犯之敌。",
                                {{kMilitary, 0.9f}, {kOrder, 0.4f}, {kRisk, 0.4f}, {kFoodSecurity, -0.2f}, {kSelfPower, 0.2f}},
                                act("defend"));
        o.action.set("n", n);
        o.facts.set("fit_to_fight", serving + fit);
        o.facts.set("already_serving", serving);
        if (n <= serving) {
            o.feasible = false;
            o.why_not = fit == 0 ? "已没有能拿起武器的人" : "能征召的人都已在军中";
        }
        O.push_back(o);
    }
    {
        const int h = std::min(3, serving + fit);
        DecisionOption o = make("hold", strfmt("小股守卫（征召 %d 人）", h), "只派少数人守卫，其余照常劳作。",
                                {{kMilitary, 0.4f}, {kFrugality, 0.4f}, {kFoodSecurity, 0.2f}, {kRisk, 0.2f}}, act("defend"));
        o.action.set("n", h);
        if (h <= 0) {
            o.feasible = false;
            o.why_not = "已没有能拿起武器的人";
        }
        O.push_back(o);
    }
    {
        DecisionOption o = make("sue_for_peace", "向「" + en + "」求和", "请求停战，由对方决定。",
                                {{kCooperation, 0.7f}, {kWelfare, 0.6f}, {kRisk, -0.6f}, {kSelfPower, -0.5f}, {kMilitary, -0.4f}},
                                act("offer_peace"));
        o.action.set("other", (int)w->enemy);
        O.push_back(o);
    }
}

void Decisions::build_peace_options(Decision& d, Polity& p, u16 from) {
    auto& O = d.options;
    Polity* other = ctx_.society->polity(from);
    const std::string on = other ? other->name : "?";
    d.petition = Json::object();
    d.petition.set("other", (int)from);
    DecisionOption a = make("accept_peace", "接受「" + on + "」的议和", "停战，士兵回家。",
                            {{kCooperation, 0.8f}, {kWelfare, 0.5f}, {kRisk, -0.5f}, {kMilitary, -0.4f}}, act("peace_accept"));
    a.action.set("other", (int)from);
    if (const War* w = p.war_with(from)) {
        a.bias += war_weariness(p, *w, now_);
        a.facts.set("war_days", (float)(now_ - w->since) / (float)kTicksPerDay);
        a.facts.set("losses", w->losses);
    }
    O.push_back(a);
    DecisionOption r = make("refuse_peace", "拒绝议和", "战争继续。",
                            {{kMilitary, 0.6f}, {kSelfPower, 0.5f}, {kRisk, 0.4f}, {kCooperation, -0.6f}}, act("wait"));
    O.push_back(r);
}

bool Decisions::execute_war(Decision& d, const DecisionOption& o, Polity& p, Character& g) {
    const Json& a = o.action;
    const std::string what = a.str("do");
    const EventId cause = d.decision_event;
    const u16 other = (u16)a.integer("other", d.petition.integer("other", 0));
    if (what == "declare_war") {
        EventId ev = ctx_.society->declare_war(p.id, other, a.str("aim", "raid"), g.id, cause);
        ctx_.society->draft(p.id, a.integer("soldiers", 4), ev);
        if (Polity* pp = ctx_.society->polity(p.id)) pp->op.party = ctx_.society->soldiers(p.id);
        return true;
    }
    if (what == "reconcile") {
        Polity* op = ctx_.society->polity(other);
        p.attitude_ref(other) = std::min(1.0f, p.attitude_to(other) + 0.25f);
        if (op) op->attitude_ref(p.id) = std::min(1.0f, op->attitude_to(p.id) + 0.25f);
        Event e;
        e.type = EventType::Info;
        e.severity = 3;
        e.actor = g.id;
        e.polity = p.id;
        e.causes[0] = cause;
        e.text = strfmt("「%s」遣使前往「%s」修好", p.name.c_str(), op ? op->name.c_str() : "?");
        ctx_.chron->emit(std::move(e));
        return true;
    }
    if (what == "launch") {
        if (!ctx_.society->at_war(p.id, other) || p.op.active) return true;
        ctx_.society->draft(p.id, a.integer("soldiers", 4), cause);
        ctx_.society->start_operation(p.id, other, a.str("aim", "raid"), cause);
        if (Polity* pp = ctx_.society->polity(p.id)) pp->op.party = ctx_.society->soldiers(p.id);
        return true;
    }
    if (what == "reinforce") {
        ctx_.society->draft(p.id, ctx_.society->soldiers(p.id) + a.integer("n", 3), cause);
        return true;
    }
    if (what == "withdraw") {
        if (p.op.active) {
            p.op.phase = 3;
            p.op.since = now_;
        }
        return true;
    }
    if (what == "defend") {
        ctx_.society->draft(p.id, a.integer("n", 4), cause);
        const Building* seat = ctx_.buildings->get(p.seat);
        if (seat && !p.op.active) {
            p.op = Operation{};
            p.op.active = true;
            p.op.enemy = other;
            p.op.aim = "defend";
            p.op.rally = seat->entrance;
            p.op.objective = seat->entrance;
            p.op.phase = 2;
            p.op.since = now_;
            p.op.event = cause;
            p.op.party = ctx_.society->soldiers(p.id);
        }
        return true;
    }
    if (what == "offer_peace") {
        Polity* op = ctx_.society->polity(other);
        Character* ruler = op ? ctx_.agents->get(op->ruler) : nullptr;
        if (op && ruler && ruler->alive) {
            Decision pd;
            pd.girl = ruler->id;
            pd.polity = op->id;
            pd.kind = "peace_offer";
            pd.topic = "「" + p.name + "」提出议和";
            pd.cause = cause;
            pd.petitioner = g.id;
            build_peace_options(pd, *op, p.id);
            pd.situation = describe_situation(*op, *ruler, pd.topic);
            open(std::move(pd));
        }
        return true;
    }
    if (what == "peace_accept") {
        ctx_.society->make_peace(p.id, other, "议和停战", cause);
        return true;
    }
    return false;
}

void Decisions::consider_foreign(Polity& p, Character& ruler) {
    for (const Polity& oc : ctx_.society->polities()) {
        if (!oc.alive || oc.id == p.id) continue;
        Polity& other = *ctx_.society->polity(oc.id);
        // Not asked again too soon about the same neighbour.
        bool recent = false;
        for (size_t i = list_.size() > 96 ? list_.size() - 96 : 1; i < list_.size(); ++i) {
            const Decision& x = list_[i];
            if (x.polity != p.id || x.petition.integer("other", 0) != other.id) continue;
            if (x.kind != "diplomacy" && x.kind != "war") continue;
            if (x.status == DecisionStatus::Pending || x.status == DecisionStatus::AwaitingRemote ||
                now_ - x.created < (x.kind == "war" ? kTicksPerDay : kTicksPerDay * 3 / 2))
                recent = true;
        }
        if (recent) continue;
        War* w = p.war_with(other.id);
        // Both sides decide how to carry on a war; the attacked leave it to their
        // defence while the enemy's army is actually upon them.
        const bool under_attack = other.op.active && other.op.enemy == p.id;
        if (w && (w->attacker || !under_attack)) {
            Decision d;
            d.girl = ruler.id;
            d.polity = p.id;
            d.kind = "war";
            d.topic = "与「" + other.name + "」的战事";
            d.cause = w->event;
            build_war_options(d, p, *w);
            d.situation = describe_situation(p, ruler, d.topic);
            gather_proposals(d, p);
            open(std::move(d));
            return;
        }
        // A newly founded polity is left alone for a day or so.
        const bool settled = now_ > p.founded + kTicksPerDay && now_ > other.founded + kTicksPerDay;
        if (!w && settled && p.attitude_to(other.id) < 0.1f && now_ > kTicksPerDay / 2) {
            Decision d;
            d.girl = ruler.id;
            d.polity = p.id;
            d.kind = "diplomacy";
            d.topic = "对「" + other.name + "」的方针";
            build_diplomacy_options(d, p, other);
            d.situation = describe_situation(p, ruler, d.topic);
            gather_proposals(d, p);
            open(std::move(d));
            return;
        }
    }
}

}  // namespace icarus
