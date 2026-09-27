// Jev pipeline: request lifecycle, providers, local persona model, persistence.
#include "icarus/decision/decisions.h"

#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

const char* feature_key(int f) {
    static const char* k[] = {"food_security", "welfare", "order", "harshness", "cooperation", "self_power",
                              "growth", "frugality", "military", "risk", "fairness", "speed"};
    return (f >= 0 && f < kFeatureCount) ? k[f] : "?";
}

const char* feature_name_zh(int f) {
    static const char* k[] = {"粮食安全", "民生福祉", "秩序", "严厉惩罚", "与其他魔法少女合作", "个人权力",
                              "长远发展", "节省资源", "武力", "冒险", "公平", "见效速度"};
    return (f >= 0 && f < kFeatureCount) ? k[f] : "?";
}

Decisions::Decisions(SimContext& ctx) : ctx_(ctx) {}

float Decisions::jitter(u64 a, u64 b, u64 c) const {
    u64 h = hash_combine(hash_combine(noise_seed_, a), hash_combine(b, c));
    // Sum of two uniforms: a soft bell in [-1, 1], scaled to a small tie-breaker.
    float u1 = (float)(h & 0xFFFFFF) / 16777216.0f, u2 = (float)((h >> 24) & 0xFFFFFF) / 16777216.0f;
    return (u1 + u2 - 1.0f) * 0.08f;
}

void Decisions::reset(u64 seed) {
    rng_.seed(seed, 0xDEC1);
    noise_seed_ = hash_combine(seed, 0x7E5E);
    list_.assign(1, Decision{});
    replay_.clear();
    whispers_.clear();
    value_whispers_.clear();
    whisper_cause_.clear();
    remote_used_today = 0;
}

namespace {
const Json* drive_json(const Registry& reg, const std::string& key) {
    for (const Json& d : reg.doc("drives")["drives"].items())
        if (d.str("key") == key) return &d;
    return nullptr;
}
std::string drive_zh(const Registry& reg, const std::string& key) {
    const Json* d = drive_json(reg, key);
    return d ? d->str("name", key) : key;
}
}  // namespace

std::vector<float> Decisions::weights(const Character& g) const {
    std::vector<float> w(kFeatureCount, 0.0f);
    if (!g.girl) return w;
    if (const Json* d = drive_json(*ctx_.reg, g.girl->drive)) {
        const Json& v = (*d)["values"];
        for (int f = 0; f < kFeatureCount; ++f) w[f] = v.flt(feature_key(f), 0.0f);
    }
    // Personality modulates how the drive is pursued.
    const Personality& p = g.girl->persona;
    auto c = [](float x) { return (x - 0.5f) * 2.0f; };  // -1..1
    w[kRisk] -= 0.6f * c(p.caution);
    w[kSpeed] -= 0.2f * c(p.caution);
    w[kWelfare] += 0.5f * c(p.altruism);
    w[kFairness] += 0.3f * c(p.altruism);
    w[kHarshness] -= 0.4f * c(p.altruism);
    w[kSelfPower] += 0.6f * c(p.ambition);
    w[kGrowth] += 0.3f * c(p.diligence);
    w[kOrder] += 0.4f * c(p.conformity);
    w[kCooperation] += 0.3f * c(p.conformity) + 0.3f * c(p.sociability);
    w[kMilitary] += 0.5f * c(p.aggression);
    w[kHarshness] += 0.4f * c(p.aggression);
    w[kFairness] += 0.4f * c(p.idealism);
    w[kFrugality] -= 0.3f * c(p.idealism);
    w[kSpeed] -= 0.2f * c(p.idealism);
    if (const ValueWhisper* vw = active_value_whisper(g.id)) w[(size_t)vw->feature] += vw->delta;
    return w;
}

const Decisions::ValueWhisper* Decisions::active_value_whisper(EntityId girl) const {
    auto it = value_whispers_.find(girl);
    if (it == value_whispers_.end() || now_ - it->second.at >= kTicksPerDay * 2) return nullptr;
    return &it->second;
}

void Decisions::whisper_value(EntityId girl, int feature, float delta, EventId cause) {
    if (feature < 0 || feature >= kFeatureCount) return;
    value_whispers_[girl] = ValueWhisper{feature, delta, now_, cause};
}

u32 Decisions::open(Decision d) {
    d.id = (u32)list_.size();
    d.created = now_;
    d.status = DecisionStatus::Pending;
    d.context = context_of(d);
    const Character* g = ctx_.agents->get(d.girl);
    Event e;
    e.type = EventType::DecisionRequested;
    e.severity = 2;
    e.actor = d.girl;
    e.polity = d.polity;
    e.causes[0] = d.cause;
    e.text = strfmt("%s正在考虑：%s", g ? g->name.c_str() : "?", d.topic.c_str());
    e.data.set("decision", (double)d.id);
    d.request_event = ctx_.chron->emit(std::move(e));
    list_.push_back(std::move(d));
    return list_.back().id;
}

u64 Decisions::context_of(const Decision& d) const {
    const Polity* p = ctx_.society->polity(d.polity);
    u64 h = hash_combine(d.polity, d.girl);
    if (!p) return h;
    h = hash_combine(h, p->ruler);
    for (const Crisis& c : p->crises) h = hash_combine(h, c.active ? (u64)c.kind * 16 + (u64)(c.severity * 4.0f) : 0);
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.polity == d.polity) h = hash_combine(h, b.functional ? b.id : ~(u64)b.id);
    return h;
}

