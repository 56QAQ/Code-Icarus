// Agents: production planning. Each polity works out what it needs — materials short at
// construction sites, tools for its workers, arms for the soldiers it wants — and turns
// that into craft jobs, propagating needs through recipe inputs (a sword needs copper,
// copper needs ore and coal). Raw materials become gathering jobs: felling trees,
// quarrying stone, mining exposed ore, or cutting a ramp down to a buried iron vein.
#include <algorithm>
#include <cmath>
#include <map>

#include "icarus/agents/agents.h"
#include "icarus/sim/ecology.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

bool Agents::holds_water(const Vec3i& p) const {
    const MatId water = ctx_.reg->m().water;
    const World& w = *ctx_.world;
    if (vmat(w.peek(p + Vec3i{0, 1, 0})) == water) return true;
    for (int d = 0; d < 4; ++d)
        if (vmat(w.peek(p + kDir4H[d])) == water) return true;
    return false;
}

namespace {
constexpr int kGatherRadius = 90;

float output_rank(const Registry& reg, const Json& r) {
    float v = 0;
    for (const auto& [k, n] : r["outputs"].members()) {
        (void)n;
        ItemId it = reg.find_item(k);
        if (it != kNoItem) v = std::max({v, reg.item(it).power, reg.item(it).armor, reg.item(it).warmth});
    }
    return v;
}
}  // namespace

