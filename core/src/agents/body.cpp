#include "icarus/agents/body.h"

#include <algorithm>
#include <cmath>

namespace icarus {

namespace {
// Body layout (body voxels, feet centre at origin, +z = facing direction).
//   legs 3x8x3, torso 6x7x4, arms 2x7x2 hanging from the shoulders, head 6x6x6.
const PartShape kShapes[kPartCount] = {
    {{6, 6, 6}, {-3, 15, -3}, 0},   // head (joint = bottom layer, the neck)
    {{6, 7, 4}, {-3, 8, -2}, -1},   // torso
    {{2, 7, 2}, {-5, 8, -1}, 6},    // left arm (joint = top layer, the shoulder)
    {{2, 7, 2}, {3, 8, -1}, 6},     // right arm
    {{3, 8, 3}, {-3, 0, -2}, 7},    // left leg (joint = top layer, the hip)
    {{3, 8, 3}, {0, 0, -2}, 7},     // right leg
};
inline int vidx(const PartShape& s, int x, int y, int z) { return (y * s.size.z + z) * s.size.x + x; }
}  // namespace

const char* body_part_name_zh(int p) {
    static const char* n[] = {"头部", "躯干", "左臂", "右臂", "左腿", "右腿"};
    return (p >= 0 && p < kPartCount) ? n[p] : "?";
}

const PartShape& part_shape(int p) { return kShapes[p]; }

void Body::paint_part(int p, const Appearance& a) {
    const PartShape& s = kShapes[p];
    BodyPart& bp = parts[p];
    bp.vox.assign((size_t)(s.size.x * s.size.y * s.size.z), 0);
    for (int y = 0; y < s.size.y; ++y)
        for (int z = 0; z < s.size.z; ++z)
            for (int x = 0; x < s.size.x; ++x) {
                u8 c = kCloth;
                switch (p) {
                    case kHead: {
                        c = kSkin;
                        bool top = y >= s.size.y - 2;
                        bool back = z == 0;
                        bool side = (x == 0 || x == s.size.x - 1) && y >= 2;
                        if (top || (back && y >= (a.long_hair ? 0 : 2)) || side) c = kHair;
                        // The face: eyes two voxels tall, a small mouth; girls blush.
                        const bool front = z == s.size.z - 1;
                        if (front && (y == 2 || y == 3) && (x == 1 || x == s.size.x - 2)) c = kEyes;
                        if (front && y == 1 && (x == 2 || x == 3)) c = kMouth;
                        if (front && a.ribbon && y == 1 && (x == 1 || x == s.size.x - 2)) c = kBlush;
                        if (a.ribbon && y == s.size.y - 1 && (x == 1 || x == 4) && z == 1) c = kAccent;
                        break;
                    }
                    case kTorso:
                        c = kCloth;
                        if (y == 1) c = kAccent;  // belt / sash
                        if (a.ribbon && y == s.size.y - 1) c = kAccent;  // a magical girl's collar
                        if (y == s.size.y - 1 && (x == 2 || x == 3) && z == s.size.z - 1) c = kSkin;  // neckline
                        break;
                    case kArmL:
                    case kArmR:
                        c = y <= 1 ? kSkin : kCloth;
                        if (a.ribbon && y == 2) c = kAccent;  // cuffs
                        break;
                    case kLegL:
                    case kLegR:
                        c = y <= 1 ? kShoes : (a.dress && y >= 4 ? kCloth : kSkin);
                        if (!a.dress && y >= 2) c = kCloth;
                        if (a.dress && a.ribbon && y == 4) c = kAccent;  // hem
                        break;
                }
                bp.vox[vidx(s, x, y, z)] = c;
            }
    bp.total = (i32)bp.vox.size();
    bp.alive = bp.total;
    bp.severed = false;
}

void Body::build(const Appearance& a) {
    for (int p = 0; p < kPartCount; ++p) paint_part(p, a);
    version++;
    bleeding = 0;
    vitality = 1.0f;
}

void Body::check_severed(int p, DamageReport& rep) {
    const PartShape& s = kShapes[p];
    BodyPart& bp = parts[p];
    if (bp.severed || s.joint_y < 0) return;
    int y = s.joint_y;
    bool any = false;
    for (int z = 0; z < s.size.z && !any; ++z)
        for (int x = 0; x < s.size.x && !any; ++x)
            if (bp.vox[vidx(s, x, y, z)]) any = true;
    if (!any) {
        bp.severed = true;
        bp.alive = 0;
        std::fill(bp.vox.begin(), bp.vox.end(), 0);
        rep.severed_part = p;
        bleeding += 0.8f;
    }
}

DamageReport Body::damage_sphere(const Vec3f& c, float radius, Rng& rng) {
    DamageReport rep;
    float r2 = radius * radius;
    for (int p = 0; p < kPartCount; ++p) {
        const PartShape& s = kShapes[p];
        BodyPart& bp = parts[p];
        if (bp.severed) continue;
        for (int y = 0; y < s.size.y; ++y)
            for (int z = 0; z < s.size.z; ++z)
                for (int x = 0; x < s.size.x; ++x) {
                    u8& v = bp.vox[vidx(s, x, y, z)];
                    if (!v) continue;
                    float dx = (float)(s.origin.x + x) + 0.5f - c.x;
                    float dy = (float)(s.origin.y + y) + 0.5f - c.y;
                    float dz = (float)(s.origin.z + z) + 0.5f - c.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > r2) continue;
                    // Ragged edge.
                    if (d2 > r2 * 0.6f && rng.chance(0.4f)) continue;
                    v = 0;
                    bp.alive--;
                    rep.removed++;
                }
        check_severed(p, rep);
    }
    if (rep.removed > 0) {
        bleeding += (float)rep.removed / (float)std::max(1, total_voxels()) * 3.0f;
        version++;
    }
    rep.lethal = fatal();
    return rep;
}