void Decisions::step(Tick now) {
    now_ = now;
    if (now % kTicksPerDay == 0) remote_used_today = 0;
    if (now % kTicksPerHour == 0) hourly(now);
    size_t start = list_.size() > 256 ? list_.size() - 256 : 1;
    for (size_t i = start; i < list_.size(); ++i) {
        Decision& d = list_[i];
        switch (d.status) {
            case DecisionStatus::Pending: {
                auto rp = replay_.find(d.id);
                if (mode == "replay" && rp != replay_.end()) {
                    if (rp->second.answered > now) {
                        // The original answer arrived later (remote); wait for the same tick.
                        d.status = DecisionStatus::AwaitingRemote;
                        d.deadline = rp->second.answered;
                        break;
                    }
                    std::string err;
                    if (!submit(d.id, rp->second.choice, rp->second.rationale, "replay", err)) {
                        d.note = "回放记录不可用（" + err + "），改用本地模型";
                        decide_local(d, "fallback");
                    }
                } else if (mode == "remote" && remote_used_today < remote_budget_per_day) {
                    d.status = DecisionStatus::AwaitingRemote;
                    d.deadline = now + remote_deadline;
                    remote_used_today++;
                } else {
                    if (mode == "remote") d.note = "远程调用预算已用尽，改用本地人格模型";
                    decide_local(d, mode == "remote" ? "fallback" : "local");
                }
                break;
            }
            case DecisionStatus::AwaitingRemote:
                if (mode == "replay" && replay_.count(d.id) && now >= d.deadline) {
                    const ReplayEntry& re = replay_[d.id];
                    std::string err;
                    if (!submit(d.id, re.choice, re.rationale, "replay", err)) {
                        d.note = "回放记录不可用（" + err + "），改用本地模型";
                        decide_local(d, "fallback");
                    }
                } else if (now >= d.deadline) {
                    d.note = "远程决策超时，本地人格模型接管";
                    decide_local(d, "fallback");
                }
                break;
            case DecisionStatus::Executed:
                if (d.review_at && now >= d.review_at && d.outcome.empty()) review(d);
                break;
            default: break;
        }
    }
}

int Decisions::decisions_today(EntityId girl) const {
    int n = 0;
    for (size_t i = list_.size() > 64 ? list_.size() - 64 : 1; i < list_.size(); ++i)
        if (list_[i].girl == girl && now_ - list_[i].created < kTicksPerDay) ++n;
    return n;
}

void Decisions::hourly(Tick now) {
    (void)now;
    for (const Polity& pc : ctx_.society->polities()) {
        if (!pc.alive) continue;
        Polity* p = ctx_.society->polity(pc.id);
        girls_politics(*p);
        consider(*p);
    }
}

void Decisions::girls_politics(Polity& p) {
    Character* ruler = ctx_.agents->get(p.ruler);
    if (!ruler || !ruler->is_girl()) return;
    const Json* rd = drive_json(*ctx_.reg, ruler->girl->drive);
    float support_ruler = p.stats.ruler_support;
    for (auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed || !cp->is_girl() || cp->polity != p.id || cp->id == p.ruler) continue;
        GirlData& g = *cp->girl;
        const Json* gd = drive_json(*ctx_.reg, g.drive);
        float compat = 0;
        if (rd && gd) {
            compat += (rd->integer("valence", 1) == gd->integer("valence", 1)) ? 0.3f : -0.3f;
            compat += 0.3f * (rd->flt("cooperation", 0.5f) + gd->flt("cooperation", 0.5f) - 1.0f);
        }
        float my_support = 0;
        int n = 0;
        for (auto& rp : ctx_.agents->all())
            if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id) {
                my_support += rp->support_for(cp->id);
                ++n;
            }
        my_support = n ? my_support / (float)n : 0.0f;
        float crisis = 0;
        for (const Crisis& c : p.crises)
            if (c.active) crisis = std::max(crisis, c.severity);
        float d = 0.004f * compat + 0.003f * (g.persona.conformity - 0.5f) -
                  0.02f * g.persona.ambition * std::max(0.0f, my_support - support_ruler) -
                  0.004f * crisis * (1.0f - g.persona.conformity) + 0.002f * cp->affinity(p.ruler);
        // Time heals most quarrels, and a foreign enemy draws the girls together.
        d += 0.003f * (0.4f - g.loyalty);
        for (const War& w : p.wars)
            if (!w.attacker) {
                d += 0.004f;
                break;
            }
        g.loyalty = clampv(g.loyalty + d, -1.0f, 1.0f);
        g.stance = g.loyalty > 0.4f ? "loyal" : (g.loyalty > 0.1f ? "critical" : (g.loyalty > -0.3f ? "defiant" : "rebel"));
        // Mana regenerates slowly.
        float regen = gd ? gd->flt("mana_regen", 1.0f) : 1.0f;
        g.mana = std::min(1.0f, g.mana + 0.02f * regen);
    }
    if (ruler->girl) {
        ruler->girl->stance = "loyal";
        ruler->girl->loyalty = 1.0f;
        float regen = rd ? rd->flt("mana_regen", 1.0f) : 1.0f;
        ruler->girl->mana = std::min(1.0f, ruler->girl->mana + 0.02f * regen);
    }
}

