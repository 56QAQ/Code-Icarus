// Jev pipeline: program-generated options (with computed facts), execution and review.
#include <algorithm>
#include <map>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/agents/jobs.h"
#include "icarus/decision/decisions.h"
#include "decision_util.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/physics.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

using decision_util::F;
using decision_util::make;
using decision_util::act;

namespace {

const Json* drive_of(const Registry& reg, const std::string& key) {
    for (const Json& d : reg.doc("drives")["drives"].items())
        if (d.str("key") == key) return &d;
    return nullptr;
}

// Spell the girl can cast now (unlocked by level, enough mana), or nullptr.
const Json* castable(const Registry& reg, const Character& g, const std::string& effect) {
    if (!g.girl) return nullptr;
    const Json* d = drive_of(reg, g.girl->drive);
    if (!d) return nullptr;
    for (const Json& sp : (*d)["spells"].items()) {
        if (sp.str("effect") != effect || sp.str("type") != "active") continue;
        if (g.girl->level < sp.integer("level", 1)) return nullptr;
        if (g.girl->mana < sp.flt("mana", 0.3f)) return nullptr;
        return &sp;
    }
    return nullptr;
}

float avg_support(SimContext& ctx, u16 polity, EntityId girl) {
    float s = 0;
    int n = 0;
    for (auto& rp : ctx.agents->all())
        if (rp && rp->alive && !rp->departed && !rp->is_girl() && rp->polity == polity) {
            s += rp->support_for(girl);
            ++n;
        }
    return n ? s / (float)n : 0.0f;
}

int count_followers(SimContext& ctx, u16 polity, EntityId girl, EntityId ruler) {
    int n = 0;
    for (auto& rp : ctx.agents->all())
        if (rp && rp->alive && !rp->departed && !rp->is_girl() && rp->polity == polity) {
            float mine = rp->support_for(girl), theirs = rp->support_for(ruler);
            if (mine > 0.1f && mine > theirs + 0.1f) ++n;
        }
    return n;
}

const Building* broken_bridge(SimContext& ctx, u16 polity) {
    for (const Building& b : ctx.buildings->all())
        if (b.alive && b.is_bridge && b.polity == polity && !b.functional) return &b;
    return nullptr;
}

u32 polity_farm(SimContext& ctx, u16 polity) {
    for (const Farm& f : ctx.farming->all())
        if (f.alive && f.polity == polity) return f.id;
    return 0;
}

// The polity's field with the most room to grow by, and that room (up to n plots).
u32 farm_with_room(SimContext& ctx, u16 polity, int n, int& room) {
    room = 0;
    u32 best = 0;
    for (const Farm& f : ctx.farming->all())
        if (f.alive && f.polity == polity) {
            const int r = (int)ctx.farming->expansion(f.id, n).size();
            if (r > room) {
                room = r;
                best = f.id;
            }
        }
    return best;
}

i64 public_count(SimContext& ctx, u16 polity, const char* item) {
    ItemId it = ctx.reg->find_item(item);
    i64 n = 0;
    for (StoreId s : ctx.society->public_stores(polity)) n += ctx.econ->available(s, it);
    return n;
}

// Food residents hold privately: what idle people carry and what sits in their homes
// (cargo being hauled for a job is not private).
bool holds_private(const Character& r) { return r.task.type != TaskType::Work; }

int carried_food(SimContext& ctx, u16 polity) {
    int n = 0;
    for (auto& rp : ctx.agents->all()) {
        if (!rp || !rp->alive || rp->is_girl() || rp->polity != polity || !holds_private(*rp)) continue;
        if (const Store* s = ctx.econ->store(rp->inv))
            for (auto& st : s->items)
                if (ctx.reg->item(st.item).nutrition > 0) n += st.count;
    }
    for (const Store& s : ctx.econ->stores()) {
        if (!s.alive || s.kind != StoreKind::Home || s.polity != polity || !s.owner) continue;
        const Character* o = ctx.agents->get(s.owner);
        if (!o || o->is_girl()) continue;
        for (auto& st : s.items)
            if (ctx.reg->item(st.item).nutrition > 0) n += st.count;
    }
    return n;
}

// Cubes near p that are solid now but were open in the island's original terrain:
// whatever sealed a spring (a landslide, a meteor, a god's whim) can be dug away.
std::vector<Vec3i> obstruction_around(SimContext& ctx, const Vec3i& p, int radius) {
    std::map<Vec3i, std::vector<Voxel>> cells;
    const Registry& reg = *ctx.reg;
    auto pristine = [&](const Vec3i& q) -> MatId {
        Vec3i cc{floordiv(q.x, kCellSize), floordiv(q.y, kCellSize), floordiv(q.z, kCellSize)};
        auto it = cells.find(cc);
        if (it == cells.end()) {
            std::vector<Voxel> buf((size_t)kCellSize * kCellSize * kCellSize);
            ctx.world->gen().generate_cell(cc, buf.data());
            it = cells.emplace(cc, std::move(buf)).first;
        }
        Vec3i l{q.x - cc.x * kCellSize, q.y - cc.y * kCellSize, q.z - cc.z * kCellSize};
        return vmat(it->second[(size_t)((l.y * kCellSize + l.z) * kCellSize + l.x)]);
    };
    std::vector<Vec3i> out;
    for (int dy = -radius; dy <= radius + 2; ++dy)
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx) {
                Vec3i q = p + Vec3i{dx, dy, dz};
                if (!ctx.world->in_bounds(q) || q == p) continue;
                const Material& now = reg.mat(ctx.world->mat(q));
                if (!now.solid || !now.diggable) continue;
                if (reg.mat(pristine(q)).solid) continue;
                out.push_back(q);
            }
    return out;
}

