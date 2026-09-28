// Boats (木船): a fisher takes a boat from the store, carries it to the shore, rows out over
// the water to a school beyond reach of the shore, casts her net, rows back and brings the
// catch and the boat home. Afloat she needs no ground under her feet; the boat keeps to
// open water (a route around headlands when the straight way is blocked).
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/agents/nav.h"
#include "icarus/fauna/fauna.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
constexpr float kRowSpeed = 0.2f;  // cubes a tick

// Open water at a column: a full water cube at the surface level with air above (not
// under ice, a bridge or a roof).
bool open_water(const World& w, int x, int z, int level) {
    const CoreMats& M = w.reg().m();
    const Voxel v = w.peek({x, level, z});
    if (vmat(v) != M.water || vlevel(v) < 8) return false;
    return vmat(w.peek({x, level + 1, z})) == M.air;
}
}  // namespace

StoreId Agents::boat_store(const Character& c) const {
    const ItemId boat = ctx_.reg->find_item("boat");
    if (boat == kNoItem) return kNoStore;
    StoreId best = kNoStore;
    i64 bd = 1LL << 60;
    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
        if (ctx_.econ->available(sid, boat, c.id) <= 0) continue;
        const Store* s = ctx_.econ->store(sid);
        const i64 d = c.foot.dist2(s->pos);
        if (d < bd) {
            bd = d;
            best = sid;
        }
    }
    return best;
}

bool Agents::water_route(const Vec3i& from, const Vec3i& to, int level, std::vector<Vec3i>& out) const {
    const World& w = *ctx_.world;
    out.clear();
    auto clear_line = [&](const Vec3i& a, const Vec3i& b) {
        const float dx = (float)(b.x - a.x), dz = (float)(b.z - a.z);
        const int n = std::max(1, (int)std::ceil(std::sqrt(dx * dx + dz * dz) * 2.0f));
        for (int i = 0; i <= n; ++i) {
            const float k = (float)i / (float)n;
            const int x = (int)std::floor((float)a.x + 0.5f + dx * k), z = (int)std::floor((float)a.z + 0.5f + dz * k);
            if (!open_water(w, x, z, level)) return false;
        }
        return true;
    };
    if (clear_line(from, to)) {
        out.push_back(to);
        return true;
    }
    // Round the headlands: a search over the water on a grid of four-cube squares.
    constexpr int S = 4, kMargin = 96;
    const int x0 = std::min(from.x, to.x) - kMargin, z0 = std::min(from.z, to.z) - kMargin;
    const int x1 = std::max(from.x, to.x) + kMargin, z1 = std::max(from.z, to.z) + kMargin;
    const int gw = (x1 - x0) / S + 1, gd = (z1 - z0) / S + 1;
    if ((i64)gw * gd > 90000) return false;
    auto cx = [&](int i) { return x0 + i * S + S / 2; };
    auto cz = [&](int j) { return z0 + j * S + S / 2; };
    const int si = clampv((from.x - x0) / S, 0, gw - 1), sj = clampv((from.z - z0) / S, 0, gd - 1);
    const int ti = clampv((to.x - x0) / S, 0, gw - 1), tj = clampv((to.z - z0) / S, 0, gd - 1);
    std::vector<int> prev((size_t)gw * gd, -2);
    std::vector<u8> ok((size_t)gw * gd, 2);  // 2 unknown
    auto node_ok = [&](int i, int j) {
        u8& o = ok[(size_t)(j * gw + i)];
        if (o == 2) o = open_water(w, cx(i), cz(j), level) ? 1 : 0;
        return o == 1;
    };
    std::vector<int> q{sj * gw + si};
    prev[(size_t)(sj * gw + si)] = -1;
    bool found = false;
    static const int d8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (size_t h = 0; h < q.size() && !found; ++h) {
        const int i = q[h] % gw, j = q[h] / gw;
        for (auto& d : d8) {
            const int a = i + d[0], b = j + d[1];
            if (a < 0 || b < 0 || a >= gw || b >= gd || prev[(size_t)(b * gw + a)] != -2) continue;
            if (!node_ok(a, b) && !(a == ti && b == tj)) continue;
            if (d[0] && d[1] && (!node_ok(i + d[0], j) || !node_ok(i, j + d[1]))) continue;  // no corner cutting
            prev[(size_t)(b * gw + a)] = q[h];
            if (a == ti && b == tj) {
                found = true;
                break;
            }
            q.push_back(b * gw + a);
        }
    }
    if (!found) return false;
    std::vector<Vec3i> nodes;
    for (int n = tj * gw + ti; n >= 0; n = prev[(size_t)n]) nodes.push_back(Vec3i{cx(n % gw), level, cz(n / gw)});
    std::reverse(nodes.begin(), nodes.end());
    nodes.back() = to;
    // Straighten: from each point on, the farthest waypoint in plain sight.
    Vec3i at = from;
    size_t k = 0;
    while (k < nodes.size()) {
        size_t far = k;
        for (size_t m = nodes.size(); m-- > k + 1;)
            if (clear_line(at, nodes[m])) {
                far = m;
                break;
            }
        out.push_back(nodes[far]);
        at = nodes[far];
        k = far + 1;
    }
    return true;
}

