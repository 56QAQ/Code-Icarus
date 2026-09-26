// Jev pipeline: foreign policy and war. Rulers facing another polity choose between
// peace, reconciliation, trade, raids and conquest; at war they press on, reinforce,
// withdraw or offer peace; the attacked call their people to arms or sue for peace.
// Offers of peace and of trade are decisions of the other ruler.
#include <algorithm>
#include <cmath>

#include "decision_util.h"
#include "icarus/agents/agents.h"
#include "icarus/agents/jobs.h"
#include "icarus/decision/decisions.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

using decision_util::act;
using decision_util::make;

namespace {
constexpr float kAidHunger = 3.0f;  // a neighbour with less food than this (days) may be helped
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
// What trading would bring beyond the ruler's taste for it: goods we lack (food above
// all when stores run low), and a use for what would otherwise sit or spoil.
float trade_gain(const Registry& reg, const Polity& p, ItemId in, ItemId out) {
    float g = 0;
    if (in != kNoItem) g += 0.25f;
    if (out != kNoItem) g += 0.15f;
    if (in != kNoItem && reg.item(in).nutrition > 0 && p.stats.food_days < 4.0f)
        g += 0.5f * std::min(1.0f, (4.0f - p.stats.food_days) / 4.0f);
    return g;
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
    // Trade: what each side could spare for the other.
    if (const TradePact* pact = p.pact_with(other.id)) {
        i32 n_out = 0, n_in = 0;
        const ItemId out = ctx_.society->trade_export(p.id, other.id, &n_out);
        const ItemId in = ctx_.society->trade_export(other.id, p.id, &n_in);
        DecisionOption o = make("end_trade", "断绝与「" + other.name + "」的通商",
                                strfmt("通商以来我方商队往来 %d 次，送出价值 %.0f，换回 %.0f。", pact->trips, pact->sent,
                                       pact->received),
                                {{kSelfPower, 0.4f}, {kHarshness, 0.3f}, {kCooperation, -0.8f}, {kGrowth, -0.3f}, {kWelfare, -0.2f}},
                                act("end_trade"));
        o.action.set("other", (int)other.id);
        o.bias -= trade_gain(*ctx_.reg, p, in, out);  // what the trade still brings
        O.push_back(o);
    } else {
        const Registry& reg = *ctx_.reg;
        i32 n_out = 0, n_in = 0;
        const ItemId out = ctx_.society->trade_export(p.id, other.id, &n_out);
        const ItemId in = ctx_.society->trade_export(other.id, p.id, &n_in);
        std::string what;
        if (out != kNoItem) what = strfmt("我们富余的%s（约 %d）", reg.item(out).name.c_str(), n_out);
        if (in != kNoItem)
            what += (what.empty() ? "" : "，") + strfmt("换取对方富余的%s（约 %d）", reg.item(in).name.c_str(), n_in);
        const bool food_in = in != kNoItem && reg.item(in).nutrition > 0;
        const bool food_out = out != kNoItem && reg.item(out).nutrition > 0;
        DecisionOption o = make("propose_trade", "提议与「" + other.name + "」通商",
                                (what.empty() ? std::string("互通有无") : "以" + what) + "。由对方的统治者决定是否接受。",
                                {{kCooperation, 0.7f}, {kGrowth, 0.5f}, {kWelfare, 0.3f},
                                 {kFoodSecurity, food_in ? 0.7f : (food_out ? -0.1f : 0.1f)}, {kFrugality, 0.2f},
                                 {kSelfPower, -0.1f}, {kRisk, -0.1f}},
                                act("propose_trade"));
        o.action.set("other", (int)other.id);
        o.bias += trade_gain(reg, p, in, out);
        if (out != kNoItem) o.facts.set("export", reg.item(out).name);
        if (in != kNoItem) o.facts.set("import", reg.item(in).name);
        if (out == kNoItem && in == kNoItem) {
            o.feasible = false;
            o.why_not = "双方都没有可交换的富余物资";
        } else if (att < -0.4f) {
            o.feasible = false;
            o.why_not = "两国积怨太深，对方不会接受";
        }
        O.push_back(o);
    }
    // Aid: food we can spare for a neighbour going hungry, given without payment.
    {
        const Registry& reg = *ctx_.reg;
        i32 n = 0;
        const ItemId food = ctx_.society->aid_food(p.id, other.id, &n);
        DecisionOption o = make("send_aid",
                                food != kNoItem ? strfmt("援助「%s」粮食（%s×%d）", other.name.c_str(), reg.item(food).name.c_str(), n)
                                                : "援助「" + other.name + "」粮食",
                                strfmt("对方存粮只够 %.1f 天。由我方的人把粮食送去，不求回报。", other.stats.food_days),
                                {{kWelfare, 0.6f}, {kCooperation, 0.8f}, {kFairness, 0.5f}, {kFrugality, -0.6f},
                                 {kFoodSecurity, -0.2f}, {kSelfPower, -0.1f}, {kRisk, -0.1f}},
                                act("send_aid"));
        o.action.set("other", (int)other.id);
        o.action.set("item", food != kNoItem ? reg.item(food).key : std::string());
        o.action.set("n", n);
        o.facts.set("their_food_days", other.stats.food_days);
        if (food == kNoItem) {
            o.feasible = false;
            o.why_not = other.stats.food_days < kAidHunger ? "我们自己也没有富余的粮食" : "对方并不缺粮";
        } else if (att < -0.5f) {
            o.feasible = false;
            o.why_not = "两国积怨太深";
        }
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

void Decisions::build_trade_offer_options(Decision& d, Polity& p, u16 from) {
    auto& O = d.options;
    Polity* other = ctx_.society->polity(from);
    const std::string on = other ? other->name : "?";
    const Registry& reg = *ctx_.reg;
    d.petition = Json::object();
    d.petition.set("other", (int)from);
    i32 n_in = 0, n_out = 0;
    const ItemId in = ctx_.society->trade_export(from, p.id, &n_in);
    const ItemId out = ctx_.society->trade_export(p.id, from, &n_out);
    std::string what;
    if (in != kNoItem) what = strfmt("对方能送来%s（约 %d）", reg.item(in).name.c_str(), n_in);
    if (out != kNoItem) what += (what.empty() ? "" : "，") + strfmt("我们可以用富余的%s（约 %d）交换", reg.item(out).name.c_str(), n_out);
    const bool food_in = in != kNoItem && reg.item(in).nutrition > 0;
    DecisionOption a = make("accept_trade", "接受与「" + on + "」通商", (what.empty() ? std::string("互通有无") : what) + "。",
                            {{kCooperation, 0.7f}, {kGrowth, 0.5f}, {kWelfare, 0.3f}, {kFoodSecurity, food_in ? 0.7f : 0.1f},
                             {kSelfPower, -0.1f}, {kRisk, -0.1f}},
                            act("trade_accept"));
    a.action.set("other", (int)from);
    a.bias += 0.5f * p.attitude_to(from) + trade_gain(reg, p, in, out);
    if (p.war_with(from)) {
        a.feasible = false;
        a.why_not = "两国正在交战";
    }
    O.push_back(a);
    O.push_back(make("refuse_trade", "拒绝通商", "不与对方往来，自给自足。",
                     {{kSelfPower, 0.4f}, {kOrder, 0.2f}, {kRisk, -0.2f}, {kCooperation, -0.5f}}, act("wait")));
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
    if (what == "propose_trade") {
        Polity* op = ctx_.society->polity(other);
        Character* ruler = op ? ctx_.agents->get(op->ruler) : nullptr;
        if (op && ruler && ruler->alive && !p.pact_with(other)) {
            Decision td;
            td.girl = ruler->id;
            td.polity = op->id;
            td.kind = "trade_offer";
            td.topic = "「" + p.name + "」提议通商";
            td.cause = cause;
            td.petitioner = g.id;
            build_trade_offer_options(td, *op, p.id);
            td.situation = describe_situation(*op, *ruler, td.topic);
            open(std::move(td));
        }
        return true;
    }
    if (what == "send_aid") {
        // Carriers take the food over in loads, like caravans that ask nothing back.
        const ItemId item = ctx_.reg->find_item(a.str("item"));
        i32 left = a.integer("n", 0);
        if (item == kNoItem || left <= 0 || !ctx_.society->polity(other)) return true;
        if (ctx_.society->at_war(p.id, other)) {
            // War broke out while she was deciding: the food stays home.
            Event e;
            e.type = EventType::Trade;
            e.severity = 3;
            e.actor = g.id;
            e.polity = p.id;
            e.causes[0] = cause;
            if (const War* w = p.war_with(other)) e.causes[1] = w->event;
            e.text = strfmt("「%s」本想援助「%s」，但战事已起，援粮没能送出", p.name.c_str(),
                            ctx_.society->polity(other)->name.c_str());
            ctx_.chron->emit(std::move(e));
            return true;
        }
        Event e;
        e.type = EventType::Trade;
        e.severity = 4;
        e.actor = g.id;
        e.polity = p.id;
        e.causes[0] = cause;
        e.text = strfmt("「%s」向「%s」送出援粮：%s×%d", p.name.c_str(), ctx_.society->polity(other)->name.c_str(),
                        ctx_.reg->item(item).name.c_str(), left);
        e.data.set("other", (int)other);
        const EventId ev = ctx_.chron->emit(std::move(e));
        StoreId dst = kNoStore;
        const Building* seat = ctx_.buildings->get(p.seat);
        float bd = 1e30f;
        for (StoreId sid : ctx_.society->public_stores(other))
            if (const Store* s = ctx_.econ->store(sid)) {
                const float d = seat ? (float)s->pos.dist2(seat->entrance) : 0.0f;
                if (d < bd) {
                    bd = d;
                    dst = sid;
                }
            }
        if (!dst) return true;
        const i32 load = std::max(1, (i32)std::floor(ctx_.agents->tune.carry_capacity / std::max(0.05f, ctx_.reg->item(item).weight)));
        for (StoreId sid : ctx_.society->public_stores(p.id)) {
            const Store* src = ctx_.econ->store(sid);
            i32 here = src ? std::min(left, ctx_.econ->available(sid, item)) : 0;
            while (here > 0) {
                Job j;
                j.type = JobType::Trade;
                j.polity = p.id;
                j.pos = src->pos;
                j.from = sid;
                j.to = dst;
                j.item = item;
                j.count = std::min(here, load);
                j.project = other;
                j.plot = 1;  // aid
                j.priority = 1.2f;
                j.created = now_;
                j.cause = ev;
                ctx_.jobs->add(j);
                here -= j.count;
                left -= j.count;
            }
            if (left <= 0) break;
        }
        return true;
    }
    if (what == "trade_accept") {
        ctx_.society->open_trade(other, p.id, g.id, cause);
        return true;
    }
    if (what == "end_trade") {
        if (Polity* op = ctx_.society->polity(other)) op->attitude_ref(p.id) = std::max(-1.0f, op->attitude_to(p.id) - 0.15f);
        ctx_.society->end_trade(p.id, other, g.name + "下令断绝往来", cause);
        return true;
    }
    return false;
}

void Decisions::consider_foreign(Polity& p, Character& ruler) {
    for (const Polity& oc : ctx_.society->polities()) {
        if (!oc.alive || oc.id == p.id) continue;
        Polity& other = *ctx_.society->polity(oc.id);
        const float att = p.attitude_to(other.id);
        // Not asked again too soon about the same neighbour (a mere chance to trade
        // comes up less often than a grudge).
        bool recent = false;
        for (size_t i = list_.size() > 96 ? list_.size() - 96 : 1; i < list_.size(); ++i) {
            const Decision& x = list_[i];
            if (x.polity != p.id || x.petition.integer("other", 0) != other.id) continue;
            if (x.kind != "diplomacy" && x.kind != "war" && x.kind != "trade_offer") continue;
            const Tick wait = x.kind == "war" ? kTicksPerDay : (att < 0.1f ? kTicksPerDay * 3 / 2 : kTicksPerDay * 3);
            if (x.status == DecisionStatus::Pending || x.status == DecisionStatus::AwaitingRemote || now_ - x.created < wait)
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
        const bool prospect = !w && att > -0.5f &&
                              ((!p.pact_with(other.id) && ctx_.society->trade_prospect(p.id, other.id)) ||
                               (other.stats.food_days < kAidHunger && ctx_.society->aid_food(p.id, other.id) != kNoItem));
        if (!w && settled && (att < 0.1f || prospect) && now_ > kTicksPerDay / 2) {
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