const Vec3i* sealed_spring(SimContext& ctx, const Polity& p) {
    const Building* seat = ctx.buildings->get(p.seat);
    const auto& sp = ctx.physics->springs();
    const auto& ss = ctx.physics->spring_states();
    for (size_t i = 0; i < sp.size() && i < ss.size(); ++i)
        if (!ss[i].flowing && (!seat || sp[i].dist2(seat->entrance) < 200LL * 200LL)) return &sp[i];
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------------------------ builders

void Decisions::build_crisis_options(Decision& d, Polity& p, const Crisis& c, Character& girl) {
    const PolityStats& s = p.stats;
    const Policies& q = p.policies;
    auto& O = d.options;
    const Building* bb = broken_bridge(ctx_, p.id);
    u32 farm = polity_farm(ctx_, p.id);
    i64 planks = public_count(ctx_, p.id, "planks"), wood = public_count(ctx_, p.id, "wood");
    auto add_bridge = [&]() {
        if (!bb) return;
        if (const Project* running = bb->project ? ctx_.society->project(bb->project) : nullptr;
            running && running->status == 0) {
            // Already being repaired: the question is only whether to push harder.
            DecisionOption o = make("expedite_bridge", strfmt("加紧修复断桥（已完成 %.0f%%）", bb->integrity * 100.0f),
                                    "抽调更多人手优先修桥，其他工作放缓。",
                                    {{kFoodSecurity, 0.5f}, {kGrowth, 0.3f}, {kSpeed, 0.5f}, {kWelfare, 0.1f}, {kFrugality, -0.1f}},
                                    act("expedite"));
            o.action.set("project", (double)running->id);
            O.push_back(o);
            return;
        }
        auto need = ctx_.buildings->remaining_cost(*bb);
        int np = need.count(ctx_.reg->find_item("planks")) ? need[ctx_.reg->find_item("planks")] : 0;
        int nl = need.count(ctx_.reg->find_item("wood")) ? need[ctx_.reg->find_item("wood")] : 0;
        float days = 0.4f + (float)(np + nl) / 60.0f;
        DecisionOption o = make("rebuild_bridge", "修复断桥",
                                strfmt("需要木板 %d（库存 %lld，不足可用木材劈制，木材库存 %lld）、原木 %d，约 %.1f 天完工；修好后两岸物流恢复。",
                                       np, (long long)planks, (long long)wood, nl, days),
                                {{kFoodSecurity, 0.6f}, {kGrowth, 0.5f}, {kWelfare, 0.3f}, {kSpeed, -0.2f}, {kFrugality, -0.4f}, {kCooperation, 0.2f}},
                                act("repair"));
        o.action.set("building", (double)bb->id);
        o.facts.set("planks_needed", np);
        o.facts.set("days", days);
        if ((i64)np > planks + wood * 2) {
            o.feasible = false;
            o.why_not = "木材不足";
        }
        O.push_back(o);
    };
    auto add_field_storehouse = [&]() {
        if (!farm) return;
        const Farm* f = ctx_.farming->get(farm);
        DecisionOption o = make("field_storehouse", "在田边新建仓库",
                                "在田地旁建一座仓库，收成就近入库，减少往返搬运；需要原木约 40、木板约 60，约 2 天。",
                                {{kFoodSecurity, 0.5f}, {kGrowth, 0.6f}, {kSpeed, -0.4f}, {kFrugality, -0.5f}},
                                act("build"));
        o.action.set("def", "storehouse");
        Json near = Json::array();
        near.push(f->center.x);
        near.push(f->center.y);
        near.push(f->center.z);
        o.action.set("near", near);
        if (planks + wood * 2 < 60) {
            o.feasible = false;
            o.why_not = "木材不足";
        }
        O.push_back(o);
    };
    auto add_wait = [&](float food_pen) {
        O.push_back(make("wait", "静观其变", "不采取特别行动，等待局势自行好转。",
                         {{kFrugality, 0.8f}, {kSpeed, -0.8f}, {kRisk, 0.4f}, {kWelfare, -0.2f}, {kFoodSecurity, food_pen}},
                         act("wait")));
    };
    auto add_spell = [&](const std::string& effect, const std::string& key, std::initializer_list<F> feats) {
        const Json* sp = castable(*ctx_.reg, girl, effect);
        if (!sp) return;
        // Magic transforms real matter: no grain, no feast; no growing crops, nothing to hasten.
        if (effect == "feast" && public_count(ctx_, p.id, "grain") < 6) return;
        if (effect == "grow" && ctx_.farming->stats_polity(p.id).growing < 4) return;
        DecisionOption o = make(key, "施展魔法「" + sp->str("name") + "」", sp->str("desc"), feats, act("spell"));
        o.action.set("effect", effect);
        o.action.set("mana", sp->flt("mana", 0.3f));
        o.action.set("name", sp->str("name"));
        o.action.set("amount", sp->num("amount", 0));
        O.push_back(o);
    };

    switch (c.kind) {
        case CrisisKind::Food: {
            float days = s.food_days;
            // A foraging band has no larder to ration: it lives from what it finds each day.
            if (q.ration > 0.75f && !ctx_.society->foraging_band(p)) {
                DecisionOption o = make("ration", "实行口粮配给（×0.7）",
                                        strfmt("存粮约够 %.1f 天，配给后约 %.1f 天；居民会更常挨饿，不满会上升。", days, days / 0.8f),
                                        {{kFoodSecurity, 0.5f}, {kWelfare, -0.5f}, {kFairness, 0.3f}, {kHarshness, 0.15f}, {kFrugality, 0.6f}, {kSpeed, 0.8f}},
                                        act("policy"));
                o.action.set("ration", 0.7);
                O.push_back(o);
            }
            {
                int plots = 0, pop = std::max(1, s.population);
                for (const Farm& f : ctx_.farming->all())
                    if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
                if (plots < pop * 3) {
                    DecisionOption o = make("found_farm", "在水源附近开垦一片新田",
                                            strfmt("现有田地 %d 块，对 %d 口人来说太少；在湖边或池塘边新开 24 块田，约 3 天后见收成。", plots, pop),
                                            {{kFoodSecurity, 0.8f}, {kGrowth, 0.9f}, {kWelfare, 0.2f}, {kSpeed, -0.6f}, {kFrugality, -0.2f}},
                                            act("found_farm"));
                    o.action.set("n", 24);
                    if (public_count(ctx_, p.id, "grain") < 44) {
                        o.feasible = false;
                        o.why_not = "存粮太少，拿不出谷种";
                    }
                    O.push_back(o);
                }
            }
            if (farm) {
                const bool band = ctx_.society->foraging_band(p);
                const int grain = public_count(ctx_, p.id, "grain");
                const int keep = band ? 4 : 12;
                int n = std::clamp(grain - keep, 4, 20);
                int room = 0;
                const u32 fid = farm_with_room(ctx_, p.id, n, room);
                const bool no_room = room == 0;
                if (!no_room) n = std::min(n, room);
                DecisionOption o = make("expand_farms", strfmt("扩建田地（约 +%d 块）", n),
                                        strfmt("在灌溉渠附近开垦新田：每块要一份谷种（共约 %d 份），约 3 天后才有收成。", n),
                                        {{kFoodSecurity, 0.7f}, {kGrowth, 0.8f}, {kWelfare, 0.2f}, {kSpeed, -0.6f}, {kFrugality, -0.2f}},
                                        act("expand_farm"));
                o.action.set("n", n);
                if (fid) o.action.set("farm", (double)fid);
                int plots = 0;
                for (const Farm& f : ctx_.farming->all())
                    if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
                if (plots >= 3 * std::max(4, s.population)) {
                    o.feasible = false;
                    o.why_not = "田地已多到种不过来";
                } else if (no_room) {
                    o.feasible = false;
                    o.why_not = "田边已没有能灌溉的空地，只能去别处水边另开新田";
                } else if (grain < n + keep) {
                    o.feasible = false;
                    o.why_not = "存粮太少，拿不出谷种";
                }
                O.push_back(o);
            }
            {
                DecisionOption o = make("prioritize_food", "全力务农：粮食工作优先，其余暂缓",
                                        "把人手集中到收割、播种和烹饪上，建设与采集放慢。",
                                        {{kFoodSecurity, 0.5f}, {kGrowth, -0.3f}, {kSpeed, 0.4f}, {kOrder, 0.1f}}, act("policy"));
                o.action.set("pri_food", 1.8);
                o.action.set("pri_build", 0.5);
                o.action.set("pri_gather", 0.5);
                O.push_back(o);
            }
            int carried = carried_food(ctx_, p.id);
            {
                DecisionOption o = make("requisition", "征收居民私藏的食物",
                                        strfmt("居民身上约有 %d 份食物，没收后统一入库；会被视为强取豪夺。", carried),
                                        {{kFoodSecurity, carried > 6 ? 0.35f : 0.1f}, {kWelfare, -0.6f}, {kOrder, 0.4f}, {kHarshness, 0.7f}, {kFairness, -0.4f}, {kSpeed, 0.9f}, {kSelfPower, 0.3f}},
                                        act("requisition"));
                o.facts.set("carried_food", carried);
                if (carried < 3) {
                    o.feasible = false;
                    o.why_not = "居民身上几乎没有食物";
                }
                O.push_back(o);
            }
            if (q.distribution != 2) {
                DecisionOption o = make("elite_first", "精英优先分配",
                                        "粮食紧张时优先供给魔法少女与骨干，普通居民只能分到少量。",
                                        {{kWelfare, -0.5f}, {kFairness, -0.8f}, {kSelfPower, 0.6f}, {kOrder, 0.2f}, {kFoodSecurity, 0.15f}},
                                        act("policy"));
                o.action.set("distribution", 2);
                O.push_back(o);
            }
            {
                DecisionOption o = make("forage", "组织采集野果", "动员人手在附近采集浆果，见效快但数量有限。",
                                        {{kFoodSecurity, 0.25f}, {kSpeed, 0.6f}, {kWelfare, 0.1f}, {kGrowth, -0.1f}}, act("forage"));
                O.push_back(o);
            }
            int shirkers = 0;
            for (auto& rp : ctx_.agents->all())
                if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id && rp->work_debt > 3.0f) ++shirkers;
            if (shirkers > 0) {
                DecisionOption o = make("punish_shirkers", strfmt("严惩怠工者（%d 人）", shirkers),
                                        "惩罚长期不干活的人，以儆效尤；其他人会更害怕，也更不满。",
                                        {{kOrder, 0.7f}, {kHarshness, 0.9f}, {kWelfare, -0.4f}, {kSpeed, 0.3f}, {kFoodSecurity, 0.2f}}, act("punish"));
                o.facts.set("shirkers", shirkers);
                O.push_back(o);
            }
            add_bridge();
            if (bb) add_field_storehouse();
            add_spell("feast", "cast_feast", {{kWelfare, 0.8f}, {kFoodSecurity, 0.2f}, {kCooperation, 0.2f}, {kSpeed, 0.8f}, {kFrugality, -0.2f}});
            add_spell("grow", "cast_grow", {{kFoodSecurity, 0.6f}, {kSpeed, 0.9f}, {kGrowth, 0.2f}});
            if (girl.girl && girl.girl->drive == "gluttony") {
                DecisionOption o = make("hoard", "把余粮收归自己与亲信", "把公共粮仓的一部分划归魔法少女私用。",
                                        {{kSelfPower, 0.9f}, {kFoodSecurity, 0.1f}, {kWelfare, -0.7f}, {kFairness, -0.9f}, {kHarshness, 0.4f}},
                                        act("hoard"));
                if (ctx_.society->public_food(p.id) < 15.0f) {
                    o.feasible = false;
                    o.why_not = "公仓里已没有余粮可占";
                }
                O.push_back(o);
            }
            add_wait(-0.3f);
            break;
        }
        case CrisisKind::Logistics: {
            add_bridge();
            add_field_storehouse();
            if (farm) {
                const Farm* f = ctx_.farming->get(farm);
                DecisionOption o = make("field_huts", "让农夫在田边安家（建茅屋）",
                                        "在田地附近建茅屋，让部分居民就近居住耕作。",
                                        {{kWelfare, 0.2f}, {kGrowth, 0.5f}, {kFrugality, -0.4f}, {kSpeed, -0.3f}, {kFoodSecurity, 0.3f}},
                                        act("build"));
                o.action.set("def", "hut");
                Json near = Json::array();
                near.push(f->center.x);
                near.push(f->center.y);
                near.push(f->center.z);
                o.action.set("near", near);
                O.push_back(o);
            }
            O.push_back(make("detour", "绕路运输，不做改变", "接受更远的绕行路线，暂不投入建设。",
                             {{kFrugality, 0.7f}, {kSpeed, -0.5f}, {kWelfare, -0.2f}}, act("wait")));
            break;
        }
        case CrisisKind::Water: {
            const IslandFeatures& ft = ctx_.world->gen().features();
            const Building* seat = ctx_.buildings->get(p.seat);
            if (const Vec3i* spring = sealed_spring(ctx_, p)) {
                std::vector<Vec3i> cubes = obstruction_around(ctx_, *spring, 4);
                float days = 0.2f + (float)cubes.size() / 45.0f;
                DecisionOption o = make("clear_spring", strfmt("疏通泉眼（挖开 %zu 块堵塞物）", cubes.size()),
                                        strfmt("组织人手挖开封住泉眼的岩土，约 %.1f 天；泉水恢复后湖泊和灌溉渠会重新充盈。", days),
                                        {{kFoodSecurity, 0.7f}, {kWelfare, 0.4f}, {kGrowth, 0.5f}, {kSpeed, -0.2f}, {kFrugality, -0.1f}, {kCooperation, 0.2f}},
                                        act("dig"));
                Json list = Json::array();
                for (const Vec3i& q : cubes) {
                    Json v = Json::array();
                    v.push(q.x);
                    v.push(q.y);
                    v.push(q.z);
                    list.push(v);
                }
                o.action.set("cubes", list);
                o.action.set("title", "疏通泉眼");
                Json at = Json::array();
                at.push(spring->x);
                at.push(spring->y);
                at.push(spring->z);
                o.action.set("at", at);
                o.facts.set("cubes", (int)cubes.size());
                o.facts.set("days", days);
                if (cubes.empty()) {
                    o.feasible = false;
                    o.why_not = "找不到可以挖开的堵塞物";
                }
                O.push_back(o);
            }
            {
                int plots = 0;
                for (const Farm& f : ctx_.farming->all())
                    if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
                DecisionOption o = make("found_farm", "在仍有水的地方另开新田",
                                        "趁湖水尚在，在水边开垦新田；若水源不复，这些田也会干涸。",
                                        {{kFoodSecurity, 0.5f}, {kGrowth, 0.6f}, {kSpeed, -0.3f}, {kFrugality, -0.2f}}, act("found_farm"));
                o.action.set("n", 20);
                if (plots >= 3 * std::max(4, p.stats.population)) {
                    o.feasible = false;
                    o.why_not = "田地已多到种不过来";
                } else if (public_count(ctx_, p.id, "grain") < 40) {
                    o.feasible = false;
                    o.why_not = "存粮太少，拿不出谷种";
                }
                O.push_back(o);
            }
            DecisionOption o = make("lakeside_huts", "在湖边新建茅屋", "让居民住到水源附近。",
                                    {{kWelfare, 0.3f}, {kGrowth, 0.4f}, {kFrugality, -0.4f}, {kSpeed, -0.3f}}, act("build"));
            o.action.set("def", "hut");
            Json near = Json::array();
            near.push(ft.lake.x);
            near.push(ft.lake.y);
            near.push(ft.lake.z);
            o.action.set("near", near);
            O.push_back(o);
            add_spell("inspire", "cast_prayer", {{kWelfare, 0.5f}, {kCooperation, 0.3f}, {kSpeed, 0.7f}});
            (void)seat;
            add_wait(-0.1f);
            break;
        }
        case CrisisKind::Unrest: {
            O.push_back(make("crackdown", "镇压抗议者", "惩罚聚集抗议的人，驱散人群。",
                             {{kOrder, 0.8f}, {kHarshness, 1.0f}, {kWelfare, -0.6f}, {kSelfPower, 0.4f}, {kSpeed, 0.7f}, {kFairness, -0.4f}},
                             act("crackdown")));
            {
                DecisionOption o = make("concession", "让步：恢复口粮、缩短工时、减轻惩罚", "满足民众的主要诉求。",
                                        {{kWelfare, 0.7f}, {kFairness, 0.4f}, {kOrder, -0.3f}, {kSelfPower, -0.3f}, {kFrugality, -0.4f}, {kCooperation, 0.3f}},
                                        act("policy"));
                o.action.set("ration", std::max(1.0f, q.ration));
                o.action.set("work_hours", std::max(7.0f, q.work_hours - 1.0f));
                o.action.set("punishment", std::max(0.0f, q.punishment - 0.2f));
                O.push_back(o);
            }
            O.push_back(make("dialogue", "亲自倾听诉求", "统治者走到人群中与抗议者对话。",
                             {{kWelfare, 0.4f}, {kCooperation, 0.5f}, {kFairness, 0.5f}, {kSpeed, 0.3f}, {kSelfPower, -0.1f}}, act("dialogue")));
            // Blame or share power with another girl.
            Character* popular = nullptr;
            float ps = -9;
            for (auto& cp : ctx_.agents->all()) {
                if (!cp || !cp->alive || !cp->is_girl() || cp->polity != p.id || cp->id == girl.id) continue;
                float sp = avg_support(ctx_, p.id, cp->id);
                if (sp > ps) {
                    ps = sp;
                    popular = cp.get();
                }
            }
            if (popular) {
                DecisionOption o = make("scapegoat", "把矛头引向" + popular->name, "宣称危机源于她的失职。",
                                        {{kSelfPower, 0.6f}, {kCooperation, -0.8f}, {kFairness, -0.7f}, {kOrder, 0.3f}}, act("scapegoat"));
                o.action.set("target", (double)popular->id);
                O.push_back(o);
                DecisionOption a = make("share_power", "任命" + popular->name + "为大臣以平息民怨", "与她分享权力。",
                                        {{kCooperation, 0.9f}, {kSelfPower, -0.6f}, {kWelfare, 0.3f}, {kOrder, 0.2f}}, act("appoint"));
                a.action.set("target", (double)popular->id);
                O.push_back(a);
            }
            add_spell("inspire", "cast_prayer", {{kWelfare, 0.6f}, {kCooperation, 0.3f}, {kSpeed, 0.7f}});
            add_spell("terrify", "cast_terrify", {{kOrder, 0.9f}, {kHarshness, 0.9f}, {kWelfare, -0.7f}, {kSpeed, 0.9f}, {kSelfPower, 0.4f}});
            add_wait(0.0f);
            break;
        }
        case CrisisKind::War:
            build_defense_options(d, p, c);
            break;
        default:
            add_wait(0.0f);
    }
}

