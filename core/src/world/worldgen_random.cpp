// Random layout (随机空岛): an island generated from the seed and the new-game options.
//
// Initialisation works on a coarse grid (one node every 4 cubes): elevation from noise
// and a few mountain ranges, then hydrology — a priority flood from the coast fills the
// hollows (the deep ones become lakes) and gives every node a way downhill; where enough
// rain gathers, rivers run. Rivers are still water in steps: each reach is level and the
// next one down starts behind a stone weir as high as the reach above (a ford, too), so
// no water ever flows and nothing costs anything at run time. The rivers are drawn into
// a full-resolution map; everything else stays a pure function of the coordinates, so
// cells generate lazily in any order.
//
// Water never leaks: a dry column is never lower than the water beside it, and a water
// column next to higher water becomes a weir. Both rules look only at the four
// neighbours' raw water levels, so a column can be computed on its own.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <queue>

#include "icarus/util/noise.h"
#include "icarus/util/rng.h"
#include "icarus/world/worldgen.h"

namespace icarus {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int G = 4;  // coarse grid step in cubes

inline float smoothstep(float e0, float e1, float x) {
    float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline float dist2d(float ax, float az, float bx, float bz) {
    float dx = ax - bx, dz = az - bz;
    return std::sqrt(dx * dx + dz * dz);
}

inline float seg_dist(float px, float pz, float ax, float az, float bx, float bz, float& t) {
    float vx = bx - ax, vz = bz - az;
    float wx = px - ax, wz = pz - az;
    float l2 = vx * vx + vz * vz;
    t = l2 > 0 ? saturate((wx * vx + wz * vz) / l2) : 0.0f;
    return dist2d(px, pz, ax + vx * t, az + vz * t);
}

inline u8 unit_u8(float v) { return (u8)clampv((int)std::lround(v * 255.0f), 0, 255); }

Biome classify(float t, float m, float alt) {
    if (alt > 30.0f) return Biome::Highland;
    if (t < 0.24f) return Biome::Snowfield;
    if (t < 0.40f) return Biome::Taiga;
    if (t > 0.74f && m < 0.44f) return Biome::Desert;
    if (t > 0.60f && m < 0.52f) return Biome::Savanna;
    if (m > 0.68f && alt < 6.0f) return Biome::Wetland;
    if (m > 0.54f) return Biome::Forest;
    return Biome::Grassland;
}

enum : u8 { kLand = 0, kOutside = 1, kLake = 2 };
}  // namespace

struct WorldGen::RandomIsland {
    int W = 0, D = 0;    // world extent in cubes
    int gw = 0, gd = 0;  // coarse grid size
    float cx = 0, cz = 0, R = 0, Rs = 0;
    int SL = 140;
    bool sea = true;
    float relief = 1.0f, rich = 1.0f;
    u64 salt = 0;
    std::vector<float> H;           // final coarse terrain height
    std::vector<float> T, M;        // coarse climate (0..1)
    std::vector<u8> kind;           // kLand / kOutside / kLake
    std::vector<u16> lake_of;       // lake index + 1
    std::vector<int> lake_level;    // per lake: y of the top water cube
    std::vector<u8> near_water;     // water within two nodes (neighbour rules apply)
    std::vector<u8> near_sea;       // the outside within three nodes (beaches)
    std::vector<u8> river;          // node carries a river
    std::vector<float> width;       // river width (cubes) per node
    std::vector<int> down;          // drainage: the next node downhill (-1 at the coast)

    // Rivers at full resolution, stored sparsely per cell column.
    struct RBlock {
        std::array<i16, kCellSize * kCellSize> w;    // water level, -1 none
        std::array<i16, kCellSize * kCellSize> cap;  // highest the land may be (valley), 32767 none
        std::array<u8, kCellSize * kCellSize> depth; // bed below the water
        std::array<u8, kCellSize * kCellSize> dist;  // distance to the channel's axis ×8 (ties)
    };
    std::vector<std::unique_ptr<RBlock>> rb;

    struct Flat {
        float x = 0, z = 0, level = 0, r = 0, keep = 0;
    };
    std::vector<Flat> flats;  // site plateaus and field terraces

    int node(int i, int j) const { return j * gw + i; }
    float nx(int i) const { return (float)(i * G) + G * 0.5f; }
    // Bilinear sample of a coarse field at world position (x, z).
    float sample(const std::vector<float>& a, float x, float z) const {
        const float fx = x / G - 0.5f, fz = z / G - 0.5f;
        const int i0 = clampv((int)std::floor(fx), 0, gw - 2), j0 = clampv((int)std::floor(fz), 0, gd - 2);
        const float tx = saturate(fx - (float)i0), tz = saturate(fz - (float)j0);
        const float a0 = lerpf(a[(size_t)node(i0, j0)], a[(size_t)node(i0 + 1, j0)], tx);
        const float a1 = lerpf(a[(size_t)node(i0, j0 + 1)], a[(size_t)node(i0 + 1, j0 + 1)], tx);
        return lerpf(a0, a1, tz);
    }
    // The four nodes around (x, z).
    void corners(float x, float z, int out[4]) const {
        const float fx = x / G - 0.5f, fz = z / G - 0.5f;
        const int i0 = clampv((int)std::floor(fx), 0, gw - 2), j0 = clampv((int)std::floor(fz), 0, gd - 2);
        out[0] = node(i0, j0);
        out[1] = node(i0 + 1, j0);
        out[2] = node(i0, j0 + 1);
        out[3] = node(i0 + 1, j0 + 1);
    }
    int nearest(float x, float z) const {
        const int i = clampv((int)std::floor(x / G), 0, gw - 1), j = clampv((int)std::floor(z / G), 0, gd - 1);
        return node(i, j);
    }
    const RBlock* block(int x, int z) const {
        const int bx = x >> kCellBits, bz = z >> kCellBits;
        if (bx < 0 || bz < 0 || bx >= W / kCellSize || bz >= D / kCellSize) return nullptr;
        return rb[(size_t)(bz * (W / kCellSize) + bx)].get();
    }

