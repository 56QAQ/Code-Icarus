// The steward's plan (内政规划): see plan.h. Every two hours each polity takes stock,
// looks ahead and decides how to share out its hands, whether families can afford more
// children, and what to recommend to its ruler. Everything here is estimated from the
// rules and from what has actually happened lately (the food ledger), never from a
// particular map: the same reasoning serves a band in the wild and a kingdom by a river.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/agents/jobs.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/fauna/fauna.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/ecology.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

const char* food_source_zh(int s) {
    switch (s) {
        case kSrcFarm: return "田地";
        case kSrcForage: return "采集";
        case kSrcHunt: return "狩猎";
        case kSrcFish: return "捕鱼";
        default: return "?";
    }
}

const char* trade_key(int t) {
    switch (t) {
        case kTradeFood: return "food";
        case kTradeBuild: return "build";
        default: return "gather";
    }
}

const char* trade_zh(int t) {
    switch (t) {
        case kTradeFood: return "觅食务农";
        case kTradeBuild: return "建造";
        default: return "伐木采石";
    }
}

namespace {
// Days a gathered plant takes to bear again (see Ecology::daily).
constexpr float kFruitRegrowDays = 3.0f, kPlantRegrowDays = 4.0f;
// How far people go for food and game from their homes.
constexpr int kForageReach = 64, kHuntReach = 100, kFishReach = 90;

float sustainable_rate(const SpeciesDef& s) {
    // Young per head per day (half the herd bears a litter every breed_days).
    const float litter = 0.5f * (float)(s.litter_min + s.litter_max);
    return 0.5f * litter / std::max(1.0f, s.breed_days);
}
}  // namespace