void Decisions::build_governance_options(Decision& d, Polity& p, Character& girl) {
    const Policies& q = p.policies;
    auto& O = d.options;
    O.push_back(make("keep", "维持现行政策", "一切照旧。", {{kOrder, 0.3f}, {kFrugality, 0.3f}}, act("wait")));
    int beds = 0, residents = 0;
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.polity == p.id && b.functional && b.def != "hall") beds += b.beds;  // (the hall's are the girls')
    for (auto& rp : ctx_.agents->all())
        if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id) ++residents;
    // The best home the polity knows how to build.
    std::string home_def, home_name;
    for (const char* k : {"lean_to", "hut", "longhouse"})
        if (const BuildingDef* hd = ctx_.buildings->def(k); hd && (hd->tech.empty() || p.has_tech(hd->tech))) {
            home_def = k;
            home_name = hd->name;
        }
    if (beds < residents && !home_def.empty()) {
        const Building* seat = ctx_.buildings->get(p.seat);
        DecisionOption o = make("build_housing", strfmt("兴建%s（缺 %d 个床位）", home_name.c_str(), residents - beds),
                                beds == 0 ? "大家至今露宿在外：搭起住处，夜里不再受冻。" : "让无家可归者有地方住。",
                                {{kWelfare, 0.5f}, {kGrowth, 0.6f}, {kFrugality, -0.4f}}, act("build"));
        o.action.set("def", home_def);
        if (seat) {
            Json near = Json::array();
            near.push(seat->entrance.x);
            near.push(seat->entrance.y);
            near.push(seat->entrance.z);
            o.action.set("near", near);
        }
        // Everyone sleeping on the ground presses harder than a few without a bed.
        o.bias += beds == 0 ? 0.4f : 0.3f * (float)(residents - beds) / (float)std::max(1, residents);
        // Many without a roof: several homes at once, as many as the builders can take on.
        if (const BuildingDef* hd = ctx_.buildings->def(home_def); hd && hd->beds > 0) {
            const int homes = (residents - beds + hd->beds - 1) / hd->beds;
            const int count = std::clamp(homes, 1, std::clamp(p.plan.staff[1] / 3, 1, 4));
            if (count > 1) {
                o.action.set("count", count);
                o.title = strfmt("兴建%s %d 座（缺 %d 个床位）", home_name.c_str(), count, residents - beds);
            }
        }
        O.push_back(o);
    }
    // Expansion: once the home village has grown, a new one by distant water. It widens
    // the land but brings the borders of others closer.
    // (Not while the home camp itself still sleeps in the open.)
    if (p.has_tech("farming") && now_ > p.founded + kTicksPerDay * 3 && p.outposts.size() < 3 && residents >= 16 &&
        !home_def.empty() && beds * 2 >= residents) {
        bool busy = false;
        for (const Project& pr : ctx_.society->projects())
            if (pr.alive && pr.status == 0 && pr.polity == p.id && pr.title.find("新村") != std::string::npos) busy = true;
        Vec3i center, water;
        const Building* seat = ctx_.buildings->get(p.seat);
        if (!busy && seat && ctx_.society->outpost_site(p.id, center, water)) {
            static const char* dirs[] = {"东", "东南", "南", "西南", "西", "西北", "北", "东北"};
            const float ang = std::atan2((float)(center.z - seat->entrance.z), (float)(center.x - seat->entrance.x));
            const int oct = ((int)std::lround(ang / 0.7853982f) + 8) % 8;
            const int dist = (int)std::sqrt((double)center.dist2(seat->entrance));
            int plots = 0;
            for (const Farm& f : ctx_.farming->all())
                if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
            DecisionOption o = make("found_outpost", strfmt("在%s方约 %d 步外的水边开拓新村落", dirs[oct], dist),
                                    "派人去远处建屋开田，扩大国土、养活更多的人；新村离别国更近，边境也会更紧张。",
                                    {{kGrowth, 1.0f}, {kFoodSecurity, 0.4f}, {kSelfPower, 0.3f}, {kRisk, 0.2f}, {kFrugality, -0.3f}},
                                    act("found_outpost"));
            Json cj = Json::array(), wj = Json::array();
            for (int v : {center.x, center.y, center.z}) cj.push(v);
            for (int v : {water.x, water.y, water.z}) wj.push(v);
            o.action.set("center", cj);
            o.action.set("water", wj);
            o.action.set("def", home_def);
            o.bias += (beds < residents ? 0.2f : 0.0f) + (plots < residents * 2 ? 0.25f : 0.0f);
            O.push_back(o);
        }
    }
    // Once people know how to farm: the first fields, sown with gathered wild grain.
    if (p.has_tech("farming")) {
        int plots = 0;
        for (const Farm& f : ctx_.farming->all())
            if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
        const int grain = public_count(ctx_, p.id, "grain");
        if (plots == 0) {
            DecisionOption o = make("found_farm", "开垦第一片田地",
                                    strfmt("把采来的野麦（%d 份）当作种子，种进水边翻好的土里；从此不必只靠采集和狩猎。", grain),
                                    {{kFoodSecurity, 0.9f}, {kGrowth, 0.9f}, {kWelfare, 0.2f}, {kSpeed, -0.4f}},
                                    act("found_farm"));
            o.action.set("n", std::clamp(grain - 2, 4, 24));
            if (grain < 6) {
                o.feasible = false;
                o.why_not = "还没攒下足够的野麦种子";
            }
            O.push_back(o);
        }
    }
    // Buildings a known technology allows but the polity does not have yet (homes are
    // offered above; the hall only as the step up from a campfire), the most basic first.
    {
        const Building* seat_b = ctx_.buildings->get(p.seat);
        const bool camp = seat_b && seat_b->def == "campfire";
        std::vector<const Json*> cands;
        for (const Json& bd : ctx_.reg->doc("buildings")["buildings"].items()) {
            if (!bd.has("tech") || !p.has_tech(bd.str("tech"))) continue;
            const std::string key = bd.str("key");
            if (bd.str("category") == "housing") continue;
            if (bd.boolean("seat", false) && !(key == "hall" && camp)) continue;
            bool have = false;
            for (const Building& b : ctx_.buildings->all())
                if (b.alive && b.polity == p.id && b.def == key) have = true;
            if (!have) cands.push_back(&bd);
        }
        // A people that can write but has nowhere to study builds that first: without it
        // no knowledge beyond the wild era grows.
        const bool no_study = ctx_.society->scholar_seats(p.id) == 0;
        // Once every wild-era tech is known, knowledge stands still until there is one.
        bool wild_left = false;
        for (const Json& t : ctx_.reg->doc("techs")["techs"].items())
            if (!ctx_.society->needs_scholars(t.str("key")) && !p.has_tech(t.str("key"))) wild_left = true;
        auto rank = [no_study](const std::string& cat) {
            if (cat == "research" && no_study) return -1;
            if (cat == "storage") return 0;
            if (cat == "production") return 1;
            if (cat == "civic") return 2;
            if (cat == "research") return 3;
            if (cat == "medicine") return 4;
            return 5;
        };
        std::stable_sort(cands.begin(), cands.end(),
                         [&](const Json* a, const Json* b) { return rank(a->str("category")) < rank(b->str("category")); });
        int offered = 0;
        for (const Json* bdp : cands) {
            if (offered >= 3) break;
            const Json& bd = *bdp;
            const std::string key = bd.str("key");
            const std::string cat = bd.str("category");
            std::vector<std::pair<int, float>> vals = {{kGrowth, 0.5f}, {kFrugality, -0.3f}};
            if (cat == "production") vals = {{kGrowth, 0.8f}, {kSpeed, 0.2f}, {kFrugality, -0.3f}};
            else if (cat == "research") vals = {{kGrowth, 0.9f}, {kOrder, 0.2f}, {kFrugality, -0.4f}};
            else if (cat == "storage") vals = {{kFoodSecurity, 0.7f}, {kFrugality, 0.2f}, {kGrowth, 0.3f}};
            else if (cat == "defense") vals = {{kMilitary, 0.8f}, {kOrder, 0.3f}, {kFrugality, -0.3f}};
            else if (cat == "medicine") vals = {{kWelfare, 0.9f}, {kGrowth, 0.2f}, {kFrugality, -0.2f}};
            else if (cat == "housing") vals = {{kWelfare, 0.5f}, {kGrowth, 0.5f}, {kFrugality, -0.4f}};
            else if (cat == "civic") vals = {{kOrder, 0.6f}, {kSelfPower, 0.4f}, {kGrowth, 0.3f}, {kFrugality, -0.5f}};
            DecisionOption o = make("build_" + key, "兴建" + bd.str("name"), bd.str("description"), {}, act("build"));
            for (const auto& [f, v] : vals) o.f[f] = v;
            if (cat == "research" && no_study) o.bias += wild_left ? 0.35f : 0.8f;
            o.action.set("def", key);
            if (const Building* seat = ctx_.buildings->get(p.seat)) {
                Json near = Json::array();
                near.push(seat->entrance.x);
                near.push(seat->entrance.y);
                near.push(seat->entrance.z);
                o.action.set("near", near);
            }
            O.push_back(o);
            ++offered;
        }
    }
    if (polity_farm(ctx_, p.id)) {
        // New fields grow by the seed put by, a few grain kept back for bread. Short of
        // fields for its people, a village wants them all the more when food runs low.
        const bool band = ctx_.society->foraging_band(p);
        const int grain = public_count(ctx_, p.id, "grain");
        const int keep = band ? 4 : 12;
        int n = std::clamp(grain - keep, 4, 16);
        int room = 0;
        const u32 fid = farm_with_room(ctx_, p.id, n, room);
        const bool no_room = room == 0;
        if (!no_room) n = std::min(n, room);
        int plots = 0;
        for (const Farm& f : ctx_.farming->all())
            if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
        // A field feeds less than half a person: short of fields below two per head.
        const bool short_fields = plots < 2 * p.stats.population;
        DecisionOption o = make("expand_farms", strfmt("扩建田地（约 +%d 块）", n),
                                band ? strfmt("把存下的谷种（%d 份）种进新开的田里，少靠一点采集。", grain)
                                     : strfmt("现有田地 %d 块、居民 %d 人；每块新田要一份谷种（存谷 %d 份）。", plots, p.stats.population, grain),
                                {{kFoodSecurity, band || short_fields ? 0.8f : 0.5f}, {kGrowth, 0.7f}, {kFrugality, -0.2f}, {kSpeed, -0.4f}},
                                act("expand_farm"));
        o.action.set("n", n);
        if (fid) o.action.set("farm", (double)fid);
        if (short_fields) o.bias += p.stats.food_days < 3.0f ? 0.35f : 0.2f;
        // What more fields are worth: much while there are too few to feed everyone, little
        // once there are more than enough (then the hands, the homes, the stores are what
        // is short — and a new field every day would crowd them all out).
        const float marginal = band ? 1.0f
                                    : clampv(0.25f + 2.0f * (float)(p.plan.plots_needed - plots) /
                                                         (float)std::max(1, p.plan.plots_needed),
                                             0.25f, 1.0f);
        for (float& f : o.f) f *= marginal;
        if (plots >= 3 * std::max(4, p.stats.population)) {
            o.feasible = false;
            o.why_not = "田地已多到种不过来";
        } else if (no_room) {
            o.feasible = false;
            o.why_not = "田边已没有能灌溉的空地";
        } else if (grain < n + keep) {
            o.feasible = false;
            o.why_not = "还没攒下足够的谷种";
        } else if (!band && p.stats.food_days < 1.0f && p.stats.food_access < 0.9f &&
                   (float)grain * 0.3f >= std::max(1.0f, p.plan.need)) {
            // (Only when the seed is a real meal for everyone: a handful of grain eaten is
            // gone in an hour, sown it feeds them many times over.)
            o.feasible = false;
            o.why_not = "正闹饥荒，谷种先留作口粮";
        }
        O.push_back(o);
        // The fields have filled the ground their water reaches: a new field by other water.
        if (no_room && short_fields && !band) {
            const int m = std::clamp(grain - keep, 4, 24);
            DecisionOption nf = make("found_farm", "在别处水边另开一片新田",
                                     strfmt("现有 %d 块田已占满了水渠能浇到的地；去附近另一处水边开出约 %d 块新田。", plots, m),
                                     {{kFoodSecurity, 0.8f}, {kGrowth, 0.8f}, {kWelfare, 0.2f}, {kSpeed, -0.5f}, {kFrugality, -0.2f}},
                                     act("found_farm"));
            nf.action.set("n", m);
            nf.bias += p.stats.food_days < 3.0f ? 0.35f : 0.2f;
            for (float& f : nf.f) f *= marginal;
            if (grain < m + keep) {
                nf.feasible = false;
                nf.why_not = "还没攒下足够的谷种";
            }
            O.push_back(nf);
        }
    }
    if (q.punishment < 0.85f) {
        DecisionOption o = make("harsher", "加重惩罚", "更严厉地对待怠工与偷窃。",
                                {{kOrder, 0.6f}, {kHarshness, 0.8f}, {kWelfare, -0.3f}, {kSelfPower, 0.2f}}, act("policy"));
        o.action.set("punishment", std::min(1.0f, q.punishment + 0.25f));
        O.push_back(o);
    }
    if (q.punishment > 0.15f) {
        DecisionOption o = make("gentler", "减轻惩罚", "宽以待人。",
                                {{kWelfare, 0.4f}, {kFairness, 0.3f}, {kOrder, -0.3f}, {kHarshness, -0.6f}}, act("policy"));
        o.action.set("punishment", std::max(0.0f, q.punishment - 0.25f));
        O.push_back(o);
    }
    if (q.work_hours < 11.0f) {
        DecisionOption o = make("longer_hours", "延长工时（+1小时）", "更多劳动，更快发展，但居民更累。",
                                {{kGrowth, 0.4f}, {kWelfare, -0.4f}, {kOrder, 0.2f}, {kFoodSecurity, 0.2f}}, act("policy"));
        o.action.set("work_hours", q.work_hours + 1.0f);
        O.push_back(o);
    }
    if (q.work_hours > 7.0f) {
        DecisionOption o = make("shorter_hours", "缩短工时（-1小时）", "让居民多休息。",
                                {{kWelfare, 0.4f}, {kGrowth, -0.3f}, {kFoodSecurity, -0.1f}}, act("policy"));
        o.action.set("work_hours", q.work_hours - 1.0f);
        O.push_back(o);
    }
    if (q.ration < 1.0f) {
        // Once the shortage is over, lifting the ration costs little.
        const Crisis* fc = p.crisis(CrisisKind::Food);
        const bool short_now = fc && fc->active;
        DecisionOption o = make("full_rations", "恢复足额口粮", short_now ? "让大家吃饱。" : "粮荒已过，让大家吃饱。",
                                {{kWelfare, short_now ? 0.6f : 0.9f}, {kFoodSecurity, short_now ? -0.3f : 0.0f},
                                 {kFairness, 0.2f}, {kFrugality, short_now ? -0.4f : -0.1f}},
                                act("policy"));
        o.action.set("ration", 1.0);
        O.push_back(o);
    }
    if (q.distribution != 1) {
        DecisionOption o = make("merit", "按劳分配并奖励勤劳者", "多劳多得，勤劳者得到额外口粮。",
                                {{kGrowth, 0.4f}, {kFairness, 0.3f}, {kOrder, 0.2f}, {kWelfare, 0.1f}, {kFrugality, -0.2f}}, act("policy"));
        o.action.set("distribution", 1);
        o.action.set("wage", 0.5);
        O.push_back(o);
    }
    if (q.distribution != 0) {
        DecisionOption o = make("equal", "平均分配", "人人一份。", {{kFairness, 0.6f}, {kWelfare, 0.3f}, {kSelfPower, -0.2f}}, act("policy"));
        o.action.set("distribution", 0);
        o.action.set("wage", 0.0);
        O.push_back(o);
    }
    const Json* sp = castable(*ctx_.reg, girl, "feast");
    if (sp && public_count(ctx_, p.id, "grain") >= 6) {
        DecisionOption o = make("cast_feast", "举办宴会：施展「" + sp->str("name") + "」", sp->str("desc"),
                                {{kWelfare, 0.7f}, {kCooperation, 0.3f}, {kFrugality, -0.3f}}, act("spell"));
        o.action.set("effect", "feast");
        o.action.set("mana", sp->flt("mana", 0.4f));
        o.action.set("name", sp->str("name"));
        o.action.set("amount", sp->num("amount", 12));
        O.push_back(o);
    }
    sp = castable(*ctx_.reg, girl, "inspire");
    if (sp) {
        DecisionOption o = make("cast_prayer", "鼓舞民心：施展「" + sp->str("name") + "」", sp->str("desc"),
                                {{kWelfare, 0.5f}, {kCooperation, 0.3f}, {kSelfPower, 0.2f}}, act("spell"));
        o.action.set("effect", "inspire");
        o.action.set("mana", sp->flt("mana", 0.5f));
        o.action.set("name", sp->str("name"));
        o.action.set("amount", sp->num("amount", 0.15));
        O.push_back(o);
    }
    // A change of policy is weighed against the policy it gives up: what rewards by work
    // gain in growth, equal shares lose in fairness. (Without this every change looks like
    // pure gain, and the rules flip back and forth every other day.)
    auto option = [&](const char* key) -> DecisionOption* {
        for (DecisionOption& o : O)
            if (o.key == key) return &o;
        return nullptr;
    };
    auto against = [&](DecisionOption* a, const DecisionOption& given_up, float scale) {
        if (!a) return;
        for (int f = 0; f < kFeatureCount; ++f) a->f[f] = clampv((a->f[f] - given_up.f[f]) * scale, -1.0f, 1.0f);
    };
    auto pair = [&](const char* ka, const char* kb, float scale) {
        DecisionOption* a = option(ka);
        DecisionOption* b = option(kb);
        if (!a || !b) return;
        const DecisionOption a0 = *a, b0 = *b;
        against(a, b0, scale);
        against(b, a0, scale);
    };
    pair("harsher", "gentler", 0.6f);
    pair("longer_hours", "shorter_hours", 0.7f);
    {
        const DecisionOption merit = make("merit", "", "", {{kGrowth, 0.4f}, {kFairness, 0.3f}, {kOrder, 0.2f}, {kWelfare, 0.1f}, {kFrugality, -0.2f}}, act("policy"));
        const DecisionOption equal = make("equal", "", "", {{kFairness, 0.6f}, {kWelfare, 0.3f}, {kSelfPower, -0.2f}}, act("policy"));
        if (q.distribution == 0) against(option("merit"), equal, 1.0f);
        if (q.distribution == 1) against(option("equal"), merit, 1.0f);
    }
}