    // Raw terrain and water of a column, before the neighbour rules.
    struct Raw {
        bool land = false;
        float t = 0;       // terrain height (continuous)
        int top = 0;       // terrain top cube
        int water = -1;    // water level (top water cube), -1 none
        bool sea = false;
        bool river = false;
        bool bank = false;  // within the river's valley
        bool reserved = false;
        int lake = -1;
    };
    Raw raw(int xi, int zi) const;
};

WorldGen::RandomIsland::Raw WorldGen::RandomIsland::raw(int xi, int zi) const {
    Raw r;
    if (xi < 0 || zi < 0 || xi >= W || zi >= D) return r;
    const float x = (float)xi + 0.5f, z = (float)zi + 0.5f;
    if (sea && dist2d(x, z, cx, cz) > Rs) return r;
    int cn[4];
    corners(x, z, cn);
    bool outside = false;
    int lake = -1;
    for (int k = 0; k < 4; ++k) {
        if (kind[(size_t)cn[k]] == kOutside) outside = true;
        if (lake_of[(size_t)cn[k]] && (lake < 0 || lake_of[(size_t)cn[k]] - 1 < lake)) lake = lake_of[(size_t)cn[k]] - 1;
    }
    const RBlock* b = block(xi, zi);
    const int li = (zi & kCellMask) * kCellSize + (xi & kCellMask);
    // Open sky beyond a sealess island's edge (no noise to work out).
    const float coarse = sample(H, x, z);
    if (!sea && outside && lake < 0 && !(b && b->w[(size_t)li] >= 0) && coarse + relief * 1.1f + 0.6f < (float)SL) {
        bool flat = false;
        for (const Flat& f : flats) flat = flat || dist2d(x, z, f.x, f.z) < f.r;
        if (!flat) return r;
    }
    float t = coarse + relief * 1.1f * fbm2(salt + 21, x / 33.0f, z / 33.0f, 3) + 0.55f * fbm2(salt + 22, x / 9.0f, z / 9.0f, 2);
    // River valley.
    if (b && b->cap[(size_t)li] < 32767) {
        t = std::min(t, (float)b->cap[(size_t)li]);
        r.bank = true;
    }
    // Field terraces, then settlement plateaus (level around their centre).
    for (const Flat& f : flats) {
        const float dd = dist2d(x, z, f.x, f.z);
        if (dd >= f.r) continue;
        t = lerpf(t, f.level, 1.0f - smoothstep(f.r * 0.62f, f.r, dd));
        if (dd < f.keep) r.reserved = true;
    }
    int top = (int)std::floor(t);
    r.t = t;
    r.land = true;
    // Still water: lake, sea, river (in that order: a river's mouth opens into them).
    if (lake >= 0 && top < lake_level[(size_t)lake]) {
        r.water = lake_level[(size_t)lake];
        r.lake = lake;
    } else if (outside && top < SL) {
        if (!sea) {  // the island's edge: sky beyond
            r.land = false;
            return r;
        }
        r.water = SL;
        r.sea = true;
        top = std::max(top, SL - 14);
    } else if (b && b->w[(size_t)li] >= 0) {
        r.water = b->w[(size_t)li];
        top = std::min(top, r.water - (int)b->depth[(size_t)li]);
        r.river = true;
    } else if (!sea && outside && top <= SL) {
        r.land = false;
        return r;
    }
    r.top = top;
    return r;
}

void WorldGen::init_random() {
    WorldConfig& cfg = cfg_;
    auto isl = std::make_shared<RandomIsland>();
    RandomIsland& I = *isl;
    I.W = cfg.cells_x * kCellSize;
    I.D = cfg.cells_z * kCellSize;
    I.gw = I.W / G;
    I.gd = I.D / G;
    I.cx = (float)I.W * 0.5f;
    I.cz = (float)I.D * 0.5f;
    I.R = cfg.island_radius;
    I.sea = cfg.sea;
    I.Rs = cfg.sea ? (float)std::min(I.W, I.D) * 0.5f - 20.0f : 0.0f;
    I.SL = cfg.sea_level;
    I.relief = cfg.relief <= 0 ? 0.55f : (cfg.relief == 1 ? 1.0f : 1.5f);
    I.rich = cfg.richness <= 0 ? 0.6f : (cfg.richness == 1 ? 1.0f : 1.45f);
    I.salt = hash_combine(cfg.seed, 0x51A7);
    Rng rng(cfg.seed, 0x3A7D0);
    const u64 salt = I.salt;
    const float R = I.R, cx = I.cx, cz = I.cz;
    const int SL = I.SL, gw = I.gw, gd = I.gd;
    const size_t N = (size_t)gw * (size_t)gd;

    // ------------------------------------------------------------ mountain ranges
    // Each range is a chain of peaks along a wandering line, joined by a lower ridge.
    struct Peak {
        float x, z, h, w;
    };
    std::vector<std::vector<Peak>> ranges;
    const int nranges = 1 + std::max(0, cfg.relief) + (int)rng.below(2);
    for (int k = 0; k < nranges; ++k) {
        const float a0 = rng.uniform(0.0f, 2.0f * kPi), r0 = rng.uniform(0.05f, 0.5f) * R;
        float x = cx + std::cos(a0) * r0, z = cz + std::sin(a0) * r0;
        float dir = rng.uniform(0.0f, 2.0f * kPi);
        const float tall = rng.uniform(34.0f, 62.0f);
        std::vector<Peak> rv;
        const int n = 6 + (int)rng.below(6);
        for (int i = 0; i < n; ++i) {
            if (dist2d(x, z, cx, cz) > 0.74f * R) break;
            rv.push_back({x, z, tall * rng.uniform(0.55f, 1.15f), rng.uniform(30.0f, 48.0f)});
            const float step = rng.uniform(0.055f, 0.095f) * R;
            dir += rng.uniform(-0.45f, 0.45f);
            x += std::cos(dir) * step;
            z += std::sin(dir) * step;
        }
        if (rv.size() >= 2) ranges.push_back(rv);
    }
    // The island's outline: stretched along one axis, with bays and headlands.
    const float stretch_a = rng.uniform(0.0f, kPi), stretch = rng.uniform(0.72f, 1.0f);

    // ------------------------------------------------------------ coarse elevation
    // Climate axes: warmth varies along one direction, rain along another.
    const float ta = rng.uniform(0.0f, 2.0f * kPi), ma = ta + rng.uniform(1.2f, 2.0f);
    float T0 = 0.52f, Tg = 0.30f, M0 = 0.52f;
    switch (cfg.climate) {
        case 1: T0 = 0.56f, Tg = 0.10f; break;             // temperate
        case 2: T0 = 0.32f, Tg = 0.12f; break;             // cold
        case 3: T0 = 0.72f, Tg = 0.10f, M0 = 0.38f; break;  // hot and dry
        case 4: T0 = 0.58f, Tg = 0.14f, M0 = 0.70f; break;  // wet
        default: break;                                     // varied
    }
    std::vector<float> E(N), cont(N);
    I.T.assign(N, 0.5f);
    I.M.assign(N, 0.5f);
    for (int j = 0; j < gd; ++j)
        for (int i = 0; i < gw; ++i) {
            const size_t n = (size_t)I.node(i, j);
            const float x = I.nx(i), z = I.nx(j);
            const float wx = x + 90.0f * fbm2(salt + 1, x / 260.0f, z / 260.0f, 3);
            const float wz = z + 90.0f * fbm2(salt + 2, x / 260.0f, z / 260.0f, 3);
            float ex = wx - cx, ez = wz - cz;
            {
                const float u = ex * std::cos(stretch_a) + ez * std::sin(stretch_a);
                const float v = -ex * std::sin(stretch_a) + ez * std::cos(stretch_a);
                ex = u;
                ez = v / stretch;
            }
            const float shape = R * (1.0f + 0.30f * fbm2(salt + 3, x / 520.0f, z / 520.0f, 3) +
                                     0.15f * fbm2(salt + 4, x / 170.0f, z / 170.0f, 3) + 0.05f * fbm2(salt + 10, x / 60.0f, z / 60.0f, 2));
            const float c = 1.0f - std::sqrt(ex * ex + ez * ez) / shape;
            cont[n] = c;
            float h;
            if (c >= 0.0f) h = (float)SL + 1.5f + 13.0f * smoothstep(0.0f, 0.3f, c);
            else h = std::max((float)SL - 16.0f, (float)SL + 1.5f + 60.0f * c);
            const float inland = smoothstep(0.02f, 0.2f, c);
            h += I.relief * inland * (8.0f * fbm2(salt + 5, x / 170.0f, z / 170.0f, 4) + 3.5f * fbm2(salt + 6, x / 50.0f, z / 50.0f, 3));
            const float mass = std::max(0.0f, fbm2(salt + 7, x / 380.0f, z / 380.0f, 3) - 0.12f) * 2.2f;
            h += I.relief * inland * mass * (20.0f + 28.0f * ridged2(salt + 8, x / 90.0f, z / 90.0f, 4));
            // Ranges: the highest peak nearby, a ridge between neighbours, foothills.
            const float rx = x + 22.0f * fbm2(salt + 11, x / 70.0f, z / 70.0f, 2);
            const float rz = z + 22.0f * fbm2(salt + 12, x / 70.0f, z / 70.0f, 2);
            float bump = 0.0f, foot = 0.0f;
            for (const auto& rv : ranges)
                for (size_t s = 0; s < rv.size(); ++s) {
                    const Peak& a = rv[s];
                    const float dd = dist2d(rx, rz, a.x, a.z);
                    if (dd < a.w * 3.5f) {
                        bump = std::max(bump, a.h * std::exp(-(dd / a.w) * (dd / a.w) * 1.4f));
                        foot = std::max(foot, a.h * 0.34f * std::exp(-(dd / (a.w * 2.6f)) * (dd / (a.w * 2.6f))));
                    }
                    if (s + 1 == rv.size()) continue;
                    const Peak& b = rv[s + 1];
                    float tt;
                    const float ds = seg_dist(rx, rz, a.x, a.z, b.x, b.z, tt);
                    const float w = lerpf(a.w, b.w, tt) * 0.8f;
                    if (ds > w * 3.0f) continue;
                    const float amp = lerpf(a.h, b.h, tt) * (0.74f - 0.16f * std::sin(tt * kPi));
                    bump = std::max(bump, amp * std::exp(-(ds / w) * (ds / w) * 1.4f));
                }
            bump = std::max(bump, foot) * (0.62f + 0.75f * ridged2(salt + 9, x / 40.0f, z / 40.0f, 3));
            h += I.relief * smoothstep(0.03f, 0.2f, c) * bump;
            E[n] = std::min(h, (float)SL + 105.0f);
            // Climate before water: warmth along its axis, cooler higher up; rain.
            const float px = (x - cx) / R, pz = (z - cz) / R;
            float Tv = T0 + Tg * (px * std::cos(ta) + pz * std::sin(ta)) + 0.12f * fbm2(salt + 61, x / 260.0f, z / 260.0f, 3);
            Tv -= std::max(0.0f, E[n] - (float)SL - 16.0f) / 120.0f;
            I.T[n] = Tv;
            I.M[n] = M0 + 0.12f * (px * std::cos(ma) + pz * std::sin(ma)) + 0.28f * fbm2(salt + 67, x / 250.0f, z / 250.0f, 3);
        }

    // ------------------------------------------------------------ the outside
    // Nodes below the sea (or the island's rim) connected to the world's edge.
    I.kind.assign(N, kLand);
    {
        std::vector<int> q;
        auto try_push = [&](int i, int j) {
            const size_t n = (size_t)I.node(i, j);
            if (I.kind[n] == kOutside || E[n] >= (float)SL + 0.5f) return;
            I.kind[n] = kOutside;
            q.push_back((int)n);
        };
        for (int i = 0; i < gw; ++i) try_push(i, 0), try_push(i, gd - 1);
        for (int j = 0; j < gd; ++j) try_push(0, j), try_push(gw - 1, j);
        for (size_t h = 0; h < q.size(); ++h) {
            const int i = q[h] % gw, j = q[h] / gw;
            if (i > 0) try_push(i - 1, j);
            if (i + 1 < gw) try_push(i + 1, j);
            if (j > 0) try_push(i, j - 1);
            if (j + 1 < gd) try_push(i, j + 1);
        }
    }
    // Beyond the sea's rim nothing is land (the sea's floor ends there).
    if (I.sea)
        for (int j = 0; j < gd; ++j)
            for (int i = 0; i < gw; ++i)
                if (dist2d(I.nx(i), I.nx(j), cx, cz) > I.Rs - 2.0f) I.kind[(size_t)I.node(i, j)] = kOutside;

    // ------------------------------------------------------------ priority flood
    static const int kD8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    std::vector<float> F(E);
    I.down.assign(N, -1);
    std::vector<int> order;
    order.reserve(N);
    {
        std::vector<u8> closed(N, 0);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        for (int j = 0; j < gd; ++j)
            for (int i = 0; i < gw; ++i) {
                const size_t n = (size_t)I.node(i, j);
                if (I.kind[n] == kOutside) {
                    closed[n] = 1;
                    continue;
                }
                // Coast: next to the outside (or the world's edge).
                int best = -1;
                for (auto& d : kD8) {
                    const int a = i + d[0], b = j + d[1];
                    if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                    const int m = I.node(a, b);
                    if (I.kind[(size_t)m] == kOutside && (best < 0 || E[(size_t)m] < E[(size_t)best])) best = m;
                }
                if (best >= 0) {
                    closed[n] = 1;
                    I.down[n] = best;
                    pq.push({E[n], (int)n});
                }
            }
        while (!pq.empty()) {
            const auto [f, c] = pq.top();
            pq.pop();
            order.push_back(c);
            const int i = c % gw, j = c / gw;
            for (auto& d : kD8) {
                const int a = i + d[0], b = j + d[1];
                if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                const size_t m = (size_t)I.node(a, b);
                if (closed[m]) continue;
                closed[m] = 1;
                F[m] = std::max(E[m], f + 0.01f);
                I.down[m] = c;
                pq.push({F[m], (int)m});
            }
        }
    }

    // ------------------------------------------------------------ lakes
    I.H.assign(N, 0.0f);
    I.lake_of.assign(N, 0);
    for (size_t n = 0; n < N; ++n) I.H[n] = I.kind[n] == kOutside ? E[n] : F[n];
    {
        std::vector<u8> seen(N, 0);
        struct Cand {
            std::vector<int> nodes;
            float spill = 0, deepest = 0;
        };
        std::vector<Cand> cands;
        for (size_t s = 0; s < N; ++s) {
            if (seen[s] || I.kind[s] != kLand || F[s] - E[s] < 0.9f) continue;
            Cand cd;
            cd.spill = 1e9f;
            std::vector<int> q{(int)s};
            seen[s] = 1;
            for (size_t h = 0; h < q.size(); ++h) {
                const int c = q[h];
                cd.nodes.push_back(c);
                cd.spill = std::min(cd.spill, F[(size_t)c]);
                cd.deepest = std::max(cd.deepest, F[(size_t)c] - E[(size_t)c]);
                const int i = c % gw, j = c / gw;
                for (int k = 0; k < 4; ++k) {
                    const int a = i + kD8[k][0], b = j + kD8[k][1];
                    if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                    const size_t m = (size_t)I.node(a, b);
                    if (seen[m] || I.kind[m] != kLand || F[m] - E[m] < 0.9f) continue;
                    seen[m] = 1;
                    q.push_back((int)m);
                }
            }
            cands.push_back(std::move(cd));
        }
        // The largest hollows hold lakes; the rest are filled in (level meadows).
        std::vector<int> idx(cands.size());
        for (size_t k = 0; k < idx.size(); ++k) idx[k] = (int)k;
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return cands[(size_t)a].nodes.size() > cands[(size_t)b].nodes.size(); });
        for (int k : idx) {
            const Cand& cd = cands[(size_t)k];
            const int level = (int)std::floor(cd.spill) - 1;
            const bool big = cd.nodes.size() >= 14 && cd.deepest >= 2.2f && I.lake_level.size() < 14;
            for (int c : cd.nodes) {
                if (big && E[(size_t)c] < (float)level + 0.5f) {
                    I.kind[(size_t)c] = kLake;
                    I.lake_of[(size_t)c] = (u16)(I.lake_level.size() + 1);
                    I.H[(size_t)c] = E[(size_t)c];
                }
            }
            if (big) I.lake_level.push_back(level);
        }
    }

    // ------------------------------------------------------------ rivers
    // Rain gathers downhill; where enough of it runs together, a river.
    std::vector<float> acc(N, 0.0f);
    for (size_t n = 0; n < N; ++n)
        if (I.kind[n] != kOutside) acc[n] = 0.4f + saturate(I.M[n]);
    for (size_t k = order.size(); k-- > 0;) {
        const int c = order[k];
        const int d = I.down[(size_t)c];
        if (d >= 0 && I.kind[(size_t)d] != kOutside) acc[(size_t)d] += acc[(size_t)c];
    }
    size_t land_nodes = 0;
    for (size_t n = 0; n < N; ++n) land_nodes += I.kind[n] == kLand ? 1 : 0;
    // Distance (in nodes) to the outside, to end rivers short of a sky-island's rim.
    std::vector<int> to_out(N, 1 << 20);
    {
        std::vector<int> q;
        for (size_t n = 0; n < N; ++n)
            if (I.kind[n] == kOutside) {
                to_out[n] = 0;
                q.push_back((int)n);
            }
        for (size_t h = 0; h < q.size(); ++h) {
            const int c = q[h], i = c % gw, j = c / gw;
            for (int k = 0; k < 4; ++k) {
                const int a = i + kD8[k][0], b = j + kD8[k][1];
                if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                const size_t m = (size_t)I.node(a, b);
                if (to_out[m] <= to_out[(size_t)c] + 1) continue;
                to_out[m] = to_out[(size_t)c] + 1;
                q.push_back((int)m);
            }
        }
    }
    float threshold = 160.0f;
    I.river.assign(N, 0);
    for (int pass = 0; pass < 12; ++pass) {
        size_t count = 0;
        for (size_t n = 0; n < N; ++n) {
            I.river[n] = I.kind[n] == kLand && acc[n] >= threshold && (I.sea || to_out[n] > 3);
            count += I.river[n];
        }
        if ((float)count <= 0.022f * (float)land_nodes) break;
        threshold *= 1.3f;
    }
    I.width.assign(N, 0.0f);
    std::vector<int> W(N, 1 << 20);
    for (size_t k = order.size(); k-- > 0;) {
        const size_t c = (size_t)order[k];
        if (!I.river[c]) continue;
        I.width[c] = clampv(1.6f + 1.25f * std::sqrt(acc[c] / threshold), 3.0f, 9.0f);
        int w = std::min(W[c], (int)std::floor(I.H[c]) - 1);
        if (I.sea) w = std::max(w, SL);
        W[c] = w;
        const int d = I.down[c];
        if (d >= 0) W[(size_t)d] = std::min(W[(size_t)d], w);
    }
    // Draw the channels and their valleys at full resolution.
    const int bw = I.W / kCellSize, bd = I.D / kCellSize;
    I.rb.resize((size_t)bw * (size_t)bd);
    for (int c : order) {
        const size_t n = (size_t)c;
        if (!I.river[n]) continue;
        const int d = I.down[n];
        // Channels wander a little off the grid (the same offset for a node wherever it
        // is used, so consecutive segments meet).
        auto jit = [&](int m, int axis) {
            return 1.7f * (hash_to_unit(hash3(salt + 71, m % gw, axis, m / gw)) * 2.0f - 1.0f);
        };
        const float ax = I.nx(c % gw) + jit(c, 0), az = I.nx(c / gw) + jit(c, 1);
        float bx = ax, bz = az;
        if (d >= 0 && (I.river[(size_t)d] || I.kind[(size_t)d] != kLand)) {
            bx = I.nx(d % gw) + (I.river[(size_t)d] ? jit(d, 0) : 0.0f);
            bz = I.nx(d / gw) + (I.river[(size_t)d] ? jit(d, 1) : 0.0f);
        }
        const float hw = I.width[n] * 0.5f;
        const float vr = hw + 5.0f + 1.5f * I.width[n];
        const int wl = W[n];
        const int x0 = (int)std::floor(std::min(ax, bx) - vr), x1 = (int)std::ceil(std::max(ax, bx) + vr);
        const int z0 = (int)std::floor(std::min(az, bz) - vr), z1 = (int)std::ceil(std::max(az, bz) + vr);
        const int deep = I.width[n] >= 6.0f ? 3 : (I.width[n] >= 4.0f ? 2 : 1);
        for (int z = std::max(0, z0); z <= std::min(I.D - 1, z1); ++z)
            for (int x = std::max(0, x0); x <= std::min(I.W - 1, x1); ++x) {
                float tt;
                const float ds = seg_dist((float)x + 0.5f, (float)z + 0.5f, ax, az, bx, bz, tt);
                if (ds > vr) continue;
                auto& slot = I.rb[(size_t)((z >> kCellBits) * bw + (x >> kCellBits))];
                if (!slot) {
                    slot = std::make_unique<RandomIsland::RBlock>();
                    slot->w.fill(-1);
                    slot->cap.fill(32767);
                    slot->depth.fill(0);
                    slot->dist.fill(255);
                }
                const size_t li = (size_t)((z & kCellMask) * kCellSize + (x & kCellMask));
                // Banks flush with the water, the valley rising gently beyond them.
                const int cap = wl + (int)std::floor(std::max(0.0f, ds - hw - 1.0f) * 0.5f);
                slot->cap[li] = (i16)std::min<int>(slot->cap[li], cap);
                if (ds <= hw) {
                    const u8 q = (u8)std::min(254.0f, ds * 8.0f);
                    if (slot->w[li] < 0 || q < slot->dist[li]) {
                        slot->w[li] = (i16)wl;
                        slot->dist[li] = q;
                        // One deeper for each cube in from the edge (never a step anyone
                        // wading in could not climb back out of).
                        slot->depth[li] = (u8)(1 + std::min(deep, (int)std::floor(hw - ds)));
                    }
                }
            }
    }

    // ------------------------------------------------------------ moisture from water
    {
        std::vector<int> dist(N, 1 << 20);
        std::vector<int> q;
        for (size_t n = 0; n < N; ++n)
            if (I.river[n] || I.kind[n] == kLake) {
                dist[n] = 0;
                q.push_back((int)n);
            }
        for (size_t h = 0; h < q.size(); ++h) {
            const int c = q[h], i = c % gw, j = c / gw;
            if (dist[(size_t)c] >= 12) continue;
            for (auto& dd : kD8) {
                const int a = i + dd[0], b = j + dd[1];
                if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                const size_t m = (size_t)I.node(a, b);
                if (dist[m] <= dist[(size_t)c] + 1) continue;
                dist[m] = dist[(size_t)c] + 1;
                q.push_back((int)m);
            }
        }
        for (size_t n = 0; n < N; ++n) {
            if (dist[n] < 12) I.M[n] += 0.2f * (1.0f - (float)dist[n] / 12.0f);
            if (I.sea && to_out[n] < 8) I.M[n] += 0.08f * (1.0f - (float)to_out[n] / 8.0f);
        }
    }
    // Where land and water meet within two nodes, columns look at their neighbours'
    // water (open sea and dry inland need not).
    I.near_water.assign(N, 0);
    for (int j = 0; j < gd; ++j)
        for (int i = 0; i < gw; ++i) {
            bool wet = false, dry = false;
            for (int b = std::max(0, j - 2); b <= std::min(gd - 1, j + 2); ++b)
                for (int a = std::max(0, i - 2); a <= std::min(gw - 1, i + 2); ++a) {
                    const size_t m = (size_t)I.node(a, b);
                    if (I.kind[m] != kLand || I.river[m]) wet = true;
                    if (I.kind[m] == kLand || I.H[m] > (float)SL - 3.0f) dry = true;
                }
            I.near_water[(size_t)I.node(i, j)] = wet && dry;
        }
    I.near_sea.assign(N, 0);
    for (size_t n = 0; n < N; ++n) I.near_sea[n] = to_out[n] <= 3;

    // ------------------------------------------------------------ settlement sites
    // Flat, fertile ground a short walk from fresh water, back from the shore, apart from
    // each other and in different climates where the island allows; with a sea, one of
    // them near the coast.
    {
        std::vector<int> wdist(N, 1 << 20);
        std::vector<int> q;
        for (size_t n = 0; n < N; ++n)
            if (I.river[n] || I.kind[n] == kLake) {
                wdist[n] = 0;
                q.push_back((int)n);
            }
        for (size_t h = 0; h < q.size(); ++h) {
            const int c = q[h], i = c % gw, j = c / gw;
            if (wdist[(size_t)c] >= 16) continue;
            for (int k = 0; k < 4; ++k) {
                const int a = i + kD8[k][0], b = j + kD8[k][1];
                if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                const size_t m = (size_t)I.node(a, b);
                if (wdist[m] <= wdist[(size_t)c] + 1) continue;
                wdist[m] = wdist[(size_t)c] + 1;
                q.push_back((int)m);
            }
        }
        struct SiteCand {
            int n;
            float score;
            Biome biome;
            bool coast;  // a short walk from the sea (fishing from the shore, boats)
        };
        std::vector<SiteCand> sc;
        for (int j = 6; j < gd - 6; j += 2)
            for (int i = 6; i < gw - 6; i += 2) {
                const size_t n = (size_t)I.node(i, j);
                if (I.kind[n] != kLand || I.river[n] || wdist[n] < 6 || wdist[n] > 12 || to_out[n] < 10) continue;
                const float alt = I.H[n] - (float)SL;
                if (alt < 3.0f || alt > 40.0f) continue;
                float lo = 1e9f, hi = -1e9f;
                for (int b = j - 5; b <= j + 5; ++b)
                    for (int a = i - 5; a <= i + 5; ++a) {
                        const size_t m = (size_t)I.node(a, b);
                        if (I.kind[m] != kLand || I.river[m]) continue;
                        lo = std::min(lo, I.H[m]);
                        hi = std::max(hi, I.H[m]);
                    }
                const float range = hi - lo;
                const Biome bi = classify(I.T[n], saturate(I.M[n]), alt - 10.0f);
                float s = 4.0f - 0.18f * range - 0.05f * (float)std::max(0, wdist[n] - 7);
                if (bi == Biome::Grassland || bi == Biome::Forest) s += 0.6f;
                else if (bi == Biome::Savanna || bi == Biome::Taiga) s += 0.3f;
                else if (bi == Biome::Wetland) s -= 0.6f;
                else s -= 1.5f;  // desert, snow, highland
                // Level ground near the water for fields.
                int fertile = 0;
                for (int b = j - 10; b <= j + 10; b += 2)
                    for (int a = i - 10; a <= i + 10; a += 2) {
                        const size_t m = (size_t)I.node(a, b);
                        if (I.kind[m] == kLand && !I.river[m] && wdist[m] <= 4 && std::fabs(I.H[m] - I.H[n]) < 5.0f) ++fertile;
                    }
                s += 0.02f * (float)fertile;
                sc.push_back({(int)n, s, bi, I.sea && to_out[n] <= 16});
            }
        std::stable_sort(sc.begin(), sc.end(), [](const SiteCand& a, const SiteCand& b) { return a.score > b.score; });
        std::vector<SiteCand> chosen;
        float spacing = 0.78f * R;
        while (chosen.size() < 4 && spacing > 0.2f * R) {
            const SiteCand* best = nullptr;
            float bs = -1e9f;
            for (const SiteCand& c : sc) {
                float md = 1e9f;
                bool used_biome = false, coast_taken = false;
                for (const SiteCand& o : chosen) {
                    coast_taken = coast_taken || o.coast;
                    md = std::min(md, dist2d(I.nx(c.n % gw), I.nx(c.n / gw), I.nx(o.n % gw), I.nx(o.n / gw)));
                    if (o.biome == c.biome) used_biome = true;
                }
                if (md < spacing) continue;
                const bool poor = c.biome == Biome::Wetland || c.biome == Biome::Desert || c.biome == Biome::Snowfield;
                // With a sea, one people lives by the coast.
                const float s = c.score + (chosen.empty() ? 0.0f : 0.004f * std::min(md, R)) + (used_biome || poor ? 0.0f : 0.5f) +
                                (c.coast && !coast_taken ? 1.0f : 0.0f);
                if (s > bs) {
                    bs = s;
                    best = &c;
                }
            }
            if (best) chosen.push_back(*best);
            else spacing *= 0.85f;
        }
        std::vector<RandomIsland::Flat> plateaus;
        for (const SiteCand& c : chosen) {
            const int i = c.n % gw, j = c.n / gw;
            // The nearest fresh water: walk the water-distance field down.
            int w = c.n;
            for (int guard = 0; guard < 40 && wdist[(size_t)w] > 0; ++guard) {
                const int a0 = w % gw, b0 = w / gw;
                int nb = w;
                for (int k = 0; k < 4; ++k) {
                    const int a = a0 + kD8[k][0], b = b0 + kD8[k][1];
                    if (a < 0 || b < 0 || a >= gw || b >= gd) continue;
                    const int m = I.node(a, b);
                    if (wdist[(size_t)m] < wdist[(size_t)nb]) nb = m;
                }
                if (nb == w) break;
                w = nb;
            }
            const float sx = I.nx(i), sz = I.nx(j);
            float wx = I.nx(w % gw), wz = I.nx(w / gw);
            const int wl = I.kind[(size_t)w] == kLake ? I.lake_level[(size_t)I.lake_of[(size_t)w] - 1] : W[(size_t)w];
            const float plateau = std::max((float)std::lround(I.H[(size_t)c.n]), (float)wl + 3.0f);
            const float dl = std::max(1.0f, dist2d(sx, sz, wx, wz));
            const float fx = wx + (sx - wx) / dl * std::min(14.0f, dl * 0.45f);
            const float fz = wz + (sz - wz) / dl * std::min(14.0f, dl * 0.45f);
            plateaus.push_back({sx, sz, plateau + 0.5f, 40.0f, 16.0f});
            I.flats.push_back({fx, fz, (float)wl + 1.5f, 24.0f, 0.0f});
            Site s;
            s.center = Vec3i{(i32)std::lround(sx), 0, (i32)std::lround(sz)};
            s.farms = Vec3i{(i32)std::lround(fx), 0, (i32)std::lround(fz)};
            s.water = Vec3i{(i32)std::lround(wx), wl, (i32)std::lround(wz)};
            s.biome = c.biome;
            feat_.sites.push_back(s);
        }
        for (const auto& f : plateaus) I.flats.push_back(f);
    }

    // ------------------------------------------------------------ islands and features
    IslandDef main;
    main.cx = cx;
    main.cz = cz;
    main.radius = I.sea ? I.Rs / 1.3f + 4.0f : R * 1.35f;
    main.base_h = (float)SL + 10.0f;
    main.thickness = std::min(R * 0.6f, 118.0f) + 20.0f;
    main.salt = hash_combine(cfg.seed, 1);
    main.main = true;
    islands_.push_back(main);
    // Small floating islets beyond the island (with a sea: beyond its rim, in the corners).
    for (int k = 0; k < cfg.islet_count; ++k) {
        IslandDef is;
        is.radius = rng.uniform(16.0f, 30.0f);
        const float ang = (I.sea ? kPi * 0.25f + kPi * 0.5f * (float)(k % 4) : 2.0f * kPi * (float)k / (float)cfg.islet_count) +
                          rng.uniform(-0.25f, 0.25f);
        const float half = (float)std::min(I.W, I.D) * 0.5f;
        float dist = I.sea ? I.Rs + is.radius + rng.uniform(40.0f, 90.0f) : R * 1.3f + rng.uniform(50.0f, 110.0f);
        dist = std::min(dist, (I.sea ? half * 1.38f : half) - is.radius * 1.3f - 8.0f);
        is.cx = cx + std::cos(ang) * dist;
        is.cz = cz + std::sin(ang) * dist;
        if (is.cx - is.radius * 1.3f < 4 || is.cz - is.radius * 1.3f < 4 || is.cx + is.radius * 1.3f > I.W - 4 ||
            is.cz + is.radius * 1.3f > I.D - 4)
            continue;
        if (I.sea && dist2d(is.cx, is.cz, cx, cz) - is.radius * 1.3f < I.Rs + 10.0f) continue;
        is.base_h = (float)SL + rng.uniform(-10.0f, 40.0f);
        is.thickness = is.radius * 1.3f;
        is.salt = hash_combine(cfg.seed, 100 + (u64)k);
        islands_.push_back(is);
    }
    const int rk = clampv(cfg.richness, 0, 2);
    rich_trees_ = rk == 0 ? 0.75f : (rk == 1 ? 1.0f : 1.2f);
    rich_plants_ = rk == 0 ? 0.55f : (rk == 1 ? 1.0f : 1.5f);
    rich_ores_ = I.rich;
    has_sea_ = I.sea;
    sea_cx_ = cx;
    sea_cz_ = cz;
    sea_r2_ = I.Rs * I.Rs;
    rnd_ = isl;
    col_cache_.clear();

    for (Site& s : feat_.sites) {
        s.center.y = column(s.center.x, s.center.z).top;
        s.farms.y = column(s.farms.x, s.farms.z).top;
        // The water point on a column that really holds fresh water.
        Vec3i best = s.water;
        i64 bd = 1LL << 60;
        for (int dz = -8; dz <= 8; ++dz)
            for (int dx = -8; dx <= 8; ++dx) {
                const ColumnInfo c = column(s.water.x + dx, s.water.z + dz);
                if (c.water_top < 0 || c.sea || c.frozen) continue;
                const Vec3i p{s.water.x + dx, c.water_top, s.water.z + dz};
                const i64 d = p.dist2(s.center);
                if (d < bd) {
                    bd = d;
                    best = p;
                }
            }
        s.water = best;
        feat_.waters.push_back(best);
    }
    for (size_t k = 0; k < I.lake_level.size(); ++k) {
        // A point of each lake (its deepest node).
        int best = -1;
        for (size_t n = 0; n < N; ++n)
            if (I.lake_of[n] == k + 1 && (best < 0 || I.H[n] < I.H[(size_t)best])) best = (int)n;
        if (best >= 0)
            feat_.waters.push_back(Vec3i{(i32)std::lround(I.nx(best % gw)), I.lake_level[k], (i32)std::lround(I.nx(best / gw))});
    }
    {
        int hi = 0;
        for (size_t n = 0; n < N; ++n)
            if (I.kind[n] == kLand && I.H[n] > I.H[(size_t)hi]) hi = (int)n;
        feat_.mountain = Vec3i{(i32)std::lround(I.nx(hi % gw)), 0, (i32)std::lround(I.nx(hi / gw))};
        feat_.mountain.y = column(feat_.mountain.x, feat_.mountain.z).top;
    }
    if (!feat_.sites.empty()) {
        const Site& home = feat_.sites[0];
        feat_.village = home.center;
        feat_.farms = home.farms;
        feat_.lake = home.water;
        feat_.pond = home.water;
    }
    feat_.lake_radius = feat_.pond_radius = 10;
    feat_.bridge_a = feat_.bridge_b = feat_.ravine_end = feat_.village;
}

