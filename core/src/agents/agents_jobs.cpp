// Agents: job generation. Work appears where the world needs it: fields to till, sow and
// harvest; loose goods to store; construction sites to supply and build; kitchens to run.
#include <algorithm>
#include <cmath>
#include <map>

#include "icarus/agents/agents.h"
#include "icarus/fauna/fauna.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"

namespace icarus {

bool Agents::food_plant_at(int x, int z, Vec3i& out) {
    World& w = *ctx_.world;
    const Registry& reg = *ctx_.reg;
    const ColumnInfo col = w.gen().column(x, z);
    if (!col.land) return false;
    // The ground may have been dug or built on since generation: look a little around.
    for (int y = col.top + 1; y <= col.top + 4; ++y) {
        const Vec3i p{x, y, z};
        const Material& m = reg.mat(vmat(w.peek(p)));  // looking only: no cells woken
        if (m.forage_item == kNoItem || reg.item(m.forage_item).nutrition <= 0.0f) continue;
        // Fruit hangs in the crown: pickable only from the ground below.
        if (m.foliage && y > col.top + 3) continue;
        out = p;
        return true;
    }
    return false;
}

bool Agents::wild_food_near(const Vec3i& from, int radius, Vec3i& out) {
    for (int r = 1; r <= radius; ++r) {
        // Close by every column; further out every other one (the eye skips a little).
        const int stride = r <= 16 ? 1 : 2;
        for (int dz = -r; dz <= r; dz += stride)
            for (int dx = -r; dx <= r; dx += (std::abs(dz) == r ? stride : 2 * r)) {
                Vec3i p;
                if (!food_plant_at(from.x + dx, from.z + dz, p)) continue;
                if (std::abs(p.y - from.y) > 12) continue;
                bool taken = false;
                for (const Job& j : ctx_.jobs->all())
                    if (j.alive && j.type == JobType::Forage && j.pos == p && j.claimed_by != kNoEntity) taken = true;
                if (taken) continue;
                out = p;
                return true;
            }
    }
    return false;
}

void Agents::generate_jobs() {
    const Registry& reg = *ctx_.reg;
    JobBoard& jobs = *ctx_.jobs;
    Economy& econ = *ctx_.econ;
    const ItemId grain = reg.find_item("grain");
    auto add = [&](JobType t, u16 polity, const Vec3i& pos, float prio) -> Job& {
        Job j;
        j.type = t;
        j.polity = polity;
        j.pos = pos;
        j.priority = prio;
        j.created = now_;
        u32 id = jobs.add(j);
        return *jobs.get(id);
    };

    // Existing jobs by (type,pos) for de-duplication.
    std::map<std::pair<int, Vec3i>, u32> existing;
    std::map<std::pair<u32, ItemId>, i32> in_transit;  // (site store, item) -> units being hauled
    std::map<u32, int> build_jobs;                     // building -> open build jobs
    std::map<std::pair<u32, ItemId>, int> pile_jobs;
    // Units already promised from a store by haul jobs nobody has taken yet (taken ones
    // hold a reservation, which `available` already accounts for).
    std::map<std::pair<StoreId, ItemId>, i32> promised;
    for (const Job& j : jobs.all()) {
        if (!j.alive) continue;
        existing[{(int)j.type, j.pos}] = j.id;
        if (j.type == JobType::HaulToSite) {
            in_transit[{j.to, j.item}] += j.count;
            if (j.claimed_by == kNoEntity) promised[{j.from, j.item}] += j.count;
        }
        if (j.type == JobType::Build) build_jobs[j.building]++;
        if (j.type == JobType::HaulPile) pile_jobs[{j.from, j.item}]++;
    }
    auto has = [&](JobType t, const Vec3i& p) { return existing.count({(int)t, p}) != 0; };

    // Fields. Work on plots whose ground is gone is withdrawn (nobody can stand there).
    for (const Job& j : jobs.all()) {
        if (!j.alive || !j.farm || j.claimed_by != kNoEntity) continue;
        Farm* f = ctx_.farming->get(j.farm);
        if (!f || j.plot >= f->plots.size() || ctx_.farming->state(f->plots[j.plot]) == PlotState::Lost)
            jobs.complete(j.id);
    }
    for (auto& farm : ctx_.farming->all()) {
        if (!farm.alive) continue;
        Farm* f = ctx_.farming->get(farm.id);
        bool seeds = ctx_.society->polity(f->polity) && [&]() {
            for (StoreId sid : ctx_.society->public_stores(f->polity))
                if (econ.available(sid, grain) > 0) return true;
            return false;
        }();
        for (size_t i = 0; i < f->plots.size(); ++i) {
            Plot& p = f->plots[i];
            Vec3i crop = p.ground + Vec3i{0, 1, 0};
            PlotState st = ctx_.farming->state(p);
            JobType t = JobType::None;
            Vec3i at = crop;
            if (st == PlotState::NeedsTill) {
                const Material& g = ctx_.world->material(p.ground);
                if (!g.solid || !g.fertile) continue;  // soil destroyed
                t = JobType::Till;
                at = p.ground;
            } else if (st == PlotState::Empty && seeds) {
                t = JobType::Sow;
            } else if (st == PlotState::Mature) {
                t = JobType::Harvest;
            }
            if (t == JobType::None || has(t, at)) continue;
            Job& j = add(t, f->polity, at, t == JobType::Harvest ? 1.3f : 1.0f);
            j.farm = f->id;
            j.plot = (u32)i;
        }
    }

    // Loose piles → storage. A pile job nobody has taken for a day is dropped (the pile
    // may be out of reach); it is planned again if someone can get there later.
    for (const Job& j : jobs.all())
        if (j.alive && j.type == JobType::HaulPile && j.claimed_by == kNoEntity && now_ - j.created > kTicksPerDay) {
            pile_jobs[{j.from, j.item}]--;
            jobs.complete(j.id);
        }
    auto region_of = [&](const Vec3i& p) -> u16 {
        for (int dy = 0; dy <= 1; ++dy)
            if (auto it = region_map_.find(p + Vec3i{0, dy, 0}); it != region_map_.end()) return it->second;
        return 0;
    };
    for (const Store& s : econ.stores()) {
        if (!s.alive || s.kind != StoreKind::Pile || s.empty()) continue;
        // Which polity cares? The one with the nearest storage that can be walked to
        // from the pile (when the regions are known).
        const u16 pr = region_of(s.pos);
        u16 owner = 0;
        float bd = 1e30f;
        for (auto& p : ctx_.society->polities()) {
            if (!p.alive) continue;
            for (StoreId sid : ctx_.society->public_stores(p.id)) {
                const Store* st = econ.store(sid);
                if (pr && region_of(st->pos) && region_of(st->pos) != pr) continue;
                float d = (float)st->pos.dist2(s.pos);
                if (d < bd) {
                    bd = d;
                    owner = p.id;
                }
            }
        }
        if (!owner || bd > 260.0f * 260.0f) continue;
        for (auto& st : s.items) {
            if (pile_jobs[{s.id, st.item}] > 0) continue;
            if (reg.item(st.item).value < 0.3f) continue;  // spoil (dirt, sand) stays in heaps
            Job& j = add(JobType::HaulPile, owner, s.pos, reg.item(st.item).nutrition > 0 ? 1.1f : 0.8f);
            j.from = s.id;
            j.item = st.item;
            j.count = st.count;
        }
    }

    // Construction sites.
    for (const Building& bc : ctx_.buildings->all()) {
        if (!bc.alive || bc.complete) continue;
        Building* b = ctx_.buildings->get(bc.id);
        const Project* pr = b->project ? ctx_.society->project(b->project) : nullptr;
        if (b->project && (!pr || pr->status != 0)) continue;
        float prio = pr ? pr->priority : 1.0f;
        // Materials still to deliver.
        auto need = ctx_.buildings->remaining_cost(*b);
        for (auto& [item, count] : need) {
            i32 missing = count - in_transit[{b->site, item}];
            float unit = std::max(0.01f, reg.item(item).weight);
            // Loads are sized for a hauler with a cart when the polity has any; one
            // without takes what fits and the rest is planned again.
            float cap = tune.carry_capacity;
            for (const auto& cp : chars_)
                if (cp && cp->alive && cp->polity == b->polity && cp->cart != kNoItem) {
                    cap = carry_capacity(*cp);
                    break;
                }
            i32 load = std::max(1, (i32)std::floor(cap / unit));
            while (missing > 0) {
                // Source: nearest public store holding the item.
                StoreId src = kNoStore;
                float bd = 1e30f;
                for (StoreId sid : ctx_.society->public_stores(b->polity)) {
                    if (econ.available(sid, item) - promised[{sid, item}] <= 0) continue;
                    const Store* s = econ.store(sid);
                    float d = (float)s->pos.dist2(b->entrance);
                    if (d < bd) {
                        bd = d;
                        src = sid;
                    }
                }
                if (!src) break;
                i32 n = std::min({missing, load, econ.available(src, item) - promised[{src, item}]});
                promised[{src, item}] += n;
                Job& j = add(JobType::HaulToSite, b->polity, econ.store(src)->pos, prio);
                j.from = src;
                j.to = b->site;
                j.item = item;
                j.count = n;
                j.building = b->id;
                j.project = b->project;
                missing -= n;
                in_transit[{b->site, item}] += n;
                if (in_transit[{b->site, item}] > 400) break;
            }
        }
        // Buildable cells (a few at a time, spread over the plan).
        int open = build_jobs[b->id];
        int hint = 0;
        for (int k = 0; open < 4 && k < 8; ++k) {
            int idx = ctx_.buildings->next_buildable(*b, hint);
            if (idx < 0) break;
            hint = idx + 1;
            Vec3i p = b->plan_pos[(size_t)idx];
            if (has(JobType::Build, p)) continue;
            MatId want = vmat(b->plan_vox[(size_t)idx]);
            if (want) {
                ItemId it = item_for_material(reg, want);
                if (it != kNoItem && econ.available(b->site, it) <= 0 && !ctx_.world->material(p).solid) continue;
            }
            Job& j = add(JobType::Build, b->polity, p, prio);
            j.building = b->id;
            j.plot = (u32)idx;
            j.project = b->project;
            existing[{(int)JobType::Build, p}] = j.id;
            ++open;
        }
        if (ctx_.buildings->site_done(*b)) {
            ctx_.buildings->finish(*b, pr ? pr->cause : 0);
            if (b->project) ctx_.society->finish_project(b->project, true, b->last_event);
        }
    }

    // Dig projects (clearing a sealed spring, channels): expose cubes from the outside in.
    for (const Project& prc : ctx_.society->projects()) {
        if (!prc.alive || prc.status != 0 || prc.kind != "dig") continue;
        const u32 pid = prc.id;
        World& w = *ctx_.world;
        std::vector<Vec3i> left;
        for (const Json& c : prc.params["cubes"].items()) {
            Vec3i q{c[0].as_int(), c[1].as_int(), c[2].as_int()};
            const Material& m = reg.mat(w.mat(q));
            if (m.solid && m.diggable) left.push_back(q);
        }
        if (left.empty()) {
            ctx_.society->finish_project(pid, true, prc.cause);
            continue;
        }
        std::sort(left.begin(), left.end(), [](const Vec3i& a, const Vec3i& b) { return a.y != b.y ? a.y > b.y : a < b; });
        int open = 0;
        for (const Job& j : jobs.all())
            if (j.alive && j.type == JobType::Dig && j.project == pid) ++open;
        for (const Vec3i& q : left) {
            if (open >= 4) break;
            if (has(JobType::Dig, q)) continue;
            bool exposed = false;
            for (int k = 0; k < 6 && !exposed; ++k) exposed = !reg.mat(w.mat(q + kDir6[k])).solid;
            if (!exposed) continue;
            Job& j = add(JobType::Dig, prc.polity, q, prc.priority);
            j.project = pid;
            j.cause = prc.cause;
            existing[{(int)JobType::Dig, q}] = j.id;
            ++open;
        }
    }

    // Crafting and raw materials on demand (agents_production.cpp).
    production_jobs();

    // Research at the study (or the hall) while a research direction is set.
    for (auto& pc : ctx_.society->polities()) {
        if (!pc.alive || pc.policies.research.empty()) continue;
        const Building* place = nullptr;
        for (const Building& b : ctx_.buildings->all())
            if (b.alive && b.functional && b.polity == pc.id && b.def == "study") place = &b;
        if (!place) place = ctx_.buildings->get(pc.seat);
        if (!place || !place->functional) continue;
        int open = 0;
        for (const Job& j : jobs.all())
            if (j.alive && j.type == JobType::Research && j.polity == pc.id) ++open;
        const int want = place->def == "study" ? 3 : 2;
        for (int k = open; k < want; ++k) {
            Job& j = add(JobType::Research, pc.id, place->inside, 0.9f);
            j.building = place->id;
        }
    }

    // Cooking: kitchens bake bread from spare grain; kitchens and campfires roast meat.
    {
        const ItemId bread = reg.find_item("bread"), meat = reg.find_item("meat"), roast = reg.find_item("cooked_meat");
        for (const Building& k : ctx_.buildings->all()) {
            if (!k.alive || !k.complete || !k.functional || !k.store) continue;
            if (k.def != "kitchen" && k.def != "campfire") continue;
            const Polity* p = ctx_.society->polity(k.polity);
            if (!p || has(JobType::Cook, k.inside)) continue;
            const Store* ks = econ.store(k.store);
            if (!ks) continue;
            auto spare = [&](ItemId it) {
                i64 n = 0;
                for (StoreId sid : ctx_.society->public_stores(k.polity)) n += econ.available(sid, it);
                return n;
            };
            ItemId in = kNoItem;
            if (meat != kNoItem && roast != kNoItem && spare(meat) >= 3 && ks->count(roast) < 30) in = meat;
            else if (k.def == "kitchen" && ks->count(bread) <= 30 && spare(grain) >= 12) in = grain;
            if (in == kNoItem) continue;
            Job& j = add(JobType::Cook, k.polity, k.inside, in == meat ? 1.0f : 0.9f);
            j.building = k.id;
            j.item = in;
        }
    }

    // Herbs for the wounded: gatherers look for medicinal plants among the bushes when
    // the stock runs low (one bundle for every four people, and one per wounded).
    const ItemId herbs = reg.find_item("herbs");
    for (auto& pc : ctx_.society->polities()) {
        if (!pc.alive || herbs == kNoItem || !pc.has_tech("herbalism")) continue;
        const Building* seat = ctx_.buildings->get(pc.seat);
        if (!seat) continue;
        int people = 0, wounded = 0;
        for (const auto& cp : chars_)
            if (cp && cp->alive && !cp->departed && cp->polity == pc.id) {
                ++people;
                if (treatment_need(*cp) > 0.0f) ++wounded;
            }
        i64 stock = 0;
        for (StoreId sid : ctx_.society->public_stores(pc.id))
            if (const Store* st = ctx_.econ->store(sid)) stock += st->count(herbs);
        const int want = people / 4 + wounded;
        int open = 0;
        for (const Job& j : jobs.all())
            if (j.alive && j.type == JobType::Forage && j.polity == pc.id && j.item == herbs) ++open;
        if (stock + open * 3 >= want) continue;
        World& w = *ctx_.world;
        const MatId bush = reg.m().berry_bush, herb = reg.m().herb_plant;
        for (int r = 6; r <= 60 && open < 2; r += 4)
            for (int i = 0; i < 24 && open < 2; ++i) {
                float a = (float)i / 24.0f * 6.2831853f + 0.13f;
                int x = seat->entrance.x + (int)std::lround(std::cos(a) * (float)r);
                int z = seat->entrance.z + (int)std::lround(std::sin(a) * (float)r);
                ColumnInfo col = w.gen().column(x, z);
                if (!col.land) continue;
                Vec3i p{x, col.top + 1, z};
                const MatId m = w.mat(p);
                // Wild medicinal plants, or medicinal leaves among the berry bushes.
                if ((m != bush && (herb == reg.m().air || m != herb)) || has(JobType::Forage, p)) continue;
                Job& j = add(JobType::Forage, pc.id, p, 0.9f);
                j.item = herbs;
                existing[{(int)JobType::Forage, p}] = 1;
                ++open;
            }
    }

    // Trade caravans: one at a time to each partner, when there is something worth
    // taking. The load leaves from our store holding most of it for the partner's store
    // nearest our seat.
    if (now_ % kTicksPerHour == 0) {
        // A load nobody set out with for half a day is planned afresh (aid waits longer).
        for (const Job& j : jobs.all())
            if (j.alive && j.type == JobType::Trade && j.claimed_by == kNoEntity &&
                now_ - j.created > (j.plot == 1 ? kTicksPerDay * 2 : kTicksPerDay / 2))
                jobs.complete(j.id);
        for (const Polity& pc : ctx_.society->polities()) {
            if (!pc.alive) continue;
            const Building* seat = ctx_.buildings->get(pc.seat);
            for (const TradePact& t : pc.pacts) {
                if (t.blocked_until > now_ || ctx_.society->at_war(pc.id, t.partner)) continue;
                bool open = false;
                for (const Job& j : jobs.all())
                    if (j.alive && j.type == JobType::Trade && j.polity == pc.id && j.project == t.partner) open = true;
                if (open) continue;
                i32 amount = 0;
                const ItemId it = ctx_.society->trade_export(pc.id, t.partner, &amount);
                if (it == kNoItem || amount <= 0) continue;
                StoreId src = kNoStore, dst = kNoStore;
                i32 most = 0;
                for (StoreId sid : ctx_.society->public_stores(pc.id)) {
                    const i32 n = econ.available(sid, it) - promised[{sid, it}];
                    if (n > most) {
                        most = n;
                        src = sid;
                    }
                }
                const Vec3i home = seat ? seat->entrance : (src ? econ.store(src)->pos : Vec3i{});
                float bd = 1e30f;
                for (StoreId sid : ctx_.society->public_stores(t.partner)) {
                    const Store* s = econ.store(sid);
                    const float d = (float)s->pos.dist2(home) * (s->kind == StoreKind::Stockpile ? 1.0f : 1.5f);
                    if (d < bd) {
                        bd = d;
                        dst = sid;
                    }
                }
                if (!src || !dst) continue;
                float cap = tune.carry_capacity;
                for (const auto& cp : chars_)
                    if (cp && cp->alive && cp->polity == pc.id && cp->cart != kNoItem) {
                        cap = carry_capacity(*cp);
                        break;
                    }
                const i32 load = std::min({amount, most, std::max(1, (i32)std::floor(cap / std::max(0.05f, reg.item(it).weight)))});
                if ((float)load * reg.item(it).value < 3.0f) continue;  // not worth the walk
                Job& j = add(JobType::Trade, pc.id, econ.store(src)->pos, 0.9f);
                j.from = src;
                j.to = dst;
                j.item = it;
                j.count = load;
                j.project = t.partner;
                j.cause = t.event;
                promised[{src, it}] += load;
            }
        }
    }

    // Foraging: the main work of a band without fields; for farmers a stopgap when food
    // is short.
    for (auto& pc : ctx_.society->polities()) {
        const Building* seat = ctx_.buildings->get(pc.seat);
        if (!pc.alive || !seat) continue;
        int people = 0;
        for (const auto& cp : chars_)
            if (cp && cp->alive && !cp->departed && cp->polity == pc.id) ++people;
        const bool band = ctx_.society->foraging_band(pc);
        if (pc.stats.food_days > (band ? 5.0f : 3.0f)) continue;
        const int max_open = band ? std::max(4, people * 2 / 3) : 4;
        const int max_r = band ? 90 : 60;
        int open = 0;
        for (const Job& j : jobs.all())
            if (j.alive && j.type == JobType::Forage && j.polity == pc.id) ++open;
        // Look about at scattered spots, nearer ones more often (a pure hash of the time:
        // the same run always looks at the same places).
        std::vector<std::pair<i64, Vec3i>> found;
        const int samples = band ? 220 : 90;
        for (int i = 0; i < samples && open + (int)found.size() < max_open * 2; ++i) {
            const u64 h = hash3(0x0F0A6Eull + pc.id, (i32)(now_ / 50), i, 7);
            const float u = (float)(h & 0xFFFF) / 65535.0f, v = (float)((h >> 16) & 0xFFFF) / 65535.0f;
            const float r = 3.0f + (float)(max_r - 3) * u * (0.35f + 0.65f * u);
            const float a = v * 6.2831853f;
            const int x = seat->entrance.x + (int)std::lround(std::cos(a) * r);
            const int z = seat->entrance.z + (int)std::lround(std::sin(a) * r);
            Vec3i p;
            if (!food_plant_at(x, z, p) || has(JobType::Forage, p)) continue;
            found.push_back({p.dist2(seat->entrance), p});
        }
        std::sort(found.begin(), found.end());
        for (const auto& [d, p] : found) {
            if (open >= max_open) break;
            if (has(JobType::Forage, p)) continue;
            add(JobType::Forage, pc.id, p, 1.0f);
            existing[{(int)JobType::Forage, p}] = 1;
            ++open;
        }
    }

    // Seed grain: farmers-to-be gather wild grain whatever the larder holds.
    if (now_ % 300 == 0) {
        const MatId wild_grain = reg.m().wild_grain;
        for (auto& pc : ctx_.society->polities()) {
            if (!pc.alive || !pc.has_tech("farming") || wild_grain == reg.m().air) continue;
            const Building* seat = ctx_.buildings->get(pc.seat);
            if (!seat) continue;
            i64 have = 0;
            for (StoreId sid : ctx_.society->public_stores(pc.id)) have += econ.available(sid, grain);
            if (have >= 40) continue;
            int open = 0;
            for (const Job& j : jobs.all())
                if (j.alive && j.type == JobType::Forage && j.polity == pc.id &&
                    ctx_.world->mat(j.pos) == wild_grain)
                    ++open;
            for (int i = 0; i < 400 && open < 5; ++i) {
                const u64 h = hash3(0x5EEDull + pc.id, (i32)(now_ / 300), i, 3);
                const float u = (float)(h & 0xFFFF) / 65535.0f, v = (float)((h >> 16) & 0xFFFF) / 65535.0f;
                const float r = 4.0f + 100.0f * u;
                const int x = seat->entrance.x + (int)std::lround(std::cos(v * 6.2831853f) * r);
                const int z = seat->entrance.z + (int)std::lround(std::sin(v * 6.2831853f) * r);
                const ColumnInfo col = ctx_.world->gen().column(x, z);
                if (!col.land) continue;
                const Vec3i p{x, col.top + 1, z};
                if (vmat(ctx_.world->peek(p)) != wild_grain || has(JobType::Forage, p)) continue;
                add(JobType::Forage, pc.id, p, 0.95f);
                existing[{(int)JobType::Forage, p}] = 1;
                ++open;
            }
        }
    }

    // Hunting: game for the pot (and hides for clothes), once people know how. Each hunt
    // is one animal, spoken for until the hunter gives up or brings it home.
    if (ctx_.fauna && now_ % 300 == 0) {
        const ItemId hide = reg.find_item("hide");
        // Animals spoken for by a hunt that no longer exists are free again.
        for (const Animal& a : ctx_.fauna->all()) {
            if (!a.alive || !(a.hunted_by & 0x80000000u)) continue;
            bool live = false;
            for (const Job& j : jobs.all())
                if (j.alive && j.type == JobType::Hunt && j.project == a.id) live = true;
            if (!live) ctx_.fauna->get(a.id)->hunted_by = kNoEntity;
        }
        for (auto& pc : ctx_.society->polities()) {
            if (!pc.alive || !(pc.has_tech("hunting") || pc.has_tech("hunting_weapons"))) continue;
            const Building* seat = ctx_.buildings->get(pc.seat);
            if (!seat) continue;
            const Vec3i home = seat->entrance;
            int people = 0, unclothed = 0;
            for (const auto& cp : chars_)
                if (cp && cp->alive && !cp->departed && cp->polity == pc.id && !cp->is_girl()) {
                    ++people;
                    if (cp->clothes == kNoItem) ++unclothed;
                }
            i64 hides = 0;
            for (StoreId sid : ctx_.society->public_stores(pc.id))
                if (const Store* st = ctx_.econ->store(sid); st && hide != kNoItem) hides += st->count(hide);
            const bool want_food = pc.stats.food_days < 6.0f;
            const bool want_hides = pc.has_tech("hide_working") && hides < unclothed * 2;
            if (!want_food && !want_hides) continue;
            int open = 0;
            for (const Job& j : jobs.all())
                if (j.alive && j.type == JobType::Hunt && j.polity == pc.id) ++open;
            const int max_open = std::max(1, people / 5);
            while (open < max_open) {
                const u32 prey = ctx_.fauna->find_prey(home, 100, pc.has_tech("hunting_weapons"));
                Animal* a = ctx_.fauna->get(prey);
                if (!a) break;
                Job& j = add(JobType::Hunt, pc.id, a->foot, want_food ? 1.05f : 0.9f);
                j.project = prey;
                a->hunted_by = 0x80000000u | pc.id;  // spoken for
                ++open;
            }
        }
    }
}

}  // namespace icarus