void Decisions::build_stance_options(Decision& d, Polity& p, Character& girl) {
    auto& O = d.options;
    Character* ruler = ctx_.agents->get(p.ruler);
    std::string rn = ruler ? ruler->name : "统治者";
    const GirlData& g = *girl.girl;
    O.push_back(make("stay_loyal", "继续效忠" + rn, "履行职责，维护团结。",
                     {{kCooperation, 0.8f}, {kOrder, 0.4f}, {kSelfPower, -0.3f}, {kRisk, -0.3f}}, act("stance_loyal")));
    // Petition: demand what her values most object to.
    const Policies& q = p.policies;
    std::vector<float> w = weights(girl);
    Json demand = Json::object();
    std::string dtext;
    if (q.ration < 0.95f && w[kWelfare] > 0.3f) {
        demand.set("ration", 1.0);
        dtext = "恢复足额口粮";
    } else if (q.punishment > 0.45f && w[kHarshness] < 0.2f) {
        demand.set("punishment", std::max(0.1f, q.punishment - 0.3f));
        dtext = "减轻惩罚";
    } else if (q.punishment < 0.4f && w[kHarshness] > 0.4f) {
        demand.set("punishment", std::min(1.0f, q.punishment + 0.3f));
        dtext = "加重惩罚、整肃秩序";
    } else if (q.work_hours > 9.5f && w[kWelfare] > 0.3f) {
        demand.set("work_hours", q.work_hours - 1.5f);
        dtext = "缩短工时";
    } else if (q.distribution == 2 && w[kFairness] > 0.3f) {
        demand.set("distribution", 0);
        dtext = "平均分配粮食";
    } else {
        demand.set("pri_food", 1.5);
        dtext = "把粮食生产放在首位";
    }
    {
        DecisionOption o = make("petition", "向" + rn + "进谏：" + dtext, "正式提出要求，由统治者决定是否采纳。",
                                {{kCooperation, 0.4f}, {kFairness, 0.5f}, {kWelfare, 0.3f}, {kSelfPower, 0.2f}, {kRisk, 0.1f}},
                                act("petition"));
        o.action.set("demand", demand);
        o.action.set("text", dtext);
        for (const Memory& m : girl.memories)
            if (m.subject == p.ruler && (m.kind == MemoryKind::Punished || m.kind == MemoryKind::Insulted) &&
                now_ - m.tick < kTicksPerDay * 3) {
                o.feasible = false;
                o.why_not = m.kind == MemoryKind::Punished ? "刚因进谏受罚，不敢再提" : "上次进谏刚被驳回";
            }
        O.push_back(o);
    }
    {
        DecisionOption o = make("withdraw", "消极抵抗：拒绝履行职务", "不再为统治者效力，但也不公开反叛。",
                                {{kSelfPower, 0.3f}, {kCooperation, -0.6f}, {kOrder, -0.3f}, {kRisk, 0.3f}}, act("withdraw"));
        if (g.loyalty > 0.25f) {
            o.feasible = false;
            o.why_not = "她对统治者仍有相当的忠诚";
        }
        O.push_back(o);
    }
    int followers = count_followers(ctx_, p.id, girl.id, p.ruler);
    bool crisis = false;
    for (const Crisis& c : p.crises)
        if (c.active) crisis = true;
    {
        DecisionOption o = make("secede", strfmt("带领支持者（%d 人）脱离，另立国家", followers),
                                "带着追随者到别处建立自己的国家；新国家起初一无所有。",
                                {{kSelfPower, 0.9f}, {kCooperation, -0.9f}, {kRisk, 0.7f}, {kOrder, -0.3f}, {kGrowth, 0.2f}, {kFairness, 0.2f}},
                                act("secede"));
        o.facts.set("followers", followers);
        int residents = 0;
        for (auto& rp : ctx_.agents->all())
            if (rp && rp->alive && !rp->departed && !rp->is_girl() && rp->polity == p.id) ++residents;
        bool defending = false;
        for (const War& w : p.wars)
            if (!w.attacker) defending = true;
        if (followers < std::max(4, residents / 4)) {
            o.feasible = false;
            o.why_not = "追随者太少";
        } else if (g.loyalty > -0.4f) {
            o.feasible = false;
            o.why_not = "她尚未与统治者决裂";
        } else if (now_ < p.founded + kTicksPerDay * 4 || residents < 12) {
            o.feasible = false;
            o.why_not = "国家初立、人口尚少，分裂只会两败俱伤";
        } else if (defending && g.loyalty > -0.7f) {
            o.feasible = false;
            o.why_not = "外敌当前，不宜内讧";
        }
        O.push_back(o);
    }
    {
        float mine = (float)g.level * 2.0f + (float)followers;
        int loyalists = 0;
        for (auto& rp : ctx_.agents->all())
            if (rp && rp->alive && !rp->is_girl() && rp->polity == p.id && rp->support_for(p.ruler) > 0.2f) ++loyalists;
        float theirs = (ruler && ruler->girl ? (float)ruler->girl->level * 2.0f : 2.0f) + (float)loyalists;
        float chance = mine / std::max(1.0f, mine + theirs);
        DecisionOption o = make("coup", strfmt("发动政变夺权（胜算约 %.0f%%）", chance * 100.0f),
                                "推翻现任统治者，自己登上王座；失败则身败名裂。",
                                {{kSelfPower, 1.0f}, {kRisk, 0.9f}, {kHarshness, 0.4f}, {kCooperation, -1.0f}, {kOrder, -0.4f}, {kSpeed, 0.6f}},
                                act("coup"));
        o.facts.set("chance", chance);
        o.action.set("chance", chance);
        if (followers < 4 || chance < 0.25f) {
            o.feasible = false;
            o.why_not = "实力不足";
        } else if (now_ < p.founded + kTicksPerDay * 3) {
            o.feasible = false;
            o.why_not = "国家初立，人心未定";
        } else if (!p.reigns.empty() && now_ < p.reigns.back().from + kTicksPerDay * 3) {
            o.feasible = false;
            o.why_not = "新君初立，人心思定";
        } else if (g.loyalty > -0.3f) {
            o.feasible = false;
            o.why_not = "她还没有到要推翻统治者的地步";
        } else if (!crisis && p.stats.ruler_support > 0.2f) {
            o.feasible = false;
            o.why_not = "统治者仍得人心，时机未到";
        }
        O.push_back(o);
    }
    // Her relationship with the ruler pulls the local persona model.
    const float L = g.loyalty;
    for (DecisionOption& o : O) {
        if (o.key == "stay_loyal") o.bias = 0.8f * L;
        else if (o.key == "petition") o.bias = 0.3f - 0.5f * std::fabs(L - 0.2f);
        else if (o.key == "withdraw") o.bias = -0.4f * L;
        else if (o.key == "secede" || o.key == "coup") o.bias = -0.6f * L - 0.2f;
    }
}