// Raw columns of one cell column and a one-column rim, each worked out when first
// needed (the neighbour rules look at the four next door).
struct WorldGen::RawBlock {
    static constexpr int N = kCellSize + 2;
    int x0 = 0, z0 = 0;  // world position of entry (0, 0)
    std::array<RandomIsland::Raw, N * N> r;
    std::array<bool, N * N> done{};
};

void WorldGen::column_block_random(int cx, int cz, ColumnBlock& out) const {
    auto rb = std::make_unique<RawBlock>();
    rb->x0 = cx * kCellSize - 1;
    rb->z0 = cz * kCellSize - 1;
    for (int lz = 0; lz < kCellSize; ++lz)
        for (int lx = 0; lx < kCellSize; ++lx)
            out.cols[lz * kCellSize + lx] = column_random(cx * kCellSize + lx, cz * kCellSize + lz, rb.get());
}

ColumnInfo WorldGen::compute_column_random(int xi, int zi) const { return column_random(xi, zi, nullptr); }

ColumnInfo WorldGen::column_random(int xi, int zi, RawBlock* rb) const {
    ColumnInfo c;
    const RandomIsland& I = *rnd_;
    const float x = (float)xi + 0.5f, z = (float)zi + 0.5f;
    // Islets floating beyond the island.
    for (size_t ii = 1; ii < islands_.size(); ++ii) {
        const IslandDef& is = islands_[ii];
        const float d = dist2d(x, z, is.cx, is.cz);
        if (d > is.radius * 1.3f) continue;
        const float r = d / (is.radius * (1.0f + 0.18f * fbm2(is.salt, x / 70.0f, z / 70.0f, 3)));
        if (r >= 1.0f) continue;
        float h = is.base_h + 2.5f * fbm2(is.salt + 1, x / 45.0f, z / 45.0f, 3) + 1.2f * fbm2(is.salt + 2, x / 12.0f, z / 12.0f, 2);
        if (r > 0.80f) {
            const float e = (r - 0.80f) / 0.20f;
            h -= e * e * 7.0f;
        }
        const float bottom = is.base_h - is.thickness * std::pow(std::max(0.0f, 1.0f - r), 1.25f) - 4.0f;
        c.land = true;
        c.top = (i16)std::floor(h);
        c.bottom = (i16)clampv((int)std::floor(bottom), 0, (int)c.top);
        c.biome = Biome::Islet;
        c.island = (u8)ii;
        return c;
    }
    auto raw = [&](int x, int z) -> RandomIsland::Raw {
        if (!rb) return I.raw(x, z);
        const int i = (z - rb->z0) * RawBlock::N + (x - rb->x0);
        if (!rb->done[(size_t)i]) {
            rb->r[(size_t)i] = I.raw(x, z);
            rb->done[(size_t)i] = true;
        }
        return rb->r[(size_t)i];
    };
    RandomIsland::Raw r = raw(xi, zi);
    if (!r.land) return c;
    int top = r.top;
    int water = r.water;
    bool weir = false, bank = false;
    // Neighbour rules: never lower than the water next door; lower water next to higher
    // water becomes a weir holding it.
    if (I.near_water[(size_t)I.nearest(x, z)]) {
        int hi = -1;
        static const int k4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto& d : k4) {
            const int nx = xi + d[0], nz = zi + d[1];
            if (has_sea_ && sea_wall(nx, nz)) continue;
            const RandomIsland::Raw n = raw(nx, nz);
            if (n.land && n.water > hi) hi = n.water;
        }
        if (water < 0 && hi > top) {
            top = hi;
            bank = true;
        } else if (water >= 0 && hi > water) {
            top = hi;
            water = -1;
            weir = true;
        } else if (water < 0 && hi >= 0) {
            bank = true;
        }
    }
    const RandomIsland::Raw& R0 = r;
    const float alt = (float)top - (float)(I.SL + 10);
    float T = I.sample(I.T, x, z) + 0.03f * fbm2(I.salt + 91, x / 40.0f, z / 40.0f, 2);
    float Mo = I.sample(I.M, x, z) + 0.04f * fbm2(I.salt + 92, x / 40.0f, z / 40.0f, 2);
    T -= std::max(0.0f, (float)top - I.sample(I.H, x, z)) / 120.0f;  // (lapse beyond the coarse field)
    Biome biome;
    if (R0.sea && water >= 0) biome = Biome::Ocean;
    else if (R0.lake >= 0 && water >= 0) biome = Biome::Lakeshore;
    else if (I.sea && top <= I.SL + 2 && I.near_sea[(size_t)I.nearest(x, z)] && !R0.river && !R0.bank) biome = Biome::Beach;
    else biome = classify(T, saturate(Mo), alt);
    // Lake shores and big rivers' banks.
    if (biome != Biome::Ocean && biome != Biome::Beach && biome != Biome::Highland && water < 0 && R0.lake < 0) {
        int cn[4];
        I.corners(x, z, cn);
        for (int k = 0; k < 4; ++k)
            if (I.kind[(size_t)cn[k]] == kLake) biome = Biome::Lakeshore;
    }
    const float rr = dist2d(x, z, I.cx, I.cz) / std::max(1.0f, I.R);
    const float cone = std::min(I.R * 0.6f, 118.0f) * std::pow(std::max(0.0f, 1.0f - rr * 0.85f), 1.3f);
    float bottom = (float)I.SL - 10.0f - cone + 5.0f * fbm2(I.salt + 31, x / 25.0f, z / 25.0f, 3);
    if (R0.sea) bottom = std::min(bottom, (float)top - 8.0f - 3.0f * (1.0f + fbm2(I.salt + 32, x / 20.0f, z / 20.0f, 2)));
    bottom = std::min(bottom, (float)top - 5.0f);

    c.land = true;
    c.top = (i16)clampv(top, 1, cfg_.cells_y * kCellSize - 2);
    c.bottom = (i16)clampv((int)std::floor(bottom), 0, (int)c.top);
    c.water_top = (i16)water;
    c.biome = biome;
    c.island = 0;
    c.sea = R0.sea && water >= 0;
    c.weir = weir;
    c.reserved = R0.reserved || weir || (bank && water < 0 && !R0.sea);
    c.frozen = R0.lake >= 0 && water >= 0 && T < 0.22f;
    c.temp = unit_u8(T);
    c.moist = unit_u8(saturate(Mo));
    return c;
}

