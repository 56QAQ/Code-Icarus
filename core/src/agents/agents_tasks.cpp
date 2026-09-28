// Agents: task execution state machines.
#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "icarus/agents/agents.h"
#include "icarus/agents/footprint.h"
#include "icarus/fauna/fauna.h"
#include "icarus/sim/ecology.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
bool food_item(const Registry& reg, ItemId it) { return reg.item(it).nutrition > 0; }
}  // namespace

void Agents::run_task(Character& c) {
    bool ok = true;
    switch (c.task.type) {
        case TaskType::Eat: ok = task_eat(c); break;
        case TaskType::Drink: ok = task_drink(c); break;
        case TaskType::Sleep: ok = task_sleep(c); break;
        case TaskType::Socialize: ok = task_social(c); break;
        case TaskType::Work: ok = task_work(c); break;
        case TaskType::Flee: ok = task_flee(c); break;
        case TaskType::Protest: ok = task_protest(c); break;
        case TaskType::Steal: ok = task_steal(c); break;
        case TaskType::Govern: ok = task_govern(c); break;
        case TaskType::Cast: ok = task_cast(c); break;
        case TaskType::Escape: ok = task_escape(c); break;
        case TaskType::Fight: ok = task_fight(c); break;
        case TaskType::Leave: ok = task_leave(c); break;
        case TaskType::Heal: ok = task_treat(c); break;
        case TaskType::Wander:
        case TaskType::Idle: ok = task_wander(c); break;
        default: break;
    }
    (void)ok;
}

// ------------------------------------------------------------------------------ helpers

bool Agents::seed_kept(const Character& c) const {
    // Farmers without enough seed keep the grain for sowing unless someone is starving.
    const Polity* p = ctx_.society->polity(c.polity);
    if (!p || !p->has_tech("farming") || c.needs.food <= 0.15f) return false;
    const ItemId grain = ctx_.reg->find_item("grain");
    i64 seed = 0;
    for (StoreId sid : ctx_.society->public_stores(c.polity)) seed += ctx_.econ->available(sid, grain);
    return seed < kSeedKept;
}

StoreId Agents::find_food_store(Character& c, bool public_only, bool allow_over_ration) {
    (void)public_only;
    Polity* p = ctx_.society->polity(c.polity);
    if (!p) return kNoStore;
    // Distribution policy can deny access under scarcity.
    if (!allow_over_ration && !c.is_girl()) {
        float fd = p->stats.food_days;
        if (p->policies.distribution == 1 && fd < 4.0f && c.work_debt > 3.0f) return kNoStore;
        (void)fd;
    }
    StoreId best = kNoStore;
    float bd = 1e30f;
    // Seed grain kept for sowing is not food for this trip: a store holding nothing else
    // is not worth the walk (the hungry forage instead).
    const ItemId seed = seed_kept(c) ? ctx_.reg->find_item("grain") : kNoItem;
    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
        const Store* s = ctx_.econ->store(sid);
        if (!s) continue;
        bool has = false;
        for (auto& st : s->items)
            if (st.item != seed && food_item(*ctx_.reg, st.item) && ctx_.econ->available(sid, st.item, c.id) > 0) has = true;
        if (!has) continue;
        if (blacklisted(c, s->pos)) continue;
        float d = (float)c.foot.dist2(s->pos);
        if (d < bd) {
            bd = d;
            best = sid;
        }
    }
    return best;
}

void Agents::refresh_water_spots() {
    // Scan around every settlement for water surfaces people can drink from, and label
    // walkable regions so nobody plans a trip to water they cannot reach.
    World& w = *ctx_.world;
    Nav& nav = *ctx_.nav;
    const MatId WATER = ctx_.reg->m().water;
    water_spots_.clear();
    water_regions_.clear();
    std::vector<Vec3i> centers;
    for (const Polity& p : ctx_.society->polities()) {
        if (!p.alive) continue;
        if (const Building* b = ctx_.buildings->get(p.seat)) centers.push_back(b->entrance);
    }
    // Water is sought around every place people live, not only the seats: a colony or a
    // conquered village far from the hall drinks from its own lake.
    std::vector<Vec3i> scan = centers;
    for (const Polity& p : ctx_.society->polities())
        if (p.alive)
            for (const Vec3i& o : p.outposts) scan.push_back(o);
    for (const Building& b : ctx_.buildings->all()) {
        if (!b.alive || !b.complete || !b.polity || (b.beds <= 0 && b.def != "hall")) continue;
        bool near = false;
        for (const Vec3i& c : scan) near = near || c.dist2(b.entrance) < 70 * 70;
        if (!near) scan.push_back(b.entrance);
    }
    const int R = 110;
    std::unordered_set<Vec3i, Vec3iHash> seen;
    for (const Vec3i& c : scan) {
        for (int dz = -R; dz <= R; dz += 2)
            for (int dx = -R; dx <= R; dx += 2) {
                if (dx * dx + dz * dz > R * R) continue;
                int x = c.x + dx, z = c.z + dz;
                ColumnInfo col = w.gen().column(x, z);
                if (!col.land || col.sea) continue;  // (the sea is salt)
                for (int y = col.top + 4; y >= col.top - 4; --y) {
                    Voxel v = w.get({x, y, z});
                    if (vmat(v) == 0) continue;
                    if (vmat(v) != WATER || vlevel(v) < 3) break;
                    // Water surface found: look for a place to stand beside or in it.
                    Vec3i cand[10] = {{x, y, z},         {x + 1, y + 1, z}, {x - 1, y + 1, z}, {x, y + 1, z + 1},
                                      {x, y + 1, z - 1}, {x, y + 1, z},     {x + 1, y + 2, z}, {x - 1, y + 2, z},
                                      {x, y + 2, z + 1}, {x, y + 2, z - 1}};
                    for (const Vec3i& s : cand) {
                        if (nav.standable(s) && seen.insert(s).second) {
                            water_spots_.push_back(s);
                            break;
                        }
                    }
                    break;
                }
            }
    }
    relabel_regions();
    // Regions: flood from settlement anchors (seats, farms). Characters elsewhere get
    // region 0 (unknown) and fall back to plain path searches.
    if (survey_.active) return;  // one survey at a time; it finishes within a few ticks
    std::vector<Vec3i> anchors;
    for (const Vec3i& c : centers) anchors.push_back(c);
    for (const Farm& f : ctx_.farming->all())
        if (f.alive) anchors.push_back(f.center + Vec3i{0, 1, 0});
    // New anchors (a new field, a new village) inside ground already surveyed need no new
    // survey; only changed walkability or truly new ground does.
    bool moved = anchors != region_anchors_;
    if (moved && !region_map_.empty()) {
        bool covered = true;
        for (const Vec3i& a : anchors) {
            if (std::find(region_anchors_.begin(), region_anchors_.end(), a) != region_anchors_.end()) continue;
            Vec3i st = a;
            if ((!nav.standable(st) && !nav.find_standable_near(a, st, 3)) || !region_map_.count(st)) covered = false;
        }
        if (covered) {
            region_anchors_ = anchors;
            moved = false;
        }
    }
    // A full survey is costly: after building work or collapses at most every few hours
    // (paths are still searched for real in between; the survey only guides them).
    bool stale = moved || (nav.major_dirty && now_ >= region_built_ + kTicksPerHour * 6) ||
                 (nav.minor_dirty && now_ >= region_built_ + kTicksPerDay);
    if (stale) {
        region_built_ = now_;
        nav.major_dirty = nav.minor_dirty = false;
        survey_ = Survey{};
        survey_.active = true;
        survey_.anchors = std::move(anchors);
        survey_.map.reserve(1 << 16);
        survey_.open.assign(1, 0);
        survey_step();
    }
}

namespace {
constexpr int kSurveyRadius = 110;     // xz reach of one flood around its anchor
constexpr u32 kSurveyFloodMax = 60000; // positions one flood may label
constexpr int kSurveyTickNodes = 4000; // positions expanded per tick
}  // namespace

void Agents::survey_step() {
    Survey& s = survey_;
    if (!s.active) return;
    Nav& nav = *ctx_.nav;
    const i64 r2 = (i64)kSurveyRadius * kSurveyRadius;
    int budget = kSurveyTickNodes;
    Vec3i nb[8];
    float nc[8];
    while (budget > 0) {
        if (s.id == 0) {
            if (s.anchor >= s.anchors.size() || s.open.size() >= 65535) {
                // Done: the new survey replaces the old one.
                region_map_ = std::move(s.map);
                region_open_ = std::move(s.open);
                region_anchors_ = std::move(s.anchors);
                survey_ = Survey{};
                relabel_regions();
                return;
            }
            const Vec3i a = s.anchors[s.anchor++];
            Vec3i st = a;
            if (!nav.standable(st) && !nav.find_standable_near(a, st, 3)) continue;
            if (s.map.count(st)) continue;
            s.id = (u16)s.open.size();
            s.seed = st;
            s.queue.assign(1, st);
            s.head = 0;
            s.pushed = 1;
            s.cut = false;
            s.map.emplace(st, s.id);
            --budget;
        }
        while (budget > 0 && s.head < s.queue.size() && s.pushed < kSurveyFloodMax) {
            const Vec3i p = s.queue[s.head++];
            --budget;
            const int n = nav.neighbors(p, nb, nc);
            for (int i = 0; i < n; ++i) {
                const Vec3i& q = nb[i];
                const i64 dx = q.x - s.seed.x, dz = q.z - s.seed.z;
                if (dx * dx + dz * dz > r2) {
                    s.cut = true;
                    continue;
                }
                if (s.map.emplace(q, s.id)) {
                    s.queue.push_back(q);
                    ++s.pushed;
                }
            }
        }
        if (s.head >= s.queue.size() || s.pushed >= kSurveyFloodMax) {
            // This flood is finished; a flood stopped at its limit may reach further.
            s.open.push_back(s.cut || s.head < s.queue.size() ? 1 : 0);
            nav.stats.flood_nodes += s.pushed;
            s.id = 0;
            s.queue.clear();
            s.head = 0;
            s.pushed = 0;
        } else if (s.head > 4096 && s.head * 2 > s.queue.size()) {
            s.queue.erase(s.queue.begin(), s.queue.begin() + (long)s.head);
            s.head = 0;
        }
    }
}

