// Voxel ("cube") encoding.
//   bits 0..7   material id
//   bits 8..11  amount/stage (fluid level 1..15, crop growth stage, ...)
//   bits 12..14 damage (0..7)
//   bit  15     burning flag
#pragma once

#include "icarus/data/registry.h"
#include "icarus/util/types.h"

namespace icarus {

using Voxel = u16;

constexpr int kCellBits = 5;
constexpr int kCellSize = 1 << kCellBits;  // 32 cubes per cell edge
constexpr int kCellMask = kCellSize - 1;
constexpr int kCellVol = kCellSize * kCellSize * kCellSize;

constexpr u8 kFluidFull = 15;

inline constexpr MatId vmat(Voxel v) { return (MatId)(v & 0xFF); }
inline constexpr u8 vlevel(Voxel v) { return (u8)((v >> 8) & 0xF); }
inline constexpr u8 vdamage(Voxel v) { return (u8)((v >> 12) & 0x7); }
inline constexpr bool vburning(Voxel v) { return (v & 0x8000) != 0; }

inline constexpr Voxel make_voxel(MatId m, u8 level = 0, u8 damage = 0, bool burning = false) {
    return (Voxel)(m | ((level & 0xF) << 8) | ((damage & 0x7) << 12) | (burning ? 0x8000 : 0));
}
inline constexpr Voxel with_level(Voxel v, u8 level) { return (Voxel)((v & ~0x0F00) | ((level & 0xF) << 8)); }
inline constexpr Voxel with_damage(Voxel v, u8 d) { return (Voxel)((v & ~0x7000) | ((d & 0x7) << 12)); }
inline constexpr Voxel with_burning(Voxel v, bool b) { return (Voxel)(b ? (v | 0x8000) : (v & ~0x8000)); }

inline constexpr int local_index(int lx, int ly, int lz) { return (ly * kCellSize + lz) * kCellSize + lx; }

inline Vec3i cell_of(const Vec3i& p) { return {p.x >> kCellBits, p.y >> kCellBits, p.z >> kCellBits}; }
inline Vec3i local_of(const Vec3i& p) { return {p.x & kCellMask, p.y & kCellMask, p.z & kCellMask}; }

}  // namespace icarus