void WorldGen::generate_cell_random(const Vec3i& cc, Voxel* out) const {
    const RandomIsland& I = *rnd_;
    const CoreMats& M = reg_->m();
    const Voxel air = make_voxel(M.air);
    const ColumnBlock& cb = column_block(cc.x, cc.z);
    const int y0 = cc.y * kCellSize;
    const float base = (float)(I.SL + 10);
    auto pick = [&](MatId m, MatId fallback) { return m != M.air ? m : fallback; };
    const MatId snow = pick(M.snow, M.grass), mud = pick(M.mud, M.dirt), red_sand = pick(M.red_sand, M.sand);
    const MatId sandstone = pick(M.sandstone, M.stone), granite = pick(M.granite, M.stone);
    const MatId limestone = pick(M.limestone, M.stone), flint = pick(M.flint, M.stone);
    const MatId dry_grass = pick(M.dry_grass, M.grass), ice = pick(M.ice, M.water);

    for (int lz = 0; lz < kCellSize; ++lz)
        for (int lx = 0; lx < kCellSize; ++lx) {
            const ColumnInfo& col = cb.cols[lz * kCellSize + lx];
            const int wx = cc.x * kCellSize + lx, wz = cc.z * kCellSize + lz;
            if (!col.land) {
                for (int ly = 0; ly < kCellSize; ++ly) out[local_index(lx, ly, lz)] = air;
                continue;
            }
            const IslandDef& is = islands_[col.island];
            const float alt = (float)col.top - base;
            const float tmp = (float)col.temp / 255.0f;
            const float lime = fbm2(is.salt + 81, wx / 60.0f, wz / 60.0f, 2);
            const float patch = gradient_noise2(is.salt + 43, wx / 7.0f, wz / 7.0f);
            const float cn = gradient_noise2(is.salt + 41, wx / 6.0f, wz / 6.0f);
            auto rock_at = [&](int depth, int y) -> MatId {
                if (col.biome == Biome::Highland || alt > 30.0f) return granite;
                if ((col.biome == Biome::Desert || col.biome == Biome::Savanna || col.biome == Biome::Beach) && depth < 12)
                    return sandstone;
                if ((col.biome == Biome::Grassland || col.biome == Biome::Forest || col.biome == Biome::Wetland) &&
                    lime > 0.05f && depth >= 3 && depth < 14) {
                    const u64 hh = hash3(cfg_.seed ^ 0xF117, wx, y, wz);
                    return (hh % 100) < (u64)std::lround(3.0f * I.rich) ? flint : limestone;
                }
                return M.stone;
            };
            for (int ly = 0; ly < kCellSize; ++ly) {
                const int y = y0 + ly;
                Voxel v = air;
                if (y >= col.bottom && y <= col.top) {
                    const int depth = col.top - y;
                    MatId m = rock_at(depth, y);
                    if (y - col.bottom < 3) {
                        m = M.stone;
                    } else if (col.weir) {
                        if (depth <= 1) m = M.stone;
                    } else if (col.water_top >= 0) {
                        if (col.sea) {
                            if (depth <= 2) m = col.water_top - col.top > 6 ? (cn > 0.1f ? M.gravel : M.sand) : M.sand;
                            else if (depth <= 4) m = cn > 0.3f ? M.clay : M.sand;
                        } else if (depth <= 1) {
                            m = cn > 0.25f ? M.clay : (col.biome == Biome::Wetland ? mud : (cn < -0.3f ? M.gravel : M.sand));
                        } else if (depth <= 3) {
                            m = cn > 0.0f ? M.clay : M.dirt;
                        }
                    } else {
                        switch (col.biome) {
                            case Biome::Beach:
                                if (depth <= 3) m = M.sand;
                                break;
                            case Biome::Lakeshore:
                                if (depth <= 2) m = M.sand;
                                else if (depth <= 4) m = M.dirt;
                                break;
                            case Biome::Highland:
                                if (depth == 0 && (alt > 56.0f + 6.0f * patch || tmp < 0.2f)) m = snow;
                                else if (patch > 0.15f && alt < 48.0f) {
                                    if (depth == 0) m = M.grass;
                                    else if (depth <= 1) m = M.dirt;
                                } else if (depth <= 1 && patch < -0.35f) {
                                    m = M.gravel;
                                }
                                break;
                            case Biome::Snowfield:
                                if (depth == 0) m = snow;
                                else if (depth <= 3) m = M.dirt;
                                break;
                            case Biome::Taiga:
                                if (depth == 0) m = (tmp < 0.31f && patch > 0.0f) ? snow : M.grass;
                                else if (depth <= 3) m = M.dirt;
                                break;
                            case Biome::Desert:
                                if (depth <= 3) m = red_sand;
                                break;
                            case Biome::Savanna:
                                if (depth == 0) m = patch > 0.45f ? red_sand : dry_grass;
                                else if (depth <= 3) m = M.dirt;
                                break;
                            case Biome::Wetland:
                                if (depth == 0) m = patch > -0.1f ? M.grass : mud;
                                else if (depth <= 2) m = patch > 0.0f ? mud : M.clay;
                                else if (depth <= 4) m = M.dirt;
                                break;
                            default:
                                if (depth == 0) m = M.grass;
                                else if (depth <= 3) m = (depth <= 2 && cn > 0.55f) ? M.clay : M.dirt;
                                break;
                        }
                    }
                    v = make_voxel(m);
                } else if (col.water_top >= 0 && y > col.top && y <= col.water_top) {
                    v = (col.frozen && y == col.water_top) ? make_voxel(ice) : make_voxel(M.water, kFluidFull);
                }
                out[local_index(lx, ly, lz)] = v;
            }
        }

    // A levistone core holds the island (and each islet) in the sky.
    for (const auto& is : islands_) {
        const float ccx = is.cx, ccz = is.cz;
        const float ccy = is.main ? (float)I.SL - 40.0f : is.base_h - is.thickness * 0.45f;
        const float rx = is.main ? I.R * 0.16f : std::max(4.0f, is.radius * 0.28f);
        const float ry = is.main ? 26.0f : std::max(3.0f, is.radius * 0.2f);
        const int bx0 = (int)std::floor(ccx - rx) - cc.x * kCellSize, bx1 = (int)std::ceil(ccx + rx) - cc.x * kCellSize;
        const int by0 = (int)std::floor(ccy - ry) - y0, by1 = (int)std::ceil(ccy + ry) - y0;
        const int bz0 = (int)std::floor(ccz - rx) - cc.z * kCellSize, bz1 = (int)std::ceil(ccz + rx) - cc.z * kCellSize;
        if (bx1 < 0 || by1 < 0 || bz1 < 0 || bx0 >= kCellSize || by0 >= kCellSize || bz0 >= kCellSize) continue;
        for (int ly = std::max(0, by0); ly <= std::min(kCellSize - 1, by1); ++ly)
            for (int lz = std::max(0, bz0); lz <= std::min(kCellSize - 1, bz1); ++lz)
                for (int lx = std::max(0, bx0); lx <= std::min(kCellSize - 1, bx1); ++lx) {
                    const float dx = (cc.x * kCellSize + lx + 0.5f - ccx) / rx;
                    const float dy = (y0 + ly + 0.5f - ccy) / ry;
                    const float dz = (cc.z * kCellSize + lz + 0.5f - ccz) / rx;
                    const float n = 0.15f * gradient_noise3(is.salt + 51, (cc.x * kCellSize + lx) / 6.0f, (y0 + ly) / 6.0f,
                                                            (cc.z * kCellSize + lz) / 6.0f);
                    if (dx * dx + dy * dy + dz * dz < 1.0f + n) {
                        Voxel& v = out[local_index(lx, ly, lz)];
                        if (vmat(v) != M.air && vmat(v) != M.water) v = make_voxel(M.levistone);
                    }
                }
    }

    place_ores(cc, out);
    place_trees_continent(cc, out);
    place_plants_continent(cc, out);
}

