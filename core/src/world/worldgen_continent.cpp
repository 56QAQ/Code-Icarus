// Continent layout: a large floating island with a mountain spine, lakes fed by
// mountain springs and climate-driven biomes (cold north beyond the mountains, a hot and
// dry south-east, wet lowlands in the south-west). Three settlement sites sit in
// different biomes, far enough apart for separate civilisations.
//
// Like the classic layout, everything is a pure function of (seed, coordinates).
#include <cmath>

#include "icarus/util/noise.h"
#include "icarus/util/rng.h"
#include "icarus/world/worldgen.h"

namespace icarus {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int kTreeSlot = 6;  // same grid as the classic layout (see tree_bases)

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

// The climate each site pulls its surroundings toward.
void site_climate(Biome b, float& t, float& m) {
    switch (b) {
        case Biome::Savanna: t = 0.70f, m = 0.42f; break;
        case Biome::Taiga: t = 0.33f, m = 0.56f; break;
        default: t = 0.55f, m = 0.58f; break;
    }
}

Biome classify(float t, float m, float alt) {
    if (alt > 22.0f) return Biome::Highland;
    if (t < 0.24f) return Biome::Snowfield;
    if (t < 0.40f) return Biome::Taiga;
    if (t > 0.74f && m < 0.44f) return Biome::Desert;
    if (t > 0.60f && m < 0.52f) return Biome::Savanna;
    if (m > 0.64f && alt < 4.0f) return Biome::Wetland;
    if (m > 0.54f) return Biome::Forest;
    return Biome::Grassland;
}

inline u8 unit_u8(float v) { return (u8)clampv((int)std::lround(v * 255.0f), 0, 255); }
}  // namespace