DamageReport Body::damage_spread(float fraction, Rng& rng, int preferred_part) {
    DamageReport rep;
    int want = (int)std::ceil(fraction * (float)total_voxels());
    for (int attempt = 0; attempt < want * 6 && rep.removed < want; ++attempt) {
        int p = (preferred_part >= 0 && rng.chance(0.6f)) ? preferred_part : (int)rng.below(kPartCount);
        BodyPart& bp = parts[p];
        if (bp.severed || bp.alive <= 0) continue;
        const PartShape& s = kShapes[p];
        // Pick a random voxel; only remove if exposed (has an empty 6-neighbour or on the boundary).
        int x = (int)rng.below((u32)s.size.x), y = (int)rng.below((u32)s.size.y), z = (int)rng.below((u32)s.size.z);
        u8& v = bp.vox[vidx(s, x, y, z)];
        if (!v) continue;
        bool exposed = false;
        for (int d = 0; d < 6 && !exposed; ++d) {
            int nx = x + kDir6[d].x, ny = y + kDir6[d].y, nz = z + kDir6[d].z;
            if (nx < 0 || ny < 0 || nz < 0 || nx >= s.size.x || ny >= s.size.y || nz >= s.size.z) exposed = true;
            else if (!bp.vox[vidx(s, nx, ny, nz)]) exposed = true;
        }
        if (!exposed) continue;
        v = 0;
        bp.alive--;
        rep.removed++;
    }
    for (int p = 0; p < kPartCount; ++p) check_severed(p, rep);
    if (rep.removed > 0) {
        bleeding += (float)rep.removed / (float)std::max(1, total_voxels()) * 2.0f;
        version++;
    }
    rep.lethal = fatal();
    return rep;
}

int Body::regrow(int count, const Appearance& a, bool limbs) {
    if (count <= 0 && !limbs) return 0;
    Body fresh;
    fresh.build(a);
    int done = 0;
    for (int p = 0; p < kPartCount; ++p) {
        BodyPart& bp = parts[p];
        if (bp.severed && limbs) {
            bp = fresh.parts[p];
            done += bp.alive;
        }
    }
    for (int p = 0; p < kPartCount && done < count; ++p) {
        BodyPart& bp = parts[p];
        if (bp.severed) continue;
        const PartShape& s = kShapes[p];
        // Regrow from the inside out: voxels adjacent to existing ones first.
        for (int y = 0; y < s.size.y && done < count; ++y)
            for (int z = 0; z < s.size.z && done < count; ++z)
                for (int x = 0; x < s.size.x && done < count; ++x) {
                    int i = vidx(s, x, y, z);
                    if (bp.vox[i]) continue;
                    bool attached = false;
                    for (int d = 0; d < 6 && !attached; ++d) {
                        int nx = x + kDir6[d].x, ny = y + kDir6[d].y, nz = z + kDir6[d].z;
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= s.size.x || ny >= s.size.y || nz >= s.size.z) continue;
                        if (bp.vox[vidx(s, nx, ny, nz)]) attached = true;
                    }
                    if (!attached) continue;
                    bp.vox[i] = fresh.parts[p].vox[i];
                    bp.alive++;
                    done++;
                }
    }
    if (done) version++;
    return done;
}

float Body::mobility() const {
    float l = part_integrity(kLegL), r = part_integrity(kLegR);
    if (parts[kLegL].severed && parts[kLegR].severed) return 0.12f;  // crawling
    if (parts[kLegL].severed || parts[kLegR].severed) return 0.3f * std::max(l, r) + 0.05f;
    return clampv(0.25f + 0.75f * std::min(l, r) * 0.6f + 0.75f * std::max(l, r) * 0.4f, 0.1f, 1.0f);
}

float Body::manipulation() const {
    float l = part_integrity(kArmL), r = part_integrity(kArmR);
    return clampv(std::max(l, r) * 0.75f + std::min(l, r) * 0.25f, 0.0f, 1.0f);
}

bool Body::can_hold() const {
    // A hand is the bottom two layers of an arm.
    for (int p : {kArmL, kArmR}) {
        const BodyPart& bp = parts[p];
        if (bp.severed) continue;
        const PartShape& s = kShapes[p];
        int hand = 0;
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < s.size.z; ++z)
                for (int x = 0; x < s.size.x; ++x)
                    if (bp.vox[vidx(s, x, y, z)]) ++hand;
        if (hand >= 4) return true;
    }
    return false;
}

bool Body::fatal() const {
    return part_integrity(kHead) < 0.45f || part_integrity(kTorso) < 0.40f || vitality <= 0.0f;
}

int Body::total_alive() const {
    int n = 0;
    for (auto& p : parts) n += p.alive;
    return n;
}

int Body::total_voxels() const {
    int n = 0;
    for (auto& p : parts) n += p.total;
    return n;
}

void Body::save(BinWriter& w) const {
    w.u32v(version);
    w.f32(bleeding);
    w.f32(vitality);
    for (auto& p : parts) {
        w.boolean(p.severed);
        w.vari(p.alive);
        w.bytes(p.vox);
    }
}

void Body::load(BinReader& r) {
    version = r.u32v();
    bleeding = r.f32();
    vitality = r.f32();
    for (int i = 0; i < kPartCount; ++i) {
        BodyPart& p = parts[i];
        p.severed = r.boolean();
        p.alive = (i32)r.vari();
        p.vox = r.bytes();
        const PartShape& s = kShapes[i];
        p.total = s.size.x * s.size.y * s.size.z;
        if ((int)p.vox.size() != p.total) throw BinError("body part size mismatch");
    }
}

}  // namespace icarus