std::vector<u32> WorldGen::preview(int px) const {
    std::vector<u32> img((size_t)px * (size_t)px, 0x0B1220);
    const float W = (float)(cfg_.cells_x * kCellSize), D = (float)(cfg_.cells_z * kCellSize);
    auto biome_color = [](Biome b) -> u32 {
        switch (b) {
            case Biome::Grassland: return 0x6FA64A;
            case Biome::Forest: return 0x3E7D35;
            case Biome::Highland: return 0x8C8577;
            case Biome::Lakeshore: return 0xC9B98A;
            case Biome::Islet: return 0x6F9E55;
            case Biome::Taiga: return 0x4C7A55;
            case Biome::Snowfield: return 0xE6ECEF;
            case Biome::Desert: return 0xD49A5B;
            case Biome::Savanna: return 0xB8A55A;
            case Biome::Wetland: return 0x5A7A4A;
            case Biome::Beach: return 0xE2D29C;
            default: return 0x6FA64A;
        }
    };
    auto shade = [](u32 c, float k) {
        const int r = clampv((int)((float)((c >> 16) & 255) * k), 0, 255);
        const int g = clampv((int)((float)((c >> 8) & 255) * k), 0, 255);
        const int b = clampv((int)((float)(c & 255) * k), 0, 255);
        return (u32)((r << 16) | (g << 8) | b);
    };
    std::vector<int> tops((size_t)px * (size_t)px, -1);
    for (int j = 0; j < px; ++j)
        for (int i = 0; i < px; ++i) {
            const int x = (int)(((float)i + 0.5f) * W / (float)px), z = (int)(((float)j + 0.5f) * D / (float)px);
            const ColumnInfo c = compute_column(x, z);
            if (!c.land) continue;
            u32 col;
            if (c.water_top >= 0 && !c.frozen) {
                const int depth = c.water_top - c.top;
                col = c.sea ? shade(0x2A6FA8, 1.05f - 0.035f * (float)std::min(depth, 14)) : 0x3C8CC4;
            } else if (c.frozen) {
                col = 0xCFE6F2;
            } else {
                col = biome_color(c.biome);
                const float k = 0.8f + 0.5f * saturate((float)(c.top - cfg_.sea_level) / 110.0f);
                col = shade(col, k);
                if (c.weir) col = 0x9A9A9A;
            }
            img[(size_t)(j * px + i)] = col;
            tops[(size_t)(j * px + i)] = c.water_top >= 0 ? c.water_top : c.top;
        }
    // Hill shading from the north-west.
    for (int j = px - 1; j >= 1; --j)
        for (int i = px - 1; i >= 1; --i) {
            const int t = tops[(size_t)(j * px + i)], tn = tops[(size_t)((j - 1) * px + i - 1)];
            if (t < 0 || tn < 0) continue;
            img[(size_t)(j * px + i)] = shade(img[(size_t)(j * px + i)], clampv(1.0f + 0.05f * (float)(t - tn) * (float)px / 256.0f, 0.7f, 1.3f));
        }
    for (const Site& s : feat_.sites) {
        const int i = (int)((float)s.center.x / W * (float)px), j = (int)((float)s.center.z / D * (float)px);
        for (int b = -2; b <= 2; ++b)
            for (int a = -2; a <= 2; ++a) {
                const int x = i + a, y = j + b;
                if (x < 0 || y < 0 || x >= px || y >= px) continue;
                img[(size_t)(y * px + x)] = (std::abs(a) == 2 || std::abs(b) == 2) ? 0x1A1A1A : 0xF25C54;
            }
    }
    return img;
}

}  // namespace icarus
