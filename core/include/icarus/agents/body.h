// Voxel bodies. A character is six parts, each a small grid of body voxels
// (1/8 of a world cube). Damage removes voxels; function (walking, holding, living)
// is derived from what remains, so injuries have real consequences.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "icarus/util/binio.h"
#include "icarus/util/rng.h"
#include "icarus/util/types.h"

namespace icarus {

enum BodyPartId : u8 { kHead = 0, kTorso, kArmL, kArmR, kLegL, kLegR, kPartCount };
const char* body_part_name_zh(int p);

constexpr float kBodyScale = 1.0f / 8.0f;  // body voxel edge in world cubes

struct PartShape {
    Vec3i size;     // x (width), y (height), z (depth) in body voxels
    Vec3i origin;   // min corner relative to the body origin (feet centre at x=0,z=0)
    int joint_y;    // local y of the layer that connects to the torso (-1 for torso)
};
const PartShape& part_shape(int p);

// Palette slots (value stored in voxels; 0 = empty).
enum BodyPaint : u8 { kSkin = 1, kHair, kCloth, kAccent, kShoes, kEyes, kBone, kPaintCount };

struct BodyPart {
    std::vector<u8> vox;  // size.x*size.y*size.z, index (y*sz+z)*sx+x
    i32 total = 0;
    i32 alive = 0;
    bool severed = false;
    float integrity() const { return total > 0 ? (float)alive / (float)total : 0.0f; }
};

struct Appearance {
    u32 skin = 0xE8C4A8, hair = 0x4A3528, cloth = 0x6B7F99, accent = 0xC9B27A, shoes = 0x3A2E26, eyes = 0x2B3A55;
    bool long_hair = false;
    bool dress = false;
    bool ribbon = false;
    std::vector<u32> palette() const { return {skin, hair, cloth, accent, shoes, eyes, 0xE8E0D0}; }
};

struct DamageReport {
    int removed = 0;
    int severed_part = -1;
    bool lethal = false;
};

struct Body {
    std::array<BodyPart, kPartCount> parts;
    u32 version = 1;
    float bleeding = 0.0f;   // per-day vitality loss rate
    float vitality = 1.0f;   // 0 = death

    void build(const Appearance& a);
    // Remove voxels within a sphere given in body-local voxel coordinates.
    DamageReport damage_sphere(const Vec3f& local_center, float radius, Rng& rng);
    // Remove about `fraction` of total voxels spread over exposed surfaces (fire, crush).
    DamageReport damage_spread(float fraction, Rng& rng, int preferred_part = -1);
    // Regrow up to `count` missing voxels in non-severed parts; returns voxels regrown.
    // Regrows missing voxels next to living tissue; with `limbs`, severed parts regrow too.
    int regrow(int count, const Appearance& a, bool limbs = false);

    float part_integrity(int p) const { return parts[p].severed ? 0.0f : parts[p].integrity(); }
    float mobility() const;      // 0..1, from legs
    float manipulation() const;  // 0..1, from arms/hands
    bool can_hold() const;       // at least one working hand
    bool fatal() const;
    int total_alive() const;
    int total_voxels() const;

    void save(BinWriter& w) const;
    void load(BinReader& r);

private:
    void paint_part(int p, const Appearance& a);
    void check_severed(int p, DamageReport& rep);
};

}  // namespace icarus
