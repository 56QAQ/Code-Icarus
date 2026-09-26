// Deterministic random numbers.
//  - Rng: PCG32 stream generator, fully serialisable (state + increment).
//  - hash functions: pure, order-independent randomness for world generation.
#pragma once

#include "icarus/util/types.h"

namespace icarus {

inline u64 splitmix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline u64 hash_combine(u64 a, u64 b) { return splitmix64(a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2))); }

inline u64 hash3(u64 seed, i32 x, i32 y, i32 z) {
    u64 h = splitmix64(seed);
    h = hash_combine(h, (u64)(u32)x);
    h = hash_combine(h, (u64)(u32)y);
    h = hash_combine(h, (u64)(u32)z);
    return h;
}

// Uniform float in [0,1) from a hash.
inline float hash_to_unit(u64 h) { return (float)((h >> 40) & 0xFFFFFF) / 16777216.0f; }

inline u64 hash_string(const std::string& s) {
    u64 h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

class Rng {
public:
    Rng() { seed(0x853C49E6748FEA9Bull, 0xDA3E39CB94B95BDBull); }
    explicit Rng(u64 s, u64 stream = 0xDA3E39CB94B95BDBull) { seed(s, stream); }

    void seed(u64 s, u64 stream) {
        state_ = 0;
        inc_ = (stream << 1u) | 1u;
        next_u32();
        state_ += s;
        next_u32();
    }

    u32 next_u32() {
        u64 old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        u32 xorshifted = (u32)(((old >> 18u) ^ old) >> 27u);
        u32 rot = (u32)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }
    u64 next_u64() { return ((u64)next_u32() << 32) | next_u32(); }

    // [0, n)
    u32 below(u32 n) {
        if (n <= 1) return 0;
        u32 threshold = (0u - n) % n;
        for (;;) {
            u32 r = next_u32();
            if (r >= threshold) return r % n;
        }
    }
    // [lo, hi] inclusive
    i32 range(i32 lo, i32 hi) { return hi <= lo ? lo : lo + (i32)below((u32)(hi - lo + 1)); }
    float unit() { return (float)(next_u32() >> 8) / 16777216.0f; }
    float uniform(float lo, float hi) { return lo + (hi - lo) * unit(); }
    bool chance(float p) { return unit() < p; }
    // Approximate normal via sum of uniforms (deterministic, cheap).
    float normalish(float mean, float sd) {
        float s = unit() + unit() + unit() + unit() - 2.0f;
        return mean + sd * s * 1.7320508f;
    }

    u64 state() const { return state_; }
    u64 inc() const { return inc_; }
    void set_raw(u64 st, u64 inc) { state_ = st; inc_ = inc; }

private:
    u64 state_ = 0;
    u64 inc_ = 1;
};

}  // namespace icarus