void Decisions::consider(Polity& p) {
    Character* ruler = ctx_.agents->get(p.ruler);
    if (!ruler || !ruler->alive || ruler->departed) {
        // Succession: the most supported living girl takes over.
        Character* best = nullptr;
        float bs = -9;
        for (auto& cp : ctx_.agents->all()) {
            if (!cp || !cp->alive || cp->departed || !cp->is_girl() || cp->polity != p.id) continue;
            float s = 0;
            for (auto& rp : ctx_.agents->all())
                if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id) s += rp->support_for(cp->id);
            s += (float)cp->girl->level * 0.5f;
            if (s > bs) {
                bs = s;
                best = cp.get();
            }
        }
        if (best) ctx_.society->set_ruler(p.id, best->id, "succession", ruler ? ruler->death_event : 0);
        return;
    }
    bool ruler_busy = false;
    for (size_t i = list_.size() > 64 ? list_.size() - 64 : 1; i < list_.size(); ++i) {
        const Decision& d = list_[i];
        if (d.polity == p.id && d.girl == p.ruler &&
            (d.status == DecisionStatus::Pending || d.status == DecisionStatus::AwaitingRemote))
            ruler_busy = true;
    }
    // Crises first: the ruler responds to the most severe unaddressed one.
    if (!ruler_busy && decisions_today(p.ruler) < 6) {
        Crisis* pick = nullptr;
        for (Crisis& c : p.crises) {
            if (!c.active) continue;
            bool unaddressed = c.decision == 0;
            if (!unaddressed) {
                const Decision* prev = get(c.decision);
                // Re-plan if the earlier response has been judged and the crisis persists.
                if (prev && !prev->outcome.empty() && now_ - prev->answered > kTicksPerDay) unaddressed = true;
            }
            if (unaddressed && (!pick || c.severity > pick->severity)) pick = &c;
        }
        if (pick) {
            Decision d;
            d.girl = p.ruler;
            d.polity = p.id;
            d.kind = "crisis";
            d.crisis = pick->kind;
            d.topic = std::string("如何应对") + crisis_name_zh(pick->kind);
            d.cause = pick->event;
            build_crisis_options(d, p, *pick, *ruler);
            d.situation = describe_situation(p, *ruler, d.topic);
            gather_proposals(d, p);
            pick->decision = open(std::move(d));
            ruler_busy = true;
        }
    }
    // Choosing what to study next, whenever no research direction is set.
    if (!ruler_busy && p.policies.research.empty() && now_ > kTicksPerDay / 3 &&
        !ctx_.society->available_techs(p).empty()) {
        bool open_research = false;
        for (size_t i = list_.size() > 64 ? list_.size() - 64 : 1; i < list_.size(); ++i)
            if (list_[i].polity == p.id && list_[i].kind == "research" &&
                (list_[i].status == DecisionStatus::Pending || list_[i].status == DecisionStatus::AwaitingRemote ||
                 now_ - list_[i].created < kTicksPerHour * 6))
                open_research = true;
        if (!open_research) {
            Decision d;
            d.girl = p.ruler;
            d.polity = p.id;
            d.kind = "research";
            d.topic = "确定研究方向";
            build_research_options(d, p, *ruler);
            d.situation = describe_situation(p, *ruler, d.topic);
            gather_proposals(d, p);
            open(std::move(d));
            ruler_busy = true;
        }
    }
    // Neighbours: diplomacy, and the conduct of wars we started.
    if (!ruler_busy) consider_foreign(p, *ruler);
    // Routine governance every two days when calm.
    if (!ruler_busy && now_ - ruler->girl->last_decision > kTicksPerDay * 2 && now_ > kTicksPerDay / 2) {
        Decision d;
        d.girl = p.ruler;
        d.polity = p.id;
        d.kind = "governance";
        d.topic = "日常施政：调整政策与规划";
        build_governance_options(d, p, *ruler);
        d.situation = describe_situation(p, *ruler, d.topic);
        gather_proposals(d, p);
        ruler->girl->last_decision = now_;
        open(std::move(d));
    }
    // Other girls reconsider their stance toward the ruler.
    for (auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed || !cp->is_girl() || cp->polity != p.id || cp->id == p.ruler) continue;
        GirlData& g = *cp->girl;
        if (now_ - g.last_decision < kTicksPerDay * 2) continue;
        bool pending = false;
        for (size_t i = list_.size() > 64 ? list_.size() - 64 : 1; i < list_.size(); ++i)
            if (list_[i].girl == cp->id &&
                (list_[i].status == DecisionStatus::Pending || list_[i].status == DecisionStatus::AwaitingRemote))
                pending = true;
        if (pending) continue;
        float my_support = 0;
        int n = 0;
        for (auto& rp : ctx_.agents->all())
            if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id) {
                my_support += rp->support_for(cp->id);
                ++n;
            }
        my_support = n ? my_support / (float)n : 0.0f;
        bool long_crisis = false;
        for (const Crisis& c : p.crises)
            if (c.active && now_ - c.since > kTicksPerDay * 3 / 2) long_crisis = true;
        if (g.loyalty < 0.35f || my_support > p.stats.ruler_support + 0.15f || long_crisis) {
            Decision d;
            d.girl = cp->id;
            d.polity = p.id;
            d.kind = "stance";
            d.topic = "对统治者" + ruler->name + "的立场";
            for (const Crisis& c : p.crises)
                if (c.active) d.cause = c.event;
            build_stance_options(d, p, *cp);
            d.situation = describe_situation(p, *cp, d.topic);
            g.last_decision = now_;
            open(std::move(d));
        }
    }
}