void Agents::production_jobs() {
    const Registry& reg = *ctx_.reg;
    JobBoard& jobs = *ctx_.jobs;
    Economy& econ = *ctx_.econ;
    const Json& recipes = reg.doc("recipes")["recipes"];
    auto add = [&](JobType t, u16 polity, const Vec3i& pos, float prio) -> Job& {
        Job j;
        j.type = t;
        j.polity = polity;
        j.pos = pos;
        j.priority = prio;
        j.created = now_;
        return *jobs.get(jobs.add(j));
    };

    for (const Polity& pc : ctx_.society->polities()) {
        if (!pc.alive) continue;
        const std::vector<StoreId> stores = ctx_.society->public_stores(pc.id);
        auto stock = [&](ItemId it) {
            i64 n = 0;
            for (StoreId sid : stores) n += econ.available(sid, it);
            return n;
        };
        int residents = 0, armed = 0, armoured = 0, archers = 0, unclothed = 0;
        for (auto& cp : chars_) {
            if (!cp || !cp->alive || cp->departed || cp->is_girl() || cp->polity != pc.id) continue;
            ++residents;
            if (cp->clothes == kNoItem) ++unclothed;
            if (cp->weapon != kNoItem) (reg.item(cp->weapon).range > 3.0f ? archers : armed)++;
            if (cp->armor != kNoItem) ++armoured;
        }
        // Known recipes; the best of each group gets the stock and military targets.
        std::vector<int> known;
        std::map<std::string, int> best_in_group;
        for (size_t i = 0; i < recipes.size(); ++i) {
            const Json& r = recipes[i];
            if (!r.str("tech").empty() && !pc.has_tech(r.str("tech"))) continue;
            known.push_back((int)i);
            const std::string g = r.str("group");
            if (g.empty()) continue;
            auto it = best_in_group.find(g);
            if (it == best_in_group.end() || output_rank(reg, r) > output_rank(reg, recipes[(size_t)it->second]))
                best_in_group[g] = (int)i;
        }
        // Direct wants.
        std::map<ItemId, i64> want;
        for (const Building& b : ctx_.buildings->all()) {
            if (!b.alive || b.complete || b.polity != pc.id) continue;
            for (auto& [it, n] : ctx_.buildings->remaining_cost(b)) want[it] += n;
        }
        for (auto& [g, ri] : best_in_group) {
            const Json& r = recipes[(size_t)ri];
            ItemId out = reg.find_item(r["outputs"].members().front().first);
            if (r.has("stock_per_resident")) want[out] += (i64)std::ceil(r.flt("stock_per_resident") * (float)residents);
            if (g == "clothes") want[out] += unclothed;  // everyone should have something to wear
            if (r.boolean("military") && pc.policies.army > 0) {
                i64 soldiers = pc.policies.army;
                if (g == "weapon") want[out] += std::max<i64>(0, soldiers * 7 / 10 - armed);
                else if (g == "ranged") want[out] += std::max<i64>(0, soldiers - soldiers * 7 / 10 - archers);
                else if (g == "armor") want[out] += std::max<i64>(0, soldiers - armoured);
            }
        }
        // Propagate through recipe inputs (a few levels) and schedule batches.
        std::map<int, int> batches;       // recipe -> batches wanted
        std::map<ItemId, i64> raw_short;  // raw materials short for all this
        std::map<ItemId, i64> planned;    // stock already promised to planned batches
        for (int level = 0; level < 3 && !want.empty(); ++level) {
            std::map<ItemId, i64> next;
            for (auto& [item, n] : want) {
                i64 have = stock(item) - planned[item];
                i64 short_by = n - std::max<i64>(0, have);
                planned[item] += std::min<i64>(n, std::max<i64>(0, have));
                if (short_by <= 0) continue;
                // Recipes making it: prefer one whose inputs are on hand.
                int pick = -1;
                for (int ri : known) {
                    const Json& r = recipes[(size_t)ri];
                    if (r["outputs"].members().front().first != reg.item(item).key) continue;
                    bool inputs_ok = true;
                    for (const auto& [k, v] : r["inputs"].members())
                        if (stock(reg.find_item(k)) < v.as_int()) inputs_ok = false;
                    if (pick < 0 || inputs_ok) pick = ri;
                    if (inputs_ok) break;
                }
                if (pick < 0) {
                    raw_short[item] += short_by;
                    continue;
                }
                const Json& r = recipes[(size_t)pick];
                int per = std::max(1, r["outputs"].members().front().second.as_int());
                int nb = (int)((short_by + per - 1) / per);
                batches[pick] += nb;
                for (const auto& [k, v] : r["inputs"].members()) next[reg.find_item(k)] += (i64)v.as_int() * nb;
            }
            want.swap(next);
        }
        // Materials building sites still wait for are not crafted into other things
        // (only into what the sites need, such as planks).
        std::map<ItemId, i64> site_hold;
        for (const Building& b : ctx_.buildings->all())
            if (b.alive && !b.complete && b.polity == pc.id)
                for (auto& [it, n] : ctx_.buildings->remaining_cost(b)) site_hold[it] += n;
        // Craft jobs: at a store holding the inputs, up to two open per recipe.
        for (auto& [ri, nb] : batches) {
            const Json& r = recipes[(size_t)ri];
            bool for_sites = false;
            for (const auto& [k, v] : r["outputs"].members())
                if (site_hold.count(reg.find_item(k))) for_sites = true;
            int open = 0;
            for (const Job& j : jobs.all())
                if (j.alive && j.type == JobType::Craft && j.polity == pc.id && j.plot == (u32)ri) ++open;
            for (StoreId sid : stores) {
                if (open >= 2) break;
                const Store* st = econ.store(sid);
                if (!st || st->kind != StoreKind::Stockpile) continue;
                int can = std::min(nb, r.integer("batch", 1));
                for (const auto& [k, v] : r["inputs"].members()) {
                    const ItemId in = reg.find_item(k);
                    i64 have = econ.available(sid, in);
                    if (!for_sites) {
                        auto held = site_hold.find(in);
                        if (held != site_hold.end()) have -= held->second;
                    }
                    can = std::min<i64>(can, std::max<i64>(0, have) / std::max(1, v.as_int()));
                }
                if (can <= 0) continue;
                Job& j = add(JobType::Craft, pc.id, st->pos, 1.15f);
                j.from = sid;
                j.plot = (u32)ri;
                j.count = can;
                ++open;
                nb -= can;
            }
        }

        // Gathering: every 2.4 game hours, look for trees, stone and ore to take (each
        // polity in its own slot of the job rounds, which come every 50 ticks, so the
        // surveys do not pile into one tick).
        if ((now_ / 50 + (Tick)pc.id * 5) % 12 != 0) continue;
        const Building* seat = ctx_.buildings->get(pc.seat);
        if (!seat) continue;
        World& w = *ctx_.world;
        const CoreMats& M = reg.m();
        auto open_of = [&](JobType t) {
            int n = 0;
            for (const Job& j : jobs.all())
                if (j.alive && j.type == t && j.polity == pc.id) ++n;
            return n;
        };
        const ItemId wood = reg.find_item("wood"), stone = reg.find_item("stone");
        // A band without stone tools keeps only a little wood by the fire and no stone.
        const bool toolmakers = pc.has_tech("stone_tools");
        const bool need_wood = stock(wood) < (toolmakers ? 40 : 12) + raw_short[wood];
        const bool need_stone = stock(stone) < (toolmakers ? 16 : 0) + raw_short[stone];
        std::vector<std::pair<MatId, i64>> ores;  // ore material -> units short
        for (auto& [it, n] : raw_short) {
            if (n <= 0 || it == wood || it == stone) continue;
            for (size_t m = 0; m < reg.mat_count(); ++m)
                if (reg.mat((MatId)m).drop_item_id == it && reg.mat((MatId)m).solid) ores.push_back({(MatId)m, n});
        }
        if (!need_wood && !need_stone && ores.empty()) continue;
        struct Spot {
            i64 d;
            Vec3i p;
            int kind;  // 0 tree, 1 stone, 2+ ore index
        };
        std::vector<Spot> spots;
        std::vector<std::pair<i64, Vec3i>> buried;  // for ore kinds with nothing exposed
        const Vec3i c0 = seat->entrance;
        // Only cliff faces can expose stone and ore: the cubes of a column between its
        // top and the lowest neighbouring top. Trees stand on the surface.
        for (int dz = -kGatherRadius; dz <= kGatherRadius; ++dz)
            for (int dx = -kGatherRadius; dx <= kGatherRadius; ++dx) {
                if (dx * dx + dz * dz > kGatherRadius * kGatherRadius) continue;
                // Near home every column; further out every other one (plenty to choose from).
                if (dx * dx + dz * dz > 32 * 32 && ((dx | dz) & 1)) continue;
                int x = c0.x + dx, z = c0.z + dz;
                ColumnInfo col = w.gen().column(x, z);
                if (!col.land) continue;
                i64 d = (i64)dx * dx + (i64)dz * dz;
                if (need_wood && d > 12 * 12) {
                    for (int y = col.top + 1; y <= col.top + 2; ++y) {
                        Vec3i p{x, y, z};
                        if (!reg.mat(vmat(w.peek(p))).trunk || reg.mat(vmat(w.peek(p + Vec3i{0, -1, 0}))).trunk) continue;
                        // A tree, not a wall: no building owns it, and leaves crown the trunk.
                        if (ctx_.buildings->at(p)) break;
                        bool crown = false;
                        for (int k = 1; k <= 13 && !crown; ++k) crown = reg.mat(vmat(w.peek(p + Vec3i{0, k, 0}))).foliage;
                        // Young trees grown back from saplings are felled last.
                        if (crown) spots.push_back({ctx_.ecology && ctx_.ecology->regrown_at(p) ? d + 90 * 90 : d, p, 0});
                        break;
                    }
                }
                if (!need_stone && ores.empty()) continue;
                int lowest = col.top;
                for (int k = 0; k < 4; ++k) {
                    ColumnInfo n = w.gen().column(x + kDir4H[k].x, z + kDir4H[k].z);
                    lowest = std::min(lowest, n.land ? (int)n.top : 0);
                }
                // Boulders lying on the ground come first: nothing to dig, no hole left.
                {
                    const Vec3i p{x, col.top + 1, z};
                    const MatId m = vmat(w.peek(p));
                    const Material& bm = reg.mat(m);
                    if (m != M.air && bm.solid && !bm.trunk && !ctx_.buildings->at(p)) {
                        for (size_t oi = 0; oi < ores.size(); ++oi)
                            if (m == ores[oi].first) spots.push_back({d / 2, p, 2 + (int)oi});
                        if (need_stone && bm.drop_item_id == stone) spots.push_back({d / 2, p, 1});
                    }
                }
                for (int y = col.top; y > lowest && y > col.top - 24 && y > 1; --y) {
                    Vec3i p{x, y, z};
                    MatId m = vmat(w.peek(p));
                    bool exposed = false;
                    for (int k = 0; k < 6 && !exposed; ++k) exposed = !reg.mat(vmat(w.peek(p + kDir6[k]))).solid;
                    if (!exposed) continue;
                    for (size_t oi = 0; oi < ores.size(); ++oi)
                        if (m == ores[oi].first) spots.push_back({d, p, 2 + (int)oi});
                    if (need_stone && m == M.stone && d > 20 * 20 && y < col.top - 1) spots.push_back({d, p, 1});
                }
            }
        // Buried ore: searched only when some ore is needed and none is exposed.
        bool any_exposed_ore = false;
        for (const Spot& sp : spots)
            if (sp.kind >= 2) any_exposed_ore = true;
        if (!ores.empty() && !any_exposed_ore)
            for (int dz = -60; dz <= 60; dz += 2)
                for (int dx = -60; dx <= 60; dx += 2) {
                    int x = c0.x + dx, z = c0.z + dz;
                    ColumnInfo col = w.gen().column(x, z);
                    if (!col.land) continue;
                    for (int y = col.top - 1; y >= col.top - 10 && y > 1; --y) {
                        MatId m = vmat(w.peek({x, y, z}));
                        for (auto& o : ores)
                            if (m == o.first)
                                buried.push_back({(i64)dx * dx + (i64)dz * dz + (i64)(col.top - y) * 200, Vec3i{x, y, z}});
                    }
                }
        // Only spots someone can stand next to, within the settlement's walkable region.
        u16 home_region = 0;
        if (auto it = region_map_.find(seat->entrance); it != region_map_.end()) home_region = it->second;
        auto workable = [&](const Vec3i& p) {
            if (!home_region) return true;  // no survey yet
            for (int dy = -3; dy <= 2; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto it = region_map_.find(p + Vec3i{dx, dy, dz});
                        if (it != region_map_.end() && !regions_apart(it->second, home_region)) return true;
                    }
            return false;
        };
        // (Nor a cube that holds water back: the bank of a river or lake, cut open, lets it
        // run away and leaves the fields beside it dry.)
        spots.erase(std::remove_if(spots.begin(), spots.end(),
                                   [&](const Spot& sp) { return !workable(sp.p) || (sp.kind >= 1 && holds_water(sp.p)); }),
                    spots.end());
        // Gathering jobs nobody took for half a day are probably out of reach: retire them.
        for (const Job& j : jobs.all())
            if (j.alive && j.polity == pc.id && (j.type == JobType::Mine || j.type == JobType::Chop) &&
                j.claimed_by == kNoEntity && now_ - j.created > kTicksPerDay / 2)
                jobs.cancel(j.id);
        std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) {
            return a.d != b.d ? a.d < b.d : a.p < b.p;
        });
        int chop = open_of(JobType::Chop), mine = open_of(JobType::Mine);
        int want_ore_jobs = 0;
        for (auto& [m, n] : ores) want_ore_jobs += (int)std::min<i64>(4, n);
        for (const Spot& sp : spots) {
            if (sp.kind == 0 && need_wood && chop < 3) {
                if (jobs.find(JobType::Chop, sp.p)) continue;
                add(JobType::Chop, pc.id, sp.p, 0.8f);
                ++chop;
            } else if (sp.kind >= 1 && mine < std::max(6, want_ore_jobs) && (sp.kind >= 2 || need_stone)) {
                if (jobs.find(JobType::Mine, sp.p)) continue;
                add(JobType::Mine, pc.id, sp.p, sp.kind >= 2 ? 1.0f : 0.7f);
                ++mine;
            }
        }
        // No stone face within reach: open a quarry pit away from the houses.
        int stone_spots = 0;
        for (const Spot& sp : spots)
            if (sp.kind == 1) ++stone_spots;
        if (need_stone && stone_spots < 2 && toolmakers) {
            bool running = false;
            for (const Project& pr : ctx_.society->projects())
                if (pr.alive && pr.status == 0 && pr.polity == pc.id && pr.kind == "dig" && pr.title == "开采石料")
                    running = true;
            for (int attempt = 0; attempt < 24 && !running; ++attempt) {
                float ang = (float)attempt * 2.39996f;
                int r = 26 + attempt;
                int x0 = c0.x + (int)std::lround(std::cos(ang) * (float)r), z0 = c0.z + (int)std::lround(std::sin(ang) * (float)r);
                ColumnInfo col = w.gen().column(x0, z0);
                if (!col.land) continue;
                // An open-cast cut, 8 long and 3 wide, stepping down one cube every two:
                // always walkable, nobody gets stuck at the bottom.
                bool ok = true;
                // (Well away from any water: a pit cut into a river's bank drains it.)
                for (int dz = -5; dz <= 7 && ok; ++dz)
                    for (int dx = -5; dx <= 12 && ok; ++dx) {
                        const ColumnInfo cc = w.gen().column(x0 + dx, z0 + dz);
                        if (cc.water_top >= 0) ok = false;
                        for (int y = col.top - 5; y <= col.top + 1 && ok; ++y)
                            if (vmat(w.peek({x0 + dx, y, z0 + dz})) == M.water) ok = false;
                    }
                for (int dz = -2; dz <= 4 && ok; ++dz)
                    for (int dx = -2; dx <= 9 && ok; ++dx) {
                        ColumnInfo cc = w.gen().column(x0 + dx, z0 + dz);
                        if (!cc.land || std::abs((int)cc.top - (int)col.top) > 1) ok = false;
                        for (int y = col.top - 1; y <= col.top + 3 && ok; ++y)
                            if (ctx_.buildings->at({x0 + dx, y, z0 + dz})) ok = false;
                        // Under open sky: a tree's crown over the cut would take the
                        // headroom off the steps and shut the pit.
                        for (int y = col.top + 2; y <= col.top + 6 && ok; ++y)
                            if (reg.mat(vmat(w.peek({x0 + dx, y, z0 + dz}))).solid) ok = false;
                    }
                if (!ok) continue;
                Json cubes = Json::array();
                for (int dx = 0; dx < 8; ++dx)
                    for (int dz = 0; dz < 3; ++dz)
                        for (int y = col.top + 1; y >= col.top - std::min(4, dx / 2); --y) {
                            Vec3i q{x0 + dx, y, z0 + dz};
                            if (!reg.mat(vmat(w.peek(q))).solid) continue;
                            Json v = Json::array();
                            v.push(q.x);
                            v.push(q.y);
                            v.push(q.z);
                            cubes.push(v);
                        }
                if (cubes.size() < 8 || !workable({x0 - 1, (int)col.top + 1, z0})) continue;
                Project pr;
                pr.polity = pc.id;
                pr.kind = "dig";
                pr.title = "开采石料";
                pr.target = {x0, (int)col.top, z0};
                pr.priority = 0.8f;
                pr.params = Json::object();
                pr.params.set("cubes", cubes);
                ctx_.society->add_project(pr);
                running = true;
            }
        }
        // Buried ore only: cut a ramp down to the nearest vein (one dig project at a time).
        bool exposed_ore = false;
        for (const Spot& sp : spots)
            if (sp.kind >= 2) exposed_ore = true;
        if (!ores.empty() && !exposed_ore && !buried.empty()) {
            bool running = false;
            for (const Project& pr : ctx_.society->projects())
                if (pr.alive && pr.status == 0 && pr.polity == pc.id && pr.kind == "dig" && pr.title == "开凿矿坑")
                    running = true;
            if (!running) {
                std::sort(buried.begin(), buried.end());
                const Vec3i ore = buried.front().second;
                const Vec3i dirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
                Json best;
                size_t best_n = 0;
                for (const Vec3i& dir : dirs) {
                    Json cubes = Json::array();
                    size_t n = 0;
                    bool ok = true;
                    auto push = [&](const Vec3i& q) {
                        const Material& m = reg.mat(vmat(w.peek(q)));
                        if (!m.solid) return;
                        if (!m.diggable) ok = false;
                        Json v = Json::array();
                        v.push(q.x);
                        v.push(q.y);
                        v.push(q.z);
                        cubes.push(v);
                        ++n;
                    };
                    push(ore);
                    for (int k = 1; k <= 16 && ok; ++k) {
                        Vec3i s{ore.x + dir.x * k, ore.y + k - 1, ore.z + dir.z * k};
                        push(s);
                        push(s + Vec3i{0, 1, 0});
                        push(s + Vec3i{0, 2, 0});
                        if (w.gen().column(s.x, s.z).top < s.y) break;  // reached the open surface
                    }
                    if (ok && (best_n == 0 || n < best_n)) {
                        best_n = n;
                        best = cubes;
                    }
                }
                if (best_n > 0) {
                    Project pr;
                    pr.polity = pc.id;
                    pr.kind = "dig";
                    pr.title = "开凿矿坑";
                    pr.target = ore;
                    pr.priority = 0.9f;
                    pr.params = Json::object();
                    pr.params.set("cubes", best);
                    ctx_.society->add_project(pr);
                }
            }
        }
    }
}

}  // namespace icarus
