// Agents: getting out of places with no way out. Someone who fell into a ravine (or was
// walled in) and cannot reach food or water plans a staircase: walk to a wall, then cut
// steps up at 45 degrees until reaching ground that belongs to a settlement's walkable
// region. The cubes are really removed; the rubble is left in piles on the steps.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/sim/clock.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
constexpr int kMaxWalk = 40;
constexpr int kMaxSteps = 70;
const Vec3i kDirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
}  // namespace

bool Agents::trapped(const Character& c) const {
    // Outside every settlement's walkable region (as of the last survey) and here now,
    // and unable to get anywhere (being far out in open country is not being trapped).
    if (c.region != 0 || region_map_.empty() || region_map_.count(c.foot)) return false;
    return (c.needs.food < 0.8f || c.needs.water < 0.8f) && c.unreachable.size() >= 3;
}

bool Agents::task_escape(Character& c) {
    Task& t = c.task;
    World& w = *ctx_.world;
    Nav& nav = *ctx_.nav;
    const Registry& reg = *ctx_.reg;
    auto solid = [&](const Vec3i& q) {
        const Material& m = reg.mat(w.mat(q));
        return m.solid && !m.passable;
    };
    auto arrived = [&](const Vec3i& q) {
        return region_map_.count(q) || region_map_.count(q + Vec3i{0, 1, 0}) || region_map_.count(q + Vec3i{0, -1, 0});
    };
    // Cost of escaping in one direction (walk + cubes to cut), or -1.
    auto plan = [&](const Vec3i& dir) -> int {
        Vec3i p = c.foot;
        int cost = 0;
        for (int k = 0; k < kMaxWalk && nav.standable(p + dir) && !solid(p + dir); ++k) {
            p = p + dir;
            ++cost;
            if (arrived(p)) return cost;
        }
        for (int k = 0; k < kMaxSteps; ++k) {
            Vec3i ahead = p + dir;
            if (!w.in_bounds(ahead + Vec3i{0, 3, 0})) return -1;
            if (!solid(ahead)) {
                // Open ahead: either flat ground on top (done or walk on) or a drop.
                if (nav.standable(ahead)) {
                    p = ahead;
                    ++cost;
                    if (arrived(p)) return cost;
                    continue;
                }
                return -1;
            }
            Vec3i up = ahead + Vec3i{0, 1, 0};
            for (const Vec3i& q : {up, up + Vec3i{0, 1, 0}, up + Vec3i{0, 2, 0}, p + Vec3i{0, 3, 0}}) {
                if (!solid(q)) continue;
                if (!reg.mat(w.mat(q)).diggable) return -1;
                cost += 3;
            }
            p = up;
            ++cost;
            if (arrived(p)) return cost;
        }
        return -1;
    };

    if (t.step == 0) {
        int best = -1;
        Vec3i bdir{0, 0, 0};
        for (const Vec3i& d : kDirs) {
            int cost = plan(d);
            if (cost >= 0 && (best < 0 || cost < best)) {
                best = cost;
                bdir = d;
            }
        }
        if (best < 0) {
            say(c, "四面都是绝壁，无路可走");
            blacklist(c, c.foot, kTicksPerHour);
            end_task(c, false);
            return false;
        }
        t.target = bdir;  // committed direction
        t.count = 0;
        t.step = 1;
        say(c, "被困住了，决定凿出一条路");
    }
    const Vec3i dir = t.target;
    if (t.step == 1) {
        if (arrived(c.foot)) {
            c.unreachable.clear();
            end_task(c, true);
            return true;
        }
        if (++t.count > kMaxWalk + kMaxSteps * 5) {
            end_task(c, false);
            return false;
        }
        const Vec3i ahead = c.foot + dir;
        auto step_to = [&](const Vec3i& q) {
            place_at(c, q);
            c.yaw = std::atan2((float)dir.x, (float)dir.z);
            t.until = now_ + 12;
            t.step = 3;
        };
        if (!solid(ahead)) {
            if (nav.standable(ahead)) {
                step_to(ahead);
                return true;
            }
            t.step = 0;  // the way changed (a drop): plan again
            return true;
        }
        const Vec3i up = ahead + Vec3i{0, 1, 0};
        for (const Vec3i& q : {up, up + Vec3i{0, 1, 0}, up + Vec3i{0, 2, 0}, c.foot + Vec3i{0, 3, 0}}) {
            if (!solid(q)) continue;
            const Material& m = reg.mat(w.mat(q));
            if (!m.diggable) {
                t.step = 0;
                return true;
            }
            c.path.clear();
            t.target2 = q;
            t.until = now_ + std::max<Tick>(15, (Tick)((float)m.hardness * 0.7f / (0.6f + 0.8f * c.skills[kMining])));
            t.step = 2;
            c.yaw = std::atan2((float)dir.x, (float)dir.z);
            say(c, "在岩壁上凿出台阶");
            return true;
        }
        if (nav.standable(up)) {
            step_to(up);
            return true;
        }
        t.step = 0;
        return true;
    }
    if (t.step == 2) {
        if (now_ < t.until) return true;
        const Vec3i q = t.target2;
        const Material& m = reg.mat(w.mat(q));
        if (m.solid && m.diggable) {
            w.set(q, make_voxel(0), 0);
            if (m.drop_item_id != kNoItem) {
                StoreId pile = ctx_.econ->pile_at(c.foot);
                ctx_.econ->add(pile, m.drop_item_id, std::max(1, m.drop_count), "dug");
            }
        }
        c.skills[kMining] = std::min(1.0f, c.skills[kMining] + 0.004f);
        t.step = 1;
        return true;
    }
    if (t.step == 3) {
        if (now_ >= t.until) t.step = 1;
        return true;
    }
    return true;
}

}  // namespace icarus
