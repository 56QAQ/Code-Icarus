#include "icarus/world/worldgen.h"

#include <cmath>

#include "icarus/util/noise.h"
#include "icarus/util/rng.h"

namespace icarus {

namespace {
constexpr float kPi = 3.14159265358979f;

inline float smoothstep(float e0, float e1, float x) {
    float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline float dist2d(float ax, float az, float bx, float bz) {
    float dx = ax - bx, dz = az - bz;
    return std::sqrt(dx * dx + dz * dz);
}

// Distance from point p to segment ab (2D), also returns t along segment.
inline float seg_dist(float px, float pz, float ax, float az, float bx, float bz, float& t) {
    float vx = bx - ax, vz = bz - az;
    float wx = px - ax, wz = pz - az;
    float l2 = vx * vx + vz * vz;
    t = l2 > 0 ? saturate((wx * vx + wz * vz) / l2) : 0.0f;
    float cx = ax + vx * t, cz = az + vz * t;
    return dist2d(px, pz, cx, cz);
}
}  // namespace

const char* biome_key(Biome b) {
    switch (b) {
        case Biome::Sky: return "sky";
        case Biome::Grassland: return "grassland";
        case Biome::Forest: return "forest";
        case Biome::Highland: return "highland";
        case Biome::Lakeshore: return "lakeshore";
        case Biome::Ravine: return "ravine";
        case Biome::Underside: return "underside";
        case Biome::Islet: return "islet";
        case Biome::Taiga: return "taiga";
        case Biome::Snowfield: return "snowfield";
        case Biome::Desert: return "desert";
        case Biome::Savanna: return "savanna";
        case Biome::Wetland: return "wetland";
        case Biome::Ocean: return "ocean";
        case Biome::Beach: return "beach";
        default: return "unknown";
    }
}

const char* biome_name_zh(Biome b) {
    switch (b) {
        case Biome::Sky: return "天空";
        case Biome::Grassland: return "草原";
        case Biome::Forest: return "森林";
        case Biome::Highland: return "高地";
        case Biome::Lakeshore: return "湖岸";
        case Biome::Ravine: return "峡谷";
        case Biome::Underside: return "浮岛底层";
        case Biome::Islet: return "小浮岛";
        case Biome::Taiga: return "针叶林";
        case Biome::Snowfield: return "雪原";
        case Biome::Desert: return "荒漠";
        case Biome::Savanna: return "稀树草原";
        case Biome::Wetland: return "沼泽";
        case Biome::Ocean: return "海洋";
        case Biome::Beach: return "海滩";
        default: return "未知";
    }
}

const char* layout_key(WorldLayout l) {
    return l == WorldLayout::Continent ? "continent" : (l == WorldLayout::Random ? "random" : "classic");
}

WorldLayout layout_from_key(const std::string& k) {
    if (k == "random") return WorldLayout::Random;
    return (k == "continent" || k == "large") ? WorldLayout::Continent : WorldLayout::Classic;
}

WorldConfig WorldConfig::for_layout(WorldLayout l, u64 seed) {
    WorldConfig c;
    c.seed = seed;
    c.layout = l;
    if (l == WorldLayout::Continent) {
        c.cells_y = 10;  // room for the mountains
        c.island_radius = 330.0f;
        c.base_height = 150;
        c.islet_count = 6;
    } else if (l == WorldLayout::Random) {
        c.sized();
    }
    return c;
}

void WorldConfig::sized() {
    // Medium: a little larger than the continent; large: half as wide again. The sea
    // (when there is one) fills the ring between the island and the world's edge.
    const int cells = size <= 0 ? 40 : 48;
    cells_x = cells_z = cells;
    cells_y = 10;
    base_height = sea_level + 10;
    island_radius = (float)(cells * kCellSize) * (sea ? 0.31f : 0.36f);
    islet_count = 5;
}

void WorldGen::init(const WorldConfig& cfg, const Registry& reg) {
    cfg_ = cfg;
    reg_ = &reg;
    col_cache_.clear();
    islands_.clear();
    lakes_.clear();
    streams_.clear();
    peaks_.clear();
    site_climate_.clear();
    feat_ = IslandFeatures{};
    rnd_.reset();
    has_sea_ = false;
    rich_trees_ = rich_plants_ = rich_ores_ = 1.0f;
    if (cfg.layout == WorldLayout::Continent) {
        init_continent();
        return;
    }
    if (cfg.layout == WorldLayout::Random) {
        init_random();
        return;
    }

    Rng rng(cfg.seed, 0x15A4D);
    const float W = (float)(cfg.cells_x * kCellSize);
    const float D = (float)(cfg.cells_z * kCellSize);
    const float R = cfg.island_radius;

    IslandDef main;
    main.cx = W * 0.5f;
    main.cz = D * 0.5f;
    main.radius = R;
    main.base_h = (float)cfg.base_height;
    main.thickness = R * 0.85f;
    main.salt = hash_combine(cfg.seed, 1);
    main.main = true;
    islands_.push_back(main);

    for (int i = 0; i < cfg.islet_count; ++i) {
        IslandDef is;
        float ang = 2.0f * kPi * (float)i / (float)std::max(1, cfg.islet_count) + rng.uniform(-0.4f, 0.4f);
        float dist = R + rng.uniform(95.0f, 170.0f);
        is.radius = rng.uniform(18.0f, 32.0f);
        float maxd = std::min(W, D) * 0.5f - is.radius * 1.3f - 8.0f;
        dist = std::min(dist, maxd);
        is.cx = main.cx + std::cos(ang) * dist;
        is.cz = main.cz + std::sin(ang) * dist;
        is.base_h = (float)cfg.base_height + rng.uniform(-28.0f, 22.0f);
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

    auto world_xz = [&](float lu, float lv, int& x, int& z) {
        x = (int)std::lround(main.cx + lu * cos0_ - lv * sin0_);
        z = (int)std::lround(main.cz + lu * sin0_ + lv * cos0_);
    };
    auto feature = [&](float lu, float lv) {
        Vec3i p;
        world_xz(lu, lv, p.x, p.z);
        p.y = 0;
        return p;
    };
    // Horizontal feature layout in the island's local frame. Heights are filled below
    // once column() can be evaluated.
    feat_.village = feature(-0.42f * R, -0.12f * R);
    feat_.farms = feature(0.40f * R, -0.18f * R);
    feat_.lake = feature(0.40f * R, 0.22f * R);
    feat_.pond = feature(-0.30f * R, 0.24f * R);
    feat_.mountain = feature(0.44f * R, 0.64f * R);
    feat_.lake_radius = 13;
    feat_.pond_radius = 6;
    {
        // Spring sits on the mountain slope, facing the lake.
        float lu_l = 0.40f * R, lv_l = 0.22f * R, lu_m = 0.44f * R, lv_m = 0.64f * R;
        float dx = lu_m - lu_l, dz = lv_m - lv_l;
        float d = std::sqrt(dx * dx + dz * dz);
        float k = ((float)feat_.lake_radius + 9.0f) / d;
        feat_.spring = feature(lu_l + dx * k, lv_l + dz * k);
    }
    float lvb = -0.12f * R;
    float uc = ravine_center_u(lvb);
    feat_.bridge_a = feature(uc - 9.0f, lvb);
    feat_.bridge_b = feature(uc + 9.0f, lvb);
    feat_.ravine_end = feature(ravine_center_u(0.60f * R), 0.62f * R);

    // The spring's stream channel depends on the spring height, which depends on the
    // terrain: evaluate the terrain without the channel first, then invalidate the cache.
    feat_.spring.y = 0;
    col_cache_.clear();
    feat_.spring.y = column(feat_.spring.x, feat_.spring.z).top - 1;
    col_cache_.clear();

    auto fill_h = [&](Vec3i& p) { p.y = column(p.x, p.z).top; };
    fill_h(feat_.village);
    fill_h(feat_.farms);
    fill_h(feat_.mountain);
    fill_h(feat_.bridge_a);
    fill_h(feat_.bridge_b);
    fill_h(feat_.ravine_end);
    feat_.lake.y = column(feat_.lake.x, feat_.lake.z).water_top;
    feat_.pond.y = column(feat_.pond.x, feat_.pond.z).water_top;
    feat_.sites.push_back({feat_.village, feat_.farms, feat_.pond, Biome::Grassland});
    feat_.springs.push_back(feat_.spring);
    feat_.waters = {feat_.pond, feat_.lake};
}

void WorldGen::to_local(float x, float z, float& lu, float& lv) const {
    const IslandDef& m = islands_[0];
    float dx = x - m.cx, dz = z - m.cz;
    lu = dx * cos0_ + dz * sin0_;
    lv = -dx * sin0_ + dz * cos0_;
}

float WorldGen::ravine_center_u(float lv) const {
    const IslandDef& m = islands_[0];
    float R = m.radius;
    return R * 0.05f * std::sin(lv / R * 3.0f + (float)(m.salt % 628) * 0.01f) +
           3.0f * gradient_noise2(m.salt + 77, lv / 22.0f, 0.5f);
}

ColumnInfo WorldGen::compute_column(int xi, int zi) const {
    if (cfg_.layout == WorldLayout::Continent) return compute_column_continent(xi, zi);
    if (cfg_.layout == WorldLayout::Random) return compute_column_random(xi, zi);
    ColumnInfo c;
    const float x = (float)xi + 0.5f, z = (float)zi + 0.5f;
    for (size_t ii = 0; ii < islands_.size(); ++ii) {
        const IslandDef& is = islands_[ii];
        float d = dist2d(x, z, is.cx, is.cz);
        if (d > is.radius * 1.3f) continue;
        float n = fbm2(is.salt, x / 70.0f, z / 70.0f, 3);
        float r = d / (is.radius * (1.0f + 0.18f * n));
        if (r >= 1.0f) continue;

        float h = is.base_h + 2.5f * fbm2(is.salt + 1, x / 45.0f, z / 45.0f, 3) +
                  1.2f * fbm2(is.salt + 2, x / 12.0f, z / 12.0f, 2);
        if (r > 0.80f) {
            float e = (r - 0.80f) / 0.20f;
            h -= e * e * 7.0f;
        }
        Biome biome = is.main ? Biome::Grassland : Biome::Islet;
        bool ravine = false, reserved = false;
        int water_top = -1;
        float top = h;

        if (is.main) {
            const float R = is.radius;
            float lu, lv;
            to_local(x, z, lu, lv);
            // Mountain.
            float dm = dist2d(x, z, (float)feat_.mountain.x, (float)feat_.mountain.z);
            float bump = 34.0f * std::exp(-(dm / 32.0f) * (dm / 32.0f) * 2.0f);
            bump *= 0.8f + 0.4f * ridged2(is.salt + 3, x / 30.0f, z / 30.0f, 3);
            top += bump;
            if (bump > 9.0f) biome = Biome::Highland;

            // Flattened plateaus for village, farms and around water.
            auto flatten = [&](const Vec3i& c0, float rad, float target) {
                float dd = dist2d(x, z, (float)c0.x, (float)c0.z);
                float w = 1.0f - smoothstep(rad * 0.65f, rad, dd);
                top = lerpf(top, target, w);
                if (dd < rad * 0.8f) reserved = true;
            };
            flatten(feat_.village, 30.0f, is.base_h + 1.0f);
            flatten(feat_.farms, 30.0f, is.base_h);
            flatten(feat_.lake, (float)feat_.lake_radius + 7.0f, is.base_h);
            flatten(feat_.pond, (float)feat_.pond_radius + 6.0f, is.base_h + 1.0f);

            float ft = std::floor(top);
            // Lake and pond basins.
            auto basin = [&](const Vec3i& c0, int rad, int wt, u64 salt) {
                float dd = dist2d(x, z, (float)c0.x, (float)c0.z);
                float rr = (float)rad * (1.0f + 0.12f * gradient_noise2(salt, x / 9.0f, z / 9.0f));
                if (dd < rr) {
                    float depth = 1.0f + 5.0f * (1.0f - (dd / rr) * (dd / rr));
                    ft = std::min(ft, (float)wt - std::floor(depth));
                    water_top = wt;
                    biome = Biome::Lakeshore;
                } else if (dd < rr + 5.0f) {
                    biome = Biome::Lakeshore;
                }
            };
            int lake_wt = (int)is.base_h - 1;
            basin(feat_.lake, feat_.lake_radius, lake_wt, is.salt + 11);
            basin(feat_.pond, feat_.pond_radius, (int)is.base_h, is.salt + 12);

            // Stream channel from spring down to the lake.
            {
                float t;
                float ds = seg_dist(x, z, (float)feat_.spring.x, (float)feat_.spring.z, (float)feat_.lake.x,
                                    (float)feat_.lake.z, t);
                if (ds < 1.6f && feat_.spring.y > 0) {
                    float bed = lerpf((float)feat_.spring.y, (float)lake_wt, smoothstep(0.0f, 0.85f, t));
                    ft = std::min(ft, std::floor(bed));
                    reserved = true;
                }
            }
            // Outflow channel from lake toward the ravine (-u direction).
            {
                float lu_l = 0.40f * R, lv_l = 0.22f * R;
                float lu_r = ravine_center_u(lv_l) + 4.0f;
                if (lv > lv_l - 1.6f && lv < lv_l + 1.6f && lu < lu_l && lu > lu_r - 2.0f) {
                    // Only overflow leaves the lake: the channel floor stays at the lake's top
                    // layer until well past the shore, then descends toward the ravine.
                    float lu_shore = lu_l - (float)feat_.lake_radius - 4.0f;
                    float t = saturate((lu_shore - lu) / std::max(1.0f, lu_shore - lu_r));
                    float bed = (float)lake_wt - std::floor(t * 4.0f);
                    ft = std::min(ft, bed);
                    reserved = true;
                }
            }
            // Ravine: separates the village side (u<0) from the farm side (u>0).
            if (lv > -1.3f * R && lv < 0.62f * R) {
                float uc = ravine_center_u(lv);
                float taper = saturate((0.62f * R - lv) / (0.22f * R));
                float hw = (5.5f + 1.5f * gradient_noise2(is.salt + 21, lv / 15.0f, 3.3f)) * std::sqrt(taper);
                float du = std::fabs(lu - uc);
                if (hw > 0.5f && du < hw) {
                    float s = saturate(-lv / R);
                    float depth = (20.0f + 16.0f * s) * taper;
                    float carve = depth * std::sqrt(1.0f - (du / hw) * (du / hw));
                    float floor_y = std::floor(top) - carve;
                    // Floor slopes down toward the rim so water drains off the island.
                    floor_y = std::min(floor_y, is.base_h - 14.0f - 18.0f * s);
                    if (carve > 1.0f) {
                        ft = std::min(ft, std::floor(floor_y));
                        ravine = true;
                        biome = Biome::Ravine;
                    }
                }
            }
            // Bridge approaches are reserved.
            {
                float t;
                float db = seg_dist(x, z, (float)feat_.bridge_a.x, (float)feat_.bridge_a.z, (float)feat_.bridge_b.x,
                                    (float)feat_.bridge_b.z, t);
                if (db < 4.0f) reserved = true;
            }
            if (!ravine && biome == Biome::Grassland) {
                float fd = fbm2(is.salt + 31, x / 38.0f, z / 38.0f, 3);
                if (fd > 0.08f && lu > -0.1f * R) biome = Biome::Forest;
                else if (fd > 0.30f) biome = Biome::Forest;
            }
            top = ft;
        } else {
            top = std::floor(top);
        }

        float bottom = is.base_h - is.thickness * std::pow(std::max(0.0f, 1.0f - r), 1.25f) - 8.0f +
                       6.0f * fbm2(is.salt + 5, x / 25.0f, z / 25.0f, 3);
        bottom = std::min(bottom, top - 5.0f);

        c.land = true;
        c.top = (i16)clampv((int)top, 1, cfg_.cells_y * kCellSize - 2);
        c.bottom = (i16)clampv((int)std::floor(bottom), 0, (int)c.top);
        c.water_top = (i16)water_top;
        c.biome = biome;
        c.island = (u8)ii;
        c.ravine = ravine;
        c.reserved = reserved;
        return c;
    }
    return c;
}

const WorldGen::ColumnBlock& WorldGen::column_block(int cx, int cz) const {
    i64 key = ((i64)cx << 32) ^ (i64)(u32)cz;
    auto it = col_cache_.find(key);
    if (it != col_cache_.end()) return *it->second;
    auto blk = std::make_unique<ColumnBlock>();
    if (cfg_.layout == WorldLayout::Random && rnd_)
        column_block_random(cx, cz, *blk);
    else
        for (int lz = 0; lz < kCellSize; ++lz)
            for (int lx = 0; lx < kCellSize; ++lx)
                blk->cols[lz * kCellSize + lx] = compute_column(cx * kCellSize + lx, cz * kCellSize + lz);
    auto* raw = blk.get();
    col_cache_[key] = std::move(blk);
    return *raw;
}

ColumnInfo WorldGen::column(int x, int z) const {
    int cx = x >> kCellBits, cz = z >> kCellBits;
    if (cx < 0 || cz < 0 || cx >= cfg_.cells_x || cz >= cfg_.cells_z) return compute_column(x, z);
    return column_block(cx, cz).cols[(z & kCellMask) * kCellSize + (x & kCellMask)];
}

bool WorldGen::cell_maybe_nonempty(const Vec3i& cc) const {
    float x0 = (float)(cc.x * kCellSize), x1 = x0 + kCellSize;
    float z0 = (float)(cc.z * kCellSize), z1 = z0 + kCellSize;
    float y0 = (float)(cc.y * kCellSize), y1 = y0 + kCellSize;
    for (const auto& is : islands_) {
        float rr = is.radius * 1.3f + 6.0f;
        if (is.cx + rr < x0 || is.cx - rr > x1 || is.cz + rr < z0 || is.cz - rr > z1) continue;
        float ylo = is.base_h - is.thickness - 20.0f;
        float yhi = is.base_h + (cfg_.layout == WorldLayout::Random && is.main
                                     ? 125.0f
                                     : (cfg_.layout == WorldLayout::Continent && is.main ? 100.0f : 60.0f));
        if (yhi < y0 || ylo > y1) continue;
        return true;
    }
    return false;
}

Biome WorldGen::cell_biome(const Vec3i& cc) const {
    if (!cell_maybe_nonempty(cc)) return Biome::Sky;
    int counts[(int)Biome::Count] = {0};
    const int y0 = cc.y * kCellSize, y1 = y0 + kCellSize - 1;
    bool any = false;
    for (int sz = 2; sz < kCellSize; sz += 4) {
        for (int sx = 2; sx < kCellSize; sx += 4) {
            ColumnInfo col = column(cc.x * kCellSize + sx, cc.z * kCellSize + sz);
            if (!col.land) continue;
            if (col.top + 14 < y0 || col.bottom > y1) continue;
            any = true;
            if (col.top > y1 + 2) counts[(int)Biome::Underside]++;
            else counts[(int)col.biome]++;
        }
    }
    if (!any) return Biome::Sky;
    int best = 1;
    for (int b = 1; b < (int)Biome::Count; ++b)
        if (counts[b] > counts[best]) best = b;
    return (Biome)best;
}

void WorldGen::generate_cell(const Vec3i& cc, Voxel* out) const {
    const CoreMats& M = reg_->m();
    const Voxel air = make_voxel(M.air);
    if (!cell_maybe_nonempty(cc)) {
        for (int i = 0; i < kCellVol; ++i) out[i] = air;
        return;
    }
    if (cfg_.layout == WorldLayout::Continent) {
        generate_cell_continent(cc, out);
        return;
    }
    if (cfg_.layout == WorldLayout::Random) {
        generate_cell_random(cc, out);
        return;
    }
    const ColumnBlock& cb = column_block(cc.x, cc.z);
    const int y0 = cc.y * kCellSize;
    for (int lz = 0; lz < kCellSize; ++lz) {
        for (int lx = 0; lx < kCellSize; ++lx) {
            const ColumnInfo& col = cb.cols[lz * kCellSize + lx];
            const int wx = cc.x * kCellSize + lx, wz = cc.z * kCellSize + lz;
            const IslandDef* is = col.land ? &islands_[col.island] : nullptr;
            for (int ly = 0; ly < kCellSize; ++ly) {
                const int y = y0 + ly;
                Voxel v = air;
                if (col.land && y >= col.bottom && y <= col.top) {
                    int depth = col.top - y;
                    MatId m = M.stone;
                    if (y - col.bottom < 3) {
                        m = M.stone;
                    } else if (col.ravine) {
                        m = depth <= 1 ? M.gravel : M.stone;
                    } else if (col.water_top >= 0) {
                        // Lake bed.
                        float cn = gradient_noise2(is->salt + 41, wx / 6.0f, wz / 6.0f);
                        if (depth <= 1) m = cn > 0.25f ? M.clay : M.sand;
                        else if (depth <= 3) m = cn > 0.0f ? M.clay : M.dirt;
                    } else if (col.biome == Biome::Lakeshore) {
                        if (depth == 0) m = M.sand;
                        else if (depth <= 2) m = M.sand;
                        else if (depth <= 4) m = M.dirt;
                    } else if (col.biome == Biome::Highland && is && col.top > is->base_h + 16) {
                        float gn = gradient_noise2(is->salt + 43, wx / 7.0f, wz / 7.0f);
                        if (depth == 0) m = gn > 0.2f ? M.grass : M.stone;
                        else if (depth <= 1 && gn > 0.2f) m = M.dirt;
                    } else {
                        if (depth == 0) m = M.grass;
                        else if (depth <= 3) m = M.dirt;
                    }
                    v = make_voxel(m);
                } else if (col.water_top >= 0 && y > col.top && y <= col.water_top) {
                    v = make_voxel(M.water, kFluidFull);
                }
                out[local_index(lx, ly, lz)] = v;
            }
        }
    }

    // Levistone cores anchor each island in the sky.
    for (const auto& is : islands_) {
        float ccx = is.cx, ccz = is.cz;
        float ccy = is.main ? is.base_h - is.thickness * 0.55f : is.base_h - is.thickness * 0.45f;
        float rx = is.main ? 24.0f : std::max(4.0f, is.radius * 0.28f);
        float ry = is.main ? 15.0f : std::max(3.0f, is.radius * 0.2f);
        int bx0 = (int)std::floor(ccx - rx) - cc.x * kCellSize, bx1 = (int)std::ceil(ccx + rx) - cc.x * kCellSize;
        int by0 = (int)std::floor(ccy - ry) - y0, by1 = (int)std::ceil(ccy + ry) - y0;
        int bz0 = (int)std::floor(ccz - rx) - cc.z * kCellSize, bz1 = (int)std::ceil(ccz + rx) - cc.z * kCellSize;
        if (bx1 < 0 || by1 < 0 || bz1 < 0 || bx0 >= kCellSize || by0 >= kCellSize || bz0 >= kCellSize) continue;
        for (int ly = std::max(0, by0); ly <= std::min(kCellSize - 1, by1); ++ly)
            for (int lz = std::max(0, bz0); lz <= std::min(kCellSize - 1, bz1); ++lz)
                for (int lx = std::max(0, bx0); lx <= std::min(kCellSize - 1, bx1); ++lx) {
                    float dx = (cc.x * kCellSize + lx + 0.5f - ccx) / rx;
                    float dy = (y0 + ly + 0.5f - ccy) / ry;
                    float dz = (cc.z * kCellSize + lz + 0.5f - ccz) / rx;
                    float n = 0.15f * gradient_noise3(is.salt + 51, (cc.x * kCellSize + lx) / 6.0f,
                                                      (y0 + ly) / 6.0f, (cc.z * kCellSize + lz) / 6.0f);
                    if (dx * dx + dy * dy + dz * dz < 1.0f + n) {
                        Voxel& v = out[local_index(lx, ly, lz)];
                        if (vmat(v) != M.air) v = make_voxel(M.levistone);
                    }
                }
    }

    place_ores(cc, out);
    place_trees(cc, out);
    // Mushrooms, berries, herbs, wild grain and boulders: the same wild plants as the
    // continent's (the village, fields and bridge are kept clear).
    place_plants_continent(cc, out);

    // Spring cube.
    const Vec3i& sp = feat_.spring;
    if (sp.y > 0 && cell_of(sp) == cc) out[local_index(sp.x & kCellMask, sp.y & kCellMask, sp.z & kCellMask)] = make_voxel(M.spring);
}

void WorldGen::place_ores(const Vec3i& cc, Voxel* out) const {
    const CoreMats& M = reg_->m();
    constexpr int G = 12;
    const int bx0 = cc.x * kCellSize, by0 = cc.y * kCellSize, bz0 = cc.z * kCellSize;
    const float base = (float)cfg_.base_height;
    const bool continent = cfg_.layout != WorldLayout::Classic;
    auto rock = [&](MatId m) {
        return m == M.stone || (m != M.air && (m == M.granite || m == M.limestone || m == M.sandstone));
    };
    int gx0 = floordiv(bx0 - 4, G), gx1 = floordiv(bx0 + kCellSize + 4, G);
    int gy0 = floordiv(by0 - 4, G), gy1 = floordiv(by0 + kCellSize + 4, G);
    int gz0 = floordiv(bz0 - 4, G), gz1 = floordiv(bz0 + kCellSize + 4, G);
    for (int gy = gy0; gy <= gy1; ++gy)
        for (int gz = gz0; gz <= gz1; ++gz)
            for (int gx = gx0; gx <= gx1; ++gx) {
                u64 h = hash3(cfg_.seed ^ 0x0E0E, gx, gy, gz);
                float roll = hash_to_unit(h);
                float px = gx * G + hash_to_unit(splitmix64(h + 1)) * G;
                float py = gy * G + hash_to_unit(splitmix64(h + 2)) * G;
                float pz = gz * G + hash_to_unit(splitmix64(h + 3)) * G;
                float rad = 1.4f + hash_to_unit(splitmix64(h + 4)) * 1.6f;
                MatId ore;
                const float k = rich_ores_;
                if (py > base - 40.0f && roll < 0.14f * k) ore = M.copper_ore;
                else if (py < base - 22.0f && roll < 0.22f * k) ore = M.iron_ore;
                else if (continent && py > base + 12.0f && roll < 0.24f * k) ore = M.iron_ore;  // in the mountains
                else if (roll > 1.0f - 0.10f * k) ore = M.coal;
                else continue;
                int x0 = (int)std::floor(px - rad), x1 = (int)std::ceil(px + rad);
                int y0 = (int)std::floor(py - rad), y1 = (int)std::ceil(py + rad);
                int z0 = (int)std::floor(pz - rad), z1 = (int)std::ceil(pz + rad);
                for (int y = std::max(y0, by0); y <= std::min(y1, by0 + kCellSize - 1); ++y)
                    for (int z = std::max(z0, bz0); z <= std::min(z1, bz0 + kCellSize - 1); ++z)
                        for (int x = std::max(x0, bx0); x <= std::min(x1, bx0 + kCellSize - 1); ++x) {
                            float dx = x + 0.5f - px, dy = y + 0.5f - py, dz = z + 0.5f - pz;
                            if (dx * dx + dy * dy + dz * dz > rad * rad) continue;
                            Voxel& v = out[local_index(x - bx0, y - by0, z - bz0)];
                            if (rock(vmat(v))) v = make_voxel(ore);
                        }
            }
}

// Tree slots and the decision to plant one; shared by place_trees and tree_bases so
// the two can never disagree.
namespace {
constexpr int kTreeSlot = 6;
}

std::vector<Vec3i> WorldGen::tree_bases() const {
    std::vector<Vec3i> out;
    const int S = kTreeSlot;
    const int nx = cfg_.cells_x * kCellSize / S + 1, nz = cfg_.cells_z * kCellSize / S + 1;
    if (cfg_.layout != WorldLayout::Classic) {
        TreeSpec t;
        for (int sz = 0; sz < nz; ++sz)
            for (int sx = 0; sx < nx; ++sx)
                if (continent_tree(sx, sz, t)) out.push_back({t.x, t.ground + 1, t.z});
        return out;
    }
    for (int sz = 0; sz < nz; ++sz)
        for (int sx = 0; sx < nx; ++sx) {
            u64 h = hash3(cfg_.seed ^ 0x7EE5, sx, 0, sz);
            int tx = sx * S + 1 + (int)(splitmix64(h) % (S - 2));
            int tz = sz * S + 1 + (int)(splitmix64(h + 9) % (S - 2));
            if (tx >= cfg_.cells_x * kCellSize || tz >= cfg_.cells_z * kCellSize) continue;
            float roll = hash_to_unit(splitmix64(h + 17));
            ColumnInfo col = column(tx, tz);
            if (!col.land || col.ravine || col.reserved || col.water_top >= 0) continue;
            float p = 0.0f;
            switch (col.biome) {
                case Biome::Forest: p = 0.55f; break;
                case Biome::Grassland: p = 0.05f; break;
                case Biome::Highland: p = 0.12f; break;
                case Biome::Islet: p = 0.30f; break;
                default: p = 0.0f;
            }
            if (roll < p) out.push_back({tx, col.top + 1, tz});
        }
    return out;
}

void WorldGen::place_trees(const Vec3i& cc, Voxel* out) const {
    const CoreMats& M = reg_->m();
    constexpr int S = kTreeSlot;
    const int bx0 = cc.x * kCellSize, by0 = cc.y * kCellSize, bz0 = cc.z * kCellSize;
    int sx0 = floordiv(bx0 - 4, S), sx1 = floordiv(bx0 + kCellSize + 4, S);
    int sz0 = floordiv(bz0 - 4, S), sz1 = floordiv(bz0 + kCellSize + 4, S);
    auto put = [&](int x, int y, int z, MatId m, bool overwrite_solid) {
        if (x < bx0 || y < by0 || z < bz0 || x >= bx0 + kCellSize || y >= by0 + kCellSize || z >= bz0 + kCellSize)
            return;
        Voxel& v = out[local_index(x - bx0, y - by0, z - bz0)];
        MatId cur = vmat(v);
        if (cur == M.air || (overwrite_solid && cur == M.leaves)) v = make_voxel(m);
    };
    for (int sz = sz0; sz <= sz1; ++sz)
        for (int sx = sx0; sx <= sx1; ++sx) {
            u64 h = hash3(cfg_.seed ^ 0x7EE5, sx, 0, sz);
            int tx = sx * S + 1 + (int)(splitmix64(h) % (S - 2));
            int tz = sz * S + 1 + (int)(splitmix64(h + 9) % (S - 2));
            float roll = hash_to_unit(splitmix64(h + 17));
            ColumnInfo col = column(tx, tz);
            if (!col.land || col.ravine || col.reserved || col.water_top >= 0) continue;
            float p = 0.0f;
            switch (col.biome) {
                case Biome::Forest: p = 0.55f; break;
                case Biome::Grassland: p = 0.05f; break;
                case Biome::Highland: p = 0.12f; break;
                case Biome::Islet: p = 0.30f; break;
                default: p = 0.0f;
            }
            const int ground = col.top;
            if (ground + 14 < by0 || ground > by0 + kCellSize) continue;
            if (roll < p) {
                int height = 5 + (int)(splitmix64(h + 23) % 4);
                for (int y = ground + 1; y <= ground + height; ++y) put(tx, y, tz, M.log, true);
                float cr = 2.2f + hash_to_unit(splitmix64(h + 29)) * 1.0f;
                int cy = ground + height;
                int ir = (int)std::ceil(cr);
                for (int dy = -ir + 1; dy <= ir; ++dy)
                    for (int dz = -ir; dz <= ir; ++dz)
                        for (int dx = -ir; dx <= ir; ++dx) {
                            float d2 = (float)(dx * dx + dz * dz) + (float)(dy * dy) * 1.4f;
                            if (d2 > cr * cr) continue;
                            u64 lh = hash3(h, dx, dy, dz);
                            if (d2 > (cr - 1.0f) * (cr - 1.0f) && (lh & 3) == 0) continue;  // ragged edge
                            put(tx + dx, cy + dy, tz + dz, M.leaves, false);
                        }
            } else if (roll > 0.965f && (col.biome == Biome::Forest || col.biome == Biome::Grassland)) {
                put(tx, ground + 1, tz, M.berry_bush, false);
            }
        }
}

}  // namespace icarus
