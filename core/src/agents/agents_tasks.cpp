// Agents: task execution state machines.
#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "icarus/agents/agents.h"
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
    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
        const Store* s = ctx_.econ->store(sid);
        if (!s) continue;
        bool has = false;
        for (auto& st : s->items)
            if (food_item(*ctx_.reg, st.item) && ctx_.econ->available(sid, st.item, c.id) > 0) has = true;
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
    const int R = 110;
    std::unordered_set<Vec3i, Vec3iHash> seen;
    for (const Vec3i& c : centers) {
        for (int dz = -R; dz <= R; dz += 2)
            for (int dx = -R; dx <= R; dx += 2) {
                if (dx * dx + dz * dz > R * R) continue;
                int x = c.x + dx, z = c.z + dz;
                ColumnInfo col = w.gen().column(x, z);
                if (!col.land) continue;
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
    // Regions: flood from settlement anchors (seats, farms). Characters elsewhere get
    // region 0 (unknown) and fall back to plain path searches.
    std::vector<Vec3i> anchors;
    for (const Vec3i& c : centers) anchors.push_back(c);
    for (const Farm& f : ctx_.farming->all())
        if (f.alive) anchors.push_back(f.center + Vec3i{0, 1, 0});
    bool stale = anchors != region_anchors_ || nav.major_dirty ||
                 (nav.minor_dirty && now_ >= region_built_ + kTicksPerDay);
    if (stale) {
        region_anchors_ = anchors;
        region_built_ = now_;
        nav.major_dirty = nav.minor_dirty = false;
        region_map_.clear();
        region_map_.reserve(1 << 16);
        u16 next = 1;
        for (const Vec3i& a : anchors) {
            Vec3i st = a;
            if (!nav.standable(st) && !nav.find_standable_near(a, st, 3)) continue;
            if (region_map_.count(st)) continue;
            if (nav.flood(st, R + 30, 90000, region_map_, next) > 0 && next < 65535) ++next;
        }
    }
    const auto& label = region_map_;
    for (const Vec3i& s : water_spots_) {
        auto it = label.find(s);
        water_regions_.push_back(it == label.end() ? 0 : it->second);
    }
    for (size_t i = 1; i < chars_.size(); ++i) {
        Character* c = chars_[i].get();
        if (!c || !c->alive || c->departed) continue;
        auto it = label.find(c->foot);
        c->region = it == label.end() ? 0 : it->second;
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
                if (vmat(v) == WATER && vlevel(v) >= 2) {
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
    return false;
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
        if (home && there && there != home) continue;
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
    auto carried_food = [&]() {
        const Store* s = ctx_.econ->store(c.inv);
        if (!s) return false;
        for (auto& st : s->items)
            if (food_item(reg, st.item)) return true;
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
        if (s == kNoStore) {
            day.hungry_no_food++;
            say(c, "找不到可以吃的东西");
            end_task(c, false);
            return false;
        }
        t.store = s;
        t.step = 1;
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
            if (carried_weight(c) > carry_capacity(c) - 2.0f) ctx_.econ->drop(c.inv, c.foot);
            s = ctx_.econ->store(t.store);
            if (!s) {
                end_task(c, false);
                return false;
            }
        }
        std::vector<ItemStack> avail = s->items;
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
            while (c.needs.food < target - 0.02f && ctx_.econ->store(c.inv)->count(st.item) > 0) {
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
        if (!find_water(c, stand, water)) {
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
        // Drinking removes water from the world.
        Vec3i found{0, -1, 0};
        for (int dy = -2; dy <= 0 && found.y < 0; ++dy)
            for (int d = 0; d < 5 && found.y < 0; ++d) {
                Vec3i q = c.foot + (d < 4 ? kDir4H[d] : Vec3i{0, 0, 0}) + Vec3i{0, dy, 0};
                Voxel v = w.get(q);
                if (vmat(v) == WATER && vlevel(v) >= 1) found = q;
            }
        if (found.y < 0) {
            c.water_spot = {-1, -1, -1};
            end_task(c, false);
            return false;
        }
        Voxel v = w.get(found);
        int l = vlevel(v) - 1;
        w.set(found, l > 0 ? make_voxel(WATER, (u8)l) : make_voxel(0));
        c.needs.water = std::min(1.0f, c.needs.water + 0.45f);
        day.drinks++;
        c.last_drank = now_;
        if (c.needs.water < 0.85f && ++t.count < 4) {
            t.until = now_ + 30;
            return true;
        }
        end_task(c, true);
    }
    return true;
}

Vec3i Agents::sleep_spot(const Character& c, const Building* home, const Vec3i& near) {
    // Cubes taken by other sleepers (where they lie or are going to lie).
    std::vector<Vec3i> taken;
    for (const auto& op : chars_)
        if (op && op->alive && !op->departed && op->id != c.id && op->task.type == TaskType::Sleep)
            taken.push_back(op->task.step >= 2 ? op->foot : op->task.target);
    auto free_at = [&](const Vec3i& p) { return std::find(taken.begin(), taken.end(), p) == taken.end(); };
    Nav& nav = *ctx_.nav;
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
        if (!spots.empty()) {
            int rank = 0;
            for (const auto& op : chars_)
                if (op && op->alive && !op->departed && op->home == c.home && op->id < c.id) ++rank;
            for (size_t k = 0; k < spots.size(); ++k) {
                const Vec3i& p = spots[((size_t)rank + k) % spots.size()];
                if (free_at(p)) return p;
            }
        }
    }
    // Outdoors (or a full house): the nearest free standable cube around.
    for (int r = 0; r <= 3; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                for (int dy : {0, 1, -1}) {
                    const Vec3i p = near + Vec3i{dx, dy, dz};
                    if (free_at(p) && nav.standable(p)) return p;
                }
            }
    return near;
}

bool Agents::task_sleep(Character& c) {
    Task& t = c.task;
    if (t.step == 0) {
        const Building* h = ctx_.buildings->get(c.home);
        // Exhausted people far from home just lie down where they are.
        const bool go_home = h && h->functional && !(c.needs.rest < 0.3f && c.foot.chebyshev(h->inside) > 45);
        t.target = sleep_spot(c, go_home ? h : nullptr, go_home ? h->inside : c.foot);
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, "回家睡觉");
        Move m = move_to(c, t.target, false);
        if (m == Move::Moving) return true;
        t.step = 2;
        t.started = now_;
        c.sleeping = true;
    }
    if (t.step == 2) {
        c.sleeping = true;
        const Building* h = ctx_.buildings->get(c.home);
        bool at_home = h && h->functional && c.foot.chebyshev(h->inside) <= 3;
        say(c, at_home ? "在家睡觉" : "露宿");
        c.needs.comfort = clampv(c.needs.comfort + (at_home ? 0.0004f : -0.0003f), 0.0f, 1.0f);
        bool night = is_night(now_);
        bool rested = c.needs.rest >= 0.98f && (!night || now_ - t.started > kTicksPerHour * 8);
        bool urgent = c.needs.food < 0.12f || c.needs.water < 0.12f || danger_at(c) > 0.3f;
        if (rested || urgent) {
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
    if (t.step == 0) {
        Vec3i anchor = c.foot;
        const Building* h = ctx_.buildings->get(c.home);
        if (h && c.foot.chebyshev(h->entrance) > 30) anchor = h->entrance;
        Vec3i goal{anchor.x + rng_.range(-8, 8), anchor.y, anchor.z + rng_.range(-8, 8)};
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
        say(c, "闲逛");
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
    if (t.step == 0) {
        t.target = hall->inside;
        t.step = 1;
    }
    if (t.step == 1) {
        say(c, "前往议事厅");
        Move m = move_to(c, t.target, true);
        if (m == Move::Failed) {
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) return true;
        t.step = 2;
    }
    say(c, "在议事厅处理政务");
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
        if (j) blacklist(c, j->pos, kTicksPerHour * 2);
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
                ctx_.econ->drop(c.inv, c.foot);
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
            ctx_.econ->drop(c.inv, c.foot);
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
                        if (mm.forage_item != kNoItem) {
                            w.set(j->pos, make_voxel(mm.forage_to), j->cause);
                            if (reg.mat(mm.forage_to).foliage && ctx_.ecology) ctx_.ecology->picked(j->pos, now_);
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
                        if (j->type == JobType::Dig && carried_weight(c) > 0) ctx_.econ->drop(c.inv, c.foot);
                        break;
                    }
                    default: break;
                }
                // Tools wear out with use.
                if (!kind.empty() && tool_factor(c, kind) >= 0.99f) wear_tool(c);
                JobType done_type = j->type;
                Vec3i done_pos = j->pos;
                ctx_.jobs->complete(t.job);
                t.job = 0;
                // Chain to the next similar job nearby instead of walking back first.
                bool can_chain = false;
                if (done_type == JobType::Sow) can_chain = ctx_.econ->store(c.inv)->count(reg.find_item("grain")) > 0;
                else if (done_type == JobType::Harvest) can_chain = carried_weight(c) < carry_capacity(c) - 2.0f;
                else if (done_type == JobType::Till) can_chain = true;
                if (can_chain && is_work_time(c) && c.needs.food > 0.25f && c.needs.water > 0.25f) {
                    u32 next = 0;
                    i64 bd = 7 * 7 + 1;
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
            const bool aid = j->plot == 1;
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
                float unit = std::max(0.01f, reg.item(j->item).weight);
                i32 cap = (i32)std::floor((carry_capacity(c) - carried_weight(c)) / unit);
                i32 k = ctx_.econ->transfer(j->from, c.inv, j->item, std::min(j->count, cap));
                ctx_.econ->release(j->from, c.id);
                if (k <= 0) return fail("货已经被拿走了");
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
                say(c, (aid ? "押送援粮前往「" : "带着货物前往「") + other->name + "」");
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
                    ctx_.society->aid_delivered(c.polity, partner, j->item, given, c.id, j->cause);
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
                    ctx_.econ->drop(c.inv, c.foot);
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
            auto give_up = [&](const char* msg) {
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
                    const SpeciesDef& sp = fauna->spec(a->species);
                    const ItemDef* wd = c.weapon != kNoItem ? &reg.item(c.weapon) : nullptr;
                    const float reach = wd ? std::max(1.5f, wd->range) : 1.4f;
                    const float d = std::sqrt(c.pos.dist_sq(a->pos));
                    say(c, strfmt("追猎%s", sp.name.c_str()));
                    if (d <= reach) {
                        c.yaw = std::atan2(a->pos.x - c.pos.x, a->pos.z - c.pos.z);
                        if (now_ >= t.target2.x) {
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
                ctx_.jobs->complete(t.job);
                t.job = 0;
                t.step = 10;  // home with it
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
                Move m = move_to(c, j->pos, true);
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
            ItemId grain = reg.find_item("grain"), bread = reg.find_item("bread");
            if (t.step == 0) {
                // Bring grain to the kitchen if it has none.
                if (ctx_.econ->available(k->store, grain, c.id) >= 2) {
                    t.step = 2;
                } else {
                    StoreId src = kNoStore;
                    float bd = 1e30f;
                    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
                        const Store* s = ctx_.econ->store(sid);
                        if (!s || s->kind != StoreKind::Stockpile || ctx_.econ->available(sid, grain, c.id) < 4) continue;
                        float d = (float)s->pos.dist2(k->inside);
                        if (d < bd) {
                            bd = d;
                            src = sid;
                        }
                    }
                    if (!src) return fail("没有谷物可烹饪");
                    ctx_.econ->reserve(src, grain, 6, c.id, now_ + kTicksPerHour);
                    t.store = src;
                    t.step = 1;
                }
            }
            if (t.step == 1) {
                const Store* s = ctx_.econ->store(t.store);
                if (!s) return fail("仓库不见了");
                say(c, "去取谷物");
                Move m = move_to(c, s->pos, true);
                if (m == Move::Failed) return fail("取不到谷物");
                if (m != Move::Arrived) return true;
                ctx_.econ->transfer(t.store, c.inv, grain, 6);
                ctx_.econ->release(t.store, c.id);
                t.step = 2;
            }
            if (t.step == 2) {
                say(c, "去灶房");
                Move m = move_to(c, k->inside, true);
                if (m == Move::Failed) return fail("到不了灶房");
                if (m != Move::Arrived) return true;
                ctx_.econ->transfer(c.inv, k->store, grain, 99);
                t.until = now_ + work_ticks(150);
                t.step = 3;
            }
            if (t.step == 3) {
                say(c, "烹饪面包");
                if (now_ < t.until) return true;
                i32 n = ctx_.econ->remove(k->store, grain, 4, "cooked");
                if (n > 0) ctx_.econ->add(k->store, bread, n, "cooked");
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
                float pts = 3.0f * (0.6f + c.skills[kResearch]) * (b->def == "study" ? 1.5f : 1.0f);
                ctx_.society->add_research(c.polity, pts, c.id);
                c.skills[kResearch] = std::min(1.0f, c.skills[kResearch] + 0.01f);
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
