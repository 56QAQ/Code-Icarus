// Code:Icarus simulation kernel - basic value types.
// The kernel is engine-independent: nothing in core/ may include Godot headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace icarus {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using Tick = u64;
using EntityId = u32;
constexpr EntityId kNoEntity = 0;

inline i32 floordiv(i32 a, i32 b) {
    i32 q = a / b;
    i32 r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
inline i32 floormod(i32 a, i32 b) {
    i32 r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

template <typename T>
inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float saturate(float v) { return clampv(v, 0.0f, 1.0f); }

struct Vec3i {
    i32 x = 0, y = 0, z = 0;
    constexpr Vec3i() = default;
    constexpr Vec3i(i32 x_, i32 y_, i32 z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3i operator+(const Vec3i& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3i operator-(const Vec3i& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3i operator*(i32 s) const { return {x * s, y * s, z * s}; }
    constexpr bool operator==(const Vec3i& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const Vec3i& o) const { return !(*this == o); }
    constexpr bool operator<(const Vec3i& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
    i32 manhattan(const Vec3i& o) const { return std::abs(x - o.x) + std::abs(y - o.y) + std::abs(z - o.z); }
    i32 chebyshev(const Vec3i& o) const {
        return std::max(std::abs(x - o.x), std::max(std::abs(y - o.y), std::abs(z - o.z)));
    }
    i64 dist2(const Vec3i& o) const {
        i64 dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return dx * dx + dy * dy + dz * dz;
    }
    std::string str() const;
};

struct Vec3iHash {
    size_t operator()(const Vec3i& v) const noexcept {
        u64 h = (u64)(u32)v.x * 0x9E3779B97F4A7C15ull;
        h ^= (u64)(u32)v.y * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= (u64)(u32)v.z * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return (size_t)h;
    }
};

struct Vec3f {
    float x = 0, y = 0, z = 0;
    constexpr Vec3f() = default;
    constexpr Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3f(const Vec3i& v) : x((float)v.x), y((float)v.y), z((float)v.z) {}
    Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3f& operator+=(const Vec3f& o) { x += o.x; y += o.y; z += o.z; return *this; }
    float dot(const Vec3f& o) const { return x * o.x + y * o.y + z * o.z; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float dist_sq(const Vec3f& o) const {
        float dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return dx * dx + dy * dy + dz * dz;
    }
    float length_xz() const { return std::sqrt(x * x + z * z); }
    Vec3f normalized() const {
        float l = length();
        return l > 1e-6f ? Vec3f(x / l, y / l, z / l) : Vec3f(0, 0, 0);
    }
    Vec3i floor_i() const { return {(i32)std::floor(x), (i32)std::floor(y), (i32)std::floor(z)}; }
};

// Six axis directions, used everywhere for voxel neighbourhoods.
constexpr Vec3i kDir6[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
constexpr Vec3i kDir4H[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

}  // namespace icarus