void Decisions::build_petition_options(Decision& d, Polity& p, Character& ruler) {
    (void)p;
    auto& O = d.options;
    const Character* pet = ctx_.agents->get(d.petitioner);
    std::string pn = pet ? pet->name : "?";
    std::string text = d.petition.str("text");
    DecisionOption a = make("accept", "采纳" + pn + "的进谏：" + text, "接受她的要求并调整政策。",
                            {{kCooperation, 0.8f}, {kSelfPower, -0.4f}, {kFairness, 0.3f}, {kWelfare, 0.2f}}, act("petition_accept"));
    a.action.set("demand", d.petition["demand"]);
    O.push_back(a);
    O.push_back(make("refuse", "驳回进谏", "坚持原有方针。", {{kSelfPower, 0.5f}, {kCooperation, -0.4f}, {kOrder, 0.2f}},
                     act("petition_refuse")));
    O.push_back(make("punish_petitioner", "以不敬之罪惩处" + pn, "让所有人知道谁说了算。",
                     {{kSelfPower, 0.8f}, {kHarshness, 0.8f}, {kCooperation, -0.9f}, {kOrder, 0.3f}, {kRisk, 0.3f}},
                     act("petition_punish")));
    // Regard for the petitioner colours the answer.
    const float A = ruler.affinity(d.petitioner);
    for (DecisionOption& o : O) {
        if (o.key == "accept") o.bias = 0.4f * A;
        else if (o.key == "punish_petitioner") o.bias = -0.4f * A - 0.15f;
    }
}

void Decisions::build_research_options(Decision& d, Polity& p, Character& ruler) {
    (void)ruler;
    std::vector<std::string> keys = ctx_.society->available_techs(p);
    // The basics of the present age come before the wonders of the next.
    std::sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) {
        const Json* ta = ctx_.society->tech(a);
        const Json* tb = ctx_.society->tech(b);
        const int ea = ta->integer("era", 0), eb = tb->integer("era", 0);
        if (ea != eb) return ea < eb;
        float ca = ta->flt("cost"), cb = tb->flt("cost");
        return ca != cb ? ca < cb : a < b;
    });
    // What the steward recommends studying is always among those laid before the ruler.
    std::stable_partition(keys.begin(), keys.end(), [&](const std::string& k) { return p.plan.backing("research_" + k) > 0.0f; });
    if (keys.size() > 5) keys.resize(5);
    // What the people can put into it: scholars at their research buildings, or (for the
    // techs of the wild era) whoever muses at the fire or the hall.
    const int seats = ctx_.society->scholar_seats(p.id);
    int scholars = 0;
    for (const auto& cp : ctx_.agents->all())
        if (cp && cp->alive && !cp->departed && cp->polity == p.id && cp->occupation == "research") ++scholars;
    for (const std::string& k : keys) {
        const Json* t = ctx_.society->tech(k);
        float cost = t->flt("cost", 100.0f), done = 0;
        for (auto& r : p.research)
            if (r.first == k) done = r.second;
        const bool scholarly = ctx_.society->needs_scholars(k);
        float per_day = ctx_.society->research_per_day(p.id, scholarly);
        if (scholarly && seats > 0) per_day *= (float)std::max(1, scholars) / (float)seats;
        if (!scholarly) per_day += ctx_.society->research_per_day(p.id, true);
        DecisionOption o;
        o.key = "research_" + k;
        o.title = "研究「" + t->str("name") + "」";
        o.desc = per_day > 0.0f ? strfmt("%s 需要约 %.0f 点知识（已有 %.0f），约 %.1f 天。", t->str("desc").c_str(), cost, done,
                                         std::max(0.2f, (cost - done) / per_day))
                                : strfmt("%s 需要约 %.0f 点知识（已有 %.0f）。", t->str("desc").c_str(), cost, done);
        if (scholarly && seats == 0) {
            o.feasible = false;
            o.why_not = "还没有书写室：农耕时代以后的学问，要由学者在书写室里钻研";
        }
        for (const auto& [fk, fv] : (*t)["values"].members())
            for (int f = 0; f < kFeatureCount; ++f)
                if (fk == feature_key(f)) o.f[f] = fv.as_float();
        o.f[kSpeed] = clampv(0.6f - cost / 200.0f, -0.6f, 0.6f);
        o.action = act("research");
        o.action.set("tech", k);
        o.facts.set("cost", cost);
        d.options.push_back(o);
    }
}

// ------------------------------------------------------------------------------ finalize / execute

void Decisions::finalize(Decision& d, int idx, const std::string& rationale, const std::string& source) {
    d.chosen = idx;
    d.rationale = rationale;
    d.source = source;
    d.answered = now_;
    d.status = DecisionStatus::Decided;
    Character* g = ctx_.agents->get(d.girl);
    const DecisionOption& o = d.options[(size_t)idx];
    Event e;
    e.type = EventType::DecisionMade;
    e.severity = 3;
    e.actor = d.girl;
    e.polity = d.polity;
    e.causes[0] = d.cause;
    e.causes[1] = d.request_event;
    // A god's whisper that pushed her this way is part of the story.
    if (const ValueWhisper* vw = active_value_whisper(d.girl); vw && vw->delta * o.f[vw->feature] > 0.05f)
        e.causes[2] = vw->cause;
    if (auto wh = whispers_.find(d.girl);
        !e.causes[2] && wh != whispers_.end() && wh->second.first == o.key && now_ - wh->second.second < kTicksPerDay * 2)
        e.causes[2] = whisper_cause_[d.girl];
    e.text = strfmt("%s决定：「%s」", g ? g->name.c_str() : "?", o.title.c_str());
    e.data.set("decision", (double)d.id);
    e.data.set("rationale", rationale);
    e.data.set("source", source);
    if (!d.proposals.empty()) {
        Json props = Json::array();
        for (const Proposal& pr : d.proposals) {
            const Character* a = ctx_.agents->get(pr.girl);
            std::string title = pr.key;
            for (const DecisionOption& x : d.options)
                if (x.key == pr.key) title = x.title;
            Json pj = Json::object();
            pj.set("girl", (double)pr.girl);
            pj.set("name", a ? a->name : std::string(pr.girl == kNoEntity ? "内政官" : "?"));
            pj.set("option", title);
            pj.set("adopted", pr.key == o.key);
            props.push(pj);
        }
        e.data.set("proposals", props);
    }
    d.decision_event = ctx_.chron->emit(std::move(e));
    if (g && g->girl) {
        g->girl->decisions.push_back(d.id);
        if (g->girl->decisions.size() > 32) g->girl->decisions.erase(g->girl->decisions.begin());
        g->girl->xp += 10.0f;
    }
    // Advisers whose proposals were taken feel heard; the others less so.
    for (Proposal& pr : d.proposals) {
        pr.adopted = pr.key == o.key;
        Character* a = ctx_.agents->get(pr.girl);
        if (!a || !a->girl) continue;
        if (pr.adopted) {
            a->girl->loyalty = std::min(1.0f, a->girl->loyalty + 0.04f);
            a->girl->xp += 4.0f;
        } else {
            a->girl->loyalty = std::max(-1.0f, a->girl->loyalty - 0.03f * (1.2f - a->girl->persona.conformity));
        }
    }
    d.baseline = metric_for(d);
    resident_reaction(d);
    execute(d);
    d.status = DecisionStatus::Executed;
    d.review_at = now_ + kTicksPerDay * 3 / 2;
    if (g) level_ups(*g, d.decision_event);
}