void Society::draw_plan(Polity& p) {
    PolityPlan& P = p.plan;
    const Registry& reg = *ctx_.reg;
    const Agents& agents = *ctx_.agents;
    const float per_person = agents.tune.food_per_day;
    const Tick now = ctx_.now;
    const bool band = foraging_band(p);

    // ---------------------------------------------------------------- people
    int people = 0, workers = 0, kids = 0, elders = 0;
    int staff[kTrades] = {0};
    // Looking a generation ahead: the grown folk who will be old by the time a child born
    // today can work (they will need replacing), and the couples who could have children.
    const Json& life = reg.doc("life");
    const float soon_old = life.flt("elder_age", 55.0f) - life.flt("adult_age", 14.0f);
    int aging = 0, couples = 0;
    for (const auto& cp : agents.all()) {
        if (!cp || !cp->alive || cp->departed || cp->polity != p.id) continue;
        const Character& c = *cp;
        ++people;
        if (c.is_girl()) continue;
        if (agents.is_child(c)) {
            ++kids;
            continue;
        }
        if (agents.is_elder(c) || agents.age_years(c) >= soon_old) ++aging;
        if (!agents.is_elder(c) && c.partner != kNoEntity && c.id < c.partner)
            if (const Character* o = agents.get(c.partner); o && o->alive && !o->departed && o->polity == c.polity && !o->is_girl())
                ++couples;
        if (agents.is_elder(c)) ++elders;
        if (c.drafted || !c.body.can_hold() || c.occupation == "research") continue;
        ++workers;
        for (int t = 0; t < kTrades; ++t)
            if (c.occupation == trade_key(t)) staff[t]++;
    }
    P.people = people;
    P.workers = workers;
    P.children = kids;
    P.elders = elders;
    for (int t = 0; t < kTrades; ++t) P.staff[t] = staff[t];
    P.need = (float)people * per_person;

    // ---------------------------------------------------------------- stores
    P.stock = public_food(p.id);
    {
        const ItemId grain = reg.find_item("grain");
        i64 g = 0;
        for (StoreId sid : public_stores(p.id))
            if (const Store* s = ctx_.econ->store(sid)) g += s->count(grain);
        P.seed = (float)g;
    }

    // ---------------------------------------------------------------- the food ledger
    // What has actually come in over the last three days, source by source.
    const std::array<double, 4> tot = ctx_.econ->food_in(p.id);
    if (P.first == 0) P.first = now;
    {
        PolityPlan::Look look;
        look.at = now;
        for (int s = 0; s < kFoodSources; ++s) look.food[s] = tot[(size_t)s];
        look.hands = (float)staff[kTradeFood];
        P.looks.push_back(look);
        while (P.looks.size() > 2 && now - P.looks[1].at >= 3 * kTicksPerDay) P.looks.erase(P.looks.begin());
    }
    const PolityPlan::Look& oldest = P.looks.front();
    const float span = (float)(now - oldest.at) / (float)kTicksPerDay;
    P.hands = 0;
    for (const PolityPlan::Look& l : P.looks) P.hands += l.hands;
    P.hands /= (float)P.looks.size();
    P.income = 0;
    for (int s = 0; s < kFoodSources; ++s) {
        P.income_by[s] = span > 0.05f ? (float)(tot[(size_t)s] - oldest.food[s]) / span : 0.0f;
        P.income += P.income_by[s];
    }

    // ---------------------------------------------------------------- what the land gives
    // For good, at full effort: fields as they grow, wild plants as they bear again, game
    // and fish as fast as they breed.
    std::vector<Vec3i> homes;
    if (const Building* seat = ctx_.buildings->get(p.seat)) homes.push_back(seat->entrance);
    for (const Vec3i& o : p.outposts) homes.push_back(o);
    for (float& v : P.potential_by) v = 0;
    float hunt_could = 0, fish_could = 0, boat_could = 0;
    const FarmStats fs = ctx_.farming->stats_polity(p.id);
    P.plots = fs.plots;
    P.irrigated = fs.irrigated;
    const ItemId grain = reg.find_item("grain");
    const float grain_n = grain != kNoItem ? reg.item(grain).nutrition : 0.3f;
    const float per_plot = (float)std::max(0, ctx_.farming->yield - 1) * grain_n / std::max(0.5f, ctx_.farming->growth_days);
    P.potential_by[kSrcFarm] = per_plot * ((float)fs.irrigated + ctx_.farming->dry_factor * (float)(fs.plots - fs.irrigated));
    if (!homes.empty()) {
        // Wild plants within a day's gathering (every other column, counted four times).
        const World& w = *ctx_.world;
        float wild = 0;
        const int R = kForageReach;
        for (size_t h = 0; h < homes.size(); ++h) {
            const Vec3i& c = homes[h];
            for (int dz = -R; dz <= R; dz += 2)
                for (int dx = -R; dx <= R; dx += 2) {
                    if (dx * dx + dz * dz > R * R) continue;
                    const int x = c.x + dx, z = c.z + dz;
                    bool dup = false;  // a column shared with another home counts once
                    for (size_t o = 0; o < h && !dup; ++o)
                        if ((homes[o].x - x) * (homes[o].x - x) + (homes[o].z - z) * (homes[o].z - z) <= R * R) dup = true;
                    if (dup) continue;
                    const ColumnInfo col = w.gen().column(x, z);
                    if (!col.land || col.water_top >= 0) continue;
                    for (int y = col.top + 1; y <= col.top + 3; ++y) {
                        const Material& m = reg.mat(vmat(w.peek({x, y, z})));
                        if (m.forage_item == kNoItem) continue;
                        const float n = reg.item(m.forage_item).nutrition;
                        if (n <= 0.0f) continue;
                        wild += (float)std::max(1, m.forage_count) * n / (m.foliage ? kFruitRegrowDays : kPlantRegrowDays);
                    }
                    // Fruit in the crowns above.
                    for (int y = col.top + 4; y <= col.top + 9; ++y) {
                        const Material& m = reg.mat(vmat(w.peek({x, y, z})));
                        if (!m.foliage || m.forage_item == kNoItem) continue;
                        wild += 0.5f * (float)std::max(1, m.forage_count) * reg.item(m.forage_item).nutrition / kFruitRegrowDays;
                    }
                }
        }
        // Plants picked lately bear again in a few days.
        float regrowing = 0;
        if (ctx_.ecology)
            for (const Vec3i& h : homes) regrowing += ctx_.ecology->regrowing_food(h, R);
        P.potential_by[kSrcForage] = wild * 4.0f + regrowing;
        if (ctx_.fauna) {
            // (Game and fish are reckoned whether or not the people know how to take them:
            // what they could give tells the steward what is worth learning.)
            const Fauna& fauna = *ctx_.fauna;
            const bool boats = p.has_tech("boats");
            auto near_home = [&](const Vec3i& q, int r) {
                for (const Vec3i& h : homes)
                    if (h.dist2(q) <= (i64)r * r) return true;
                return false;
            };
            for (const Animal& a : fauna.all()) {
                const SpeciesDef& s = fauna.spec(a.species);
                if (!a.alive || s.aquatic || s.temper == Temper::Predator || !near_home(a.foot, kHuntReach)) continue;
                float meat = 0;
                for (const auto& [it, n] : s.yield) meat += (float)n * reg.item(it).nutrition;
                hunt_could += meat * sustainable_rate(s) * 0.5f;
            }
            for (const FishGround& g : fauna.grounds()) {
                if (!near_home(g.at, g.offshore ? kFishReach * 2 : kFishReach)) continue;
                const SpeciesDef& s = fauna.spec(g.species);
                float fish = 0;
                for (const auto& [it, n] : s.yield) fish += (float)n * reg.item(it).nutrition;
                // The most a school gives for good: a quarter of what it carries, at its breeding rate.
                (g.offshore && !boats ? boat_could : fish_could) += fish * (float)g.cap * sustainable_rate(s) * 0.5f;
            }
            if (p.has_tech("hunting") || p.has_tech("hunting_weapons")) P.potential_by[kSrcHunt] = hunt_could;
            if (p.has_tech("fishing")) P.potential_by[kSrcFish] = fish_could;
        }
    }
    P.potential = 0;
    for (float v : P.potential_by) P.potential += v;
    P.capacity = P.potential / std::max(0.1f, per_person);

    // ---------------------------------------------------------------- targets and outlook
    int storehouses = 0, study = 0;
    int beds = 0, sites = 0, housing_sites = 0;
    float materials_short = 0;
    int cubes_left = 0;
    for (const Building& b : ctx_.buildings->all()) {
        if (!b.alive || b.polity != p.id) continue;
        const BuildingDef* d = ctx_.buildings->def(b.def);
        if (b.complete) {
            beds += b.beds;
            if (d && d->storage > 0 && !d->seat) ++storehouses;
            if (d && d->scholars > 0) ++study;
            continue;
        }
        ++sites;
        if (d && d->beds > 0) ++housing_sites;
        cubes_left += std::max(0, b.solid_total - b.solid_intact);
        for (const auto& [it, n] : ctx_.buildings->remaining_cost(b)) {
            i64 have = 0;
            for (StoreId sid : public_stores(p.id))
                if (const Store* s = ctx_.econ->store(sid)) have += s->count(it);
            if (const Store* s = ctx_.econ->store(b.site)) have += s->count(it);
            materials_short += (float)std::max<i64>(0, n - have);
        }
    }
    (void)study;
    // Digging ordered by the ruler (a sealed spring, a channel) is building work too; while
    // the people are short of water it comes before everything but food.
    int digs = 0;
    for (const Job& j : ctx_.jobs->all())
        if (j.alive && j.polity == p.id && j.type == JobType::Dig) ++digs;
    bool thirsty = false;
    for (const Crisis& c : p.crises)
        if (c.active && c.kind == CrisisKind::Water) thirsty = true;
    if (digs > 0) {
        ++sites;
        cubes_left += digs * (thirsty ? 120 : 30);
    }
    P.beds = beds;
    P.sites = sites;
    // Days of food worth keeping: a band's berries spoil within days; a village keeps
    // more, and more still once it has a storehouse.
    P.target_days = band ? 2.5f : (storehouses > 0 ? 6.0f : 4.0f);
    P.balance = P.income - P.need;
    P.days_left = P.balance < -0.01f ? P.stock / -P.balance : 99.0f;
    // Fields enough to feed everyone, with what the wild gives for good counted at half.
    const float wild_for_good = 0.5f * (P.potential_by[kSrcForage] + P.potential_by[kSrcHunt] + P.potential_by[kSrcFish]);
    P.plots_needed = p.has_tech("farming") && per_plot > 0.0f
                         ? (int)std::ceil(std::max(0.0f, P.need * 1.15f - wild_for_good) / per_plot)
                         : 0;
    const int couples_growing = std::max(0, (workers - kids) / 4);
    P.beds_needed = people + std::min(4, couples_growing);

    // ---------------------------------------------------------------- sharing out the hands
    // Food: enough hands to feed everyone and bring the stores to their target within
    // five days (or let an overflowing store run down), judged by what a food worker has
    // actually brought in lately (at first, by what one usually does).
    const float stock_days = P.need > 0.0f ? P.stock / P.need : 0.0f;
    const float refill = (P.target_days * P.need - P.stock) / 5.0f;
    const float want_rate = std::max(0.3f * P.need, P.need + refill);
    const float prior = band ? 2.0f : 2.5f;
    // (Trust the ledger more as it fills: fully after a day and a half.)
    const float warm = clampv((float)(now - P.first) / (1.5f * (float)kTicksPerDay), 0.0f, 1.0f);
    const float seen_prod = P.income / std::max(1.0f, P.hands);
    const float prod = std::max(0.6f, warm * seen_prod + (1.0f - warm) * prior);
    float food_hands = want_rate / prod;
    // Fields need tending anyway (unless the stores already hold plenty).
    if (!band && stock_days < 2.0f * P.target_days) food_hands = std::max(food_hands, (float)fs.plots / 10.0f);
    const float W = (float)std::max(1, workers);
    // With a day or two in store, building and gathering keep a third of the hands at
    // least; only a larder about to run dry takes (nearly) everyone.
    // The stores tell whether that reckoning is right (it cannot see everyone who picks a
    // berry on the way): below the target the share leans up a little each time the plan
    // is drawn, above it back down.
    const float err = clampv((P.target_days - stock_days) / std::max(1.0f, P.target_days), -1.0f, 1.0f);
    // (Only once the ledger is trusted: the first days' shortfall is the start, not a lesson.)
    P.food_lean = clampv(P.food_lean + 0.03f * err * warm, -0.2f, 0.35f);
    const float food = clampv(food_hands / W + P.food_lean, 0.15f,
                              stock_days >= 1.5f ? 0.7f : (stock_days >= 0.7f ? 0.8f : 0.9f));
    // Building: what is under way (and a little for repairs); gathering: the materials
    // the sites still lack, plus a standing supply.
    float build = sites ? clampv(0.08f + 0.05f * (float)sites + (float)cubes_left / (W * 60.0f), 0.1f, 0.5f) : 0.06f;
    float gather = clampv(0.08f + materials_short / (W * 25.0f), 0.08f, 0.45f);
    const float rest = 1.0f - food;
    const float bg = build + gather;
    P.want[kTradeFood] = food;
    P.want[kTradeBuild] = rest * build / bg;
    P.want[kTradeGather] = rest * gather / bg;

    // ---------------------------------------------------------------- children
    // Families have children when the people can feed and house more mouths for good,
    // not merely because everyone ate today.
    const float food_room = P.capacity / std::max(1.0f, (float)people) - 1.0f;
    float room = food_room;
    std::string why;
    if (!band) {
        const float house_room = (float)(beds + 3 * housing_sites) / std::max(1.0f, (float)people) - 1.0f;
        room = std::min(food_room, house_room + 0.15f);
        why = house_room + 0.15f < food_room ? "住房" : "粮食";
    } else {
        why = "野外的食物";
    }
    const bool stores_ok = P.stock >= 0.5f * P.target_days * P.need || (band && P.balance > 0.0f);
    float birth = stores_ok ? clampv(0.25f + 2.0f * room, 0.05f, 1.5f) : 0.05f;
    // A generation from now the old will be gone: children enough to take their place are
    // no extra mouths for good (the children already growing up count towards them). Only
    // a people eating into its last stores puts even those off.
    const int replace = std::max(0, aging - kids);
    const bool famine = P.balance < -0.15f * P.need && P.days_left < 3.0f;
    float renew = 0.0f;
    if (replace > 0 && couples > 0 && !famine)
        renew = clampv(0.35f + 1.1f * (float)replace / (float)couples, 0.35f, 1.5f) * (stores_ok ? 1.0f : 0.6f);
    const bool renewing = renew > birth;
    if (renewing) birth = renew;
    if (P.balance < -0.15f * P.need && !renewing) birth = std::min(birth, 0.1f);
    if ((float)kids > 0.6f * W) birth *= 0.5f;
    P.birth = birth;
    P.renewing = renewing;
    P.birth_why = renewing ? strfmt("%d 个大人将在一代之内老去，需要孩子接替", aging)
                  : !stores_ok ? "存粮不足，暂不生育"
                  : room < 0.0f ? strfmt("%s只够养活现有的人，节制生育", why.c_str())
                  : room > 0.4f ? strfmt("%s还能养活更多的人，鼓励生育", why.c_str())
                                : strfmt("%s尚有余裕，量力生育", why.c_str());

    // ---------------------------------------------------------------- advice to the ruler
    P.advice.clear();
    auto advise = [&](const std::string& key, float urgency, const std::string& text) {
        if (urgency <= 0.05f) return;
        for (Advice& a : P.advice)
            if (a.key == key) {
                if (urgency > a.urgency) a = {key, urgency, text};
                return;
            }
        P.advice.push_back({key, clampv(urgency, 0.0f, 1.0f), text});
    };
    const bool farming = p.has_tech("farming");
    if (P.balance < 0.0f && P.days_left < 4.0f) {
        const float u = 0.5f + 0.45f * (1.0f - P.days_left / 4.0f);
        advise("prioritize_food", u, strfmt("照现在的收成，存粮 %.1f 天后吃完，要多派人手去弄粮食", P.days_left));
        if (P.potential_by[kSrcForage] > 0.2f * P.need) advise("forage", u * 0.9f, "附近还有可采的野果，组织采集见效最快");
        if (P.days_left < 1.2f) advise("ration", 0.6f, "存粮将尽，先实行配给撑过去");
    }
    if (!farming && tech_available(p, "farming")) {
        const float u = P.capacity < 1.3f * (float)people ? 0.85f : 0.45f;
        advise("research_farming", u,
               strfmt("周边野外可持续养活约 %.0f 人（现有 %d 人），要长远发展，得学会种地", P.capacity, people));
    }
    if (farming && fs.plots == 0)
        advise("found_farm", 0.9f,
               P.seed >= 6.0f ? strfmt("已经会种地却还没有一块田（谷种 %.0f 份）", P.seed)
                              : std::string("已经会种地，却还没有谷种：派人去长野麦的地方采集，攒够了就开第一片田"));
    else if (farming && fs.plots < P.plots_needed) {
        const float u = clampv((float)(P.plots_needed - fs.plots) / (float)std::max(1, P.plots_needed) * 1.3f, 0.25f, 0.9f);
        // (Once the fields have filled the ground their water reaches, the next ones go by
        // other water.)
        bool room = false;
        for (const Farm& f : ctx_.farming->all())
            if (f.alive && f.polity == p.id && !ctx_.farming->expansion(f.id, 1).empty()) {
                room = true;
                break;
            }
        if (room)
            advise("expand_farms", u, strfmt("要养活 %d 人约需 %d 块能灌溉的田，现在只有 %d 块", people, P.plots_needed, fs.irrigated));
        else
            advise("found_farm", u,
                   strfmt("要养活 %d 人约需 %d 块田，现有 %d 块已占满水边能浇到的地，得去别处水边另开新田", people, P.plots_needed, fs.plots));
    }
    if (farming && fs.plots >= 8 && (float)fs.irrigated < 0.6f * (float)fs.plots) {
        if (tech_available(p, "irrigation")) advise("research_irrigation", 0.55f, "许多田地浇不上水，水利能引远处的水");
        advise("found_farm", 0.5f, "在水边另开能灌溉的新田");
    }
    if (!band && beds < P.beds_needed && housing_sites == 0)
        advise("build_housing", clampv((float)(P.beds_needed - beds) / std::max(1.0f, (float)people) * 2.5f, 0.2f, 0.85f),
               strfmt("床位 %d 个，要安顿 %d 人", beds, P.beds_needed));
    if (!band && storehouses == 0 && P.stock > 0.5f * P.need)
        advise("build_storehouse", 0.5f, "粮食堆在露天容易腐坏，需要仓库");
    // What the wild could give that the people cannot yet take.
    if (P.capacity < 1.3f * (float)people) {
        if (!p.has_tech("hunting") && tech_available(p, "hunting") && hunt_could > 0.1f * P.need)
            advise("research_hunting", 0.6f, strfmt("周边的猎物每天可提供约 %.1f 份食物，学会狩猎就能多养活人", hunt_could));
        if (!p.has_tech("fishing") && tech_available(p, "fishing") && fish_could > 0.1f * P.need)
            advise("research_fishing", 0.6f, strfmt("附近水里的鱼每天可提供约 %.1f 份食物，学会捕鱼就能多养活人", fish_could));
    }
    if (p.has_tech("fishing") && !p.has_tech("boats") && tech_available(p, "boats") && boat_could > 0.25f * P.need)
        advise("research_boats", P.capacity < 1.3f * (float)people ? 0.6f : 0.35f,
               strfmt("岸上够不着的外海渔场每天可提供约 %.1f 份食物，造了木船就能去捕", boat_could));
    if (farming && P.capacity < 0.9f * (float)people && fs.plots >= P.plots_needed * 3 / 4)
        advise("found_outpost", 0.45f,
               strfmt("这里的土地只够养活约 %.0f 人，现有 %d 人，分出一部分人去开拓新的村落", P.capacity, people));
    // War and peace as the steward reckons them: a war is paid for in people, the very
    // thing the plan runs short of while the land at home could still feed more. (A
    // people that cannot feed itself for good and is running out may see plunder as the
    // lesser evil: then the steward keeps quiet.)
    {
        const float room = P.capacity / std::max(1.0f, (float)people) - 1.0f;
        const bool desperate = room < -0.1f && P.days_left < 3.0f && P.balance < 0.0f;
        if (p.wars.empty()) {
            float u = 0.0f;
            std::string why;
            if (people < 14 && !desperate) {
                u = 0.55f;
                why = strfmt("我们只有 %d 人，经不起战争的折损", people);
            }
            if (room > 0.1f && !desperate) {
                const float v = 0.35f + std::min(0.3f, 0.3f * room);
                if (v > u) {
                    u = v;
                    why = strfmt("周边的土地还能多养活约 %.0f 人，与其打仗折损人手，不如埋头发展", P.capacity - (float)people);
                }
            }
            // Just out of a war: everyone is tired of fighting.
            if (p.weary > 0.25f && !desperate) {
                const float v = std::min(0.8f, 0.3f + 0.4f * p.weary);
                if (v > u) {
                    u = v;
                    why = "上一场仗才打完，人人厌战，正该休养生息";
                }
            }
            if (u > 0.0f) advise("avoid_war", u, why);
            // Stores well past their target while a neighbour has run out: a gift feeds them
            // and takes away their reason to come and take it.
            if (P.need > 0.0f && P.stock > 1.5f * P.target_days * P.need && P.balance >= 0.0f)
                for (const Polity& o : polities_)
                    if (o.alive && o.id != p.id && !p.war_with(o.id) && o.stats.food_days < 1.0f && o.stats.population > 0) {
                        advise("send_aid", 0.45f, strfmt("「%s」已经断粮，我们的存粮有余，送去一些既救人，也免得他们来抢", o.name.c_str()));
                        break;
                    }
        } else {
            int losses = 0;
            float days = 0.0f;
            for (const War& w : p.wars) {
                losses += w.losses;
                days = std::max(days, (float)(now - w.since) / (float)kTicksPerDay);
            }
            const float u = clampv(0.25f + 0.12f * (float)losses + 0.05f * days + (P.days_left < 2.0f ? 0.2f : 0.0f) -
                                       (desperate ? 0.25f : 0.0f),
                                   0.1f, 0.9f);
            const std::string why = strfmt("战事已持续 %.0f 天，折损 %d 人，这些人手本该用来养家和营建", days, losses);
            advise("accept_peace", std::min(1.0f, u + 0.15f), why);
            advise("offer_peace", u, why);
            if (p.op.active && p.op.lost > 0) advise("withdraw", 0.8f * u, why);
            if (!p.op.active) advise("hold", 0.5f * u, why);
        }
    }
    // Studies past the wild era need scholars at a study: build one first (and before
    // that, learn to write).
    {
        float scholarly = 0;
        std::string what;
        for (const Advice& a : P.advice)
            if (a.key.rfind("research_", 0) == 0 && needs_scholars(a.key.substr(9)) && a.urgency > scholarly) {
                scholarly = a.urgency;
                if (const Json* t = tech(a.key.substr(9))) what = t->str("name");
            }
        if (scholarly > 0.0f && scholar_seats(p.id) == 0) {
            bool building = false;
            for (const Building& b : ctx_.buildings->all())
                if (b.alive && !b.complete && b.polity == p.id)
                    if (const BuildingDef* d = ctx_.buildings->def(b.def); d && d->scholars > 0) building = true;
            if (!p.has_tech("writing") && tech_available(p, "writing"))
                advise("research_writing", scholarly, strfmt("要研究「%s」得先有文字和书写室", what.c_str()));
            else if (p.has_tech("writing") && !building)
                advise("build_study", scholarly, strfmt("要研究「%s」，得先建书写室让学者钻研", what.c_str()));
        }
    }
    std::stable_sort(P.advice.begin(), P.advice.end(), [](const Advice& a, const Advice& b) { return a.urgency > b.urgency; });

    // ---------------------------------------------------------------- summary
    P.summary = strfmt("每天需粮 %.1f，近来收入 %.1f（%s）；可持续约养活 %.0f 人；存粮 %.1f 天（目标 %.0f 天）；"
                       "分工：粮食 %.0f%%、建造 %.0f%%、采集 %.0f%%",
                       P.need, P.income, P.balance >= 0.0f ? "有余" : "不足", P.capacity,
                       P.need > 0.0f ? P.stock / P.need : 0.0f, P.target_days, 100.0f * P.want[kTradeFood],
                       100.0f * P.want[kTradeBuild], 100.0f * P.want[kTradeGather]);
    P.at = now;
    ctx_.agents->assign_trades(p.id, P);
}

