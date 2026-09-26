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

// Ambient occlusion per corner (applied in linear light by the shader).
constexpr float kAO[4] = {1.0f, 0.68f, 0.48f, 0.34f};

inline int face_kind(int f) { return f == 2 ? 0 : (f == 3 ? 2 : 1); }

void push_quad(MeshData& m, const float (&p)[4][3], const float n[3], const RGBf (&c)[4], float emission,
               const int ao[4], float u = -1.0f, float v = 0.0f) {
    int base = (int)m.vertex_count();
    for (int i = 0; i < 4; ++i) {
        m.positions.insert(m.positions.end(), {p[i][0], p[i][1], p[i][2]});
        m.normals.insert(m.normals.end(), {n[0], n[1], n[2]});
        m.colors.insert(m.colors.end(), {c[i].r, c[i].g, c[i].b, emission});
        m.uvs.insert(m.uvs.end(), {u, v});
    }
    // Godot treats clockwise triangles as front faces; corners are CCW, so reverse.
    // Flip the diagonal to avoid anisotropic AO artifacts.
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        m.indices.insert(m.indices.end(), {base + 1, base + 0, base + 3, base + 1, base + 3, base + 2});
    } else {
        m.indices.insert(m.indices.end(), {base + 0, base + 2, base + 1, base + 0, base + 3, base + 2});
    }
}

// A box inside a cube (crops, bushes, berries). mat < 0 draws plain colour.
void push_box(MeshData& m, float x0, float y0, float z0, float x1, float y1, float z1, RGBf col, float emission,
              int mat = -1) {
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
        push_quad(m, p, n, cs, emission, ao0, (float)mat, (float)face_kind(f));
    }
}

// Two crossed quads standing on (cx, y0, cz): a tuft or a flower. Normals point up so
// sprites are lit like the ground they grow from.
void push_sprite(MeshData& m, float cx, float y0, float cz, float half, float h, float angle, int layer, RGBf col) {
    const int ao0[4] = {0, 0, 0, 0};
    const float n[3] = {0.0f, 1.0f, 0.0f};
    RGBf cs[4] = {col, col, col, col};
    for (int q = 0; q < 2; ++q) {
        const float a = angle + (float)q * 1.5707963f;
        const float dx = std::cos(a) * half, dz = std::sin(a) * half;
        const float p[4][3] = {{cx - dx, y0, cz - dz}, {cx - dx, y0 + h, cz - dz}, {cx + dx, y0 + h, cz + dz}, {cx + dx, y0, cz + dz}};
        const int base = (int)m.vertex_count();
        const float us[4] = {0.001f, 0.001f, 0.999f, 0.999f}, vs[4] = {0.999f, 0.001f, 0.001f, 0.999f};
        for (int i = 0; i < 4; ++i) {
            m.positions.insert(m.positions.end(), {p[i][0], p[i][1], p[i][2]});
            m.normals.insert(m.normals.end(), {n[0], n[1], n[2]});
            m.colors.insert(m.colors.end(), {cs[i].r, cs[i].g, cs[i].b, 0.0f});
            m.uvs.insert(m.uvs.end(), {(float)layer + us[i], vs[i]});
        }
        m.indices.insert(m.indices.end(), {base + 0, base + 2, base + 1, base + 0, base + 3, base + 2});
        (void)ao0;
    }
}