void WorldGen::init_continent() {
    const WorldConfig& cfg = cfg_;
    Rng rng(cfg.seed, 0xC0171);
    const float W = (float)(cfg.cells_x * kCellSize);
    const float D = (float)(cfg.cells_z * kCellSize);
    const float R = cfg.island_radius;

    IslandDef main;
    main.cx = W * 0.5f;
    main.cz = D * 0.5f;
    main.radius = R;
    main.base_h = (float)cfg.base_height;
    main.thickness = std::min(R * 0.85f, 110.0f);
    main.salt = hash_combine(cfg.seed, 1);
    main.main = true;
    islands_.push_back(main);

    for (int i = 0; i < cfg.islet_count; ++i) {
        IslandDef is;
        float ang = 2.0f * kPi * (float)i / (float)std::max(1, cfg.islet_count) + rng.uniform(-0.3f, 0.3f);
        is.radius = rng.uniform(18.0f, 34.0f);
        float dist = R + rng.uniform(55.0f, 120.0f);
        float maxd = std::min(W, D) * 0.5f - is.radius * 1.3f - 8.0f;
        dist = std::min(dist, maxd);
        is.cx = main.cx + std::cos(ang) * dist;
        is.cz = main.cz + std::sin(ang) * dist;
        is.base_h = (float)cfg.base_height + rng.uniform(-30.0f, 20.0f);
        is.thickness = is.radius * 1.3f;
        is.salt = hash_combine(cfg.seed, 100 + (u64)i);
        islands_.push_back(is);
    }

    float th = rng.uniform(0.0f, 2.0f * kPi);
    cos0_ = std::cos(th);
    sin0_ = std::sin(th);
    feat_.axis_u_x = cos0_;
    feat_.axis_u_z = sin0_;
    feat_.axis_v_x = -sin0_;
    feat_.axis_v_z = cos0_;
    auto world_x = [&](float lu, float lv) { return main.cx + lu * cos0_ - lv * sin0_; };
    auto world_z = [&](float lu, float lv) { return main.cz + lu * sin0_ + lv * cos0_; };
    auto point = [&](float lu, float lv) {
        return Vec3i{(i32)std::lround(world_x(lu, lv)), 0, (i32)std::lround(world_z(lu, lv))};
    };

    // Mountain spine north of the centre, west to east. The wide gap between the fifth
    // and sixth peaks is a pass; the fourth is the summit.
    const float ts[7] = {0.02f, 0.16f, 0.33f, 0.47f, 0.60f, 0.82f, 0.97f};
    const float phase = rng.uniform(0.0f, 2.0f * kPi);
    for (int i = 0; i < 7; ++i) {
        const float t = ts[i];
        const float lu = lerpf(-0.62f * R, 0.62f * R, t) + rng.uniform(-0.03f, 0.03f) * R;
        const float lv = 0.24f * R + 0.07f * R * std::sin(t * 5.0f + phase);
        Peak p;
        p.x = world_x(lu, lv);
        p.z = world_z(lu, lv);
        p.h = rng.uniform(34.0f, 54.0f);
        p.w = rng.uniform(28.0f, 42.0f);
        if (i == 3) {
            p.h = 76.0f;
            p.w = 46.0f;
        }
        peaks_.push_back(p);
    }

    // Settlement sites, each in its own climate, with a lake and farmland beside it.
    struct SiteDef {
        float u, v, lu, lv, lr;
        Biome b;
    };
    const SiteDef defs[3] = {
        {-0.58f, -0.06f, 0.14f, 0.10f, 14.0f, Biome::Forest},
        {0.30f, -0.56f, -0.15f, 0.09f, 13.0f, Biome::Savanna},
        {0.36f, 0.62f, -0.15f, -0.03f, 13.0f, Biome::Taiga},
    };
    const int wt = (int)main.base_h - 1;
    for (const SiteDef& d : defs) {
        const float su = (d.u + rng.uniform(-0.03f, 0.03f)) * R, sv = (d.v + rng.uniform(-0.03f, 0.03f)) * R;
        const float lu = su + d.lu * R, lv = sv + d.lv * R;
        Site s;
        s.center = point(su, sv);
        s.water = point(lu, lv);
        s.farms = point(lerpf(su, lu, 0.5f), lerpf(sv, lv, 0.5f));
        s.biome = d.b;
        feat_.sites.push_back(s);
        site_climate_.push_back({s.center, d.b});
        Lake l;
        l.x = (float)s.water.x;
        l.z = (float)s.water.z;
        l.r = d.lr;
        l.wt = wt;
        l.salt = main.salt + 200 + lakes_.size();
        lakes_.push_back(l);
    }
    // Other still water: a large central lake, a wetland mere, a frozen lake in the
    // snowfield and a small desert oasis.
    const struct {
        float u, v, r;
        bool frozen;
    } more[4] = {{-0.02f, -0.16f, 20.0f, false}, {-0.40f, -0.52f, 15.0f, false}, {-0.28f, 0.68f, 15.0f, true},
                 {0.64f, -0.36f, 7.0f, false}};
    for (const auto& m : more) {
        Lake l;
        l.x = world_x(m.u * R, m.v * R);
        l.z = world_z(m.u * R, m.v * R);
        l.r = m.r;
        l.wt = wt;
        l.frozen = m.frozen;
        l.salt = main.salt + 200 + lakes_.size();
        lakes_.push_back(l);
    }

    // A spring on the summit's slope feeds a stream down to the central lake, which
    // drains over the rim. (Flowing water is simulated cube by cube; one river keeps
    // that affordable. The other lakes are still.)
    const int links[1][2] = {{3, 3}};
    for (const auto& lk : links) {
        const Peak& p = peaks_[(size_t)lk[0]];
        const Lake& l = lakes_[(size_t)lk[1]];
        float dx = l.x - p.x, dz = l.z - p.z;
        float d = std::sqrt(dx * dx + dz * dz);
        // On the slope, but outside the lake's levelled shore.
        float k = std::min(p.w * 0.85f, std::max(0.0f, d - l.r - 16.0f)) / std::max(1.0f, d);
        Stream s;
        s.spring = Vec3i{(i32)std::lround(p.x + dx * k), 0, (i32)std::lround(p.z + dz * k)};
        s.mouth = Vec3i{(i32)std::lround(l.x), 0, (i32)std::lround(l.z)};
        s.mouth_wt = l.wt;
        streams_.push_back(s);
    }
    // Spring heights depend on the terrain without the stream channels.
    col_cache_.clear();
    for (Stream& s : streams_) s.spring.y = compute_column_continent(s.spring.x, s.spring.z).top - 1;

    // Every spring-fed lake drains to the rim, where its river falls off the island. The
    // river keeps clear of settlements, other lakes and the mountains.
    auto shape_r = [&](float x, float z) {
        const float d = dist2d(x, z, main.cx, main.cz);
        const float n = fbm2(main.salt, x / 170.0f, z / 170.0f, 3);
        const float n2 = fbm2(main.salt + 9, x / 48.0f, z / 48.0f, 2);
        return d / (R * (1.0f + 0.26f * n + 0.07f * n2));
    };
    auto peak_height = [&](float x, float z) {
        float b = 0.0f;
        for (const Peak& pk : peaks_) {
            const float dd = dist2d(x, z, pk.x, pk.z);
            b = std::max(b, pk.h * std::exp(-(dd / pk.w) * (dd / pk.w) * 1.6f));
        }
        return b;
    };
    const size_t springs = streams_.size();
    for (size_t k = 0; k < springs; ++k) {
        const Lake& l = lakes_[(size_t)links[k][1]];
        const float ox = l.x - main.cx, oz = l.z - main.cz;
        const float base_ang = std::atan2(oz, ox);
        const float tries[5] = {0.0f, 0.45f, -0.45f, 0.9f, -0.9f};
        for (float da : tries) {
            const float ang = base_ang + da;
            const float dx = std::cos(ang), dz = std::sin(ang);
            float len = l.r;
            bool ok = true;
            for (; len < 2.2f * R; len += 6.0f) {
                const float px = l.x + dx * len, pz = l.z + dz * len;
                if (shape_r(px, pz) >= 1.0f) break;
                for (const Site& st : feat_.sites)
                    if (dist2d(px, pz, (float)st.center.x, (float)st.center.z) < 50.0f) ok = false;
                for (const Lake& o : lakes_)
                    if (&o != &l && dist2d(px, pz, o.x, o.z) < o.r + 14.0f) ok = false;
                if (peak_height(px, pz) > 8.0f) ok = false;
                if (!ok) break;
            }
            if (!ok) continue;
            Stream s;
            s.outflow = true;
            s.spring = Vec3i{(i32)std::lround(l.x), l.wt, (i32)std::lround(l.z)};
            s.mouth = Vec3i{(i32)std::lround(l.x + dx * (len + 8.0f)), 0, (i32)std::lround(l.z + dz * (len + 8.0f))};
            s.mouth_wt = (int)main.base_h - 14;
            s.shore = l.r + 5.0f;
            streams_.push_back(s);
            break;
        }
    }
    col_cache_.clear();

    for (Site& s : feat_.sites) {
        s.center.y = column(s.center.x, s.center.z).top;
        s.farms.y = column(s.farms.x, s.farms.z).top;
        s.water.y = wt;
    }
    const Peak& summit = peaks_[3];
    feat_.mountain = Vec3i{(i32)std::lround(summit.x), 0, (i32)std::lround(summit.z)};
    feat_.mountain.y = column(feat_.mountain.x, feat_.mountain.z).top;
    for (const Stream& s : streams_)
        if (!s.outflow) feat_.springs.push_back(s.spring);
    for (const Lake& l : lakes_)
        if (!l.frozen) feat_.waters.push_back(Vec3i{(i32)std::lround(l.x), l.wt, (i32)std::lround(l.z)});

    // The first site doubles as the classic "village" for systems that know only one.
    const Site& home = feat_.sites[0];
    feat_.village = home.center;
    feat_.farms = home.farms;
    feat_.lake = home.water;
    feat_.pond = home.water;
    feat_.lake_radius = (int)lakes_[0].r;
    feat_.pond_radius = (int)lakes_[0].r;
    feat_.spring = feat_.springs.empty() ? Vec3i{} : feat_.springs[0];
    feat_.bridge_a = feat_.bridge_b = feat_.ravine_end = home.center;
}

