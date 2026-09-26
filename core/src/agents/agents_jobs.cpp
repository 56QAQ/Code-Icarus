// Agents: job generation. Work appears where the world needs it: fields to till, sow and
// harvest; loose goods to store; construction sites to supply and build; kitchens to run.
#include <algorithm>
#include <cmath>
#include <map>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"

namespace icarus {

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
    for (const Job& j : jobs.all()) {
        if (!j.alive) continue;
        existing[{(int)j.type, j.pos}] = j.id;
        if (j.type == JobType::HaulToSite) in_transit[{j.to, j.item}] += j.count;
        if (j.type == JobType::Build) build_jobs[j.building]++;
        if (j.type == JobType::HaulPile) pile_jobs[{j.from, j.item}]++;
    }
    auto has = [&](JobType t, const Vec3i& p) { return existing.count({(int)t, p}) != 0; };

    // Fields.
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

    // Loose piles → storage.
    for (const Store& s : econ.stores()) {
        if (!s.alive || s.kind != StoreKind::Pile || s.empty()) continue;
        // Which polity cares? The one with the nearest storage.
        u16 owner = 0;
        float bd = 1e30f;
        for (auto& p : ctx_.society->polities()) {
            if (!p.alive) continue;
            for (StoreId sid : ctx_.society->public_stores(p.id)) {
                const Store* st = econ.store(sid);
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
            i32 load = std::max(1, (i32)std::floor(tune.carry_capacity / unit));
            while (missing > 0) {
                // Source: nearest public store holding the item.
                StoreId src = kNoStore;
                float bd = 1e30f;
                for (StoreId sid : ctx_.society->public_stores(b->polity)) {
                    if (econ.available(sid, item) <= 0) continue;
                    const Store* s = econ.store(sid);
                    float d = (float)s->pos.dist2(b->entrance);
                    if (d < bd) {
                        bd = d;
                        src = sid;
                    }
                }
                if (!src) break;
                i32 n = std::min({missing, load, econ.available(src, item)});
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

    // Kitchens: turn grain into bread when there is grain to spare.
    for (const Building& k : ctx_.buildings->all()) {
        if (!k.alive || !k.complete || !k.functional || k.def != "kitchen" || !k.store) continue;
        const Polity* p = ctx_.society->polity(k.polity);
        if (!p) continue;
        ItemId bread = reg.find_item("bread");
        const Store* ks = econ.store(k.store);
        if (!ks || ks->count(bread) > 30) continue;
        i64 grain_total = 0;
        for (StoreId sid : ctx_.society->public_stores(k.polity)) grain_total += econ.available(sid, grain);
        if (grain_total < 12) continue;
        if (has(JobType::Cook, k.inside)) continue;
        Job& j = add(JobType::Cook, k.polity, k.inside, 0.9f);
        j.building = k.id;
    }

    // Foraging when food is short.
    for (auto& pc : ctx_.society->polities()) {
        if (!pc.alive || pc.stats.food_days > 3.0f) continue;
        const Building* seat = ctx_.buildings->get(pc.seat);
        if (!seat) continue;
        int open = 0;
        for (const Job& j : jobs.all())
            if (j.alive && j.type == JobType::Forage && j.polity == pc.id) ++open;
        World& w = *ctx_.world;
        const MatId bush = reg.m().berry_bush;
        for (int r = 4; r <= 60 && open < 4; r += 4) {
            for (int i = 0; i < 24 && open < 4; ++i) {
                float a = (float)i / 24.0f * 6.2831853f;
                int x = seat->entrance.x + (int)std::lround(std::cos(a) * (float)r);
                int z = seat->entrance.z + (int)std::lround(std::sin(a) * (float)r);
                ColumnInfo col = w.gen().column(x, z);
                if (!col.land) continue;
                Vec3i p{x, col.top + 1, z};
                if (w.mat(p) != bush || has(JobType::Forage, p)) continue;
                add(JobType::Forage, pc.id, p, 1.0f);
                existing[{(int)JobType::Forage, p}] = 1;
                ++open;
            }
        }
    }
}

}  // namespace icarus