// Four upright planes in a # pattern filling a cube (crops).
void push_crop(MeshData& m, float x, float y, float z, float h, int layer, RGBf col) {
    const float n[3] = {0.0f, 1.0f, 0.0f};
    const float us[4] = {0.001f, 0.001f, 0.999f, 0.999f}, vs[4] = {0.999f, 0.001f, 0.001f, 0.999f};
    for (int q = 0; q < 4; ++q) {
        const float off = (q % 2 == 0) ? 0.3f : 0.7f;
        float p[4][3];
        if (q < 2) {  // planes along x
            const float pz = z + off;
            const float c[4][3] = {{x, y, pz}, {x, y + h, pz}, {x + 1, y + h, pz}, {x + 1, y, pz}};
            std::copy(&c[0][0], &c[0][0] + 12, &p[0][0]);
        } else {  // planes along z
            const float px = x + off;
            const float c[4][3] = {{px, y, z}, {px, y + h, z}, {px, y + h, z + 1}, {px, y, z + 1}};
            std::copy(&c[0][0], &c[0][0] + 12, &p[0][0]);
        }
        const int base = (int)m.vertex_count();
        for (int i = 0; i < 4; ++i) {
            m.positions.insert(m.positions.end(), {p[i][0], p[i][1], p[i][2]});
            m.normals.insert(m.normals.end(), {n[0], n[1], n[2]});
            m.colors.insert(m.colors.end(), {col.r, col.g, col.b, 0.0f});
            m.uvs.insert(m.uvs.end(), {(float)layer + us[i], vs[i]});
        }
        m.indices.insert(m.indices.end(), {base + 0, base + 2, base + 1, base + 0, base + 3, base + 2});
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
    out.decor.clear();
    out.crops.clear();
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
                // Textured faces: the colour is only a shade (per-cube variation, burning).
                RGBf shade{1.0f + var, 1.0f + var, 1.0f + var};
                float emission = 0.0f;
                if (vburning(v)) {
                    shade = RGBf{1.6f, 0.75f, 0.35f};
                    emission = 0.9f;
                } else if (mid == M.magma) {
                    emission = 1.0f;
                }
                const float dmg = (float)std::min<int>(vdamage(v), 7);

                if (m.fluid) {
                    // Water: faces toward air/non-water. The surface height at each corner
                    // is shared by the water cubes around it, so the surface is smooth
                    // instead of stepped by each cube's level.
                    RGBf base = scalec(rgb(m.color), 1.0f + var);
                    bool water_above = vmat(at(x, y + 1, z)) == mid;
                    float h = water_above ? 1.0f : std::max(0.08f, (float)vlevel(v) / (float)kFluidFull);
                    auto corner_h = [&](int cx, int cz) {
                        float sum = 0.0f;
                        int n = 0;
                        for (int dz = cz - 1; dz <= cz; ++dz)
                            for (int dx = cx - 1; dx <= cx; ++dx) {
                                const Voxel nv = at(x + dx, y, z + dz);
                                if (vmat(nv) != mid) continue;
                                if (vmat(at(x + dx, y + 1, z + dz)) == mid) return 1.0f;
                                sum += std::max(0.08f, (float)vlevel(nv) / (float)kFluidFull);
                                ++n;
                            }
                        return n ? sum / (float)n : h;
                    };
                    const float ch[2][2] = {{corner_h(0, 0), corner_h(0, 1)}, {corner_h(1, 0), corner_h(1, 1)}};
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
                            const int cx = kFaceCorners[f][k][0], cz = kFaceCorners[f][k][2];
                            const float top = water_above ? 1.0f : ch[cx][cz];
                            p[k][0] = (float)(wx + cx);
                            p[k][1] = (float)wy + (float)kFaceCorners[f][k][1] * top;
                            p[k][2] = (float)(wz + cz);
                        }
                        float n[3] = {(float)d.x, (float)d.y, (float)d.z};
                        RGBf c = base;
                        RGBf cs[4] = {c, c, c, c};
                        const int ao0[4] = {0, 0, 0, 0};
                        // v: 1 where water keeps falling below (a cascade), 0 otherwise.
                        const Material& below = reg.mat(vmat(at(x, y - 1, z)));
                        const float falling = (!below.solid && vmat(at(x, y - 1, z)) != mid) ? 1.0f : 0.0f;
                        push_quad(out.water, p, n, cs, (float)vlevel(v) / (float)kFluidFull, ao0, (float)mid, falling);
                    }
                    continue;
                }

                if (!m.solid && !m.passable) {
                    // Small plants: crops and bushes as boxes inside the cube.
                    if (mid == M.crop) {
                        // Wheat: rows of stalks (a # of four planes) that grow and ripen;
                        // decoration layers 7 sprout, 8 green, 9 ripe.
                        const int stage = vlevel(v);
                        const float hgt = 0.28f + 0.09f * (float)stage;
                        const int layer = stage <= 2 ? 7 : (stage <= 5 ? 8 : 9);
                        const float g = 1.0f + var;
                        push_crop(out.crops, (float)wx, (float)wy, (float)wz, hgt, layer, RGBf{g, g, g});
                    } else if (m.sprite_layer >= 0) {
                        // Wild plants drawn as sprites: reeds and grain in dense stands,
                        // mushrooms and herbs as small crossed clumps.
                        const u64 hh = hash3(cseed + 91, wx, wy, wz);
                        const float g = 1.0f + var;
                        if (m.sprite == "reeds" || m.sprite == "wild_grain") {
                            const float hgt = (m.sprite == "reeds" ? 1.05f : 0.78f) + 0.2f * hash_to_unit(hh);
                            push_crop(out.decor, (float)wx, (float)wy, (float)wz, hgt, m.sprite_layer, RGBf{g, g, g});
                        } else {
                            const float cx = (float)wx + 0.35f + 0.3f * hash_to_unit(hh >> 5);
                            const float cz = (float)wz + 0.35f + 0.3f * hash_to_unit(hh >> 9);
                            const float size = 0.5f + 0.2f * hash_to_unit(hh >> 14);
                            push_sprite(out.decor, cx, (float)wy, cz, size * 0.5f, size, 0.785f * hash_to_unit(hh >> 17),
                                        m.sprite_layer, RGBf{g, g, g});
                        }
                    } else if (mid == M.campfire && mid != M.air) {
                        // A ring of stones, logs leaning together and the flames between.
                        const u64 hh = hash3(cseed + 17, wx, wy, wz);
                        const RGBf stone_c{0.52f, 0.5f, 0.47f};
                        for (int k = 0; k < 7; ++k) {
                            const float a = (float)k / 7.0f * 6.2831853f + 0.4f * hash_to_unit(hh >> k);
                            const float sx = wx + 0.5f + std::cos(a) * 0.4f, sz = wz + 0.5f + std::sin(a) * 0.4f;
                            const float r = 0.07f + 0.03f * hash_to_unit(hh >> (k + 8));
                            push_box(out.foliage, sx - r, (float)wy, sz - r, sx + r, wy + r * 1.3f, sz + r,
                                     scalec(stone_c, 0.85f + 0.3f * hash_to_unit(hh >> (k + 3))), 0.0f, M.stone);
                        }
                        const RGBf wood_c{0.42f, 0.29f, 0.18f};
                        push_box(out.foliage, wx + 0.2f, (float)wy, wz + 0.44f, wx + 0.8f, wy + 0.1f, wz + 0.56f, wood_c,
                                 0.0f, M.log);
                        push_box(out.foliage, wx + 0.44f, (float)wy, wz + 0.2f, wx + 0.56f, wy + 0.1f, wz + 0.8f, wood_c,
                                 0.0f, M.log);
                        const RGBf ember{0.95f, 0.35f, 0.08f}, flame{1.0f, 0.62f, 0.18f}, core_c{1.0f, 0.85f, 0.45f};
                        push_box(out.foliage, wx + 0.33f, wy + 0.08f, wz + 0.33f, wx + 0.67f, wy + 0.16f, wz + 0.67f, ember,
                                 0.9f);
                        push_box(out.foliage, wx + 0.38f, wy + 0.14f, wz + 0.38f, wx + 0.62f, wy + 0.46f, wz + 0.62f, flame,
                                 1.0f);
                        push_box(out.foliage, wx + 0.44f, wy + 0.3f, wz + 0.44f, wx + 0.56f, wy + 0.66f, wz + 0.56f, core_c,
                                 1.0f);
                    } else if (mid == M.cactus) {
                        // A ribbed column; the top of the stack is rounded off.
                        const bool top = vmat(at(x, y + 1, z)) != mid;
                        const float s = 0.22f;
                        push_box(out.foliage, wx + s, (float)wy, wz + s, wx + 1 - s, wy + (top ? 0.86f : 1.0f), wz + 1 - s,
                                 shade, emission, mid);
                        if (top && (hash3(cseed + 5, wx, wy, wz) & 1)) {
                            // An arm.
                            push_box(out.foliage, wx + 1 - s, wy + 0.35f, wz + 0.4f, wx + 1 - s + 0.22f, wy + 0.5f, wz + 0.6f,
                                     shade, emission, mid);
                            push_box(out.foliage, wx + 1 - s + 0.08f, wy + 0.5f, wz + 0.42f, wx + 1 - s + 0.22f, wy + 0.78f,
                                     wz + 0.58f, shade, emission, mid);
                        }
                    } else {
                        float s = 0.1f;
                        push_box(out.foliage, wx + s, (float)wy, wz + s, wx + 1 - s, wy + 0.75f, wz + 1 - s, shade,
                                 emission, mid);
                        if (mid == M.berry_bush) {
                            RGBf berry{0.75f, 0.12f, 0.2f};
                            for (int b = 0; b < 3; ++b) {
                                float bx = wx + 0.22f + 0.56f * hash_to_unit(hash3(cseed + 1 + b, wx, wy, wz));
                                float bz = wz + 0.22f + 0.56f * hash_to_unit(hash3(cseed + 11 + b, wx, wy, wz));
                                float by = wy + 0.5f + 0.22f * hash_to_unit(hash3(cseed + 21 + b, wx, wy, wz));
                                push_box(out.foliage, bx - 0.07f, by, bz - 0.07f, bx + 0.07f, by + 0.12f, bz + 0.07f, berry,
                                         0.0f);
                            }
                        }
                    }
                    continue;
                }

                // Dry tufts on the savanna (layers 10-11).
                if (mid == M.dry_grass && mid != M.air && vmat(at(x, y + 1, z)) == M.air) {
                    const u64 hh = hash3(cseed + 78, wx, wy, wz);
                    if (hash_to_unit(hh) < 0.45f) {
                        const float cx = (float)wx + 0.25f + 0.5f * hash_to_unit(hh >> 5);
                        const float cz = (float)wz + 0.25f + 0.5f * hash_to_unit(hh >> 9);
                        const float size = 0.45f + 0.35f * hash_to_unit(hh >> 14);
                        const float g = 1.0f + var;
                        push_sprite(out.decor, cx, (float)(wy + 1), cz, size * 0.5f, size, 0.785f * hash_to_unit(hh >> 17),
                                    10 + (int)((hh >> 12) & 1), RGBf{g, g, g});
                    }
                }
                // Fruit hangs under the crown of fruit trees.
                if (mid == M.fruit_leaves && mid != M.air && vmat(at(x, y - 1, z)) == M.air) {
                    const u64 hh = hash3(cseed + 79, wx, wy, wz);
                    const RGBf fruit = (hh & 1) ? RGBf{0.86f, 0.2f, 0.14f} : RGBf{0.95f, 0.6f, 0.15f};
                    for (int b = 0; b < 2; ++b) {
                        const float fx = wx + 0.2f + 0.6f * hash_to_unit(hash3(hh, b, 1, 0));
                        const float fz = wz + 0.2f + 0.6f * hash_to_unit(hash3(hh, b, 2, 0));
                        push_box(out.foliage, fx - 0.09f, wy - 0.2f, fz - 0.09f, fx + 0.09f, (float)wy - 0.02f, fz + 0.09f,
                                 fruit, 0.0f);
                    }
                }
                // Tufts of grass and the odd flower on open meadow (layers 0-2 tufts,
                // 3-6 flowers in the decoration textures).
                if (mid == M.grass && vmat(at(x, y + 1, z)) == M.air) {
                    const u64 hh = hash3(cseed + 77, wx, wy, wz);
                    const float r = hash_to_unit(hh);
                    if (r < 0.62f) {
                        const bool flower = r < 0.045f;
                        const int layer = flower ? 3 + (int)((hh >> 20) % 4) : (int)((hh >> 12) % 3);
                        const float cx = (float)wx + 0.25f + 0.5f * hash_to_unit(hh >> 5);
                        const float cz = (float)wz + 0.25f + 0.5f * hash_to_unit(hh >> 9);
                        const float size = flower ? 0.46f + 0.14f * hash_to_unit(hh >> 14)
                                                  : 0.5f + 0.4f * hash_to_unit(hh >> 14);
                        const float g = 1.0f + var;
                        push_sprite(out.decor, cx, (float)(wy + 1), cz, size * 0.5f, size, 0.785f * hash_to_unit(hh >> 17),
                                    layer, RGBf{g, g, g});
                    }
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
                        cs[k] = scalec(shade, kAO[ao[k]]);
                    }
                    float n[3] = {(float)d.x, (float)d.y, (float)d.z};
                    // A grass cube buried under another shows dirt on its sides.
                    int kind = face_kind(f);
                    if ((mid == M.grass || (mid != M.air && (mid == M.dry_grass || mid == M.snow))) && kind == 1 &&
                        reg.mat(vmat(at(x, y + 1, z))).opaque)
                        kind = 2;
                    push_quad(dst, p, n, cs, emission, ao, (float)mid, (float)kind + 3.0f * dmg);
                }
            }
        }
    }
}