std::string Decisions::describe_situation(const Polity& p, const Character& girl, const std::string& focus) const {
    const PolityStats& s = p.stats;
    std::string out;
    out += strfmt("时间：%s。你所在的国家：「%s」，人口 %d（其中魔法少女 %d 位）。\n", format_time_zh(now_).c_str(),
                  ctx_.society->title(p.id).c_str(), s.population, s.girls);
    out += strfmt("公共存粮约够 %.1f 天，%.0f%% 的居民吃得饱，%.0f%% 的居民喝得上水。居民平均心情 %.0f%%，对统治者的平均支持 %+.2f，正在抗议的居民 %d 人。\n",
                  s.food_days, s.food_access * 100, s.water_access * 100, s.mood * 100, s.ruler_support, s.protesters);
    const Policies& q = p.policies;
    static const char* dist[] = {"平均分配", "按劳分配", "精英优先"};
    out += strfmt("现行政策：口粮 ×%.2f，惩罚力度 %.0f%%，每日工时 %.0f 小时，%s。\n", q.ration, q.punishment * 100,
                  q.work_hours, dist[std::min<int>(2, q.distribution)]);
    for (const Crisis& c : p.crises) {
        if (!c.active) continue;
        out += strfmt("危机：%s（严重度 %.0f%%，已持续 %.1f 天）。", crisis_name_zh(c.kind), c.severity * 100,
                      (float)(now_ - c.since) / (float)kTicksPerDay);
        // Trace the cause chain a few steps back so she knows why.
        EventId cur = c.cause;
        std::string chain;
        for (int k = 0; k < 4 && cur; ++k) {
            const Event* e = ctx_.chron->get(cur);
            if (!e) break;
            chain += (chain.empty() ? "" : " ← ") + e->text;
            cur = e->causes[0];
        }
        if (!chain.empty()) out += "起因：" + chain + "。";
        out += "\n";
    }
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.polity == p.id && b.complete && !b.functional)
            out += strfmt("损毁的建筑：%s（完好度 %.0f%%）。\n", b.name.c_str(), b.integrity * 100);
    // The other girls.
    for (auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || !cp->is_girl() || cp->polity != p.id || cp->id == girl.id) continue;
        float sup = 0;
        int n = 0;
        for (auto& rp : ctx_.agents->all())
            if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id) {
                sup += rp->support_for(cp->id);
                ++n;
            }
        out += strfmt("象征%s的魔法少女%s（%s，民众支持 %+.2f，你对她的好感 %+.2f，她的立场：%s）。\n",
                      drive_zh(*ctx_.reg, cp->girl->drive).c_str(), cp->name.c_str(),
                      cp->id == p.ruler ? "统治者" : cp->girl->role.c_str(), n ? sup / (float)n : 0.0f,
                      girl.affinity(cp->id), cp->girl->stance.c_str());
    }
    // The neighbours: relations, wars and trade.
    for (const Polity& o : ctx_.society->polities()) {
        if (!o.alive || o.id == p.id) continue;
        std::string rel;
        if (p.war_with(o.id)) rel = "交战中";
        else if (const TradePact* t = p.pact_with(o.id))
            rel = t->blocked ? "通商，但商路受阻" : strfmt("通商中（我方商队往来 %d 次）", t->trips);
        else rel = "未通商";
        out += strfmt("邻国「%s」：人口 %d，存粮约 %.1f 天，你国对其态度 %+.2f，%s。\n", ctx_.society->title(o.id).c_str(),
                      o.stats.population, o.stats.food_days, p.attitude_to(o.id), rel.c_str());
    }
    // Her past choices and how they went.
    if (girl.girl) {
        int shown = 0;
        for (auto it = girl.girl->decisions.rbegin(); it != girl.girl->decisions.rend() && shown < 3; ++it, ++shown) {
            const Decision* d = get(*it);
            if (!d || d->chosen < 0) continue;
            out += strfmt("你之前的决定：%s → 「%s」%s。\n", d->topic.c_str(), d->options[(size_t)d->chosen].title.c_str(),
                          d->outcome.empty() ? "（结果未明）" : ("，结果：" + d->outcome).c_str());
        }
    }
    out += "当前议题：" + focus + "。";
    return out;
}