void Agents::land(Character& c) {
    if (!c.in_boat) return;
    c.in_boat = false;
    c.moving = false;
    c.path.clear();
    Vec3i np;
    const Vec3i near = c.task.target.y > 0 ? c.task.target : c.foot;
    if (ctx_.nav->find_standable_near(near, np, 12) || ctx_.nav->find_standable_near(c.foot, np, 16)) place_at(c, np);
}

bool Agents::boat_fishing(Character& c, Job& j) {
    Task& t = c.task;
    const Registry& reg = *ctx_.reg;
    const World& w = *ctx_.world;
    Fauna* fauna = ctx_.fauna;
    const ItemId boat = reg.find_item("boat");
    auto give_up = [&](const std::string& msg) {
        say(c, msg);
        land(c);
        ctx_.jobs->complete(t.job);
        t.job = 0;
        // The boat (and whatever was caught) goes back to the store.
        if (const Store* inv = ctx_.econ->store(c.inv); inv && inv->count(boat) > 0) {
            t.step = 10;
            return true;
        }
        end_task(c, false);
        return false;
    };
    const size_t g = j.project > 0 ? (size_t)(j.project - 1) : ~(size_t)0;
    if (!fauna || boat == kNoItem || g >= fauna->grounds().size()) return give_up("渔场已经没有了");
    // Work at the net goes as fast as a fisher's skill and hands allow.
    auto work_ticks = [&](float base) {
        const float f = (0.6f + 0.8f * c.skills[kFarming]) * std::max(0.2f, c.body.manipulation());
        return (Tick)std::max(10.0f, base / f);
    };
    auto afloat_at = [&](const Vec3f& p, int level) {
        c.pos = Vec3f{p.x, (float)level + 0.55f, p.z};
        c.foot = Vec3i{(int)std::floor(p.x), level + 1, (int)std::floor(p.z)};
    };
    switch (t.step) {
        case 0: {  // A boat: in hand already, or from the store.
            if (const Store* inv = ctx_.econ->store(c.inv); inv && inv->count(boat) > 0) {
                t.step = 2;
                return true;
            }
            const StoreId s = boat_store(c);
            if (!s) return give_up("没有空闲的船");
            ctx_.econ->reserve(s, boat, 1, c.id, now_ + kTicksPerHour);
            t.store = s;
            t.step = 1;
            return true;
        }
        case 1: {
            const Store* s = ctx_.econ->store(t.store);
            if (!s) {
                t.step = 0;
                return true;
            }
            say(c, "去仓库取船");
            const Move m = move_to(c, s->pos, true);
            if (m == Move::Failed) return give_up("取不到船");
            if (m != Move::Arrived) return true;
            if (ctx_.econ->transfer(t.store, c.inv, boat, 1) <= 0) return give_up("船被别人划走了");
            t.step = 2;
            return true;
        }
        case 2: {  // Carry it to the shore and put out.
            say(c, "扛着船去岸边");
            const Move m = move_to(c, j.pos, false);
            if (m == Move::Failed) {
                blacklist(c, j.pos, kTicksPerHour * 2);
                return give_up("到不了岸边");
            }
            if (m != Move::Arrived) return true;
            Vec3i water{0, -1, 0};
            for (int ey = 0; ey >= -2 && water.y < 0; --ey)
                for (int d = 0; d < 4 && water.y < 0; ++d) {
                    const Vec3i q = c.foot + kDir4H[d] + Vec3i{0, ey, 0};
                    if (open_water(w, q.x, q.z, q.y)) water = q;
                }
            if (water.y < 0) return give_up("这里下不了水");
            const int level = water.y;
            // Where the school swims now, on this sheet of water.
            Vec3i to = fauna->ground_center(g);
            to.y = level;
            if (!open_water(w, to.x, to.z, level)) {
                bool ok = false;
                for (int r = 1; r <= 8 && !ok; ++r)
                    for (int dz = -r; dz <= r && !ok; ++dz)
                        for (int dx = -r; dx <= r && !ok; ++dx)
                            if (open_water(w, to.x + dx, to.z + dz, level)) {
                                to = Vec3i{to.x + dx, level, to.z + dz};
                                ok = true;
                            }
                if (!ok) return give_up("渔场不在这片水上");
            }
            std::vector<Vec3i> route;
            if (!water_route(water, to, level, route)) return give_up("水路不通");
            t.target = c.foot;  // where she put out (and comes back)
            t.target2 = Vec3i{0, level, 0};
            t.count = 0;
            c.path.nodes = route;
            c.path.next = 0;
            c.in_boat = true;
            afloat_at(Vec3f{(float)water.x + 0.5f, 0.0f, (float)water.z + 0.5f}, level);
            t.step = 3;
            return true;
        }
        case 3:
        case 5: {  // Rowing.
            const int level = t.target2.y;
            say(c, t.step == 3 ? "划船出海捕鱼" : "满载而归");
            if (!c.path.valid()) {
                c.moving = false;
                if (t.step == 3) {
                    t.until = now_ + work_ticks(90);
                    t.step = 4;
                    return true;
                }
                // Ashore where she set out, the boat on her shoulder again.
                c.in_boat = false;
                place_at(c, t.target);
                ctx_.jobs->complete(t.job);
                t.job = 0;
                t.step = 10;
                return true;
            }
            const Vec3i wp = c.path.nodes[c.path.next];
            const float dx = (float)wp.x + 0.5f - c.pos.x, dz = (float)wp.z + 0.5f - c.pos.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d < 0.35f) {
                ++c.path.next;
                return true;
            }
            const float step = std::min(d, kRowSpeed * (0.8f + 0.4f * c.skills[kHauling]));
            afloat_at(Vec3f{c.pos.x + dx / d * step, 0.0f, c.pos.z + dz / d * step}, level);
            c.yaw = std::atan2(dx, dz);
            c.moving = true;
            c.walk_phase += 0.08f;
            return true;
        }
        case 4: {  // Casting the net from the boat.
            say(c, "在船上撒网");
            if (now_ < t.until) return true;
            const int local = fauna->fish_near(c.pos, 12.0f);
            const float chance = 0.75f * (0.75f + 0.5f * c.skills[kFarming]) * std::min(1.0f, (float)local / 4.0f);
            if (local > 0 && rng_.chance(chance)) {
                Animal* best = nullptr;
                float bd = 12.0f * 12.0f;
                for (const Animal& a : fauna->all()) {
                    if (!a.alive || !fauna->spec(a.species).aquatic || std::abs(a.pos.y - c.pos.y) > 8.0f) continue;
                    const float ax = a.pos.x - c.pos.x, az = a.pos.z - c.pos.z;
                    if (ax * ax + az * az < bd) {
                        bd = ax * ax + az * az;
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
                    e.text = strfmt("%s在船上捕到一条%s", c.name.c_str(), kind.c_str());
                    ctx_.chron->emit(std::move(e));
                    c.skills[kFarming] = std::min(1.0f, c.skills[kFarming] + 0.01f);
                    ctx_.society->practice(c.polity, "hunt", 0.3f, c.id);
                    ++t.count;
                }
            }
            if (local > 0 && t.count < 6 && ++t.target2.x < 9 && carried_weight(c) + 1.5f < carry_capacity(c) + 6.0f) {
                t.until = now_ + work_ticks(70);
                return true;
            }
            // Back to where she put out.
            const int level = t.target2.y;
            Vec3i home{0, -1, 0};
            for (int d = 0; d < 4 && home.y < 0; ++d)
                for (int ey = 0; ey >= -2 && home.y < 0; --ey) {
                    const Vec3i q = t.target + kDir4H[d] + Vec3i{0, ey, 0};
                    if (q.y == level && open_water(w, q.x, q.z, level)) home = q;
                }
            std::vector<Vec3i> route;
            const Vec3i here{c.foot.x, level, c.foot.z};
            if (home.y < 0 || !water_route(here, home, level, route)) {
                land(c);  // (the long way round is not modelled: ashore at the nearest bank)
                ctx_.jobs->complete(t.job);
                t.job = 0;
                t.step = 10;
                return true;
            }
            c.path.nodes = route;
            c.path.next = 0;
            t.step = 5;
            return true;
        }
        default:
            return give_up("?");
    }
}

}  // namespace icarus