void Decisions::level_ups(Character& g, EventId cause) {
    if (!g.girl) return;
    GirlData& gd = *g.girl;
    while (gd.level < 8 && gd.xp >= 40.0f * (float)gd.level) {
        gd.xp -= 40.0f * (float)gd.level;
        gd.level++;
        std::string learned;
        if (const Json* dj = drive_of(*ctx_.reg, gd.drive))
            for (const Json& sp : (*dj)["spells"].items())
                if (sp.integer("level", 1) == gd.level) learned = sp.str("name");
        Event le;
        le.type = EventType::LevelUp;
        le.severity = learned.empty() ? 1 : 3;
        le.actor = g.id;
        le.polity = g.polity;
        le.causes[0] = cause;
        le.text = learned.empty() ? strfmt("%s升到了 %d 级", g.name.c_str(), gd.level)
                                  : strfmt("%s升到了 %d 级，领悟了「%s」", g.name.c_str(), gd.level, learned.c_str());
        const EventId lev = ctx_.chron->emit(std::move(le));
        ctx_.agents->mark_girl(g.id, 0.0f, 0.12f, learned.empty() ? 0 : lev);  // growing into her power
    }
}

float Decisions::metric_for(const Decision& d) const {
    const Polity* p = ctx_.society->polity(d.polity);
    if (!p) return 0;
    const PolityStats& s = p->stats;
    switch (d.crisis) {
        case CrisisKind::Food: return s.food_days + 2.0f * s.food_access;
        case CrisisKind::Water: return s.water_access;
        case CrisisKind::Logistics: return broken_bridge(const_cast<SimContext&>(ctx_), d.polity) ? 0.0f : 1.0f;
        case CrisisKind::Unrest: return -(float)s.protesters + s.mood * 5.0f;
        default: return s.stability + s.mood;
    }
}

void Decisions::review(Decision& d) {
    float now_metric = metric_for(d);
    float delta = now_metric - d.baseline;
    float score = delta > 0.15f ? 1.0f : (delta < -0.15f ? -1.0f : 0.0f);
    d.outcome = score > 0 ? "局势好转" : (score < 0 ? "局势恶化" : "变化不大");
    Character* g = ctx_.agents->get(d.girl);
    if (g && g->girl && d.chosen >= 0) {
        const std::string& key = d.options[(size_t)d.chosen].key;
        bool found = false;
        for (auto& e : g->girl->experience)
            if (e.first == key) {
                e.second = 0.6f * e.second + 0.4f * score;
                found = true;
            }
        if (!found) g->girl->experience.push_back({key, 0.4f * score});
        if (score > 0) g->girl->xp += 15.0f;
    }
    Event e;
    e.type = EventType::Info;
    e.severity = 2;
    e.actor = d.girl;
    e.polity = d.polity;
    e.causes[0] = d.decision_event;
    e.text = strfmt("回顾「%s」：%s", d.chosen >= 0 ? d.options[(size_t)d.chosen].title.c_str() : "?", d.outcome.c_str());
    ctx_.chron->emit(std::move(e));
}