ColumnInfo WorldGen::compute_column_continent(int xi, int zi) const {
    ColumnInfo c;
    const float x = (float)xi + 0.5f, z = (float)zi + 0.5f;
    for (size_t ii = 0; ii < islands_.size(); ++ii) {
        const IslandDef& is = islands_[ii];
        const float d = dist2d(x, z, is.cx, is.cz);
        if (d > is.radius * 1.4f) continue;
        float r;
        if (is.main) {
            const float n = fbm2(is.salt, x / 170.0f, z / 170.0f, 3);
            const float n2 = fbm2(is.salt + 9, x / 48.0f, z / 48.0f, 2);
            r = d / (is.radius * (1.0f + 0.26f * n + 0.07f * n2));
        } else {
            r = d / (is.radius * (1.0f + 0.18f * fbm2(is.salt, x / 70.0f, z / 70.0f, 3)));
        }
        if (r >= 1.0f) continue;

        const float base = is.base_h;
        float top;
        Biome biome = Biome::Islet;
        bool reserved = false, frozen = false;
        int water_top = -1;
        float T = 0.5f, Mo = 0.5f;

        if (!is.main) {
            float h = base + 2.5f * fbm2(is.salt + 1, x / 45.0f, z / 45.0f, 3) +
                      1.2f * fbm2(is.salt + 2, x / 12.0f, z / 12.0f, 2);
            if (r > 0.80f) {
                float e = (r - 0.80f) / 0.20f;
                h -= e * e * 7.0f;
            }
            top = std::floor(h);
        } else {
            const float R = is.radius;
            float lu, lv;
            to_local(x, z, lu, lv);
            float h = base + 8.0f * fbm2(is.salt + 1, x / 110.0f, z / 110.0f, 3) +
                      3.0f * fbm2(is.salt + 2, x / 35.0f, z / 35.0f, 3) + 0.9f * fbm2(is.salt + 3, x / 11.0f, z / 11.0f, 2);
            if (r > 0.82f) {
                float e = (r - 0.82f) / 0.18f;
                h -= e * e * 9.0f;
            }
            // Mountains: the highest peak nearby decides (max, not sum), so the gaps
            // between peaks stay passes.
            float bump = 0.0f;
            for (const Peak& pk : peaks_) {
                const float dd = dist2d(x, z, pk.x, pk.z);
                if (dd > pk.w * 2.4f) continue;
                bump = std::max(bump, pk.h * std::exp(-(dd / pk.w) * (dd / pk.w) * 1.6f));
            }
            // A ridge joins the peaks into one range; it dips to a low pass between the
            // fifth and sixth peaks and to a saddle between the second and third.
            for (size_t i = 0; i + 1 < peaks_.size(); ++i) {
                const Peak& a = peaks_[i];
                const Peak& b = peaks_[i + 1];
                float t;
                const float dd = seg_dist(x, z, a.x, a.z, b.x, b.z, t);
                if (dd > 44.0f) continue;
                float amp = lerpf(a.h, b.h, t) * 0.62f;
                if (i == 4) amp *= 1.0f - 0.9f * std::sin(t * kPi);
                else if (i == 1) amp *= 1.0f - 0.5f * std::sin(t * kPi);
                const float wd = 22.0f + 7.0f * gradient_noise2(is.salt + 7, x / 40.0f, z / 40.0f);
                bump = std::max(bump, amp * std::exp(-(dd / wd) * (dd / wd) * 1.4f));
            }
            bump *= 0.78f + 0.44f * ridged2(is.salt + 4, x / 34.0f, z / 34.0f, 3);
            top = h + bump;

            // Climate: cold to the north (+v), hot to the south, wetter in the west (-u)
            // and around lakes. Sites pull their surroundings toward their own climate.
            T = 0.55f - 0.5f * (lv / R) + 0.12f * fbm2(is.salt + 61, x / 130.0f, z / 130.0f, 3);
            Mo = 0.5f + 0.30f * fbm2(is.salt + 67, x / 120.0f, z / 120.0f, 3) - 0.22f * (lu / R);
            for (const Lake& l : lakes_) {
                const float dd = dist2d(x, z, l.x, l.z);
                if (dd < l.r + 30.0f) Mo += 0.15f * (1.0f - saturate((dd - l.r) / 30.0f));
            }
            for (const auto& sc : site_climate_) {
                const float dd = dist2d(x, z, (float)sc.first.x, (float)sc.first.z);
                const float w = 1.0f - smoothstep(35.0f, 75.0f, dd);
                if (w <= 0.0f) continue;
                float st, sm;
                site_climate(sc.second, st, sm);
                T = lerpf(T, st, w);
                Mo = lerpf(Mo, sm, w);
            }
            // Wet lowland is flat (its meres need level banks). Everything at or above
            // the full-wetness threshold (0.66) sits exactly one below the base height.
            const bool wet_climate = T > 0.43f;
            if (wet_climate && bump < 2.0f && Mo > 0.60f) {
                const float w = saturate((Mo - 0.60f) / 0.06f);
                top = lerpf(top, base - 1.0f, w);
            }
            auto flatten = [&](float cx, float cz, float rad, float target, float keep_clear) {
                const float dd = dist2d(x, z, cx, cz);
                const float w = 1.0f - smoothstep(rad * 0.65f, rad, dd);
                top = lerpf(top, target, w);
                if (dd < keep_clear) reserved = true;
            };
            for (const Site& s : feat_.sites) {
                flatten((float)s.center.x, (float)s.center.z, 36.0f, base + 1.0f, 16.0f);
                flatten((float)s.farms.x, (float)s.farms.z, 22.0f, base, 12.0f);
            }
            for (const Lake& l : lakes_) flatten(l.x, l.z, l.r + 8.0f, base, 0.0f);
            float ft = std::floor(top);
            const float alt = ft - base;
            T -= std::max(0.0f, alt - 6.0f) / 120.0f;
            biome = classify(T, Mo, alt);

            // Lake basins.
            for (const Lake& l : lakes_) {
                const float dd = dist2d(x, z, l.x, l.z);
                const float rr = l.r * (1.0f + 0.12f * gradient_noise2(l.salt, x / 9.0f, z / 9.0f));
                if (dd < rr) {
                    const float depth = 1.0f + 5.0f * (1.0f - (dd / rr) * (dd / rr));
                    ft = std::min(ft, (float)l.wt - std::floor(depth));
                    water_top = l.wt;
                    frozen = l.frozen;
                    if (!l.frozen) biome = Biome::Lakeshore;
                } else if (dd < rr + 5.0f && !l.frozen && biome != Biome::Highland) {
                    biome = Biome::Lakeshore;
                }
            }
            // Meres in the wet lowland: one or two cubes deep, level with the banks.
            if (water_top < 0 && T > 0.46f && Mo > 0.69f && bump < 2.0f && r < 0.9f &&
                fbm2(is.salt + 71, x / 11.0f, z / 11.0f, 2) > 0.12f) {
                water_top = (int)base - 1;
                ft = (float)water_top - (fbm2(is.salt + 72, x / 5.0f, z / 5.0f, 1) > 0.1f ? 2.0f : 1.0f);
            }
            // Streams from the springs; banks are raised where the land dips, so the
            // water keeps to its bed.
            for (const Stream& s : streams_) {
                if (s.spring.y <= 0) continue;
                float t;
                const float ds = seg_dist(x, z, (float)s.spring.x, (float)s.spring.z, (float)s.mouth.x,
                                          (float)s.mouth.z, t);
                if (ds >= 3.6f) continue;
                float bed;
                if (s.outflow) {
                    const float len = dist2d((float)s.spring.x, (float)s.spring.z, (float)s.mouth.x, (float)s.mouth.z);
                    const float k = saturate((t * len - s.shore) / std::max(1.0f, len - s.shore));
                    bed = (float)s.spring.y - std::floor(k * (float)(s.spring.y - s.mouth_wt));
                } else {
                    bed = std::floor(lerpf((float)s.spring.y, (float)s.mouth_wt, smoothstep(0.0f, 0.85f, t)));
                }
                const bool in_lake = water_top >= 0;
                if (ds < 1.6f) {
                    if (!in_lake) ft = std::min(ft, bed);
                    reserved = true;
                } else if (!in_lake) {
                    ft = std::max(ft, bed + 1.0f);
                    reserved = true;
                }
            }
            top = ft;
        }

        float bottom = base - is.thickness * std::pow(std::max(0.0f, 1.0f - r), 1.25f) - 8.0f +
                       6.0f * fbm2(is.salt + 5, x / 25.0f, z / 25.0f, 3);
        bottom = std::min(bottom, top - 5.0f);

        c.land = true;
        c.top = (i16)clampv((int)top, 1, cfg_.cells_y * kCellSize - 2);
        c.bottom = (i16)clampv((int)std::floor(bottom), 0, (int)c.top);
        c.water_top = (i16)water_top;
        c.biome = biome;
        c.island = (u8)ii;
        c.reserved = reserved;
        c.frozen = frozen;
        c.temp = unit_u8(T);
        c.moist = unit_u8(Mo);
        return c;
    }
    return c;
}