CivIndex Society::civ_index(const Polity& p) const {
    CivIndex ci;
    const PolityStats& s = p.stats;
    const PolityPlan& P = p.plan;
    const float people = (float)std::max(0, s.population - P.children) + 0.5f * (float)P.children;
    ci.people = people;
    ci.fed = clampv(s.food_access, 0.0f, 1.0f);
    ci.secure = P.need > 0.0f ? clampv(P.stock / (P.need * std::max(1.0f, P.target_days)), 0.0f, 1.0f) : 0.0f;
    ci.housed = s.population > 0 ? clampv((float)P.beds / (float)s.population, 0.0f, 1.0f) : 0.0f;
    for (const Building& b : ctx_.buildings->all()) {
        if (!b.alive || !b.complete || b.polity != p.id) continue;
        ci.built += 1.0f + 0.02f * (float)b.solid_total;
    }
    for (const std::string& t : p.techs)
        if (const Json* tj = tech(t)) ci.known += 1.0f + (float)tj->integer("era", 0);
    ci.content = clampv(0.5f * s.mood + 0.5f * s.stability, 0.0f, 1.0f);
    // People living well count most (a people that shrinks or goes hungry cannot make up
    // for it with what it has built and learnt, which only adds a little over time).
    ci.total = people * (0.35f + 0.35f * ci.fed + 0.15f * ci.secure + 0.15f * ci.housed) * (0.6f + 0.4f * ci.content) +
               kCivBuiltWeight * ci.built + kCivKnownWeight * ci.known;
    return ci;
}

}  // namespace icarus
