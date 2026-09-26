#include "icarus/render/mesher.h"

#include <algorithm>
#include <cmath>

#include "icarus/util/rng.h"

namespace icarus {

namespace {

// Corner offsets per face, counter-clockwise seen from outside (normal = right-hand rule).
constexpr int kFaceCorners[6][4][3] = {
    {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},  // +X
    {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}},  // -X
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},  // +Y
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // -Y
    {{1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0, 0, 1}},  // +Z
    {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},  // -Z
};
constexpr int kFaceAxis[6] = {0, 0, 1, 1, 2, 2};

struct RGBf {
    float r, g, b;
};

inline RGBf rgb(u32 c) {
    return {(float)((c >> 16) & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f, (float)(c & 0xFF) / 255.0f};
}
inline RGBf mixc(RGBf a, RGBf b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
inline RGBf scalec(RGBf a, float k) { return {a.r * k, a.g * k, a.b * k}; }

constexpr float kAO[4] = {1.0f, 0.78f, 0.62f, 0.48f};

void push_quad(MeshData& m, const float (&p)[4][3], const float n[3], const RGBf (&c)[4], float emission,
               const int ao[4]) {
    int base = (int)m.vertex_count();
    for (int i = 0; i < 4; ++i) {
        m.positions.insert(m.positions.end(), {p[i][0], p[i][1], p[i][2]});
        m.normals.insert(m.normals.end(), {n[0], n[1], n[2]});
        m.colors.insert(m.colors.end(), {c[i].r, c[i].g, c[i].b, emission});
    }
    // Godot treats clockwise triangles as front faces; corners are CCW, so reverse.
    // Flip the diagonal to avoid anisotropic AO artifacts.
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        m.indices.insert(m.indices.end(), {base + 1, base + 0, base + 3, base + 1, base + 3, base + 2});
    } else {
        m.indices.insert(m.indices.end(), {base + 0, base + 2, base + 1, base + 0, base + 3, base + 2});
    }
}

void push_box(MeshData& m, float x0, float y0, float z0, float x1, float y1, float z1, RGBf col, float emission) {
    const float lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
    const int ao0[4] = {0, 0, 0, 0};
    for (int f = 0; f < 6; ++f) {
        float p[4][3];
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) p[k][a] = kFaceCorners[f][k][a] ? hi[a] : lo[a];
        float n[3] = {(float)kDir6[f].x, (float)kDir6[f].y, (float)kDir6[f].z};
        float shade = f == 3 ? 0.7f : 1.0f;
        RGBf c = scalec(col, shade);
        RGBf cs[4] = {c, c, c, c};
        push_quad(m, p, n, cs, emission, ao0);
    }
}

}  // namespace

void Mesher::load_padded(const Vec3i& cc) {
    w_.peek_cell(cc, tmp_.data());
    for (int y = 0; y < kCellSize; ++y)
        for (int z = 0; z < kCellSize; ++z)
            for (int x = 0; x < kCellSize; ++x)
                pad_[((y + 1) * 34 + (z + 1)) * 34 + (x + 1)] = tmp_[local_index(x, y, z)];
    const Vec3i o{cc.x * kCellSize, cc.y * kCellSize, cc.z * kCellSize};
    for (int y = -1; y <= kCellSize; ++y)
        for (int z = -1; z <= kCellSize; ++z)
            for (int x = -1; x <= kCellSize; ++x) {
                bool border = x < 0 || y < 0 || z < 0 || x == kCellSize || y == kCellSize || z == kCellSize;
                if (!border) continue;
                pad_[((y + 1) * 34 + (z + 1)) * 34 + (x + 1)] = w_.peek(o + Vec3i{x, y, z});
            }
}

