#include "icarus/util/noise.h"

#include <cmath>

namespace icarus {

namespace {

inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

inline float grad2(u64 seed, i32 ix, i32 iy, float dx, float dy) {
    u64 h = hash3(seed, ix, iy, 0x5bd1);
    // 8 gradient directions
    switch (h & 7u) {
        case 0: return dx + dy;
        case 1: return dx - dy;
        case 2: return -dx + dy;
        case 3: return -dx - dy;
        case 4: return dx * 1.41421356f;
        case 5: return -dx * 1.41421356f;
        case 6: return dy * 1.41421356f;
        default: return -dy * 1.41421356f;
    }
}

inline float grad3(u64 seed, i32 ix, i32 iy, i32 iz, float dx, float dy, float dz) {
    u64 h = hash3(seed, ix, iy, iz) % 12u;
    switch (h) {
        case 0: return dx + dy;
        case 1: return -dx + dy;
        case 2: return dx - dy;
        case 3: return -dx - dy;
        case 4: return dx + dz;
        case 5: return -dx + dz;
        case 6: return dx - dz;
        case 7: return -dx - dz;
        case 8: return dy + dz;
        case 9: return -dy + dz;
        case 10: return dy - dz;
        default: return -dy - dz;
    }
}

}  // namespace

float gradient_noise2(u64 seed, float x, float y) {
    float fx = std::floor(x), fy = std::floor(y);
    i32 ix = (i32)fx, iy = (i32)fy;
    float dx = x - fx, dy = y - fy;
    float u = fade(dx), v = fade(dy);
    float n00 = grad2(seed, ix, iy, dx, dy);
    float n10 = grad2(seed, ix + 1, iy, dx - 1.0f, dy);
    float n01 = grad2(seed, ix, iy + 1, dx, dy - 1.0f);
    float n11 = grad2(seed, ix + 1, iy + 1, dx - 1.0f, dy - 1.0f);
    float nx0 = lerpf(n00, n10, u);
    float nx1 = lerpf(n01, n11, u);
    return lerpf(nx0, nx1, v) * 0.7071f;
}

float gradient_noise3(u64 seed, float x, float y, float z) {
    float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    i32 ix = (i32)fx, iy = (i32)fy, iz = (i32)fz;
    float dx = x - fx, dy = y - fy, dz = z - fz;
    float u = fade(dx), v = fade(dy), w = fade(dz);
    float n000 = grad3(seed, ix, iy, iz, dx, dy, dz);
    float n100 = grad3(seed, ix + 1, iy, iz, dx - 1, dy, dz);
    float n010 = grad3(seed, ix, iy + 1, iz, dx, dy - 1, dz);
    float n110 = grad3(seed, ix + 1, iy + 1, iz, dx - 1, dy - 1, dz);
    float n001 = grad3(seed, ix, iy, iz + 1, dx, dy, dz - 1);
    float n101 = grad3(seed, ix + 1, iy, iz + 1, dx - 1, dy, dz - 1);
    float n011 = grad3(seed, ix, iy + 1, iz + 1, dx, dy - 1, dz - 1);
    float n111 = grad3(seed, ix + 1, iy + 1, iz + 1, dx - 1, dy - 1, dz - 1);
    float nx00 = lerpf(n000, n100, u), nx10 = lerpf(n010, n110, u);
    float nx01 = lerpf(n001, n101, u), nx11 = lerpf(n011, n111, u);
    float nxy0 = lerpf(nx00, nx10, v), nxy1 = lerpf(nx01, nx11, v);
    return lerpf(nxy0, nxy1, w) * 0.9f;
}

float fbm2(u64 seed, float x, float y, int octaves, float lacunarity, float gain) {
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * gradient_noise2(seed + (u64)i * 7919u, x, y);
        norm += amp;
        amp *= gain;
        x *= lacunarity;
        y *= lacunarity;
    }
    return sum / norm;
}

float fbm3(u64 seed, float x, float y, float z, int octaves, float lacunarity, float gain) {
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * gradient_noise3(seed + (u64)i * 7919u, x, y, z);
        norm += amp;
        amp *= gain;
        x *= lacunarity;
        y *= lacunarity;
        z *= lacunarity;
    }
    return sum / norm;
}

float ridged2(u64 seed, float x, float y, int octaves) {
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.0f - std::fabs(gradient_noise2(seed + (u64)i * 104729u, x, y));
        sum += amp * n * n;
        norm += amp;
        amp *= 0.5f;
        x *= 2.0f;
        y *= 2.0f;
    }
    return sum / norm;
}

}  // namespace icarus