void Agents::relabel_regions() {
    water_regions_.clear();
    for (const Vec3i& s : water_spots_) {
        auto it = region_map_.find(s);
        water_regions_.push_back(it == region_map_.end() ? 0 : it->second);
    }
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character* c = chars_[i].get();
        if (!c || !c->alive || c->departed) continue;
        auto it = region_map_.find(c->foot);
        c->region = it == region_map_.end() ? 0 : it->second;
    }
}

bool Agents::find_water(Character& c, Vec3i& stand, Vec3i& water) {
    World& w = *ctx_.world;
    Nav& nav = *ctx_.nav;
    const MatId WATER = ctx_.reg->m().water;
    auto water_next_to = [&](const Vec3i& p, Vec3i& out) {
        for (int dy = -2; dy <= 0; ++dy)
            for (int d = 0; d < 5; ++d) {
                Vec3i q = p + (d < 4 ? kDir4H[d] : Vec3i{0, 0, 0}) + Vec3i{0, dy, 0};
                Voxel v = w.get(q);
                if (vmat(v) == WATER && vlevel(v) >= 2 && !w.gen().salt(q.x, q.z)) {
                    out = q;
                    return true;
                }
            }
        return false;
    };
    if (water_spots_.empty()) refresh_water_spots();
    // The remembered spot is kept only while it is about as close as the best candidate.
    if (c.water_spot.y > 0 && !blacklisted(c, c.water_spot) && nav.standable(c.water_spot) &&
        water_next_to(c.water_spot, water)) {
        i64 mine = c.foot.dist2(c.water_spot);
        i64 best = mine;
        for (size_t i = 0; i < water_spots_.size(); ++i)
            if (!c.region || water_regions_[i] == c.region) best = std::min(best, c.foot.dist2(water_spots_[i]));
        if (mine <= best * 2 + 64) {
            stand = c.water_spot;
            return true;
        }
    }
    // Nearest candidates by straight line, verified by an actual path.
    std::vector<std::pair<i64, Vec3i>> cands;
    bool same_region = false;  // spots known to be reachable deserve a full search
    for (int pass = 0; pass < 2 && cands.empty(); ++pass) {
        same_region = pass == 0 && c.region != 0;
        if (pass == 1 && !c.region) break;
        for (size_t i = 0; i < water_spots_.size(); ++i) {
            const Vec3i& s = water_spots_[i];
            if (pass == 0 && c.region && water_regions_[i] != c.region) continue;
            if (blacklisted(c, s)) continue;
            cands.push_back({c.foot.dist2(s), s});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });
    Path tmp;
    int tries = 0;
    for (auto& [d, s] : cands) {
        if (tries >= 4) break;
        if (!nav.standable(s) || !water_next_to(s, water)) continue;
        ++tries;
        if (nav.find_path(c.foot, s, false, tmp, same_region ? 40000 : 8000)) {
            stand = s;
            c.water_spot = s;
            return true;
        }
        blacklist(c, s, kTicksPerHour * 2);
        // Anything else near an unreachable spot is probably unreachable too.
    }
    // Far from home with nothing near to be reached: back to the water at home (the walk
    // goes in legs; the spring by the village is known to be reachable from there).
    if (far_from_home(c))
        if (const Polity* p = ctx_.society->polity(c.polity))
            if (const Building* seat = ctx_.buildings->get(p->seat)) {
                i64 bd = 1LL << 60;
                for (const Vec3i& s : water_spots_) {
                    const i64 d = s.dist2(seat->entrance);
                    if (d >= bd || blacklisted(c, s) || !nav.standable(s) || !water_next_to(s, water)) continue;
                    bd = d;
                    stand = s;
                }
                if (bd < 60 * 60) {
                    c.water_spot = stand;
                    return true;
                }
            }
    return false;
}

bool Agents::far_from_home(const Character& c) const {
    const Polity* p = ctx_.society->polity(c.polity);
    const Building* seat = p ? ctx_.buildings->get(p->seat) : nullptr;
    return seat && seat->entrance.dist2(c.foot) > (i64)kLegFar * kLegFar * 4;
}

StoreId Agents::nearest_storage(u16 polity, const Vec3i& from, ItemId item) {
    StoreId best = kNoStore;
    float bd = 1e30f;
    float unit = item != kNoItem ? ctx_.reg->item(item).weight : 1.0f;
    auto region = [&](const Vec3i& p) -> u16 {
        for (int dy = 0; dy <= 1; ++dy)
            if (auto it = region_map_.find(p + Vec3i{0, dy, 0}); it != region_map_.end()) return it->second;
        return 0;
    };
    const u16 home = region(from);
    for (StoreId sid : ctx_.society->public_stores(polity)) {
        const Store* s = ctx_.econ->store(sid);
        if (!s || s->kind != StoreKind::Stockpile) continue;
        if (ctx_.econ->free_capacity(*s) < unit) continue;
        // Skip stores that cannot be walked to from here: another region, or left
        // hanging in the air (the ground beneath it blown away).
        const u16 there = region(s->pos);
        if (regions_apart(home, there)) continue;
        if (!there && !ctx_.world->solid(s->pos + Vec3i{0, -1, 0})) continue;
        float d = (float)from.dist2(s->pos);
        if (d < bd) {
            bd = d;
            best = sid;
        }
    }
    return best;
}

// ------------------------------------------------------------------------------ needs tasks