void Mesher::build_cell(const Vec3i& cc, CellMesh& out) {
    out.opaque.clear();
    out.water.clear();
    out.foliage.clear();
    const Cell* cell = w_.cell(cc);
    if (!cell) return;
    // Fast path: a uniform cell of air produces nothing; a uniform solid cell only has
    // faces if a neighbour is non-opaque, which load_padded handles generally.
    load_padded(cc);
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    const Vec3i o{cc.x * kCellSize, cc.y * kCellSize, cc.z * kCellSize};

    auto opaque_at = [&](int x, int y, int z) {
        const Material& m = reg.mat(vmat(at(x, y, z)));
        return m.opaque && m.solid;
    };
    const u64 cseed = 0xC0105EEDull;

    for (int y = 0; y < kCellSize; ++y) {
        for (int z = 0; z < kCellSize; ++z) {
            for (int x = 0; x < kCellSize; ++x) {
                Voxel v = at(x, y, z);
                MatId mid = vmat(v);
                if (mid == M.air) continue;
                const Material& m = reg.mat(mid);
                const int wx = o.x + x, wy = o.y + y, wz = o.z + z;
                float var = (hash_to_unit(hash3(cseed, wx, wy, wz)) - 0.5f) * 2.0f * m.color_var;
                RGBf base = scalec(rgb(m.color), 1.0f + var);
                float emission = 0.0f;
                if (vburning(v)) {
                    base = mixc(base, RGBf{1.0f, 0.45f, 0.1f}, 0.7f);
                    emission = 0.9f;
                } else if (mid == M.magma) {
                    emission = 1.0f;
                }
                if (vdamage(v) > 0) base = scalec(base, 1.0f - 0.06f * (float)vdamage(v));

                if (m.fluid) {
                    // Water: faces toward air/non-water; top at level height.
                    bool water_above = vmat(at(x, y + 1, z)) == mid;
                    float h = water_above ? 1.0f : std::max(0.08f, (float)vlevel(v) / (float)kFluidFull);
                    for (int f = 0; f < 6; ++f) {
                        Vec3i d = kDir6[f];
                        Voxel nv = at(x + d.x, y + d.y, z + d.z);
                        const Material& nm = reg.mat(vmat(nv));
                        if (vmat(nv) == mid && f != 2) continue;
                        if (f == 2 && water_above) continue;
                        if (nm.opaque && nm.solid && f != 2) continue;
                        if (f == 2 && nm.opaque && nm.solid && h >= 1.0f) continue;
                        float p[4][3];
                        for (int k = 0; k < 4; ++k) {
                            p[k][0] = (float)(wx + kFaceCorners[f][k][0]);
                            p[k][1] = (float)wy + (float)kFaceCorners[f][k][1] * h;
                            p[k][2] = (float)(wz + kFaceCorners[f][k][2]);
                        }
                        float n[3] = {(float)d.x, (float)d.y, (float)d.z};
                        RGBf c = base;
                        RGBf cs[4] = {c, c, c, c};
                        const int ao0[4] = {0, 0, 0, 0};
                        push_quad(out.water, p, n, cs, (float)vlevel(v) / (float)kFluidFull, ao0);
                    }
                    continue;
                }

                if (!m.solid && !m.passable) {
                    // Small plants: crops and bushes as boxes inside the cube.
                    if (mid == M.crop) {
                        int stage = vlevel(v);
                        float hgt = 0.18f + 0.1f * (float)stage;
                        RGBf young{0.45f, 0.72f, 0.28f}, ripe = rgb(m.color);
                        RGBf c = mixc(young, ripe, (float)stage / 7.0f);
                        c = scalec(c, 1.0f + var);
                        push_box(out.foliage, wx + 0.18f, (float)wy, wz + 0.18f, wx + 0.82f, wy + hgt, wz + 0.82f, c,
                                 emission);
                    } else {
                        float s = 0.1f;
                        push_box(out.foliage, wx + s, (float)wy, wz + s, wx + 1 - s, wy + 0.75f, wz + 1 - s, base,
                                 emission);
                        if (mid == M.berry_bush) {
                            RGBf berry{0.75f, 0.12f, 0.2f};
                            float bx = wx + 0.3f + 0.4f * hash_to_unit(hash3(cseed + 1, wx, wy, wz));
                            float bz = wz + 0.3f + 0.4f * hash_to_unit(hash3(cseed + 2, wx, wy, wz));
                            push_box(out.foliage, bx - 0.09f, wy + 0.72f, bz - 0.09f, bx + 0.09f, wy + 0.84f,
                                     bz + 0.09f, berry, 0.0f);
                        }
                    }
                    continue;
                }

                const bool foliage = !m.opaque;  // leaves, glass
                MeshData& dst = foliage ? out.foliage : out.opaque;
                for (int f = 0; f < 6; ++f) {
                    Vec3i d = kDir6[f];
                    Voxel nv = at(x + d.x, y + d.y, z + d.z);
                    MatId nid = vmat(nv);
                    const Material& nm = reg.mat(nid);
                    if (nm.opaque && nm.solid) continue;
                    if (foliage && nid == mid) continue;
                    RGBf fc = base;
                    if (mid == M.grass && f != 2) {
                        fc = mixc(base, scalec(rgb(reg.mat(M.dirt).color), 1.0f + var), f == 3 ? 1.0f : 0.55f);
                    }
                    float p[4][3];
                    int ao[4];
                    RGBf cs[4];
                    const int ax = kFaceAxis[f];
                    const int u = (ax + 1) % 3, w2 = (ax + 2) % 3;
                    for (int k = 0; k < 4; ++k) {
                        const int* c = kFaceCorners[f][k];
                        p[k][0] = (float)(wx + c[0]);
                        p[k][1] = (float)(wy + c[1]);
                        p[k][2] = (float)(wz + c[2]);
                        int du[3] = {0, 0, 0}, dv[3] = {0, 0, 0};
                        du[u] = c[u] ? 1 : -1;
                        dv[w2] = c[w2] ? 1 : -1;
                        int bx = x + d.x, by = y + d.y, bz = z + d.z;
                        bool s1 = opaque_at(bx + du[0], by + du[1], bz + du[2]);
                        bool s2 = opaque_at(bx + dv[0], by + dv[1], bz + dv[2]);
                        bool cr = opaque_at(bx + du[0] + dv[0], by + du[1] + dv[1], bz + du[2] + dv[2]);
                        ao[k] = (s1 && s2) ? 3 : (int)s1 + (int)s2 + (int)cr;
                        cs[k] = scalec(fc, kAO[ao[k]]);
                    }
                    float n[3] = {(float)d.x, (float)d.y, (float)d.z};
                    push_quad(dst, p, n, cs, emission, ao);
                }
            }
        }
    }
}