int Decisions::best_local(const Decision& d, const Character& g, std::string* why) {
    std::vector<float> w = weights(g);
    int best = -1;
    float bs = -1e9f;
    for (size_t i = 0; i < d.options.size(); ++i) {
        const DecisionOption& o = d.options[i];
        if (!o.feasible) continue;
        float s = 0;
        for (int f = 0; f < kFeatureCount; ++f) s += w[f] * o.f[f];
        s += o.bias;
        if (g.girl) s += 0.4f * g.girl->experience_of(o.key);
        s += jitter(d.id ? d.id : list_.size(), g.id, i);  // proposals are gathered before open()
        if (s > bs) {
            bs = s;
            best = (int)i;
        }
    }
    if (best >= 0 && why) {
        const DecisionOption& o = d.options[(size_t)best];
        int top = 0;
        float tv = -1e9f;
        for (int f = 0; f < kFeatureCount; ++f)
            if (w[f] * o.f[f] > tv) {
                tv = w[f] * o.f[f];
                top = f;
            }
        *why = std::string("出于对") + feature_name_zh(top) + "的重视";
    }
    return best;
}

void Decisions::gather_proposals(Decision& d, Polity& p) {
    std::string lines;
    for (auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed || !cp->is_girl() || cp->polity != p.id || cp->id == d.girl) continue;
        if (cp->girl->stance == "rebel") continue;  // no longer advises the ruler
        std::string why;
        int k = best_local(d, *cp, &why);
        if (k < 0) continue;
        Proposal pr;
        pr.girl = cp->id;
        pr.key = d.options[(size_t)k].key;
        pr.reason = why;
        lines += "- " + cp->name + "（" + cp->girl->temperament + "）建议：「" + d.options[(size_t)k].title + "」，" + why + "。\n";
        d.proposals.push_back(std::move(pr));
    }
    if (!lines.empty()) d.situation += "\n其他魔法少女的建议：\n" + lines;
}

void Decisions::resident_reaction(const Decision& d) {
    if (d.chosen < 0 || (d.kind != "crisis" && d.kind != "governance")) return;
    const DecisionOption& chosen = d.options[(size_t)d.chosen];
    auto util = [](const Character& r, const DecisionOption& o) {
        const Personality& q = r.pers;
        float v[kFeatureCount] = {0};
        v[kFoodSecurity] = 0.5f + 0.5f * q.caution;
        v[kWelfare] = 0.7f + 0.3f * q.altruism;
        v[kOrder] = 0.6f * q.conformity - 0.2f;
        v[kHarshness] = 0.8f * q.aggression - 0.6f;
        v[kCooperation] = 0.3f * q.sociability;
        v[kSelfPower] = -0.4f * q.idealism;
        v[kGrowth] = 0.5f * q.ambition;
        v[kFrugality] = 0.2f * q.caution;
        v[kMilitary] = 0.3f * q.aggression - 0.1f;
        v[kRisk] = -0.5f * q.caution;
        v[kFairness] = 0.8f * q.idealism;
        v[kSpeed] = 0.3f;
        float u = 0;
        for (int f = 0; f < kFeatureCount; ++f) u += v[f] * o.f[f];
        return u;
    };
    for (auto& rp : ctx_.agents->all()) {
        if (!rp || !rp->alive || rp->departed || rp->is_girl() || rp->polity != d.polity) continue;
        Character& r = *rp;
        float uc = util(r, chosen);
        r.support_ref(d.girl) = clampv(r.support_for(d.girl) + 0.05f * std::tanh(uc), -1.0f, 1.0f);
        for (const Proposal& pr : d.proposals) {
            if (pr.key == chosen.key) {
                r.support_ref(pr.girl) = clampv(r.support_for(pr.girl) + 0.02f * std::tanh(uc), -1.0f, 1.0f);
                continue;
            }
            for (const DecisionOption& o : d.options)
                if (o.key == pr.key) {
                    float delta = util(r, o) - uc;
                    r.support_ref(pr.girl) = clampv(r.support_for(pr.girl) + 0.04f * std::tanh(delta), -1.0f, 1.0f);
                }
        }
    }
}