bool Agents::task_eat(Character& c) {
    Task& t = c.task;
    const Registry& reg = *ctx_.reg;
    const Polity* p = ctx_.society->polity(c.polity);
    float ration = p ? p->policies.ration : 1.0f;
    // What in her pack is not hers to eat: a caravan's load, and grain carried home when
    // her people are short of seed — unless she is starving (a carrier far from home eats
    // a little of what she carries rather than die on the road).
    const ItemId seed_grain = seed_kept(c) ? reg.find_item("grain") : kNoItem;
    const bool starving = c.needs.food < 0.2f;
    auto not_to_eat = [&](ItemId item, i32 have) {
        return (starving ? 0 : cargo_kept(c, item)) + (item == seed_grain ? have : 0);
    };
    auto carried_food = [&]() {
        const Store* s = ctx_.econ->store(c.inv);
        if (!s) return false;
        for (auto& st : s->items)
            if (food_item(reg, st.item) && st.count > not_to_eat(st.item, st.count)) return true;
        return false;
    };
    if (t.step == 0) {
        if (carried_food()) {
            t.step = 2;
            t.until = now_ + 40;
            say(c, "吃随身带的食物");
            return true;
        }
        StoreId s = find_food_store(c, true, false);
        // Far from the stores and hungry: whatever grows wild close by comes first.
        if (s != kNoStore && c.needs.food < 0.3f)
            if (const Store* st = ctx_.econ->store(s); st && st->pos.dist2(c.foot) > 80 * 80) {
                Vec3i p;
                if (wild_food_near(c.foot, 32, p)) {
                    t.target = p;
                    t.step = 5;
                    return true;
                }
            }
        if (s == kNoStore) {
            // Nothing put by: pick something growing wild nearby and eat it on the spot
            // (the starving walk further for it).
            Vec3i p;
            if (wild_food_near(c.foot, c.needs.food < 0.25f ? 44 : 28, p)) {
                t.target = p;
                t.step = 5;
                return true;
            }
            day.hungry_no_food++;
            say(c, "找不到可以吃的东西");
            // Not looking all over again at once: an hour of work (the foragers may bring
            // something home) before the next search.
            c.no_food_until = now_ + kTicksPerHour;
            end_task(c, false);
            return false;
        }
        t.store = s;
        t.step = 1;
    }
    if (t.step == 5) {
        say(c, "去摘野果充饥");
        Move m = move_to(c, t.target, true);
        if (m == Move::Failed) {
            day.hungry_no_food++;
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        const MatId fm = ctx_.world->mat(t.target);
        const Material& mm = reg.mat(fm);
        if (mm.forage_item == kNoItem || reg.item(mm.forage_item).nutrition <= 0.0f) {
            t.step = 0;  // someone else got there first: look again
            return true;
        }
        ctx_.world->set(t.target, make_voxel(mm.forage_to), 0);
        if (ctx_.ecology) ctx_.ecology->picked(t.target, now_, fm);
        ctx_.econ->add(c.inv, mm.forage_item, std::max(1, mm.forage_count), "forage");
        t.step = 2;
        t.until = now_ + 60;
        return true;
    }
    if (t.step == 1) {
        const Store* s = ctx_.econ->store(t.store);
        if (!s) {
            end_task(c, false);
            return false;
        }
        say(c, "去仓库取食物");
        Move m = move_to(c, s->pos, true);
        if (m == Move::Failed) {
            say(c, "去不了仓库");
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        // Take a portion according to the ration policy.
        float target = c.is_girl() ? 1.0f : std::min(1.0f, 0.35f + 0.6f * ration);
        // Elite-first distribution: under scarcity commoners only get a meagre portion.
        if (p && p->policies.distribution == 2 && !c.is_girl() && !c.drafted && p->stats.food_days < 2.0f)
            target = std::min(target, 0.55f);
        float need = target - c.needs.food;
        if (need <= 0.02f) {
            end_task(c, true);
            return true;
        }
        // Hands full of cargo (e.g. building materials for an unreachable site)? Put it
        // into this store, or set it down here, before taking food.
        if (carried_weight(c) > carry_capacity(c) - 2.0f) {
            StoreId here = t.store;
            const Store* hs = ctx_.econ->store(here);
            if (hs && hs->kind == StoreKind::Stockpile) deposit_all(c, here);
            if (carried_weight(c) > carry_capacity(c) - 2.0f) drop_cargo(c);
            s = ctx_.econ->store(t.store);
            if (!s) {
                end_task(c, false);
                return false;
            }
        }
        std::vector<ItemStack> avail = s->items;
        if (seed_kept(c)) {
            const ItemId grain = reg.find_item("grain");
            avail.erase(std::remove_if(avail.begin(), avail.end(), [&](const ItemStack& is) { return is.item == grain; }),
                        avail.end());
        }
        std::stable_sort(avail.begin(), avail.end(), [&](const ItemStack& a, const ItemStack& b) {
            return reg.item(a.item).nutrition + reg.item(a.item).joy > reg.item(b.item).nutrition + reg.item(b.item).joy;
        });
        i32 took = 0;
        for (auto& st : avail) {
            const ItemDef& d = reg.item(st.item);
            if (d.nutrition <= 0 || need <= 0) continue;
            i32 want = (i32)std::ceil(need / d.nutrition);
            want = std::min(want, ctx_.econ->available(t.store, st.item, c.id));
            i32 k = ctx_.econ->transfer(t.store, c.inv, st.item, want);
            took += k;
            need -= (float)k * d.nutrition;
        }
        if (took == 0) {
            end_task(c, false);
            return false;
        }
        day.ate_public++;
        t.step = 2;
        t.until = now_ + 40;
        if (ration < 0.85f && !c.is_girl()) c.remember(now_, MemoryKind::Rationed, p ? p->ruler : kNoEntity, -0.05f, 0);
        return true;
    }
    if (t.step == 2) {
        say(c, "吃饭");
        if (now_ < t.until) return true;
        Store* s = ctx_.econ->store(c.inv);
        float target = 1.0f;
        std::vector<ItemStack> items = s ? s->items : std::vector<ItemStack>{};
        float joy = 0;
        for (auto& st : items) {
            const ItemDef& d = reg.item(st.item);
            if (d.nutrition <= 0) continue;
            const i32 kept = not_to_eat(st.item, st.count);  // (a caravan's load, seed grain)
            while (c.needs.food < target - 0.02f && ctx_.econ->store(c.inv)->count(st.item) > kept) {
                ctx_.econ->remove(c.inv, st.item, 1, "eaten");
                c.needs.food = std::min(1.0f, c.needs.food + d.nutrition);
                joy += d.joy + ctx_.society->passive(c.polity, "meal_joy");
            }
        }
        if (joy > 0.04f) c.remember(now_, joy > 0.15f ? MemoryKind::Feast : MemoryKind::AteWell, kNoEntity, std::min(0.25f, joy), 0);
        c.last_ate = now_;
        end_task(c, true);
    }
    return true;
}

bool Agents::task_drink(Character& c) {
    Task& t = c.task;
    World& w = *ctx_.world;
    const MatId WATER = ctx_.reg->m().water;
    if (t.step == 0) {
        Vec3i stand, water;
        // Far from home (on campaign, on an errand): a stream or pond at hand first.
        const bool away = far_from_home(c);
        // Far from the known springs: any water nearby, and farther afield when parched.
        if (!(away && wild_water_near(c.foot, 40, stand)) && !find_water(c, stand, water) &&
            !wild_water_near(c.foot, 40, stand) && !(c.needs.water < 0.35f && wild_water_near(c.foot, 100, stand))) {
            day.thirsty_no_water++;
            say(c, "找不到水源");
            end_task(c, false);
            return false;
        }
        t.target = stand;
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, "去水边");
        Move m = move_to(c, t.target, false);
        if (m == Move::Failed) {
            blacklist(c, t.target, kTicksPerHour * 4);
            c.water_spot = {-1, -1, -1};
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        t.step = 2;
        t.until = now_ + 30;
    }
    if (t.step == 2) {
        say(c, "喝水");
        if (now_ < t.until) return true;
        // Drinking removes water from the world: from beside her first, else from within
        // arm's reach.
        Vec3i found{0, -1, 0};
        for (int dy = -2; dy <= 0 && found.y < 0; ++dy)
            for (int d = 0; d < 5 && found.y < 0; ++d) {
                Vec3i q = c.foot + (d < 4 ? kDir4H[d] : Vec3i{0, 0, 0}) + Vec3i{0, dy, 0};
                Voxel v = w.get(q);
                if (vmat(v) == WATER && vlevel(v) >= 1 && !w.gen().salt(q.x, q.z)) found = q;
            }
        for (int dy = -2; dy <= 0 && found.y < 0; ++dy)
            for (int dz = -2; dz <= 2 && found.y < 0; ++dz)
                for (int dx = -2; dx <= 2 && found.y < 0; ++dx) {
                    const Vec3i q = c.foot + Vec3i{dx, dy, dz};
                    const Voxel v = w.get(q);
                    if (vmat(v) == WATER && vlevel(v) >= 1 && !w.gen().salt(q.x, q.z)) found = q;
                }
        if (found.y < 0) {
            // The shallows here are drunk dry: another spot on the shore.
            c.water_spot = {-1, -1, -1};
            if (c.needs.water < 0.9f && t.fails < 2) {
                blacklist(c, c.foot, kTicksPerHour);
                ++t.fails;
                t.step = 0;
                return true;
            }
            end_task(c, false);
            return false;
        }
        Voxel v = w.get(found);
        // (A river of the random island flows: what one drinks is made up from upstream.
        // Ponds, puddles and the lakes of the other layouts are drunk down.)
        const ColumnInfo col = w.gen().column(found.x, found.z);
        const bool flowing = w.config().layout == WorldLayout::Random && col.water_top >= found.y && !col.sea;
        int l = vlevel(v) - (flowing ? 0 : 1);
        if (!flowing) w.set(found, l > 0 ? make_voxel(WATER, (u8)l) : make_voxel(0));
        c.needs.water = std::min(1.0f, c.needs.water + 0.45f);
        day.drinks++;
        c.last_drank = now_;
        if (c.needs.water < 0.95f && ++t.count < 5) {
            t.until = now_ + 30;
            return true;
        }
        end_task(c, true);
    }
    return true;
}

Vec3i Agents::sleep_spot(const Character& c, const Building* home, const Vec3i& near, Vec3i& axis) {
    // Beds taken by other sleepers (where they lie or are going to lie).
    std::vector<BedPrint> taken;
    for (const auto& op : chars_)
        if (op && op->alive && !op->departed && op->id != c.id && op->task.type == TaskType::Sleep && op->task.step >= 1)
            taken.push_back(bed_print(op->task.target, op->task.target2));
    auto free_bed = [&](const Vec3i& mid, const Vec3i& ax, float gap = 0.0f) {
        BedPrint b = bed_print(mid, ax);
        b.x0 -= gap;
        b.z0 -= gap;
        b.x1 += gap;
        b.z1 += gap;
        for (const BedPrint& o : taken)
            if (b.overlaps(o)) return false;
        return true;
    };
    Nav& nav = *ctx_.nav;
    const Vec3i X{1, 0, 0}, Z{0, 0, 1};
    if (home) {
        // The floor inside, farthest from the door first; each member of the household
        // starts from their own spot.
        std::vector<Vec3i> spots;
        for (size_t i = 0; i < home->plan_pos.size(); ++i) {
            const Vec3i& p = home->plan_pos[i];
            if (p.y != home->inside.y || vmat(home->plan_vox[i]) != 0) continue;
            if (nav.standable(p)) spots.push_back(p);
        }
        std::sort(spots.begin(), spots.end(), [&](const Vec3i& a, const Vec3i& b) {
            const i64 da = a.dist2(home->entrance), db = b.dist2(home->entrance);
            return da != db ? da > db : a < b;
        });
        auto floor = [&](const Vec3i& p) { return std::find(spots.begin(), spots.end(), p) != spots.end(); };
        if (!spots.empty()) {
            int rank = 0;
            for (const auto& op : chars_)
                if (op && op->alive && !op->departed && op->home == c.home && op->id < c.id) ++rank;
            // A whole bed on the floor, three cubes in a row.
            for (size_t k = 0; k < spots.size(); ++k) {
                const Vec3i& p = spots[((size_t)rank + k) % spots.size()];
                for (const Vec3i& ax : {X, Z})
                    if (floor(p - ax) && floor(p + ax) && free_bed(p, ax)) {
                        axis = ax;
                        return p;
                    }
            }
            for (size_t k = 0; k < spots.size(); ++k) {
                const Vec3i& p = spots[((size_t)rank + k) % spots.size()];
                if (free_bed(p, Vec3i{})) {
                    axis = Vec3i{};
                    return p;
                }
            }
        }
    }
    // Outdoors (or a full house): the nearest free ground around, beds laid out like
    // the spokes of a wheel (feet towards the middle), keeping a cube away from flames,
    // and a little apart from each other while there is room.
    auto by_flames = [&](const Vec3i& p) {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (ctx_.world->material(p + Vec3i{dx, 0, dz}).key == "campfire") return true;
        return false;
    };
    auto good = [&](const Vec3i& p) { return nav.standable(p) && !by_flames(p); };
    for (int pass = 0; pass < 3; ++pass)
        for (int r = 0; r <= (pass < 2 ? 9 : 16); ++r)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                    for (int dy : {0, 1, -1}) {
                        const Vec3i p = near + Vec3i{dx, dy, dz};
                        if (!good(p)) continue;
                        if (pass < 2) {
                            const Vec3i first = std::abs(dx) >= std::abs(dz) ? X : Z, second = first == X ? Z : X;
                            for (const Vec3i& ax : {first, second})
                                if (good(p - ax) && good(p + ax) && free_bed(p, ax, pass == 0 ? 0.45f : 0.0f)) {
                                    axis = ax;
                                    return p;
                                }
                        } else if (free_bed(p, Vec3i{})) {
                            axis = Vec3i{};
                            return p;
                        }
                    }
                }
    axis = Vec3i{};
    return near;
}

bool Agents::task_sleep(Character& c) {
    Task& t = c.task;
    if (t.step == 0) {
        const Building* h = ctx_.buildings->get(c.home);
        // Exhausted people far from home just lie down where they are (the tired walk on:
        // an hour's walk costs little of what is left).
        const bool go_home = h && h->functional && !(c.needs.rest < 0.12f && c.foot.chebyshev(h->inside) > 60);
        Vec3i near = go_home ? h->inside : c.foot;
        // No roof of their own: the band sleeps around its campfire.
        if (!go_home) {
            const Polity* p = ctx_.society->polity(c.polity);
            const Building* seat = p ? ctx_.buildings->get(p->seat) : nullptr;
            if (seat && seat->functional && seat->def == "campfire" && c.foot.chebyshev(seat->inside) < 80 &&
                c.needs.rest > 0.15f)
                near = seat->inside;
        }
        t.target = sleep_spot(c, go_home ? h : nullptr, near, t.target2);
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, "回家睡觉");
        Move m = move_to(c, t.target, false);
        if (m == Move::Moving) return true;
        t.step = 2;
        t.started = now_;
        c.sleeping = true;
        // Lying down on her side along the bed (facing one way or the other).
        if (c.foot == t.target && (t.target2.x || t.target2.z)) {
            c.yaw = t.target2.x ? 0.0f : -1.5707964f;
            if (c.id & 1) c.yaw += 3.1415927f;
        }
    }
    if (t.step == 2) {
        c.sleeping = true;
        const bool at_home = this->at_home(c);
        say(c, at_home ? "在家睡觉" : "露宿");
        if (!at_home && near_campfire(c.foot)) say(c, "在篝火边睡觉");
        c.needs.comfort = clampv(c.needs.comfort + (at_home ? 0.0004f : -0.0003f), 0.0f, 1.0f);
        bool night = is_night(now_);
        bool rested = c.needs.rest >= 0.98f && (!night || now_ - t.started > kTicksPerHour * 8);
        bool urgent = c.needs.food < 0.12f || c.needs.water < 0.12f || danger_at(c) > 0.3f;
        if (rested || urgent) {
            // A night in the open sets people thinking about a roof.
            if (!at_home && now_ - t.started > kTicksPerHour * 4) ctx_.society->practice(c.polity, "sleep_rough", 1.0f, c.id);
            c.last_slept = now_;
            end_task(c, true);
        }
    }
    return true;
}