void WorldGen::generate_cell_continent(const Vec3i& cc, Voxel* out) const {
    const CoreMats& M = reg_->m();
    const Voxel air = make_voxel(M.air);
    const ColumnBlock& cb = column_block(cc.x, cc.z);
    const int y0 = cc.y * kCellSize;
    const float base = (float)cfg_.base_height;
    auto pick = [&](MatId m, MatId fallback) { return m != M.air ? m : fallback; };
    const MatId snow = pick(M.snow, M.grass), mud = pick(M.mud, M.dirt), red_sand = pick(M.red_sand, M.sand);
    const MatId sandstone = pick(M.sandstone, M.stone), granite = pick(M.granite, M.stone);
    const MatId limestone = pick(M.limestone, M.stone), flint = pick(M.flint, M.stone);
    const MatId dry_grass = pick(M.dry_grass, M.grass), ice = pick(M.ice, M.water);

    for (int lz = 0; lz < kCellSize; ++lz) {
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
            // Bedrock of the column: granite under the mountains, sandstone in the dry
            // south, limestone beds under the green lowlands (with flint nodules).
            const float lime = fbm2(is.salt + 81, wx / 60.0f, wz / 60.0f, 2);
            const float patch = gradient_noise2(is.salt + 43, wx / 7.0f, wz / 7.0f);
            auto rock_at = [&](int depth, int y) -> MatId {
                if (col.biome == Biome::Highland || alt > 22.0f) return granite;
                if ((col.biome == Biome::Desert || col.biome == Biome::Savanna) && depth < 12) return sandstone;
                if ((col.biome == Biome::Grassland || col.biome == Biome::Forest || col.biome == Biome::Wetland) &&
                    lime > 0.05f && depth >= 3 && depth < 14) {
                    const u64 hh = hash3(cfg_.seed ^ 0xF117, wx, y, wz);
                    return (hh % 100) < 3 ? flint : limestone;
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
                    } else if (col.water_top >= 0) {
                        const float cn = gradient_noise2(is.salt + 41, wx / 6.0f, wz / 6.0f);
                        if (depth <= 1) m = cn > 0.25f ? M.clay : (col.biome == Biome::Wetland ? mud : M.sand);
                        else if (depth <= 3) m = cn > 0.0f ? M.clay : M.dirt;
                    } else {
                        switch (col.biome) {
                            case Biome::Lakeshore:
                                if (depth <= 2) m = M.sand;
                                else if (depth <= 4) m = M.dirt;
                                break;
                            case Biome::Highland:
                                if (depth == 0 && (alt > 48.0f + 6.0f * patch || tmp < 0.2f)) m = snow;
                                else if (patch > 0.15f && alt < 40.0f) {
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
                                else if (depth <= 3) m = M.dirt;
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
    }

    // Levistone cores anchor each island in the sky.
    for (const auto& is : islands_) {
        const float ccx = is.cx, ccz = is.cz;
        const float ccy = is.main ? is.base_h - is.thickness * 0.55f : is.base_h - is.thickness * 0.45f;
        const float rx = is.main ? is.radius * 0.2f : std::max(4.0f, is.radius * 0.28f);
        const float ry = is.main ? 26.0f : std::max(3.0f, is.radius * 0.2f);
        int bx0 = (int)std::floor(ccx - rx) - cc.x * kCellSize, bx1 = (int)std::ceil(ccx + rx) - cc.x * kCellSize;
        int by0 = (int)std::floor(ccy - ry) - y0, by1 = (int)std::ceil(ccy + ry) - y0;
        int bz0 = (int)std::floor(ccz - rx) - cc.z * kCellSize, bz1 = (int)std::ceil(ccz + rx) - cc.z * kCellSize;
        if (bx1 < 0 || by1 < 0 || bz1 < 0 || bx0 >= kCellSize || by0 >= kCellSize || bz0 >= kCellSize) continue;
        for (int ly = std::max(0, by0); ly <= std::min(kCellSize - 1, by1); ++ly)
            for (int lz = std::max(0, bz0); lz <= std::min(kCellSize - 1, bz1); ++lz)
                for (int lx = std::max(0, bx0); lx <= std::min(kCellSize - 1, bx1); ++lx) {
                    const float dx = (cc.x * kCellSize + lx + 0.5f - ccx) / rx;
                    const float dy = (y0 + ly + 0.5f - ccy) / ry;
                    const float dz = (cc.z * kCellSize + lz + 0.5f - ccz) / rx;
                    const float n = 0.15f * gradient_noise3(is.salt + 51, (cc.x * kCellSize + lx) / 6.0f,
                                                            (y0 + ly) / 6.0f, (cc.z * kCellSize + lz) / 6.0f);
                    if (dx * dx + dy * dy + dz * dz < 1.0f + n) {
                        Voxel& v = out[local_index(lx, ly, lz)];
                        if (vmat(v) != M.air) v = make_voxel(M.levistone);
                    }
                }
    }

    place_ores(cc, out);
    place_trees_continent(cc, out);
    place_plants_continent(cc, out);

    for (const Stream& s : streams_)
        if (!s.outflow && s.spring.y > 0 && cell_of(s.spring) == cc)
            out[local_index(s.spring.x & kCellMask, s.spring.y & kCellMask, s.spring.z & kCellMask)] =
                make_voxel(M.spring);
}

bool WorldGen::continent_tree(int sx, int sz, TreeSpec& t) const {
    const int S = kTreeSlot;
    const u64 h = hash3(cfg_.seed ^ 0x7EE5, sx, 0, sz);
    const int tx = sx * S + 1 + (int)(splitmix64(h) % (S - 2));
    const int tz = sz * S + 1 + (int)(splitmix64(h + 9) % (S - 2));
    if (tx < 0 || tz < 0 || tx >= cfg_.cells_x * kCellSize || tz >= cfg_.cells_z * kCellSize) return false;
    const float roll = hash_to_unit(splitmix64(h + 17));
    const float r2 = hash_to_unit(splitmix64(h + 31));
    const ColumnInfo col = column(tx, tz);
    if (!col.land || col.ravine || col.reserved || col.water_top >= 0) return false;
    float p = 0.0f;
    u8 kind = 0;
    switch (col.biome) {
        case Biome::Forest: p = 0.55f, kind = r2 < 0.58f ? 0 : (r2 < 0.85f ? 1 : 2); break;
        case Biome::Grassland: p = 0.06f, kind = r2 < 0.5f ? 0 : (r2 < 0.75f ? 2 : 1); break;
        case Biome::Taiga: p = 0.50f, kind = r2 < 0.85f ? 3 : 1; break;
        case Biome::Snowfield: p = 0.12f, kind = 3; break;
        case Biome::Highland:
            p = col.top < cfg_.base_height + 40 ? 0.08f : 0.0f;
            kind = 3;
            break;
        case Biome::Savanna: p = 0.07f, kind = 4; break;
        case Biome::Wetland: p = 0.14f, kind = r2 < 0.6f ? 0 : 1; break;
        case Biome::Lakeshore: p = 0.03f, kind = 0; break;
        case Biome::Islet: p = 0.30f, kind = r2 < 0.8f ? 0 : 2; break;
        default: p = 0.0f;
    }
    if (roll >= p) return false;
    t.x = tx;
    t.z = tz;
    t.ground = col.top;
    t.kind = kind;
    t.h = h;
    return true;
}

void WorldGen::place_trees_continent(const Vec3i& cc, Voxel* out) const {
    const CoreMats& M = reg_->m();
    const int S = kTreeSlot;
    const int bx0 = cc.x * kCellSize, by0 = cc.y * kCellSize, bz0 = cc.z * kCellSize;
    const int sx0 = floordiv(bx0 - 5, S), sx1 = floordiv(bx0 + kCellSize + 5, S);
    const int sz0 = floordiv(bz0 - 5, S), sz1 = floordiv(bz0 + kCellSize + 5, S);
    auto pick = [&](MatId m, MatId fallback) { return m != M.air ? m : fallback; };
    auto put = [&](int x, int y, int z, MatId m, bool trunk) {
        if (x < bx0 || y < by0 || z < bz0 || x >= bx0 + kCellSize || y >= by0 + kCellSize || z >= bz0 + kCellSize)
            return;
        Voxel& v = out[local_index(x - bx0, y - by0, z - bz0)];
        const MatId cur = vmat(v);
        if (cur == M.air || (trunk && reg_->mat(cur).foliage)) v = make_voxel(m);
    };
    TreeSpec t;
    for (int sz = sz0; sz <= sz1; ++sz)
        for (int sx = sx0; sx <= sx1; ++sx) {
            if (!continent_tree(sx, sz, t)) continue;
            const int g = t.ground;
            if (g + 16 < by0 || g > by0 + kCellSize) continue;
            const u64 h = t.h;
            auto crown_ball = [&](int cx, int cy, int cz, float rx, float ry, MatId leaf, MatId alt_leaf, float alt_share) {
                const int ix = (int)std::ceil(rx), iy = (int)std::ceil(ry);
                for (int dy = -iy; dy <= iy; ++dy)
                    for (int dz = -ix; dz <= ix; ++dz)
                        for (int dx = -ix; dx <= ix; ++dx) {
                            const float d2 = (float)(dx * dx + dz * dz) / (rx * rx) + (float)(dy * dy) / (ry * ry);
                            if (d2 > 1.0f) continue;
                            const u64 lh = hash3(h, dx, dy, dz);
                            if (d2 > 0.62f && (lh & 3) == 0) continue;  // ragged edge
                            const bool alt = alt_share > 0.0f && hash_to_unit(splitmix64(lh)) < alt_share;
                            put(cx + dx, cy + dy, cz + dz, alt ? alt_leaf : leaf, false);
                        }
            };
            switch (t.kind) {
                case 0: {  // broadleaf
                    const int height = 5 + (int)(splitmix64(h + 23) % 4);
                    for (int y = g + 1; y <= g + height; ++y) put(t.x, y, t.z, M.log, true);
                    const float cr = 2.2f + hash_to_unit(splitmix64(h + 29)) * 1.0f;
                    crown_ball(t.x, g + height, t.z, cr, cr * 0.85f, M.leaves, M.leaves, 0.0f);
                    break;
                }
                case 1: {  // birch: tall, pale, narrow crown
                    const MatId trunk = pick(M.birch_log, M.log), leaf = pick(M.birch_leaves, M.leaves);
                    const int height = 6 + (int)(splitmix64(h + 23) % 4);
                    for (int y = g + 1; y <= g + height; ++y) put(t.x, y, t.z, trunk, true);
                    crown_ball(t.x, g + height - 1, t.z, 1.9f, 3.0f, leaf, leaf, 0.0f);
                    break;
                }
                case 2: {  // fruit tree: short, round, fruit hanging in the crown
                    const MatId fruit = pick(M.fruit_leaves, M.leaves);
                    const int height = 3 + (int)(splitmix64(h + 23) % 2);
                    for (int y = g + 1; y <= g + height; ++y) put(t.x, y, t.z, M.log, true);
                    const float cr = 2.0f + hash_to_unit(splitmix64(h + 29)) * 0.6f;
                    crown_ball(t.x, g + height + 1, t.z, cr, cr * 0.8f, M.leaves, fruit, 0.4f);
                    break;
                }
                case 3: {  // pine: tall trunk, conical tiers of needles
                    const MatId trunk = pick(M.pine_log, M.log), leaf = pick(M.pine_leaves, M.leaves);
                    const int height = 8 + (int)(splitmix64(h + 23) % 5);
                    for (int y = g + 1; y <= g + height; ++y) put(t.x, y, t.z, trunk, true);
                    const int top = g + height + 1;
                    put(t.x, top, t.z, leaf, false);
                    for (int y = g + 3; y < top; ++y) {
                        const float k = (float)(top - y) / (float)(top - g - 3);
                        float rad = 0.6f + 2.1f * k;
                        if (((top - y) & 1) == 0) rad *= 0.7f;  // tiers
                        const int ir = (int)std::ceil(rad);
                        for (int dz = -ir; dz <= ir; ++dz)
                            for (int dx = -ir; dx <= ir; ++dx) {
                                if (dx == 0 && dz == 0) continue;
                                const float d2 = (float)(dx * dx + dz * dz);
                                if (d2 > rad * rad) continue;
                                if (d2 > (rad - 0.8f) * (rad - 0.8f) && (hash3(h, dx, y, dz) & 3) == 0) continue;
                                put(t.x + dx, y, t.z + dz, leaf, false);
                            }
                    }
                    break;
                }
                case 4: {  // acacia: a leaning trunk and a flat, wide crown
                    const int height = 4 + (int)(splitmix64(h + 23) % 2);
                    const int lean_x = (int)(splitmix64(h + 37) % 3) - 1, lean_z = (int)(splitmix64(h + 41) % 3) - 1;
                    int x = t.x, z = t.z;
                    for (int y = g + 1; y <= g + height; ++y) {
                        if (y == g + height - 1) x += lean_x, z += lean_z;
                        put(x, y, z, M.log, true);
                    }
                    const float cr = 3.2f + hash_to_unit(splitmix64(h + 29)) * 0.8f;
                    const int ir = (int)std::ceil(cr);
                    for (int layer = 0; layer < 2; ++layer) {
                        const float rr = layer == 0 ? cr : cr - 1.2f;
                        for (int dz = -ir; dz <= ir; ++dz)
                            for (int dx = -ir; dx <= ir; ++dx) {
                                const float d2 = (float)(dx * dx + dz * dz);
                                if (d2 > rr * rr) continue;
                                if (d2 > (rr - 1.0f) * (rr - 1.0f) && (hash3(h, dx, layer, dz) & 3) == 0) continue;
                                put(x + dx, g + height + layer, z + dz, M.leaves, false);
                            }
                    }
                    break;
                }
                default: break;
            }
        }
}

void WorldGen::place_plants_continent(const Vec3i& cc, Voxel* out) const {
    const CoreMats& M = reg_->m();
    const ColumnBlock& cb = column_block(cc.x, cc.z);
    const int y0 = cc.y * kCellSize;
    const MatId air = M.air;
    for (int lz = 0; lz < kCellSize; ++lz)
        for (int lx = 0; lx < kCellSize; ++lx) {
            const ColumnInfo& col = cb.cols[lz * kCellSize + lx];
            if (!col.land || col.reserved || col.water_top >= 0) continue;
            const int y = col.top + 1;
            if (y < y0 || y >= y0 + kCellSize) continue;
            Voxel& v = out[local_index(lx, y - y0, lz)];
            if (vmat(v) != air) continue;
            const int wx = cc.x * kCellSize + lx, wz = cc.z * kCellSize + lz;
            const IslandDef& is = islands_[col.island];
            const u64 h = hash3(cfg_.seed ^ 0x9A17, wx, 0, wz);
            const float r = hash_to_unit(h);
            // Wild grain and reeds grow in stands rather than one by one.
            const float stand = saturate(fbm2(is.salt + 91, wx / 13.0f, wz / 13.0f, 2) * 2.5f - 0.05f);
            const float wet = (float)col.moist / 255.0f;
            MatId m = air;
            float acc = 0.0f;
            auto chance = [&](float p, MatId what) {
                if (m != air) return;
                acc += p;
                if (r < acc) m = what;
            };
            switch (col.biome) {
                case Biome::Grassland:
                    chance(0.16f * stand * stand, M.wild_grain);
                    chance(0.006f, M.berry_bush);
                    chance(0.004f, M.herb_plant);
                    chance(0.0035f, M.stone);
                    chance(0.002f, M.flint);
                    break;
                case Biome::Forest:
                    chance(0.016f, M.mushroom);
                    chance(0.012f, M.berry_bush);
                    chance(0.006f, M.herb_plant);
                    chance(0.002f, M.flint);
                    break;
                case Biome::Savanna:
                    chance(0.2f * stand * stand, M.wild_grain);
                    chance(0.004f, M.sandstone);
                    break;
                case Biome::Taiga:
                    chance(0.012f, M.mushroom);
                    chance(0.010f, M.berry_bush);
                    chance(0.004f, M.granite);
                    break;
                case Biome::Snowfield: chance(0.004f, M.granite); break;
                case Biome::Wetland:
                    chance(0.16f * stand * saturate(wet * 1.4f), M.reeds);
                    chance(0.012f, M.mushroom);
                    chance(0.010f, M.herb_plant);
                    break;
                case Biome::Lakeshore: chance(0.08f * stand, M.reeds); break;
                case Biome::Desert:
                    chance(0.012f, M.cactus);
                    chance(0.004f, M.sandstone);
                    break;
                case Biome::Highland:
                    chance(0.012f, M.granite);
                    chance(0.004f, M.herb_plant);
                    chance(0.002f, M.flint);
                    break;
                case Biome::Islet: chance(0.01f, M.berry_bush); break;
                default: break;
            }
            if (m == air) continue;
            v = make_voxel(m);
            if (m == M.cactus) {
                const int tall = 1 + (int)((h >> 20) % 2);
                for (int k = 1; k <= tall && y + k < y0 + kCellSize; ++k) {
                    Voxel& up = out[local_index(lx, y + k - y0, lz)];
                    if (vmat(up) == air) up = make_voxel(m);
                }
            }
        }
}

}  // namespace icarus