void Decisions::execute(Decision& d) {
    if (d.chosen < 0) return;
    const DecisionOption& o = d.options[(size_t)d.chosen];
    const Json& a = o.action;
    const std::string what = a.str("do");
    Polity* p = ctx_.society->polity(d.polity);
    Character* g = ctx_.agents->get(d.girl);
    if (!p || !g) return;
    const EventId cause = d.decision_event;
    if (execute_war(d, o, *p, *g)) return;
    auto residents = [&]() {
        std::vector<Character*> out;
        for (auto& rp : ctx_.agents->all())
            if (rp && rp->alive && !rp->departed && !rp->is_girl() && rp->polity == p->id) out.push_back(rp.get());
        return out;
    };
    auto policy_event = [&](const std::string& text) {
        Event e;
        e.type = EventType::PolicyChanged;
        e.severity = 3;
        e.actor = g->id;
        e.polity = p->id;
        e.causes[0] = cause;
        e.text = text;
        ctx_.chron->emit(std::move(e));
    };
    auto apply_policy = [&](const Json& j) {
        Policies& q = p->policies;
        // Emergency measures hold for two days (see Society::update_crises).
        if ((j.has("pri_food") && j.flt("pri_food") > 1.0f) || (j.has("pri_build") && j.flt("pri_build") < 1.0f) ||
            (j.has("ration") && j.flt("ration") < 1.0f))
            p->emergency_until = now_ + 2 * kTicksPerDay;
        if (j.has("ration")) q.ration = j.flt("ration");
        if (j.has("punishment")) q.punishment = j.flt("punishment");
        if (j.has("work_hours")) q.work_hours = j.flt("work_hours");
        if (j.has("distribution")) q.distribution = (u8)j.integer("distribution");
        if (j.has("pri_food")) q.pri_food = j.flt("pri_food");
        if (j.has("pri_build")) q.pri_build = j.flt("pri_build");
        if (j.has("pri_gather")) q.pri_gather = j.flt("pri_gather");
        if (j.has("wage")) q.wage = j.flt("wage");
        if (j.has("requisition")) q.requisition = j.flt("requisition");
    };
    auto spend_mana = [&](float m) {
        if (g->girl) g->girl->mana = std::max(0.0f, g->girl->mana - m);
        if (g->girl) g->girl->xp += 5.0f;
    };

    if (what == "policy") {
        apply_policy(a);
        policy_event("政策调整：" + o.title);
    } else if (what == "wait" || what == "stance_loyal") {
        if (what == "stance_loyal" && g->girl) g->girl->loyalty = std::min(1.0f, g->girl->loyalty + 0.1f);
    } else if (what == "repair") {
        u32 bid = (u32)a.num("building");
        Project pr;
        pr.polity = p->id;
        pr.kind = "repair";
        pr.title = o.title;
        pr.building = bid;
        pr.sponsor = g->id;
        pr.cause = cause;
        pr.decision = d.id;
        pr.priority = 1.4f;
        if (const Building* b = ctx_.buildings->get(bid)) pr.target = b->origin;
        d.project = ctx_.society->add_project(pr);
        ctx_.buildings->reopen(bid, d.project);
    } else if (what == "build") {
        const Json& nj = a["near"];
        Vec3i near{nj[0].as_int(), nj[1].as_int(), nj[2].as_int()};
        Vec3i origin;
        u8 rot = 0;
        std::string def = a.str("def");
        // Homes for many are laid out several at once (as many as the builders can take on);
        // the village grows outward when the ground near its heart is taken.
        const int count = std::max(1, (int)a.integer("count", 1));
        int started = 0;
        for (int k = 0; k < count; ++k) {
            if (!ctx_.buildings->find_site(def, near, 40, origin, rot) && !ctx_.buildings->find_site(def, near, 64, origin, rot))
                break;
            Project pr;
            pr.polity = p->id;
            pr.kind = "construct";
            pr.title = count > 1 ? strfmt("%s（%d/%d）", o.title.c_str(), k + 1, count) : o.title;
            pr.sponsor = g->id;
            pr.cause = cause;
            pr.decision = d.id;
            pr.target = origin;
            pr.priority = 1.2f;
            const u32 pid = ctx_.society->add_project(pr);
            if (!d.project) d.project = pid;
            u32 bid = ctx_.buildings->start_site(def, origin, rot, p->id, pid);
            if (Project* prp = ctx_.society->project(pid)) prp->building = bid;
            ++started;
        }
        if (!started) {
            d.note = "找不到合适的建址";
            d.fruitless = d.note;
        }
    } else if (what == "found_outpost") {
        const Json& cj = a["center"];
        const Json& wj = a["water"];
        const Vec3i center{cj[0].as_int(), cj[1].as_int(), cj[2].as_int()};
        const Vec3i water{wj[0].as_int(), wj[1].as_int(), wj[2].as_int()};
        Event e;
        e.type = EventType::Construction;
        e.severity = 4;
        e.actor = g->id;
        e.polity = p->id;
        e.pos = center;
        e.causes[0] = cause;
        e.text = "「" + p->name + "」" + o.title;
        const EventId ev = ctx_.chron->emit(std::move(e));
        // Homes and a storehouse for the settlers, and fields by the water.
        std::vector<std::string> defs{a.str("def"), a.str("def")};
        if (p->has_tech("storage")) defs.push_back("storehouse");
        int started = 0;
        for (const std::string& def : defs) {
            Vec3i origin;
            u8 rot = 0;
            if (!ctx_.buildings->find_site(def, center, 26, origin, rot)) continue;
            Project pr;
            pr.polity = p->id;
            pr.kind = "construct";
            pr.title = "新村：" + (ctx_.buildings->def(def) ? ctx_.buildings->def(def)->name : def);
            pr.sponsor = g->id;
            pr.cause = ev;
            pr.decision = d.id;
            pr.target = origin;
            pr.priority = 1.1f;
            const u32 pid = ctx_.society->add_project(pr);
            const u32 bid = ctx_.buildings->start_site(def, origin, rot, p->id, pid);
            if (Project* prp = ctx_.society->project(pid)) prp->building = bid;
            if (!d.project) d.project = pid;
            ++started;
        }
        const u32 fid = ctx_.farming->found(p->id, p->name + "新村的田地", water, 18, 24);
        p->outposts.push_back(center);
        if (!started && !fid) {
            d.note = "找不到合适的建址";
            d.fruitless = d.note;
        }
    } else if (what == "expand_farm") {
        u32 f = a.has("farm") ? (u32)a.integer("farm") : polity_farm(ctx_, p->id);
        if (const Farm* fm = ctx_.farming->get(f); !fm || fm->polity != p->id) f = polity_farm(ctx_, p->id);
        int n = ctx_.farming->expand(f, a.integer("n", 16));
        if (n > 0)
            policy_event(strfmt("开垦新田 %d 块", n));
        else
            d.fruitless = "田边已没有能灌溉的空地可开垦";
    } else if (what == "dig") {
        Project pr;
        pr.polity = p->id;
        pr.kind = "dig";
        pr.title = a.str("title", "挖掘工程");
        pr.sponsor = g->id;
        pr.cause = cause;
        pr.decision = d.id;
        pr.priority = 1.4f;
        if (a.has("at")) pr.target = Vec3i{a["at"][0].as_int(), a["at"][1].as_int(), a["at"][2].as_int()};
        pr.params = Json::object();
        pr.params.set("cubes", a["cubes"]);
        d.project = ctx_.society->add_project(pr);
    } else if (what == "research") {
        p->policies.research = a.str("tech");
        p->policies.pri_research = std::max(p->policies.pri_research, 0.6f);
        if (const Json* t = ctx_.society->tech(p->policies.research)) policy_event("确定研究方向：" + t->str("name"));
    } else if (what == "expedite") {
        if (Project* pr = ctx_.society->project((u32)a.num("project"))) {
            pr->priority = std::max(pr->priority, 2.0f);
            p->policies.pri_build = std::max(p->policies.pri_build, 1.5f);
            p->emergency_until = now_ + 2 * kTicksPerDay;
            policy_event("加紧推进：" + pr->title);
        }
    } else if (what == "found_farm") {
        // Nearest water surfaces to the seat that can irrigate fertile ground.
        const Building* seat = ctx_.buildings->get(p->seat);
        Vec3i from = seat ? seat->entrance : g->foot;
        const IslandFeatures& ft = ctx_.world->gen().features();
        std::vector<Vec3i> sites = {ft.pond, ft.lake};
        for (const Site& st : ft.sites) sites.push_back(st.water);
        for (const Vec3i& wv : ft.waters) sites.push_back(wv);
        // Only water within a morning's walk of home.
        sites.erase(std::remove_if(sites.begin(), sites.end(),
                                   [&](const Vec3i& wv) { return wv.y <= 0 || wv.dist2(from) > 110 * 110; }),
                    sites.end());
        std::sort(sites.begin(), sites.end(), [&](const Vec3i& a2, const Vec3i& b2) {
            return a2.dist2(from) != b2.dist2(from) ? a2.dist2(from) < b2.dist2(from) : a2 < b2;
        });
        u32 fid = 0;
        for (const Vec3i& w0 : sites) {
            if (w0.y <= 0) continue;
            fid = ctx_.farming->found(p->id, p->name + "的新田", w0, 24, a.integer("n", 24));
            if (fid) break;
        }
        policy_event(fid ? strfmt("在水边开垦了 %zu 块新田", ctx_.farming->get(fid)->plots.size()) : std::string("找不到可以开垦的水边良田"));
        if (!fid) d.fruitless = "找不到可以开垦的水边良田";
    } else if (what == "forage") {
        p->forage_until = now_ + kTicksPerDay;
        p->policies.pri_gather = std::max(p->policies.pri_gather, 1.2f);
        policy_event("组织采集野果");
    } else if (what == "requisition") {
        int total = 0;
        for (Character* r : residents()) {
            int n = 0;
            // Seized goods are left where they are found; porters bring them to storage.
            auto seize = [&](StoreId from, const Vec3i& at) {
                const Store* s = ctx_.econ->store(from);
                if (!s) return;
                std::vector<ItemStack> items = s->items;
                StoreId pile = kNoStore;
                for (auto& st : items) {
                    if (ctx_.reg->item(st.item).nutrition <= 0) continue;
                    if (!pile) pile = ctx_.econ->pile_at(at);
                    n += ctx_.econ->transfer(from, pile, st.item, st.count);
                }
            };
            if (holds_private(*r)) seize(r->inv, r->foot);
            std::vector<std::pair<StoreId, Vec3i>> homes;  // collected first: seizing creates piles
            for (const Store& hs : ctx_.econ->stores())
                if (hs.alive && hs.kind == StoreKind::Home && hs.owner == r->id) homes.push_back({hs.id, hs.pos});
            for (auto& [hid, hpos] : homes) seize(hid, hpos);
            if (n > 0) {
                total += n;
                r->remember(now_, MemoryKind::Punished, g->id, -0.15f, cause);
                r->fear = std::min(1.0f, r->fear + 0.2f);
            }
        }
        Event e;
        e.type = EventType::Punishment;
        e.severity = 3;
        e.actor = g->id;
        e.polity = p->id;
        e.causes[0] = cause;
        e.text = strfmt("%s下令征收私粮，共收缴 %d 份", g->name.c_str(), total);
        ctx_.chron->emit(std::move(e));
    } else if (what == "punish" || what == "crackdown") {
        int n = 0;
        for (Character* r : residents()) {
            bool target = what == "punish" ? r->work_debt > 3.0f : (r->task.type == TaskType::Protest);
            if (!target) continue;
            ++n;
            r->remember(now_, MemoryKind::Punished, g->id, what == "crackdown" ? -0.3f : -0.2f, cause);
            r->fear = std::min(1.0f, r->fear + 0.4f);
            r->work_debt = 0;
            r->support_ref(g->id) = clampv(r->support_for(g->id) - 0.15f, -1.0f, 1.0f);
            if (r->task.type == TaskType::Protest) {
                r->task = Task{};
                r->next_think = now_;
            }
            r->last_punished = now_;
        }
        for (Character* r : residents())
            if (r->pers.conformity > 0.6f) r->support_ref(g->id) = clampv(r->support_for(g->id) + 0.03f, -1.0f, 1.0f);
        if (what == "punish") p->policies.punishment = std::max(p->policies.punishment, 0.7f);
        Event e;
        e.type = EventType::Punishment;
        e.severity = 3;
        e.actor = g->id;
        e.polity = p->id;
        e.causes[0] = cause;
        e.text = strfmt("%s%s，%d 人受罚", g->name.c_str(), what == "crackdown" ? "镇压了抗议" : "惩处了怠工者", n);
        ctx_.chron->emit(std::move(e));
    } else if (what == "dialogue") {
        for (Character* r : residents())
            if (r->task.type == TaskType::Protest || r->support_for(g->id) < 0) {
                r->remember(now_, MemoryKind::Helped, g->id, 0.12f, cause);
                r->support_ref(g->id) = clampv(r->support_for(g->id) + 0.1f, -1.0f, 1.0f);
            }
    } else if (what == "scapegoat") {
        EntityId t = (EntityId)a.num("target");
        Character* tg = ctx_.agents->get(t);
        for (Character* r : residents()) r->support_ref(t) = clampv(r->support_for(t) - 0.2f, -1.0f, 1.0f);
        if (tg && tg->girl) {
            tg->girl->loyalty = std::max(-1.0f, tg->girl->loyalty - 0.35f);
            tg->girl->grudge = g->id;
            tg->affinity_ref(g->id) = clampv(tg->affinity(g->id) - 0.4f, -1.0f, 1.0f);
        }
    } else if (what == "appoint") {
        EntityId t = (EntityId)a.num("target");
        if (Character* tg = ctx_.agents->get(t); tg && tg->girl) {
            tg->girl->role = "minister";
            tg->girl->loyalty = std::min(1.0f, tg->girl->loyalty + 0.25f);
            for (Character* r : residents())
                if (r->support_for(t) > 0.2f) r->support_ref(g->id) = clampv(r->support_for(g->id) + 0.06f, -1.0f, 1.0f);
        }
    } else if (what == "hoard") {
        // Move part of the public food into a private store of the ruler (still physical).
        StoreId priv = ctx_.econ->create_store(StoreKind::Home, g->foot, 0, g->id, 400.0f);
        int moved = 0;
        for (StoreId s : ctx_.society->public_stores(p->id)) {
            const Store* st = ctx_.econ->store(s);
            if (!st) continue;
            std::vector<ItemStack> items = st->items;
            for (auto& it : items)
                if (ctx_.reg->item(it.item).nutrition > 0) moved += ctx_.econ->transfer(s, priv, it.item, it.count / 3);
        }
        for (Character* r : residents()) r->remember(now_, MemoryKind::Insulted, g->id, -0.1f, cause);
        policy_event(strfmt("%s把 %d 份粮食据为己有", g->name.c_str(), moved));
    } else if (what == "spell") {
        std::string eff = a.str("effect");
        spend_mana(a.flt("mana", 0.3f));
        Event e;
        e.type = EventType::SpellCast;
        e.severity = 3;
        e.actor = g->id;
        e.polity = p->id;
        e.causes[0] = cause;
        e.pos = g->foot;
        if (eff == "feast") {
            // Real grain becomes feast dishes: nothing is created from nothing.
            ItemId grain = ctx_.reg->find_item("grain"), feast = ctx_.reg->find_item("feast");
            int want = a.integer("amount", 12), done = 0;
            for (StoreId s : ctx_.society->public_stores(p->id)) {
                if (done >= want) break;
                int k = ctx_.econ->remove(s, grain, want - done, "feast_spell");
                if (k > 0) ctx_.econ->add(s, feast, k, "feast_spell");
                done += k;
            }
            e.text = strfmt("%s施展「%s」，把 %d 份谷物化作盛宴", g->name.c_str(), a.str("name").c_str(), done);
        } else if (eff == "grow") {
            int grown = 0;
            u32 fid = polity_farm(ctx_, p->id);
            if (Farm* f = ctx_.farming->get(fid)) {
                const MatId WATER = ctx_.reg->m().water, CROP = ctx_.reg->m().crop;
                for (auto& pl : f->plots) {
                    Vec3i up = pl.ground + Vec3i{0, 1, 0};
                    Voxel v = ctx_.world->get(up);
                    if (vmat(v) != CROP || vlevel(v) >= 7) continue;
                    // Each boosted crop draws a unit of nearby water.
                    bool drew = false;
                    for (int dz = -3; dz <= 3 && !drew; ++dz)
                        for (int dx = -3; dx <= 3 && !drew; ++dx)
                            for (int dy = -1; dy <= 0 && !drew; ++dy) {
                                Vec3i q = pl.ground + Vec3i{dx, dy, dz};
                                Voxel wv = ctx_.world->get(q);
                                if (vmat(wv) == WATER && vlevel(wv) >= 2) {
                                    ctx_.world->set(q, make_voxel(WATER, (u8)(vlevel(wv) - 1)), cause);
                                    drew = true;
                                }
                            }
                    if (!drew) continue;
                    pl.growth = std::min(1.0f, pl.growth + 0.3f);
                    ctx_.world->set(up, make_voxel(CROP, (u8)std::min(7, (int)(pl.growth * 7.0f))), cause);
                    ++grown;
                }
            }
            e.text = strfmt("%s施展「%s」，催熟了 %d 株作物", g->name.c_str(), a.str("name").c_str(), grown);
        } else if (eff == "inspire") {
            float amt = a.flt("amount", 0.15f);
            for (Character* r : residents()) {
                r->remember(now_, MemoryKind::Blessed, g->id, amt, cause);
                r->support_ref(g->id) = clampv(r->support_for(g->id) + 0.08f, -1.0f, 1.0f);
            }
            e.text = strfmt("%s施展「%s」，歌声抚慰了人心", g->name.c_str(), a.str("name").c_str());
        } else if (eff == "terrify") {
            int n = 0;
            for (Character* r : residents()) {
                r->fear = std::min(1.0f, r->fear + 0.5f);
                r->remember(now_, MemoryKind::Cursed, g->id, -0.2f, cause);
                if (r->task.type == TaskType::Protest) {
                    r->task = Task{};
                    r->next_think = now_;
                    ++n;
                }
            }
            e.text = strfmt("%s施展「%s」，%d 名抗议者在恐惧中散去", g->name.c_str(), a.str("name").c_str(), n);
        } else {
            e.text = g->name + "施展了魔法";
        }
        // Seen where it happens: the feast at her stores, growth over the fields, the song
        // and the dread around her.
        Vec3f at = g->pos + Vec3f(0.0f, 1.0f, 0.0f);
        float radius = eff == "inspire" ? 14.0f : (eff == "terrify" ? 10.0f : 4.0f);
        if (eff == "grow")
            if (const Farm* f = ctx_.farming->get(polity_farm(ctx_, p->id))) {
                at = Vec3f((float)f->center.x + 0.5f, (float)f->center.y + 1.0f, (float)f->center.z + 0.5f);
                radius = 10.0f;
            }
        ctx_.agents->note_spell(*g, eff, a.str("name"), at, kNoEntity, radius);
        ctx_.chron->emit(std::move(e));
    } else if (what == "petition") {
        Character* ruler = ctx_.agents->get(p->ruler);
        if (ruler) {
            Decision pd;
            pd.girl = ruler->id;
            pd.polity = p->id;
            pd.kind = "petition";
            pd.topic = g->name + "的进谏：" + a.str("text");
            pd.cause = cause;
            pd.petitioner = g->id;
            pd.petition = Json::object();
            pd.petition.set("demand", a["demand"]);
            pd.petition.set("text", a.str("text"));
            build_petition_options(pd, *p, *ruler);
            pd.situation = describe_situation(*p, *ruler, pd.topic);
            open(std::move(pd));
        }
    } else if (what == "petition_accept" || what == "petition_refuse" || what == "petition_punish") {
        Character* pet = ctx_.agents->get(d.petitioner);
        if (what == "petition_accept") {
            apply_policy(a["demand"]);
            policy_event("采纳进谏：" + d.petition.str("text"));
            if (pet && pet->girl) pet->girl->loyalty = std::min(1.0f, pet->girl->loyalty + 0.25f);
            for (Character* r : residents())
                if (r->support_for(d.petitioner) > 0.1f) r->support_ref(g->id) = clampv(r->support_for(g->id) + 0.05f, -1.0f, 1.0f);
        } else if (pet && pet->girl) {
            pet->girl->loyalty = std::max(-1.0f, pet->girl->loyalty - (what == "petition_punish" ? 0.3f : 0.1f));
            pet->affinity_ref(g->id) = clampv(pet->affinity(g->id) - (what == "petition_punish" ? 0.4f : 0.15f), -1.0f, 1.0f);
            pet->remember(now_, what == "petition_punish" ? MemoryKind::Punished : MemoryKind::Insulted, g->id,
                          what == "petition_punish" ? -0.3f : -0.1f, d.decision_event);
            if (what == "petition_punish") {
                pet->girl->grudge = g->id;
                pet->girl->role = "none";
                ctx_.agents->mark_girl(pet->id, 0.3f, 0.0f, d.decision_event);  // humiliated by her own ruler
                for (Character* r : residents())
                    if (r->support_for(d.petitioner) > 0.2f) r->support_ref(g->id) = clampv(r->support_for(g->id) - 0.1f, -1.0f, 1.0f);
            }
        }
    } else if (what == "withdraw") {
        if (g->girl) {
            g->girl->role = "none";
            g->girl->loyalty = std::max(-1.0f, g->girl->loyalty - 0.15f);
            g->girl->stance = "defiant";
        }
        policy_event(g->name + "拒绝继续为统治者效力");
    } else if (what == "secede") {
        // A new polity: the girl, her followers, part of the fields and a share of goods.
        const Json& names = ctx_.reg->doc("names");
        std::string name;
        for (const Json& n : names["polity_names"].items()) {
            bool used = false;
            for (const Polity& other : ctx_.society->polities())
                if (other.alive && other.name == n.as_str()) used = true;
            if (!used) {
                name = n.as_str();
                break;
            }
        }
        if (name.empty()) name = g->name + "之邦";
        const u16 old_id = p->id;
        const EntityId old_ruler = p->ruler;
        const std::string old_title = ctx_.society->title(old_id);
        const Vec3i old_seat_pos = ctx_.buildings->get(p->seat) ? ctx_.buildings->get(p->seat)->entrance : g->foot;
        int pop_before = 0;
        for (Character* r : residents()) (void)r, ++pop_before;
        const Json& colors = names["polity_colors"];
        u32 color = parse_color(colors[(size_t)(ctx_.society->polities().size() % std::max<size_t>(1, colors.size()))].as_str());
        u16 nid = ctx_.society->create_polity(name, color, old_id);
        p = ctx_.society->polity(old_id);  // the polity vector may have reallocated
        g->polity = nid;
        if (g->girl) {
            g->girl->role = "ruler";
            g->girl->loyalty = 1.0f;
            g->girl->stance = "loyal";
        }
        ctx_.society->set_ruler(nid, g->id, "secession", cause);
        Polity* np = ctx_.society->polity(nid);
        np->techs = p->techs;
        if (const Json* dj = drive_of(*ctx_.reg, g->girl->drive)) {
            np->policies.punishment = dj->flt("punish_bias", 0.3f);
            np->policies.distribution = dj->integer("valence", 1) < 0 ? 2 : 0;
        }
        std::vector<Character*> followers;
        for (Character* r : residents()) {
            float mine = r->support_for(g->id), theirs = r->support_for(old_ruler);
            if (mine > 0.1f && mine > theirs + 0.1f) followers.push_back(r);
        }
        for (Character* r : followers) {
            r->polity = nid;
            r->home = 0;
            r->task = Task{};
            r->next_think = now_;
            if (Store* inv = ctx_.econ->store(r->inv)) inv->polity = nid;
        }
        const float share = clampv((float)followers.size() / (float)std::max(1, pop_before), 0.3f, 0.7f);
        // The fields farthest from the old village go with them; the camp is set up there.
        Vec3i camp = g->foot;
        if (u32 fid = polity_farm(ctx_, old_id)) {
            const Farm* f = ctx_.farming->get(fid);
            Vec3i d{f->center.x - old_seat_pos.x, 0, f->center.z - old_seat_pos.z};
            Vec3i away{f->center.x + d.x, f->center.y, f->center.z + d.z};
            u32 nf = ctx_.farming->split(fid, nid, away, share, name + "的田地");
            if (const Farm* nfp = ctx_.farming->get(nf)) camp = nfp->center;
        }
        Vec3i st;
        Vec3i probe{camp.x + (camp.x > old_seat_pos.x ? 6 : -6), camp.y + 1, camp.z + (camp.z > old_seat_pos.z ? 6 : -6)};
        if (ctx_.nav->find_standable_near(probe, st, 6) || ctx_.nav->find_standable_near(camp + Vec3i{0, 1, 0}, st, 8)) camp = st;
        StoreId stock = ctx_.econ->create_store(StoreKind::Stockpile, camp, nid, kNoEntity, 600.0f);
        const Json* dj = drive_of(*ctx_.reg, g->girl->drive);
        float hostility = dj && dj->integer("valence", 1) < 0 ? -0.6f : -0.3f;
        np = ctx_.society->polity(nid);
        np->attitude.push_back({old_id, hostility});
        p->attitude.push_back({nid, hostility});
        Event e;
        e.type = EventType::Secession;
        e.severity = 5;
        e.actor = g->id;
        e.polity = nid;
        e.pos = camp;
        e.causes[0] = cause;
        e.text = strfmt("%s带领 %d 名支持者脱离「%s」，建立了「%s」", g->name.c_str(), (int)followers.size(),
                        old_title.c_str(), ctx_.society->title(nid).c_str());
        e.data.set("followers", (int)followers.size());
        e.data.set("from", (int)old_id);
        EventId sev = ctx_.chron->emit(std::move(e));
        // They carry off a share of the common goods: real hauling from the old stores.
        const char* take[] = {"grain", "bread", "berries", "wood", "planks", "stone_tools", "fiber"};
        const float take_share = share * 0.6f;
        for (const char* key : take) {
            ItemId it = ctx_.reg->find_item(key);
            if (it == kNoItem) continue;
            float unit = std::max(0.01f, ctx_.reg->item(it).weight);
            i32 load = std::max(1, (i32)std::floor(ctx_.agents->tune.carry_capacity / unit));
            for (StoreId sid : ctx_.society->public_stores(old_id)) {
                i32 n = (i32)std::floor((float)ctx_.econ->available(sid, it) * take_share);
                const Store* src = ctx_.econ->store(sid);
                while (n > 0 && src) {
                    Job j;
                    j.type = JobType::HaulToSite;
                    j.polity = nid;
                    j.pos = src->pos;
                    j.from = sid;
                    j.to = stock;
                    j.item = it;
                    j.count = std::min(n, load);
                    j.priority = 1.5f;
                    j.created = now_;
                    j.cause = sev;
                    ctx_.jobs->add(j);
                    n -= j.count;
                }
            }
        }
        // The first shelter of the new polity.
        Vec3i origin;
        u8 rot = 0;
        if (ctx_.buildings->find_site("hut", camp, 30, origin, rot)) {
            Project pr;
            pr.polity = nid;
            pr.kind = "construct";
            pr.title = "新国家的第一座茅屋";
            pr.sponsor = g->id;
            pr.cause = sev;
            pr.priority = 1.2f;
            pr.target = origin;
            u32 pid = ctx_.society->add_project(pr);
            u32 bid = ctx_.buildings->start_site("hut", origin, rot, nid, pid);
            if (Project* prp = ctx_.society->project(pid)) prp->building = bid;
            if (Polity* npp = ctx_.society->polity(nid)) npp->seat = bid;
        }
    } else if (what == "coup") {
        float chance = a.flt("chance", 0.3f);
        bool success = rng_.chance(chance);
        EntityId old = p->ruler;
        Character* oldc = ctx_.agents->get(old);
        Event e;
        e.type = EventType::Coup;
        e.severity = 5;
        e.actor = g->id;
        e.target = old;
        e.polity = p->id;
        e.causes[0] = cause;
        if (success) {
            e.text = strfmt("%s发动政变，推翻了%s", g->name.c_str(), oldc ? oldc->name.c_str() : "?");
            EventId ce = ctx_.chron->emit(std::move(e));
            ctx_.society->set_ruler(p->id, g->id, "coup", ce);
            g->girl->loyalty = 1.0f;
            g->girl->stance = "loyal";
            g->girl->role = "ruler";
            if (oldc && oldc->girl) {
                oldc->girl->role = "none";
                oldc->girl->loyalty = -0.8f;
                oldc->girl->grudge = g->id;
            }
            if (const Json* dj = drive_of(*ctx_.reg, g->girl->drive)) {
                p->policies.punishment = dj->flt("punish_bias", 0.3f);
                p->policies.distribution = dj->integer("valence", 1) < 0 ? 2 : 0;
            }
        } else {
            e.text = strfmt("%s的政变失败了", g->name.c_str());
            ctx_.chron->emit(std::move(e));
            g->girl->role = "none";
            g->girl->loyalty = -0.6f;
            for (Character* r : residents()) r->support_ref(g->id) = clampv(r->support_for(g->id) - 0.25f, -1.0f, 1.0f);
            g->remember(now_, MemoryKind::Punished, old, -0.4f, cause);
        }
    }
}

}  // namespace icarus