bool Agents::task_social(Character& c) {
    Task& t = c.task;
    if (t.step == 0) {
        Character* best = nullptr;
        float bd = 24.0f * 24.0f;
        for (auto& op : chars_) {
            if (!op || !op->alive || op->departed || op->id == c.id || op->polity != c.polity || op->sleeping) continue;
            if (op->task.type == TaskType::Work && op->task.step >= 1) continue;
            float d = op->pos.dist_sq(c.pos);
            d *= 1.0f - 0.3f * std::max(0.0f, c.affinity(op->id));
            if (d < bd) {
                bd = d;
                best = op.get();
            }
        }
        if (!best) {
            end_task(c, false);
            c.task.type = TaskType::None;
            return false;
        }
        t.other = best->id;
        t.step = 1;
    }
    Character* o = get(t.other);
    if (!o || !o->alive) {
        end_task(c, false);
        return false;
    }
    if (t.step == 1) {
        say(c, "找" + o->name + "聊天");
        if (c.path.valid() && c.path_goal.chebyshev(o->foot) > 2) c.path.clear();
        Move m = move_to(c, o->foot, true);
        if (m == Move::Failed || now_ - t.started > 600) {
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        t.step = 2;
        t.until = now_ + 100;
    }
    if (t.step == 2) {
        say(c, "与" + o->name + "交谈");
        c.yaw = std::atan2(o->pos.x - c.pos.x, o->pos.z - c.pos.z);
        if (now_ < t.until) return true;
        c.needs.social = std::min(1.0f, c.needs.social + 0.35f);
        o->needs.social = std::min(1.0f, o->needs.social + 0.2f);
        // Affinity follows temperament similarity; aggressive people sometimes quarrel.
        float diff = 0;
        for (int i = 0; i < Personality::kCount; ++i) diff += std::fabs(c.pers.at(i) - o->pers.at(i));
        float delta = 0.08f - diff * 0.02f;
        if (rng_.chance(c.pers.aggression * 0.2f)) delta -= 0.15f;
        c.affinity_ref(o->id) = clampv(c.affinity(o->id) + delta, -1.0f, 1.0f);
        o->affinity_ref(c.id) = clampv(o->affinity(c.id) + delta * 0.8f, -1.0f, 1.0f);
        // Opinions spread: people trade complaints and praise about the rulers.
        for (auto& s : o->support) {
            float& mine = c.support_ref(s.girl);
            float pull = 0.06f * std::max(0.0f, c.affinity(o->id) + 0.3f);
            mine += (s.value - mine) * pull;
        }
        c.last_social = now_;
        end_task(c, true);
    }
    return true;
}

bool Agents::task_wander(Character& c) {
    Task& t = c.task;
    if (t.step == 0 && carried_weight(c) > 0 && nearest_storage(c.polity, c.foot, kNoItem)) {
        // Idle hands first return whatever they still carry.
        t.type = TaskType::Work;
        t.job = 0;
        t.step = 10;
        t.label = "归还随身物资";
        return true;
    }
    const bool child = c.age0 < 14.0f && is_child(c);
    const bool toddler = child && age_years(c) < 3.0f;  // stays by the door, toddling
    if (t.step == 0) {
        Vec3i anchor = c.foot;
        const Building* h = ctx_.buildings->get(c.home);
        if (h && c.foot.chebyshev(h->entrance) > (toddler ? 2 : child ? 10 : 30)) anchor = h->entrance;
        // Children without a roof play by the fire, or near a parent.
        if (child && !h) {
            const Polity* pp = ctx_.society->polity(c.polity);
            const Building* seat = pp ? ctx_.buildings->get(pp->seat) : nullptr;
            if (seat && c.foot.chebyshev(seat->entrance) > 10) anchor = seat->entrance;
        }
        const int reach = toddler ? 2 : 8;
        Vec3i goal{anchor.x + rng_.range(-reach, reach), anchor.y, anchor.z + rng_.range(-reach, reach)};
        Vec3i st;
        if (!ctx_.nav->find_standable_near(goal, st, 3)) {
            t.step = 2;
            t.until = now_ + 60;
            return true;
        }
        t.target = st;
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, toddler ? "蹒跚学步" : child ? "玩耍" : "闲逛");
        Move m = move_to(c, t.target, false);
        if (m == Move::Moving && now_ - t.started < 400) return true;
        t.step = 2;
        t.until = now_ + 60 + rng_.below(120);
    }
    if (t.step == 2) {
        say(c, "发呆");
        if (now_ >= t.until) end_task(c, true);
    }
    return true;
}

bool Agents::task_flee(Character& c) {
    Task& t = c.task;
    if (t.step == 0) {
        Vec3f worst = c.pos;
        float wd = 1e9f;
        for (auto& [p, r] : dangers_) {
            float d = (c.pos - p).length();
            if (d < wd) {
                wd = d;
                worst = p;
            }
        }
        if (Character* foe = nearest_enemy(c, 16.0f, true)) {  // enemy fighters too
            float d = (c.pos - foe->pos).length();
            if (d < wd) {
                wd = d;
                worst = foe->pos;
            }
        }
        if (ctx_.fauna)  // and beasts
            if (const Animal* beast = ctx_.fauna->get(ctx_.fauna->nearest_threat(c.pos, 16.0f))) {
                float d = (c.pos - beast->pos).length();
                if (d < wd) {
                    wd = d;
                    worst = beast->pos;
                }
            }
        Vec3f dir = (c.pos - worst);
        dir.y = 0;
        dir = dir.normalized();
        if (dir.length() < 0.5f) dir = {1, 0, 0};
        Vec3i goal = (c.pos + dir * 18.0f).floor_i();
        Vec3i st;
        if (!ctx_.nav->find_standable_near(goal, st, 4)) {
            end_task(c, false);
            return false;
        }
        t.target = st;
        t.step = 1;
    }
    say(c, "逃离危险！");
    Move m = move_to(c, t.target, false);
    if (m != Move::Moving || danger_at(c) < 0.02f || now_ - t.started > 500) end_task(c, m != Move::Failed);
    return true;
}

bool Agents::task_protest(Character& c) {
    Task& t = c.task;
    const Polity* p = ctx_.society->polity(c.polity);
    const Building* hall = p ? ctx_.buildings->get(p->seat) : nullptr;
    if (!hall) {
        end_task(c, false);
        return false;
    }
    if (t.step == 0) {
        Vec3i goal{hall->entrance.x + rng_.range(-4, 4), hall->entrance.y, hall->entrance.z + rng_.range(1, 5)};
        Vec3i st;
        if (!ctx_.nav->find_standable_near(goal, st, 3)) st = hall->entrance;
        t.target = st;
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, "前往议事厅抗议");
        Move m = move_to(c, t.target, false);
        if (m == Move::Failed) {
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        t.step = 2;
        t.until = now_ + kTicksPerHour * 2;
        c.remember(now_, MemoryKind::Protested, p->ruler, 0.02f, 0);
    }
    if (t.step == 2) {
        say(c, "在议事厅前抗议");
        if (now_ >= t.until || !is_work_time(c)) end_task(c, true);
    }
    return true;
}

bool Agents::task_steal(Character& c) {
    Task& t = c.task;
    if (t.step == 0) {
        StoreId s = find_food_store(c, true, true);
        if (!s) {
            end_task(c, false);
            return false;
        }
        t.store = s;
        t.step = 1;
    }
    const Store* s = ctx_.econ->store(t.store);
    if (!s) {
        end_task(c, false);
        return false;
    }
    say(c, "偷偷溜进仓库");
    Move m = move_to(c, s->pos, true);
    if (m == Move::Failed) {
        end_task(c, false);
        return false;
    }
    if (m != Move::Arrived) return true;
    i32 took = 0;
    std::vector<ItemStack> items = s->items;
    for (auto& st : items) {
        if (!food_item(*ctx_.reg, st.item)) continue;
        took += ctx_.econ->transfer(t.store, c.inv, st.item, std::min(4, st.count));
        if (took >= 4) break;
    }
    if (took > 0) {
        day.thefts++;
        Event e;
        e.type = EventType::Theft;
        e.severity = 2;
        e.pos = c.foot;
        e.actor = c.id;
        e.polity = c.polity;
        e.text = strfmt("%s饥饿难耐，从公共仓库偷取了 %d 份食物", c.name.c_str(), took);
        ctx_.chron->emit(std::move(e));
        c.work_debt += 2.0f;  // counts against them if caught
    }
    end_task(c, took > 0);
    c.next_think = now_;
    return true;
}

bool Agents::task_govern(Character& c) {
    Task& t = c.task;
    const Polity* p = ctx_.society->polity(c.polity);
    const Building* hall = p ? ctx_.buildings->get(p->seat) : nullptr;
    if (!hall) {
        end_task(c, false);
        return false;
    }
    const bool fire = hall->def == "campfire";
    if (t.step == 0) {
        t.target = hall->inside;
        // Not in the flames: a seat on the ring around the fire.
        if (fire) {
            const Vec3i ring[6] = {{2, 0, 0}, {-2, 0, 1}, {1, 0, -2}, {-1, 0, 2}, {2, 0, -1}, {-2, 0, -1}};
            for (int k = 0; k < 6; ++k) {
                const Vec3i p = hall->inside + ring[(c.id + (u32)k) % 6];
                if (ctx_.nav->standable(p)) {
                    t.target = p;
                    break;
                }
            }
        }
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, fire ? "前往篝火" : "前往议事厅");
        Move m = move_to(c, t.target, true);
        if (m == Move::Failed) {
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        t.step = 2;
    }
    say(c, fire ? "在篝火旁议事" : "在议事厅处理政务");
    // Between affairs the girls think over what the people have seen and tried: a
    // trickle of knowledge toward the chosen research.
    if ((now_ + c.id) % kTicksPerHour == 0) ctx_.society->add_research(c.polity, 0.5f, c.id);
    if (!is_work_time(c)) end_task(c, true);
    return true;
}

