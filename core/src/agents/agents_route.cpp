// Agents: the long way. A trip across the island is walked in legs. A coarse route over
// tiles of 8x8 columns (from the lie of the land: heights, slopes, fresh water to wade)
// tells which way to go, and each leg is an ordinary short search to a point some way
// along it. The next leg is worked out from wherever the last one ended, so a walk goes
// on the same after a load.
#include <algorithm>
#include <cmath>
#include <queue>

#include "icarus/agents/agents.h"
#include "icarus/agents/nav.h"

namespace icarus {

namespace {
constexpr int kTile = 8;
constexpr i16 kUnknown = -32768;
constexpr i16 kBlocked = -32767;
}  // namespace

void Agents::route_tile(int tx, int tz, i16& h, u8& wet) {
    const size_t i = (size_t)tz * (size_t)route_tx_ + (size_t)tx;
    if (route_h_[i] == kUnknown) {
        const ColumnInfo col = ctx_.world->gen().column(tx * kTile + kTile / 2, tz * kTile + kTile / 2);
        if (!col.land || col.sea) {
            route_h_[i] = kBlocked;
            route_wet_[i] = 0;
        } else {
            route_h_[i] = col.top;
            const int depth = col.water_top >= 0 ? col.water_top - col.top : 0;
            route_wet_[i] = (u8)(depth <= 0 ? 0 : (depth <= 1 ? 1 : 2));
        }
    }
    h = route_h_[i];
    wet = route_wet_[i];
}

bool Agents::route_vias(const Vec3i& from, const Vec3i& to, std::vector<Vec3i>& vias) {
    vias.clear();
    const World& w = *ctx_.world;
    const int tx = (w.size_x() + kTile - 1) / kTile, tz = (w.size_z() + kTile - 1) / kTile;
    if (route_tx_ != tx || route_tz_ != tz) {
        route_tx_ = tx;
        route_tz_ = tz;
        const size_t n = (size_t)tx * (size_t)tz;
        route_h_.assign(n, kUnknown);
        route_wet_.assign(n, 0);
        route_g_.assign(n, 0.0f);
        route_gen_.assign(n, 0);
        route_parent_.assign(n, 0);
        route_stamp_ = 0;
    }
    auto tile_of = [&](const Vec3i& p, int& a, int& b) {
        a = clampv(p.x / kTile, 0, tx - 1);
        b = clampv(p.z / kTile, 0, tz - 1);
    };
    int sa, sb, ga, gb;
    tile_of(from, sa, sb);
    tile_of(to, ga, gb);
    if (sa == ga && sb == gb) return false;
    if (++route_stamp_ == 0) {
        std::fill(route_gen_.begin(), route_gen_.end(), 0u);
        route_stamp_ = 1;
    }
    const u32 stamp = route_stamp_;
    const u32 start = (u32)(sb * tx + sa), goal = (u32)(gb * tx + ga);
    auto h_of = [&](int a, int b) {
        const float dx = (float)std::abs(a - ga), dz = (float)std::abs(b - gb);
        return (std::max(dx, dz) + 0.41421f * std::min(dx, dz)) * (float)kTile;
    };
    struct QE {
        float f, g;
        u32 i;
        bool operator<(const QE& o) const {
            if (f != o.f) return f > o.f;
            if (g != o.g) return g < o.g;
            return i > o.i;
        }
    };
    std::priority_queue<QE> open;
    route_gen_[start] = stamp;
    route_g_[start] = 0.0f;
    route_parent_[start] = start;
    open.push({h_of(sa, sb), 0.0f, start});
    static const int d8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    bool found = false;
    int expanded = 0;
    while (!open.empty()) {
        const QE cur = open.top();
        open.pop();
        if (cur.g > route_g_[cur.i] + 1e-3f) continue;
        if (cur.i == goal) {
            found = true;
            break;
        }
        if (++expanded > 60000) break;
        const int a = (int)(cur.i % (u32)tx), b = (int)(cur.i / (u32)tx);
        i16 h0;
        u8 w0;
        route_tile(a, b, h0, w0);
        if (h0 == kBlocked) h0 = (i16)std::clamp(cur.i == start ? from.y - 1 : to.y - 1, 0, 32000);
        for (auto& d : d8) {
            const int na = a + d[0], nb = b + d[1];
            if (na < 0 || nb < 0 || na >= tx || nb >= tz) continue;
            const u32 ni = (u32)(nb * tx + na);
            i16 h1;
            u8 w1;
            route_tile(na, nb, h1, w1);
            if (h1 == kBlocked) {
                if (ni != goal) continue;
                h1 = (i16)std::max(0, to.y - 1);
            }
            const bool diag = d[0] && d[1];
            const int dh = std::abs((int)h1 - (int)h0);
            if (dh > (diag ? 8 : 6)) continue;  // too steep to climb on foot
            const float step = (float)kTile * (diag ? 1.41421f : 1.0f);
            const float cost = step * (w1 == 0 ? 1.0f : (w1 == 1 ? 2.5f : 6.0f)) + 0.6f * (float)dh;
            const float g = cur.g + cost;
            if (route_gen_[ni] == stamp && g >= route_g_[ni]) continue;
            route_gen_[ni] = stamp;
            route_g_[ni] = g;
            route_parent_[ni] = cur.i;
            open.push({g + h_of(na, nb), g, ni});
        }
    }
    if (!found) return false;
    std::vector<u32> chain;
    for (u32 i = goal; i != start; i = route_parent_[i]) chain.push_back(i);
    std::reverse(chain.begin(), chain.end());
    // The farthest point along the route within a leg's walk, and two nearer ones to fall
    // back on (a leg that runs into something the lie of the land did not show).
    int far = -1;
    for (size_t k = 0; k < chain.size(); ++k) {
        const int a = (int)(chain[k] % (u32)tx), b = (int)(chain[k] / (u32)tx);
        const int cx = a * kTile + kTile / 2, cz = b * kTile + kTile / 2;
        const i64 dx = cx - from.x, dz = cz - from.z;
        if (dx * dx + dz * dz > (i64)kLeg * kLeg) break;
        far = (int)k;
    }
    if (far < 0) far = 0;
    for (int k : {far, far * 2 / 3, far / 3}) {
        const int a = (int)(chain[(size_t)k] % (u32)tx), b = (int)(chain[(size_t)k] / (u32)tx);
        i16 h;
        u8 wt;
        route_tile(a, b, h, wt);
        const Vec3i v{a * kTile + kTile / 2, h == kBlocked ? to.y : h + 1, b * kTile + kTile / 2};
        if (v.dist2(from) <= 4 * 4) continue;
        if (std::find(vias.begin(), vias.end(), v) == vias.end()) vias.push_back(v);
    }
    return !vias.empty();
}

bool Agents::reachable(const Vec3i& from, const Vec3i& to, int budget) {
    Nav& nav = *ctx_.nav;
    Path tmp;
    if (std::max(std::abs(to.x - from.x), std::abs(to.z - from.z)) <= kLegFar)
        return nav.find_path(from, to, true, tmp, budget);
    std::vector<Vec3i> vias;
    if (!route_vias(from, to, vias)) return false;
    for (const Vec3i& v : vias)
        if (nav.find_path(from, v, true, tmp, 8000, 3, 3)) return true;
    return false;
}

}  // namespace icarus