void build_voxel_model(const u8* vox, int sx, int sy, int sz, const std::vector<u32>& palette, float scale,
                       MeshData& out) {
    auto get = [&](int x, int y, int z) -> u8 {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return 0;
        return vox[(y * sz + z) * sx + x];
    };
    for (int y = 0; y < sy; ++y)
        for (int z = 0; z < sz; ++z)
            for (int x = 0; x < sx; ++x) {
                u8 v = get(x, y, z);
                if (!v) continue;
                u32 col = v - 1 < (int)palette.size() ? palette[v - 1] : 0xFF00FF;
                RGBf c = rgb(col);
                for (int f = 0; f < 6; ++f) {
                    Vec3i d = kDir6[f];
                    if (get(x + d.x, y + d.y, z + d.z)) continue;
                    float p[4][3];
                    for (int k = 0; k < 4; ++k) {
                        p[k][0] = (float)(x + kFaceCorners[f][k][0]) * scale;
                        p[k][1] = (float)(y + kFaceCorners[f][k][1]) * scale;
                        p[k][2] = (float)(z + kFaceCorners[f][k][2]) * scale;
                    }
                    float n[3] = {(float)d.x, (float)d.y, (float)d.z};
                    RGBf cs[4] = {c, c, c, c};
                    const int ao0[4] = {0, 0, 0, 0};
                    push_quad(out, p, n, cs, 0.0f, ao0);
                }
            }
}

void build_debris_mesh(const Registry& reg, const std::vector<std::pair<Vec3i, Voxel>>& cubes, MeshData& out) {
    // Cubes are small in number; cull faces between members using a set lookup.
    std::vector<Vec3i> sorted;
    sorted.reserve(cubes.size());
    for (auto& c : cubes) sorted.push_back(c.first);
    std::sort(sorted.begin(), sorted.end());
    auto has = [&](const Vec3i& p) { return std::binary_search(sorted.begin(), sorted.end(), p); };
    for (auto& [p, v] : cubes) {
        const Material& m = reg.mat(vmat(v));
        RGBf c = rgb(m.color);
        for (int f = 0; f < 6; ++f) {
            if (has(p + kDir6[f])) continue;
            float q[4][3];
            for (int k = 0; k < 4; ++k) {
                q[k][0] = (float)(p.x + kFaceCorners[f][k][0]);
                q[k][1] = (float)(p.y + kFaceCorners[f][k][1]);
                q[k][2] = (float)(p.z + kFaceCorners[f][k][2]);
            }
            float n[3] = {(float)kDir6[f].x, (float)kDir6[f].y, (float)kDir6[f].z};
            RGBf cs[4] = {c, c, c, c};
            const int ao0[4] = {0, 0, 0, 0};
            push_quad(out, q, n, cs, 0.0f, ao0);
        }
    }
}

}  // namespace icarus