void Mesher::build_lod_column(int cx, int cz, int step, CellMesh& out) {
    out.opaque.clear();
    out.water.clear();
    out.foliage.clear();
    out.decor.clear();
    out.crops.clear();
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    step = std::clamp(step, 1, kCellSize);
    const int n = kCellSize / step;
    const int N = n + 2;
    struct Col {
        bool land = false;
        int top = -1, bottom = 0, water = -1;
        MatId mat = 0, under = 0;
    };
    std::vector<Col> cols((size_t)N * N);
    // Highest modified cell per cell column: built things can stand anywhere in it.
    auto scan_start = [&](int x, int z, const ColumnInfo& ci) {
        int start = ci.land ? ci.top + 20 : -1;
        const int ccx = x >> kCellBits, ccz = z >> kCellBits;
        for (int cy = w_.cells_y() - 1; cy >= 0; --cy) {
            const Cell* cell = w_.cell({ccx, cy, ccz});
            if (cell && !cell->pristine) {
                start = std::max(start, cy * kCellSize + kCellSize - 1);
                break;
            }
        }
        return std::min(start, w_.size_y() - 1);
    };
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const int x = cx * kCellSize + (i - 1) * step + step / 2;
            const int z = cz * kCellSize + (j - 1) * step + step / 2;
            if (x < 0 || z < 0 || x >= w_.size_x() || z >= w_.size_z()) continue;
            Col& c = cols[(size_t)j * N + i];
            const ColumnInfo ci = w_.gen().column(x, z);
            int water = -1;
            for (int y = scan_start(x, z, ci); y >= 0; --y) {
                const MatId m = vmat(w_.peek({x, y, z}));
                if (m == M.air) continue;
                const Material& mm = reg.mat(m);
                if (mm.fluid) {
                    if (water < 0) water = y;
                    continue;
                }
                if (!mm.solid) continue;  // plants
                c.land = true;
                c.top = y;
                c.mat = m;
                c.water = water;
                c.under = vmat(w_.peek({x, y - 1, z}));
                if (c.under == M.air || !reg.mat(c.under).solid) c.under = m;
                break;
            }
            if (!c.land) continue;
            c.bottom = ci.land ? std::min<int>(ci.bottom, c.top) : std::max(0, c.top - 2);
        }
    const int ao0[4] = {0, 0, 0, 0};
    const RGBf white[4] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    // An axis-aligned quad: face f of the box [x0,x1]x[y0,y1]x[z0,z1].
    auto face = [&](MeshData& m, int f, float x0, float y0, float z0, float x1, float y1, float z1, float u, float v,
                    float em = 0.0f) {
        const float lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
        float p[4][3];
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) p[k][a] = kFaceCorners[f][k][a] ? hi[a] : lo[a];
        const float nn[3] = {(float)kDir6[f].x, (float)kDir6[f].y, (float)kDir6[f].z};
        push_quad(m, p, nn, white, em, ao0, u, v);
    };
    const int dirs[4][3] = {{1, 0, 0}, {-1, 0, 1}, {0, 1, 4}, {0, -1, 5}};  // di, dj, face
    for (int j = 1; j <= n; ++j)
        for (int i = 1; i <= n; ++i) {
            const Col& c = cols[(size_t)j * N + i];
            if (!c.land) continue;
            const float x0 = (float)(cx * kCellSize + (i - 1) * step), z0 = (float)(cz * kCellSize + (j - 1) * step);
            const float x1 = x0 + (float)step, z1 = z0 + (float)step;
            const float top = (float)(c.top + 1);
            face(out.opaque, 2, x0, top, z0, x1, top, z1, (float)c.mat, 0.0f);
            if (c.water > c.top) {
                const float wt = (float)(c.water + 1);
                face(out.water, 2, x0, wt, z0, x1, wt, z1, (float)M.water, 0.0f, 1.0f);
            }
            face(out.opaque, 3, x0, (float)c.bottom, z0, x1, (float)c.bottom, z1, (float)M.stone, 2.0f);
            for (const auto& d : dirs) {
                const Col& nb = cols[(size_t)(j + d[1]) * N + (i + d[0])];
                const int f = d[2];
                float bx0 = x0, bx1 = x1, bz0 = z0, bz1 = z1;
                if (f == 0) bx0 = x1;
                if (f == 1) bx1 = x0;
                if (f == 4) bz0 = z1;
                if (f == 5) bz1 = z0;
                // Walls down to the neighbour's surface (or the underside at the rim):
                // the surface cube, the soil below it, then rock.
                const int lo = nb.land ? std::max(nb.top + 1, c.bottom) : c.bottom;
                if (lo < c.top + 1) {
                    const int soil = std::max(lo, c.top - 3);
                    face(out.opaque, f, bx0, (float)c.top, bz0, bx1, top, bz1, (float)c.mat, 1.0f);
                    if (soil < c.top)
                        face(out.opaque, f, bx0, (float)soil, bz0, bx1, (float)c.top, bz1, (float)c.under, 1.0f);
                    if (lo < soil) face(out.opaque, f, bx0, (float)lo, bz0, bx1, (float)soil, bz1, (float)M.stone, 1.0f);
                }
                // Steps in the underside.
                if (nb.land && nb.bottom > c.bottom)
                    face(out.opaque, f, bx0, (float)c.bottom, bz0, bx1, (float)std::min(nb.bottom, c.top), bz1,
                         (float)M.stone, 1.0f);
            }
        }
}

void build_voxel_model(const u8* vox, int sx, int sy, int sz, const std::vector<u32>& palette, float scale,
                       MeshData& out, const std::vector<float>* vary) {
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
                if (vary && v - 1 < (int)vary->size() && (*vary)[v - 1] > 0.0f) {
                    const float k = 1.0f + (*vary)[v - 1] * (hash_to_unit(hash3(0xC107u, x, y, z)) * 2.0f - 1.0f);
                    c = scalec(c, k);
                }
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
        const float var = (hash_to_unit(hash3(0xDEB415ull, p.x, p.y, p.z)) - 0.5f) * 2.0f * m.color_var;
        RGBf c{1.0f + var, 1.0f + var, 1.0f + var};
        const float dmg = (float)std::min<int>(vdamage(v), 7);
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
            push_quad(out, q, n, cs, 0.0f, ao0, (float)m.id, (float)face_kind(f) + 3.0f * dmg);
        }
    }
}

}  // namespace icarus