void Decisions::decide_local(Decision& d, const std::string& source) {
    Character* g = ctx_.agents->get(d.girl);
    if (!g || !g->girl || d.options.empty()) {
        d.status = DecisionStatus::Cancelled;
        return;
    }
    std::vector<float> w = weights(*g);
    d.local_scores.assign(d.options.size(), -1e9f);
    auto wh = whispers_.find(d.girl);
    int best = -1, second = -1;
    for (size_t i = 0; i < d.options.size(); ++i) {
        const DecisionOption& o = d.options[i];
        if (!o.feasible) continue;
        float s = 0;
        for (int f = 0; f < kFeatureCount; ++f) s += w[f] * o.f[f];
        s += o.bias;
        s += 0.4f * g->girl->experience_of(o.key);
        for (const Proposal& pr : d.proposals)
            if (pr.key == o.key) s += 0.12f * w[kCooperation] * (1.0f + g->affinity(pr.girl));
        if (wh != whispers_.end() && wh->second.first == o.key && now_ - wh->second.second < kTicksPerDay * 2) s += 0.8f;
        s += jitter(d.id, d.girl, i);
        d.local_scores[i] = s;
        if (best < 0 || s > d.local_scores[(size_t)best]) {
            second = best;
            best = (int)i;
        } else if (second < 0 || s > d.local_scores[(size_t)second]) {
            second = (int)i;
        }
    }
    if (best < 0) {
        d.status = DecisionStatus::Cancelled;
        return;
    }
    // Rationale from the strongest contributions.
    const DecisionOption& o = d.options[(size_t)best];
    std::vector<std::pair<float, int>> contrib;
    for (int f = 0; f < kFeatureCount; ++f) contrib.push_back({w[f] * o.f[f], f});
    std::sort(contrib.begin(), contrib.end(), [](auto& a, auto& b) { return a.first > b.first; });
    // A value served by avoiding something (a negative weight on a negative feature)
    // reads as what she shuns.
    auto minds = [&](int f) { return std::string(o.f[f] >= 0 ? "看重" : "排斥") + feature_name_zh(f); };
    std::string why = g->girl->temperament + "的" + g->name + "最" + minds(contrib[0].second);
    if (contrib[1].first > 0.05f) {
        const bool same = (o.f[contrib[0].second] >= 0) == (o.f[contrib[1].second] >= 0);
        why += same ? std::string("与") + feature_name_zh(contrib[1].second) : "，也" + minds(contrib[1].second);
    }
    why += "，因此选择「" + o.title + "」。";
    if (second >= 0) {
        const DecisionOption& o2 = d.options[(size_t)second];
        int worst = 0;
        float wv = 1e9f;
        for (int f = 0; f < kFeatureCount; ++f) {
            float diff = w[f] * (o2.f[f] - o.f[f]);
            if (diff < wv) {
                wv = diff;
                worst = f;
            }
        }
        why += "她没有选「" + o2.title + "」，因为那在" + feature_name_zh(worst) + "上不合她的心意。";
    }
    if (wh != whispers_.end() && wh->second.first == o.key) why += "（冥冥中似乎有声音在耳边低语）";
    if (const ValueWhisper* vw = active_value_whisper(g->id))
        if (vw->delta * o.f[vw->feature] > 0.05f)
            why += std::string("（近来她莫名地") + (vw->delta > 0 ? "更在意" : "不再在意") + feature_name_zh(vw->feature) + "）";
    finalize(d, best, why, source);
}

bool Decisions::submit(u32 id, const std::string& key, const std::string& rationale, const std::string& source,
                       std::string& err) {
    if (id == 0 || id >= list_.size()) {
        err = "unknown decision";
        return false;
    }
    Decision& d = list_[id];
    if (d.status != DecisionStatus::AwaitingRemote && d.status != DecisionStatus::Pending) {
        err = "decision is no longer open";
        return false;
    }
    int idx = -1;
    for (size_t i = 0; i < d.options.size(); ++i)
        if (d.options[i].key == key) idx = (int)i;
    if (idx < 0 || !d.options[(size_t)idx].feasible) {
        err = "invalid or infeasible option '" + key + "'";
        d.note = "远程答复无效（" + err + "），本地人格模型接管";
        decide_local(d, "fallback");
        return false;
    }
    // Staleness: if the situation changed materially, the answer no longer applies.
    if (context_of(d) != d.context && source != "replay") {
        err = "stale: situation changed";
        d.note = "局势已变，远程答复过期，本地人格模型重新判断";
        decide_local(d, "fallback");
        return false;
    }
    finalize(d, idx, rationale, source);
    return true;
}

void Decisions::remote_failed(u32 id, const std::string& why) {
    if (id == 0 || id >= list_.size()) return;
    Decision& d = list_[id];
    if (d.status != DecisionStatus::AwaitingRemote) return;
    d.note = "远程决策失败（" + why + "），本地人格模型接管";
    decide_local(d, "fallback");
}

std::vector<u32> Decisions::awaiting_remote() const {
    std::vector<u32> out;
    for (size_t i = list_.size() > 256 ? list_.size() - 256 : 1; i < list_.size(); ++i)
        if (list_[i].status == DecisionStatus::AwaitingRemote) out.push_back(list_[i].id);
    return out;
}