// ------------------------------------------------------------------------------ work

bool Agents::task_work(Character& c) {
    Task& t = c.task;
    Job* j = ctx_.jobs->get(t.job);
    const Registry& reg = *ctx_.reg;
    World& w = *ctx_.world;
    if (!j && t.step < 10) {
        // Job vanished (completed by the world or cancelled). If carrying things, deliver them.
        if (carried_weight(c) > 0) {
            t.step = 10;
        } else {
            end_task(c, false);
            return false;
        }
    }
    if (j) ctx_.jobs->claim(t.job, c.id, now_ + kTicksPerHour);  // keep the claim alive
    // The tool this work is done with, and how fast it goes with what is in hand.
    const std::string kind = j ? tool_kind_for(*j) : std::string();
    auto work_ticks = [&](float base) {
        float skill = j ? c.skills[job_skill(j->type)] : 0.3f;
        float f = (0.6f + 0.8f * skill) * std::max(0.2f, c.body.manipulation());
        f *= tool_factor(c, kind);
        if (is_elder(c)) f *= 0.75f;  // old hands are slower
        return (Tick)std::max(10.0f, base / f);
    };
    // Before setting out: fetch the right tool from a store not far out of the way.
    auto fetch_tool = [&](u8 resume) {
        const StoreId sid = tool_store_for(c, kind, j->pos);
        if (!sid) return false;
        t.store = sid;
        t.resume = resume;
        t.step = 9;
        return true;
    };
    auto fail = [&](const char* msg) {
        say(c, msg);
        if (j) {
            // Out of reach: this resident leaves the place alone for a while. Anything
            // else (materials short, goods taken) is no reason to shun the place, which
            // is often the store everyone brings things to: the job just rests a little.
            const std::string m = msg;
            if (m.find("到不了") != std::string::npos || m.find("运不过去") != std::string::npos ||
                m.find("取不到") != std::string::npos)
                blacklist(c, j->pos, kTicksPerHour * 2);
            else
                j->suspended_until = std::max(j->suspended_until, now_ + kTicksPerHour / 2);
        }
        end_task(c, false);
        return false;
    };

    // Step 10+: deliver carried goods to storage.
    if (t.step >= 10) {
        if (t.step == 10) {
            ItemId first = kNoItem;
            if (const Store* inv = ctx_.econ->store(c.inv))
                if (!inv->items.empty()) first = inv->items.front().item;
            t.store = nearest_storage(c.polity, c.foot, first);
            if (!t.store) {
                drop_cargo(c);
                end_task(c, true);
                return true;
            }
            t.step = 11;
        }
        const Store* s = ctx_.econ->store(t.store);
        if (!s) {
            t.step = 10;
            return true;
        }
        say(c, "把物资运回仓库");
        Move m = move_to(c, s->pos, true);
        if (m == Move::Failed) {
            // Can't reach storage (e.g. the bridge is gone): leave goods in a pile here.
            drop_cargo(c);
            say(c, "去不了仓库，只好把东西堆在这里");
            end_task(c, true);
            return true;
        }
        if (m != Move::Arrived) return true;
        deposit_all(c, t.store);
        if (j) ctx_.jobs->complete(t.job);
        t.job = 0;
        end_task(c, true);
        return true;
    }

    // Step 9: fetching a tool, then on with the job where it left off.
    if (t.step == 9) {
        const Store* s = ctx_.econ->store(t.store);
        if (!s) {
            t.step = t.resume;
            return true;
        }
        say(c, "去仓库取" + std::string(kind == "axe" ? "斧头" : kind == "pick" ? "镐" : kind == "hoe" ? "锄头" :
                                                 kind == "hammer" ? "锤子" : kind == "sickle" ? "镰刀" : "刀"));
        Move m = move_to(c, s->pos, true);
        if (m == Move::Failed) {
            ctx_.econ->release(t.store, c.id);
            t.step = t.resume;
            return true;
        }
        if (m != Move::Arrived) return true;
        swap_tool(c, t.store, kind);
        t.step = t.resume;
        return true;
    }

    switch (j->type) {
        case JobType::Till:
        case JobType::Sow:
        case JobType::Harvest:
        case JobType::Forage:
        case JobType::Chop:
        case JobType::Mine:
        case JobType::Dig: {
            Farm* farm = j->farm ? ctx_.farming->get(j->farm) : nullptr;
            Plot* plot = (farm && j->plot < farm->plots.size()) ? &farm->plots[j->plot] : nullptr;
            if (t.step == 0) {
                if (j->type == JobType::Sow) {
                    ItemId grain = reg.find_item("grain");
                    const Store* inv = ctx_.econ->store(c.inv);
                    if (inv && inv->count(grain) > 0) {
                        t.step = 2;
                    } else {
                        // Fetch a seed from public storage.
                        StoreId best = kNoStore;
                        float bd = 1e30f;
                        for (StoreId sid : ctx_.society->public_stores(c.polity)) {
                            if (ctx_.econ->available(sid, grain, c.id) <= 0) continue;
                            const Store* s = ctx_.econ->store(sid);
                            float d = (float)s->pos.dist2(j->pos);
                            if (d < bd) {
                                bd = d;
                                best = sid;
                            }
                        }
                        if (!best) return fail("没有种子");
                        ctx_.econ->reserve(best, grain, 8, c.id, now_ + kTicksPerHour);
                        t.store = best;
                        t.step = 1;
                    }
                } else {
                    t.step = 2;
                    if (fetch_tool(2)) return true;
                }
            }
            if (t.step == 1) {
                const Store* s = ctx_.econ->store(t.store);
                if (!s) return fail("种子仓库消失了");
                say(c, "去取种子");
                Move m = move_to(c, s->pos, true);
                if (m == Move::Failed) return fail("取不到种子");
                if (m != Move::Arrived) return true;
                ctx_.econ->transfer(t.store, c.inv, reg.find_item("grain"), 8);
                ctx_.econ->release(t.store, c.id);
                t.step = 2;
            }
            if (t.step == 2) {
                say(c, std::string("前往") + job_name_zh(j->type));
                Move m = move_to(c, j->pos, true);
                if (m == Move::Failed) return fail("到不了工作地点");
                if (m != Move::Arrived) return true;
                float base = 40;
                switch (j->type) {
                    case JobType::Till: base = 55; break;
                    case JobType::Sow: base = 30; break;
                    case JobType::Harvest: base = 45; break;
                    case JobType::Forage: base = 40; break;
                    case JobType::Chop: base = 220; break;
                    default: base = (float)reg.mat(w.mat(j->pos)).hardness; break;
                }
                t.until = now_ + work_ticks(base);
                t.step = 3;
            }
            if (t.step == 3) {
                say(c, job_name_zh(j->type));
                c.yaw = std::atan2((float)j->pos.x + 0.5f - c.pos.x, (float)j->pos.z + 0.5f - c.pos.z);
                if (now_ < t.until) return true;
                bool carrying = false;
                ItemId gathered = kNoItem;
                switch (j->type) {
                    case JobType::Till:
                        if (plot) ctx_.farming->till(*plot, j->cause);
                        break;
                    case JobType::Sow:
                        if (plot && ctx_.econ->remove(c.inv, reg.find_item("grain"), 1, "sown") == 1) {
                            if (!ctx_.farming->sow(*plot, j->cause))
                                ctx_.econ->add(c.inv, reg.find_item("grain"), 1, "unsown");
                        }
                        // Leftover seeds go back later.
                        carrying = carried_weight(c) > 0;
                        break;
                    case JobType::Harvest:
                        if (plot) {
                            int n = ctx_.farming->harvest(*plot, j->cause);
                            if (n > 0) {
                                ctx_.econ->add(c.inv, reg.find_item("grain"), n, "harvest");
                                day.harvested += n;
                            }
                        }
                        carrying = carried_weight(c) > 0;
                        break;
                    case JobType::Forage: {
                        const MatId fm = w.mat(j->pos);
                        const Material& mm = reg.mat(fm);
                        gathered = mm.forage_item;
                        if (mm.forage_item != kNoItem) {
                            w.set(j->pos, make_voxel(mm.forage_to), j->cause);
                            if (ctx_.ecology) ctx_.ecology->picked(j->pos, now_, fm);
                            // Herb gatherers also look for medicinal plants among the bushes.
                            if (j->item != kNoItem && fm == reg.m().berry_bush) ctx_.econ->add(c.inv, j->item, 3, "forage");
                            else ctx_.econ->add(c.inv, mm.forage_item, std::max(1, mm.forage_count), "forage");
                            carrying = true;
                        }
                        break;
                    }
                    case JobType::Chop: {
                        // Fell the whole tree: flood fill connected logs and leaves.
                        std::deque<Vec3i> q{j->pos};
                        std::unordered_set<Vec3i, Vec3iHash> seen{j->pos};
                        int logs = 0, leaves = 0;
                        std::vector<Vec3i> cut;
                        while (!q.empty() && cut.size() < 400) {
                            Vec3i p = q.front();
                            q.pop_front();
                            const Material& mm = reg.mat(w.mat(p));
                            if (!mm.trunk && !mm.foliage) continue;
                            if (ctx_.buildings->at(p)) continue;  // never fell a building
                            cut.push_back(p);
                            if (mm.trunk) ++logs;
                            else ++leaves;
                            for (int d = 0; d < 6; ++d) {
                                Vec3i n = p + kDir6[d];
                                if (n.y < j->pos.y) continue;
                                if (seen.insert(n).second) q.push_back(n);
                            }
                        }
                        for (const Vec3i& p : cut) w.set(p, make_voxel(0), j->cause);
                        ItemId wood = reg.find_item("wood");
                        i32 got = ctx_.econ->add(c.inv, wood, logs, "chop");
                        (void)got;
                        if (leaves >= 6) ctx_.econ->add(c.inv, reg.find_item("fiber"), leaves / 6, "chop");
                        // Too heavy to carry: the rest waits in a pile at the stump.
                        const Store* inv = ctx_.econ->store(c.inv);
                        if (inv && ctx_.econ->weight(*inv) > carry_capacity(c)) {
                            StoreId pile = ctx_.econ->pile_at(c.foot);
                            ctx_.econ->transfer(c.inv, pile, wood, logs / 2);
                        }
                        carrying = true;
                        break;
                    }
                    case JobType::Mine:
                    case JobType::Dig: {
                        const Material& m = reg.mat(w.mat(j->pos));
                        if (m.solid && m.diggable) {
                            w.set(j->pos, make_voxel(0), j->cause);
                            if (m.drop_item_id != kNoItem)
                                ctx_.econ->add(c.inv, m.drop_item_id, std::max(1, m.drop_count), "mined");
                        }
                        carrying = carried_weight(c) > 0 && j->type == JobType::Mine;
                        if (j->type == JobType::Dig && carried_weight(c) > 0) drop_cargo(c);
                        break;
                    }
                    default: break;
                }
                // Tools wear out with use.
                if (!kind.empty() && tool_factor(c, kind) >= 0.99f) wear_tool(c);
                {
                    const char* act = nullptr;
                    switch (j->type) {
                        case JobType::Till: act = "till"; break;
                        case JobType::Sow: act = "sow"; break;
                        case JobType::Harvest: act = "harvest"; break;
                        case JobType::Forage: act = "forage"; break;
                        case JobType::Chop: act = "chop"; break;
                        case JobType::Mine: act = "mine"; break;
                        default: break;
                    }
                    if (act) ctx_.society->practice(c.polity, act, 1.0f, c.id);
                    if (j->type == JobType::Forage && gathered != kNoItem)
                        ctx_.society->practice(c.polity, "forage_" + reg.item(gathered).key, 1.0f, c.id);
                }
                JobType done_type = j->type;
                Vec3i done_pos = j->pos;
                ctx_.jobs->complete(t.job);
                t.job = 0;
                // Chain to the next similar job nearby instead of walking back first.
                bool can_chain = false;
                if (done_type == JobType::Sow) can_chain = ctx_.econ->store(c.inv)->count(reg.find_item("grain")) > 0;
                else if (done_type == JobType::Harvest) can_chain = carried_weight(c) < carry_capacity(c) - 2.0f;
                else if (done_type == JobType::Till) can_chain = true;
                else if (done_type == JobType::Forage) can_chain = carried_weight(c) < carry_capacity(c) - 1.0f;
                // (Diggers leave the spoil in a pile beside the cut and dig on.)
                else if (done_type == JobType::Dig) can_chain = carried_weight(c) < carry_capacity(c) - 2.0f;
                if (can_chain && is_work_time(c) && c.needs.food > 0.25f && c.needs.water > 0.25f) {
                    u32 next = 0;
                    // Gatherers roam on to the next bush in sight before carrying it all home.
                    i64 bd = done_type == JobType::Forage ? 16 * 16 + 1 : 7 * 7 + 1;
                    for (const Job& o : ctx_.jobs->all()) {
                        if (!o.alive || o.type != done_type || o.polity != c.polity || o.claimed_by != kNoEntity) continue;
                        i64 d = o.pos.dist2(done_pos);
                        if (d < bd) {
                            bd = d;
                            next = o.id;
                        }
                    }
                    if (next && ctx_.jobs->claim(next, c.id, now_ + kTicksPerHour)) {
                        t.job = next;
                        t.step = 2;
                        return true;
                    }
                }
                if (carrying || carried_weight(c) > 0) {
                    // Leftovers (seed grain, harvest) go back to storage.
                    t.step = 10;
                    return true;
                }
                end_task(c, true);
                return true;
            }
            return true;
        }
        case JobType::Trade: {
            // A caravan: our goods to the partner's storehouse, their goods back home.
            const u16 partner = (u16)j->project;
            const Polity* home = ctx_.society->polity(c.polity);
            const Polity* other = ctx_.society->polity(partner);
            const bool aid = j->plot == 1 || j->plot == 2;  // 1 aid, 2 tribute: nothing comes back
            if (!home || !other || (!aid && !home->pact_with(partner)) || ctx_.society->at_war(c.polity, partner)) {
                ctx_.econ->release(j->from, c.id);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                if (carried_weight(c) > 0) {
                    t.step = 10;
                    return true;
                }
                end_task(c, false);
                return false;
            }
            if (t.step == 0 && j->from == c.inv) {
                // Going on after a rest on the way: the load is already in her pack.
                const Store* inv = ctx_.econ->store(c.inv);
                t.count = inv ? std::min(j->count, inv->count(j->item)) : 0;
                if (t.count <= 0) {
                    ctx_.jobs->complete(t.job);
                    t.job = 0;
                    end_task(c, false);
                    return false;
                }
                t.step = 2;
            }
            if (t.step == 0) {
                if (!ctx_.econ->store(j->from) || ctx_.econ->available(j->from, j->item, c.id) <= 0) {
                    ctx_.jobs->complete(t.job);
                    t.job = 0;
                    end_task(c, false);
                    return false;
                }
                ctx_.econ->reserve(j->from, j->item, std::min(j->count, ctx_.econ->available(j->from, j->item, c.id)), c.id,
                                   now_ + kTicksPerHour);
                t.step = 1;
            }
            if (t.step == 1) {
                const Store* src = ctx_.econ->store(j->from);
                if (!src) return fail("货源消失了");
                say(c, "为商队备货");
                Move m = move_to(c, src->pos, true);
                if (m == Move::Failed) return fail("到不了仓库");
                if (m != Move::Arrived) return true;
                // Hands free for the load: what else she carries stays in this store.
                if (src->kind == StoreKind::Stockpile) deposit_all(c, j->from);
                float unit = std::max(0.01f, reg.item(j->item).weight);
                i32 cap = (i32)std::floor((carry_capacity(c) - carried_weight(c)) / unit);
                i32 k = ctx_.econ->transfer(j->from, c.inv, j->item, std::min(j->count, cap));
                ctx_.econ->release(j->from, c.id);
                if (k <= 0) return fail("货已经被拿走了");
                if (k < j->count) {
                    // More than she can carry: the rest waits for another carrier.
                    Job rest = *j;
                    rest.count = j->count - k;
                    rest.claimed_by = kNoEntity;
                    rest.claim_expiry = 0;
                    j->count = k;
                    const u32 me = t.job;
                    ctx_.jobs->add(rest);
                    j = ctx_.jobs->get(me);
                }
                t.count = k;
                t.step = 2;
            }
            if (t.step == 2) {
                const Store* d = ctx_.econ->store(j->to);
                if (!d || d->polity != partner) {
                    // Their storehouse is gone: the nearest other one of theirs.
                    j->to = kNoStore;
                    float bd = 1e30f;
                    for (StoreId sid : ctx_.society->public_stores(partner))
                        if (const Store* s = ctx_.econ->store(sid); s && (float)s->pos.dist2(c.foot) < bd) {
                            bd = (float)s->pos.dist2(c.foot);
                            j->to = sid;
                        }
                    d = ctx_.econ->store(j->to);
                    if (!d) {
                        ctx_.jobs->complete(t.job);
                        t.job = 0;
                        t.step = 10;
                        return true;
                    }
                }
                say(c, (j->plot == 2 ? "押送贡粮前往「" : aid ? "押送援粮前往「" : "带着货物前往「") + other->name + "」");
                Move m = move_to(c, d->pos, true);
                if (m == Move::Failed) {
                    // The road there is cut: the caravan turns back with its load.
                    if (!aid) ctx_.society->trade_road_blocked(c.polity, partner);
                    ctx_.jobs->complete(t.job);
                    t.job = 0;
                    t.step = 10;
                    return true;
                }
                if (m != Move::Arrived) return true;
                if (aid) {
                    const i32 given = ctx_.econ->transfer(c.inv, j->to, j->item, t.count);
                    ctx_.society->aid_delivered(c.polity, partner, j->item, given, c.id, j->cause, j->plot == 2);
                    say(c, strfmt("把%s×%d送到了「%s」", reg.item(j->item).name.c_str(), given, other->name.c_str()));
                    ctx_.jobs->complete(t.job);
                    t.job = 0;
                    t.step = 10;
                    return true;
                }
                const TradeDeal deal =
                    ctx_.society->exchange(c.polity, partner, c.inv, j->to, j->item, t.count, carry_capacity(c));
                if (deal.out_n > 0) {
                    say(c, strfmt("用%s×%d换到%s×%d", reg.item(deal.out).name.c_str(), deal.out_n,
                                  reg.item(deal.in).name.c_str(), deal.in_n));
                    c.skills[kHauling] = std::min(1.0f, c.skills[kHauling] + 0.01f);
                } else {
                    say(c, "对方没有可换的东西，原样带回");
                }
                ctx_.jobs->complete(t.job);
                t.job = 0;
                t.step = 10;
            }
            return true;
        }
        case JobType::HaulPile:
        case JobType::HaulToSite: {
            if (t.step == 0) {
                const Store* src = ctx_.econ->store(j->from);
                if (!src || ctx_.econ->available(j->from, j->item, c.id) <= 0) {
                    ctx_.jobs->complete(t.job);
                    t.job = 0;
                    end_task(c, false);
                    return false;
                }
                ctx_.econ->reserve(j->from, j->item, std::min(j->count, ctx_.econ->available(j->from, j->item, c.id)), c.id,
                                   now_ + kTicksPerHour);
                t.step = 1;
            }
            if (t.step == 1) {
                const Store* src = ctx_.econ->store(j->from);
                if (!src) return fail("货源消失了");
                say(c, "去取货");
                Move m = move_to(c, src->pos, true);
                if (m == Move::Failed) return fail("到不了货源");
                if (m != Move::Arrived) return true;
                float unit = std::max(0.01f, reg.item(j->item).weight);
                i32 cap = (i32)std::floor((carry_capacity(c) - carried_weight(c)) / unit);
                i32 k = ctx_.econ->transfer(j->from, c.inv, j->item, std::min(j->count, cap));
                ctx_.econ->release(j->from, c.id);
                if (k <= 0) return fail("货已经被拿走了");
                t.count = k;
                t.step = 2;
            }
            if (t.step == 2) {
                StoreId dest = j->to;
                if (j->type == JobType::HaulPile || !ctx_.econ->store(dest)) {
                    if (!ctx_.econ->store(dest)) dest = nearest_storage(c.polity, c.foot, j->item);
                }
                const Store* d = ctx_.econ->store(dest);
                if (!d) {
                    t.step = 10;
                    return true;
                }
                t.store = dest;
                say(c, j->type == JobType::HaulToSite ? "运送建材" : "搬运入库");
                Move m = move_to(c, d->pos, true);
                if (m == Move::Failed) {
                    drop_cargo(c);
                    return fail("运不过去，只好卸在路边");
                }
                if (m != Move::Arrived) return true;
                // A site receives only what was ordered; anything else goes back later.
                if (j->type == JobType::HaulToSite) ctx_.econ->transfer(c.inv, dest, j->item, 1 << 30);
                else deposit_all(c, dest);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, true);
            }
            return true;
        }
        case JobType::Hunt: {
            // Stalk the animal, strike it down (it flees or fights back; running tires
            // it), butcher the carcass and bring meat and hide home.
            Fauna* fauna = ctx_.fauna;
            Animal* a = fauna ? fauna->get(j->project) : nullptr;
            auto give_up = [&](const std::string& msg) {
                if (a && a->alive && (a->hunted_by & 0x80000000u)) a->hunted_by = kNoEntity;
                if (a && a->alive && a->hunted_by == c.id) a->hunted_by = kNoEntity;
                say(c, msg);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, false);
                return false;
            };
            if (!a || (!a->alive && a->butchered)) return give_up("猎物不见了");
            if (t.step == 0) {
                a->hunted_by = c.id;
                t.until = now_ + kTicksPerHour * 2;  // how long a chase may last
                t.target = a->foot;
                t.step = 1;
                // A spear or a bow from the store on the way, if there is one.
                if (const StoreId sid = hunting_weapon_store(c, a->foot)) {
                    t.store = sid;
                    t.step = 5;
                }
            }
            if (t.step == 5) {
                const Store* s = ctx_.econ->store(t.store);
                if (s) {
                    say(c, "去取猎具");
                    Move m = move_to(c, s->pos, true);
                    if (m == Move::Moving) return true;
                    if (m == Move::Arrived) take_hunting_weapon(c, t.store);
                }
                t.step = 1;
            }
            if (t.step == 1) {
                if (!a->alive) {
                    t.step = 2;
                } else {
                    if (now_ > t.until) return give_up("猎物跑远了，只好放弃");
                    // Not across half the island after it: past a day's walk from home
                    // the chase is given up.
                    if (const Polity* hp = ctx_.society->polity(c.polity))
                        if (const Building* seat = ctx_.buildings->get(hp->seat); seat && c.foot.dist2(seat->entrance) > 100 * 100)
                            return give_up("猎物跑得太远，只好放弃");
                    const SpeciesDef& sp = fauna->spec(a->species);
                    // Mauled by the quarry: back off before it finishes the job.
                    const float hurt = 1.0f - (float)c.body.total_alive() / (float)std::max(1, c.body.total_voxels()) +
                                       0.5f * c.body.bleeding;
                    if (hurt > 0.2f && sp.temper != Temper::Shy) {
                        add_danger(a->pos, 10.0f);
                        return give_up(strfmt("被%s所伤，只好撤退", sp.name.c_str()));
                    }
                    const ItemDef* wd = c.weapon != kNoItem ? &reg.item(c.weapon) : nullptr;
                    const float reach = wd ? std::max(1.5f, wd->range) : 1.4f;
                    const float d = std::sqrt(c.pos.dist_sq(a->pos));
                    say(c, strfmt("追猎%s", sp.name.c_str()));
                    if (d <= reach) {
                        c.yaw = std::atan2(a->pos.x - c.pos.x, a->pos.z - c.pos.z);
                        if (now_ >= (Tick)t.target2.x) {
                            t.target2.x = (i32)(now_ + 24);
                            // Hunting weapons are made for game: their blows count double.
                            float power = wd ? wd->power * 2.0f : 0.04f;
                            power *= (0.7f + 0.6f * c.skills[kCombat]) * std::max(0.3f, c.body.manipulation());
                            if (fauna->strike(*a, power, c.id, j->cause)) {
                                c.skills[kCombat] = std::min(1.0f, c.skills[kCombat] + 0.02f);
                                t.step = 2;
                            }
                        }
                        if (t.step == 1) return true;
                    } else {
                        // Follow; re-plan when the animal has moved on.
                        if (a->foot.dist2(t.target) > 9) {
                            t.target = a->foot;
                            c.path.nodes.clear();
                        }
                        Move m = move_to(c, t.target, true);
                        if (m == Move::Failed) return give_up("追不上猎物");
                        return true;
                    }
                }
            }
            if (t.step == 2) {
                say(c, "去收拾猎物");
                Move m = move_to(c, a->foot, true);
                if (m == Move::Failed) return give_up("够不着猎物");
                if (m != Move::Arrived) return true;
                t.until = now_ + work_ticks(90);
                t.step = 3;
            }
            if (t.step == 3) {
                say(c, "屠宰猎物");
                c.yaw = std::atan2(a->pos.x - c.pos.x, a->pos.z - c.pos.z);
                if (now_ < t.until) return true;
                const SpeciesDef& sp = fauna->spec(a->species);
                const int n = fauna->butcher(*a, c.inv);
                if (tool_factor(c, kind) >= 0.99f) wear_tool(c);
                if (n > 0) {
                    Event e;
                    e.type = EventType::Hunt;
                    e.severity = sp.temper == Temper::Shy ? 1 : 2;
                    e.pos = c.foot;
                    e.actor = c.id;
                    e.polity = c.polity;
                    e.text = strfmt("%s猎获了一%s%s", c.name.c_str(), sp.size.y > 1.1f ? "头" : "只", sp.name.c_str());
                    ctx_.chron->emit(std::move(e));
                    c.remember(now_, MemoryKind::Rewarded, kNoEntity, 0.08f, 0);
                }
                // The carcass may be too heavy to carry at once: the rest waits in a pile.
                const Store* inv = ctx_.econ->store(c.inv);
                if (inv && ctx_.econ->weight(*inv) > carry_capacity(c)) {
                    const StoreId pile = ctx_.econ->pile_at(c.foot);
                    std::vector<ItemStack> items = inv->items;
                    for (const ItemStack& is : items) {
                        if (is.item == c.tool || is.item == c.weapon || is.item == c.clothes || is.item == c.armor)
                            continue;
                        if (ctx_.econ->weight(*ctx_.econ->store(c.inv)) <= carry_capacity(c)) break;
                        ctx_.econ->transfer(c.inv, pile, is.item, is.count / 2 + 1);
                    }
                }
                ctx_.society->practice(c.polity, "hunt", 1.0f, c.id);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                t.step = 10;  // home with it
            }
            return true;
        }
        case JobType::Fish: {
            // Out in a boat (agents_boat.cpp).
            if (j->plot == 1) return boat_fishing(c, *j);
            // From the shore: spear the fish of the school that come near (or, with nets,
            // cast for them), a few if they bite, and bring the catch home.
            Fauna* fauna = ctx_.fauna;
            auto give_up = [&](const std::string& msg) {
                say(c, msg);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, false);
                return false;
            };
            if (!fauna) return give_up("这里没有鱼");
            const Polity* pp = ctx_.society->polity(c.polity);
            const bool net = pp && pp->has_tech("weaving");
            if (t.step == 0) {
                say(c, "去水边捕鱼");
                Move m = move_to(c, j->pos, false);
                if (m == Move::Failed) {
                    blacklist(c, j->pos, kTicksPerHour * 2);
                    return give_up("到不了水边");
                }
                if (m != Move::Arrived) return true;
                t.until = now_ + work_ticks(net ? 80 : 110);
                t.step = 1;
            }
            if (t.step == 1) {
                // Face the water.
                for (int d = 0; d < 4; ++d)
                    for (int ey = -2; ey <= 0; ++ey)
                        if (w.mat(c.foot + kDir4H[d] + Vec3i{0, ey, 0}) == reg.m().water)
                            c.yaw = std::atan2((float)kDir4H[d].x, (float)kDir4H[d].z);
                say(c, net ? "撒网捕鱼" : "在水边叉鱼");
                if (now_ < t.until) return true;
                // A cast pays off the more fish there are about: a ground fished down
                // yields less and less (and the fishers move on to fuller water).
                const int local = fauna->fish_near(c.pos, 10.0f);
                // Nothing in reach: walk along the shore towards the nearest fish (twice at most).
                if (local == 0 && t.count == 0 && t.target2.y < 2) {
                    u32 nearest = fauna->find_fish(c.foot, 26);
                    Vec3i stand;
                    if (const Animal* a = fauna->get(nearest); a && shore_near(a->foot, stand, 8) && stand != c.foot) {
                        ++t.target2.y;
                        j->pos = stand;
                        t.step = 0;
                        say(c, "鱼在那边，换个地方");
                        return true;
                    }
                }
                const float chance = (net ? 0.7f : 0.45f) * (0.75f + 0.5f * c.skills[kFarming]) *
                                     std::min(1.0f, (float)local / 5.0f);
                if (local > 0 && rng_.chance(chance)) {
                    Animal* best = nullptr;
                    float bd = 10.0f * 10.0f;
                    for (const Animal& a : fauna->all()) {
                        if (!a.alive || !fauna->spec(a.species).aquatic || std::abs(a.pos.y - c.pos.y) > 6.0f) continue;
                        const float dx = a.pos.x - c.pos.x, dz = a.pos.z - c.pos.z;
                        if (dx * dx + dz * dz < bd) {
                            bd = dx * dx + dz * dz;
                            best = fauna->get(a.id);
                        }
                    }
                    const std::string kind = best ? fauna->spec(best->species).name : std::string();
                    if (best && fauna->catch_fish(*best, c.id, c.inv) > 0) {
                        Event e;
                        e.type = EventType::Hunt;
                        e.severity = 1;
                        e.pos = c.foot;
                        e.actor = c.id;
                        e.polity = c.polity;
                        e.text = strfmt("%s捕到一条%s", c.name.c_str(), kind.c_str());
                        ctx_.chron->emit(std::move(e));
                        c.skills[kFarming] = std::min(1.0f, c.skills[kFarming] + 0.01f);
                        ctx_.society->practice(c.polity, "hunt", 0.3f, c.id);
                        ++t.count;
                    }
                }
                // Another cast while the fish are there and the basket has room.
                if (local > 0 && t.count < 4 && ++t.target2.x < 6 && carried_weight(c) + 1.2f < carry_capacity(c)) {
                    t.until = now_ + work_ticks(net ? 60 : 80);
                    return true;
                }
                if (t.count == 0) return give_up(local == 0 ? "这片水里没有鱼了" : "鱼都游走了");
                ctx_.jobs->complete(t.job);
                t.job = 0;
                t.step = 10;  // home with the catch
            }
            return true;
        }
        case JobType::Build: {
            Building* b = ctx_.buildings->get(j->building);
            if (!b || b->complete) {
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, false);
                return false;
            }
            int idx = (int)j->plot;
            if (idx < 0 || idx >= (int)b->plan_pos.size()) return fail("施工图有误");
            Voxel want = b->plan_vox[(size_t)idx];
            if (t.step == 0) {
                if (vmat(w.get(b->plan_pos[(size_t)idx])) == vmat(want)) {
                    ctx_.jobs->complete(t.job);
                    t.job = 0;
                    end_task(c, true);
                    return true;
                }
                if (vmat(want) != 0) {
                    ItemId it = item_for_material(reg, vmat(want));
                    if (it != kNoItem && ctx_.econ->available(b->site, it, c.id) <= 0) return fail("工地缺少材料");
                    if (it != kNoItem) ctx_.econ->reserve(b->site, it, 1, c.id, now_ + kTicksPerHour);
                }
                t.step = 1;
                if (fetch_tool(1)) return true;
            }
            if (t.step == 1) {
                say(c, "前往工地");
                // Roofs are laid from a ladder: up to seven cubes above the builder's feet
                // and two to the side.
                Move m = move_to(c, j->pos, true, 7, 2);
                if (m == Move::Failed) return fail("到不了施工位置");
                if (m != Move::Arrived) return true;
                MatId cur = w.mat(j->pos);
                float base = vmat(want) ? 25.0f + (float)reg.mat(vmat(want)).hardness * 0.3f
                                        : (float)reg.mat(cur).hardness;
                t.until = now_ + work_ticks(base);
                t.step = 2;
            }
            if (t.step == 2) {
                say(c, "施工");
                c.yaw = std::atan2((float)j->pos.x + 0.5f - c.pos.x, (float)j->pos.z + 0.5f - c.pos.z);
                if (now_ < t.until) return true;
                MatId cur = w.mat(j->pos);
                const Material& cm = reg.mat(cur);
                ctx_.econ->release(b->site, c.id);
                if (cm.solid && cur != vmat(want)) {
                    // Clear the spot first; spoil goes to the site pile.
                    w.set(j->pos, make_voxel(0), j->cause);
                    if (cm.drop_item_id != kNoItem) {
                        StoreId pile = ctx_.econ->pile_at(c.foot);
                        ctx_.econ->add(pile, cm.drop_item_id, std::max(1, cm.drop_count), "mined");
                    }
                    if (vmat(want) == 0) {
                        ctx_.jobs->complete(t.job);
                        t.job = 0;
                        end_task(c, true);
                        return true;
                    }
                    t.until = now_ + 20;
                    return true;
                }
                if (!ctx_.buildings->place_cell(*b, idx, j->cause)) return fail("材料不足，无法施工");
                if (tool_factor(c, kind) >= 0.99f) wear_tool(c);
                ctx_.society->practice(c.polity, "build", 1.0f, c.id);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                if (ctx_.buildings->site_done(*b)) {
                    ctx_.buildings->finish(*b, j->cause);
                    if (b->project) ctx_.society->finish_project(b->project, true, b->last_event);
                }
                end_task(c, true);
            }
            return true;
        }
        case JobType::Cook: {
            Building* k = ctx_.buildings->get(j->building);
            if (!k || !k->functional || !k->store) return fail("灶房不能用了");
            // What goes in (grain or meat) and what comes out (bread or roast).
            const ItemId grain = j->item != kNoItem ? j->item : reg.find_item("grain");
            const bool fish = reg.item(grain).key == "fish";
            const bool roasting = reg.item(grain).key == "meat" || fish;
            const ItemId bread = reg.find_item(fish ? "cooked_fish" : (roasting ? "cooked_meat" : "bread"));
            if (t.step == 0) {
                // Bring grain to the kitchen if it has none.
                if (ctx_.econ->available(k->store, grain, c.id) >= 2) {
                    t.step = 2;
                } else {
                    StoreId src = kNoStore;
                    float bd = 1e30f;
                    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
                        const Store* s = ctx_.econ->store(sid);
                        if (!s || sid == k->store || ctx_.econ->available(sid, grain, c.id) < (roasting ? 2 : 4)) continue;
                        float d = (float)s->pos.dist2(k->inside);
                        if (d < bd) {
                            bd = d;
                            src = sid;
                        }
                    }
                    if (!src) return fail(fish ? "没有鱼可烤" : (roasting ? "没有肉可烤" : "没有谷物可烹饪"));
                    ctx_.econ->reserve(src, grain, 6, c.id, now_ + kTicksPerHour);
                    t.store = src;
                    t.step = 1;
                }
            }
            if (t.step == 1) {
                const Store* s = ctx_.econ->store(t.store);
                if (!s) return fail("仓库不见了");
                say(c, fish ? "去取鱼" : (roasting ? "去取肉" : "去取谷物"));
                Move m = move_to(c, s->pos, true);
                if (m == Move::Failed) return fail(fish ? "取不到鱼" : (roasting ? "取不到肉" : "取不到谷物"));
                if (m != Move::Arrived) return true;
                ctx_.econ->transfer(t.store, c.inv, grain, 6);
                ctx_.econ->release(t.store, c.id);
                t.step = 2;
            }
            if (t.step == 2) {
                say(c, k->def == "campfire" ? "去篝火边" : "去灶房");
                Move m = move_to(c, k->inside, true);
                if (m == Move::Failed) return fail("到不了灶房");
                if (m != Move::Arrived) return true;
                ctx_.econ->transfer(c.inv, k->store, grain, 99);
                t.until = now_ + work_ticks(150);
                t.step = 3;
            }
            if (t.step == 3) {
                say(c, fish ? "烤鱼" : (roasting ? "烤肉" : "烹饪面包"));
                if (now_ < t.until) return true;
                i32 n = ctx_.econ->remove(k->store, grain, 4, "cooked");
                if (n > 0) ctx_.econ->add(k->store, bread, n, "cooked");
                if (n > 0) ctx_.society->practice(c.polity, "cook", 1.0f, c.id);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, true);
            }
            return true;
        }
        case JobType::Research: {
            Building* b = ctx_.buildings->get(j->building);
            if (!b || !b->functional) return fail("研究的地方不能用了");
            if (t.step == 0) {
                say(c, "去研究");
                Move m = move_to(c, b->inside, true);
                if (m == Move::Failed) return fail("到不了研究的地方");
                if (m != Move::Arrived) return true;
                t.until = now_ + work_ticks(300);
                t.step = 1;
            }
            if (t.step == 1) {
                const Polity* p = ctx_.society->polity(c.polity);
                say(c, p && !p->policies.research.empty() ? "钻研新知" : "整理见闻");
                if (now_ < t.until) return true;
                // Scholars at a research building work fastest; musing at the fire or
                // the hall only advances the techs of the wild era.
                const BuildingDef* bd = ctx_.buildings->def(b->def);
                const bool scholarly = bd && bd->scholars > 0 && c.occupation == "research";
                const float pts = (scholarly ? 1.0f * bd->research_rate : 1.2f) * (0.6f + c.skills[kResearch]);
                ctx_.society->add_research(c.polity, pts, c.id, scholarly);
                c.skills[kResearch] = std::min(1.0f, c.skills[kResearch] + 0.005f);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, true);
            }
            return true;
        }
        case JobType::Craft: {
            // Work the recipe at the store that holds the inputs; outputs go back in.
            const Json& recipes = reg.doc("recipes")["recipes"];
            if (j->plot >= recipes.size()) return fail("不知道怎么做");
            const Json& r = recipes[(size_t)j->plot];
            const Store* s = ctx_.econ->store(j->from);
            if (!s) return fail("材料仓库不见了");
            auto have_inputs = [&](int batches) {
                for (const auto& [k, v] : r["inputs"].members())
                    if (ctx_.econ->available(j->from, reg.find_item(k), c.id) < v.as_int() * batches) return false;
                return true;
            };
            if (t.step == 0) {
                if (!have_inputs(1)) return fail("材料不够");
                for (const auto& [k, v] : r["inputs"].members())
                    ctx_.econ->reserve(j->from, reg.find_item(k), v.as_int() * std::max(1, j->count), c.id, now_ + kTicksPerHour * 2);
                t.step = 1;
            }
            if (t.step == 1) {
                say(c, "去工坊：" + r.str("name"));
                Move m = move_to(c, s->pos, true);
                if (m == Move::Failed) return fail("到不了材料仓库");
                if (m != Move::Arrived) return true;
                // The store may hold the tool for this work.
                if (tool_factor(c, kind) < 0.99f) swap_tool(c, j->from, kind);
                // A working workshop nearby makes the job quicker.
                float speed = 1.0f;
                for (const Building& b : ctx_.buildings->all())
                    if (b.alive && b.functional && b.def == r.str("station") && b.polity == c.polity &&
                        b.entrance.dist2(c.foot) < 16 * 16)
                        speed = 0.6f;
                speed /= 1.0f + ctx_.society->tech_effect(c.polity, "craft_speed");
                t.until = now_ + work_ticks(r.flt("ticks", 100.0f) * (float)std::max(1, j->count) * speed);
                t.step = 2;
            }
            if (t.step == 2) {
                say(c, r.str("name"));
                if (now_ < t.until) return true;
                int done = 0;
                const std::string reason = "craft:" + r.str("key");
                for (int b = 0; b < std::max(1, j->count) && have_inputs(1); ++b) {
                    for (const auto& [k, v] : r["inputs"].members())
                        ctx_.econ->remove(j->from, reg.find_item(k), v.as_int(), reason);
                    for (const auto& [k, v] : r["outputs"].members())
                        ctx_.econ->add(j->from, reg.find_item(k), v.as_int(), reason);
                    ++done;
                }
                ctx_.econ->release(j->from, c.id);
                c.skills[kCrafting] = std::min(1.0f, c.skills[kCrafting] + 0.01f * (float)done);
                if (done > 0 && tool_factor(c, kind) >= 0.99f) wear_tool(c);
                if (done > 0) ctx_.society->practice(c.polity, "craft", (float)done, c.id);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                end_task(c, done > 0);
            }
            return true;
        }
        default:
            ctx_.jobs->complete(t.job);
            t.job = 0;
            end_task(c, false);
            return false;
    }
}

}  // namespace icarus