std::string Decisions::remote_system_prompt(u32 id) const {
    const Decision* d = get(id);
    if (!d) return "";
    const Character* g = ctx_.agents->get(d->girl);
    if (!g || !g->girl) return "";
    const GirlData& gd = *g->girl;
    std::string drive = drive_zh(*ctx_.reg, gd.drive);
    std::vector<float> w = weights(*g);
    std::vector<std::pair<float, int>> vals;
    for (int f = 0; f < kFeatureCount; ++f) vals.push_back({w[f], f});
    std::sort(vals.begin(), vals.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string likes, dislikes;
    for (int i = 0; i < 4; ++i) likes += std::string(i ? "、" : "") + feature_name_zh(vals[(size_t)i].second);
    for (int i = 0; i < 3; ++i)
        dislikes += std::string(i ? "、" : "") + feature_name_zh(vals[vals.size() - 1 - (size_t)i].second);
    std::string pers;
    for (int i = 0; i < Personality::kCount; ++i)
        pers += strfmt("%s%s %.2f", i ? "，" : "", trait_name_zh(i), gd.persona.at(i));
    const Json* dj = drive_json(*ctx_.reg, gd.drive);
    int valence = dj ? dj->integer("valence", 1) : 1;
    std::string s;
    s += "你在一个上帝模拟游戏中扮演一位角色，为她做出战略决策。\n";
    s += "她是「象征" + drive + "的魔法少女，" + g->name + "」。源动力「" + drive + "」是" +
         (valence > 0 ? "正面" : "负面") + "的源动力。";
    s += "她最看重：" + likes + "；最不在意或排斥：" + dislikes + "。\n";
    if (const ValueWhisper* vw = active_value_whisper(g->id))
        s += std::string("近来她心中莫名地") + (vw->delta > 0 ? "更在意" : "不再在意") + feature_name_zh(vw->feature) +
             "，却说不清缘由。\n";
    s += "她的性情：" + gd.temperament + "（性格数值 0~1：" + pers + "）。\n";
    s += "职位：" + gd.role + "，魔法等级 " + std::to_string(gd.level) + "，对统治者的忠诚度 " + strfmt("%+.2f", gd.loyalty) + "。\n";
    s += "规则：方案的资源、耗时与可行性已由程序计算，不要质疑其中的数字。你要判断的是她重视什么、愿意承担什么代价、在冲突目标之间如何取舍。"
         "允许她做出冒险甚至有害的选择，只要这符合她的源动力与性格；不要替她做“最优解”，而要做“她会做的选择”。\n";
    s += "只输出 JSON：choice 为所选方案的 key，rationale 为她的内心理由（中文，第三人称或她的口吻，不超过 80 字）。";
    return s;
}

std::string Decisions::remote_user_prompt(u32 id) const {
    const Decision* d = get(id);
    if (!d) return "";
    std::string s = "【她所知道的情况】\n" + d->situation + "\n\n【可选方案】\n";
    for (const DecisionOption& o : d->options) {
        if (!o.feasible) continue;
        s += "- key: " + o.key + "｜" + o.title + "：" + o.desc;
        if (!o.facts.is_null()) s += " 数据：" + o.facts.dump();
        s += "\n";
    }
    s += "\n请为她选择一个方案。";
    return s;
}

Json Decisions::remote_schema(u32 id) const {
    const Decision* d = get(id);
    Json keys = Json::array();
    if (d)
        for (const DecisionOption& o : d->options)
            if (o.feasible) keys.push(o.key);
    Json schema = Json::object();
    schema.set("type", "object");
    Json props = Json::object();
    Json choice = Json::object();
    choice.set("type", "string");
    choice.set("enum", keys);
    props.set("choice", choice);
    Json rat = Json::object();
    rat.set("type", "string");
    props.set("rationale", rat);
    schema.set("properties", props);
    Json req = Json::array();
    req.push("choice");
    req.push("rationale");
    schema.set("required", req);
    schema.set("additionalProperties", false);
    return schema;
}

void Decisions::whisper(EntityId girl, const std::string& key, EventId cause) {
    whispers_[girl] = {key, now_};
    whisper_cause_[girl] = cause;
}

void Decisions::load_replay(const Json& log) {
    replay_.clear();
    for (const Json& e : log.items()) {
        if (e.str("choice").empty()) continue;
        ReplayEntry re;
        re.choice = e.str("choice");
        re.rationale = e.str("rationale");
        re.answered = (Tick)e.num("answered", 0);
        replay_[(u32)e.num("id")] = re;
    }
}

Json Decisions::export_log() const {
    Json out = Json::array();
    for (size_t i = 1; i < list_.size(); ++i) {
        const Decision& d = list_[i];
        Json e = Json::object();
        e.set("id", (double)d.id);
        e.set("tick", (double)d.created);
        e.set("answered", (double)d.answered);
        const Character* g = ctx_.agents->get(d.girl);
        e.set("girl", g ? g->name : "");
        e.set("kind", d.kind);
        e.set("topic", d.topic);
        Json opts = Json::array();
        for (const auto& o : d.options) {
            Json oj = Json::object();
            oj.set("key", o.key);
            oj.set("title", o.title);
            oj.set("feasible", o.feasible);
            opts.push(oj);
        }
        e.set("options", opts);
        e.set("choice", d.chosen >= 0 ? d.options[(size_t)d.chosen].key : "");
        e.set("rationale", d.rationale);
        e.set("source", d.source);
        e.set("outcome", d.outcome);
        out.push(e);
    }
    return out;
}

// ------------------------------------------------------------------------------ persistence

namespace {
void save_option(BinWriter& w, const DecisionOption& o) {
    w.str(o.key);
    w.str(o.title);
    w.str(o.desc);
    for (float f : o.f) w.f32(f);
    w.f32(o.bias);
    w.str(o.facts.is_null() ? "" : o.facts.dump());
    w.boolean(o.feasible);
    w.str(o.why_not);
    w.str(o.action.is_null() ? "" : o.action.dump());
}
DecisionOption load_option(BinReader& r) {
    DecisionOption o;
    o.key = r.str();
    o.title = r.str();
    o.desc = r.str();
    for (float& f : o.f) f = r.f32();
    o.bias = r.f32();
    std::string facts = r.str();
    if (!facts.empty()) o.facts = Json::parse(facts);
    o.feasible = r.boolean();
    o.why_not = r.str();
    std::string act = r.str();
    if (!act.empty()) o.action = Json::parse(act);
    return o;
}
}  // namespace

void Decisions::save(BinWriter& w) const {
    size_t s = w.begin_section("DECI");
    w.u64v(rng_.state());
    w.u64v(rng_.inc());
    w.u64v(noise_seed_);
    w.str(mode);
    w.vari(remote_budget_per_day);
    w.u64v(remote_deadline);
    w.vari(remote_used_today);
    w.varu(list_.size());
    for (size_t i = 1; i < list_.size(); ++i) {
        const Decision& d = list_[i];
        w.u64v(d.created);
        w.u64v(d.deadline);
        w.u64v(d.answered);
        w.u32v(d.girl);
        w.u16v(d.polity);
        w.str(d.kind);
        w.str(d.topic);
        w.u8v((u8)d.crisis);
        w.u32v(d.cause);
        w.u32v(d.request_event);
        w.u32v(d.decision_event);
        w.str(d.situation);
        w.varu(d.options.size());
        for (const auto& o : d.options) save_option(w, o);
        w.varu(d.local_scores.size());
        for (float f : d.local_scores) w.f32(f);
        w.u64v(d.context);
        w.u8v((u8)d.status);
        w.vari(d.chosen);
        w.str(d.rationale);
        w.str(d.source);
        w.str(d.note);
        w.u64v(d.review_at);
        w.f32(d.baseline);
        w.str(d.outcome);
        w.u32v(d.project);
        w.u32v(d.petitioner);
        w.str(d.petition.is_null() ? "" : d.petition.dump());
        w.varu(d.proposals.size());
        for (const Proposal& pr : d.proposals) {
            w.u32v(pr.girl);
            w.str(pr.key);
            w.str(pr.reason);
            w.boolean(pr.adopted);
        }
    }
    w.varu(whispers_.size());
    for (auto& [g, v] : whispers_) {
        w.u32v(g);
        w.str(v.first);
        w.u64v(v.second);
    }
    w.varu(value_whispers_.size());
    for (auto& [g, v] : value_whispers_) {
        w.u32v(g);
        w.vari(v.feature);
        w.f32(v.delta);
        w.u64v(v.at);
        w.u64v(v.cause);
    }
    w.varu(whisper_cause_.size());
    for (auto& [g, c] : whisper_cause_) {
        w.u32v(g);
        w.u64v(c);
    }
    w.end_section(s);
}

void Decisions::load(BinReader& outer) {
    BinReader r = outer.section("DECI");
    u64 st = r.u64v(), inc = r.u64v();
    rng_.set_raw(st, inc);
    noise_seed_ = r.u64v();
    mode = r.str();
    remote_budget_per_day = (int)r.vari();
    remote_deadline = r.u64v();
    remote_used_today = (int)r.vari();
    u64 n = r.varu();
    list_.assign(1, Decision{});
    for (size_t i = 1; i < (size_t)n; ++i) {
        Decision d;
        d.id = (u32)i;
        d.created = r.u64v();
        d.deadline = r.u64v();
        d.answered = r.u64v();
        d.girl = r.u32v();
        d.polity = r.u16v();
        d.kind = r.str();
        d.topic = r.str();
        d.crisis = (CrisisKind)r.u8v();
        d.cause = r.u32v();
        d.request_event = r.u32v();
        d.decision_event = r.u32v();
        d.situation = r.str();
        u64 no = r.varu();
        for (u64 k = 0; k < no; ++k) d.options.push_back(load_option(r));
        u64 ns = r.varu();
        for (u64 k = 0; k < ns; ++k) d.local_scores.push_back(r.f32());
        d.context = r.u64v();
        d.status = (DecisionStatus)r.u8v();
        d.chosen = (int)r.vari();
        d.rationale = r.str();
        d.source = r.str();
        d.note = r.str();
        d.review_at = r.u64v();
        d.baseline = r.f32();
        d.outcome = r.str();
        d.project = r.u32v();
        d.petitioner = r.u32v();
        std::string pj = r.str();
        if (!pj.empty()) d.petition = Json::parse(pj);
        u64 np = r.varu();
        for (u64 k = 0; k < np; ++k) {
            Proposal pr;
            pr.girl = r.u32v();
            pr.key = r.str();
            pr.reason = r.str();
            pr.adopted = r.boolean();
            d.proposals.push_back(std::move(pr));
        }
        list_.push_back(std::move(d));
    }
    whispers_.clear();
    u64 nw = r.varu();
    for (u64 k = 0; k < nw; ++k) {
        EntityId g = r.u32v();
        std::string key = r.str();
        Tick t = r.u64v();
        whispers_[g] = {key, t};
    }
    value_whispers_.clear();
    whisper_cause_.clear();
    if (!r.at_end()) {
        u64 nv = r.varu();
        for (u64 k = 0; k < nv; ++k) {
            EntityId g = r.u32v();
            ValueWhisper v;
            v.feature = (int)r.vari();
            v.delta = r.f32();
            v.at = r.u64v();
            v.cause = r.u64v();
            value_whispers_[g] = v;
        }
        u64 nc = r.varu();
        for (u64 k = 0; k < nc; ++k) {
            EntityId g = r.u32v();
            whisper_cause_[g] = r.u64v();
        }
    }
}

u64 Decisions::hash() const {
    u64 h = hash_combine(rng_.state(), list_.size());
    for (auto& d : list_) h = hash_combine(h, ((u64)d.chosen << 8) ^ (u64)d.status);
    return h;
}

}  // namespace icarus
