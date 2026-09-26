// Minimal RGB image + PNG writer (uncompressed deflate), used for headless map dumps.
#pragma once

#include <string>
#include <vector>

#include "icarus/util/types.h"

namespace icarus {

struct Image {
    int w = 0, h = 0;
    std::vector<u8> rgb;
    Image() = default;
    Image(int w_, int h_) : w(w_), h(h_), rgb((size_t)w_ * h_ * 3, 0) {}
    void set(int x, int y, u32 color) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        size_t i = ((size_t)y * w + x) * 3;
        rgb[i] = (u8)(color >> 16);
        rgb[i + 1] = (u8)(color >> 8);
        rgb[i + 2] = (u8)color;
    }
    u32 get(int x, int y) const {
        size_t i = ((size_t)y * w + x) * 3;
        return ((u32)rgb[i] << 16) | ((u32)rgb[i + 1] << 8) | rgb[i + 2];
    }
};

bool write_png(const std::string& path, const Image& img);

inline u32 shade_color(u32 c, float k) {
    auto ch = [&](int s) {
        float v = (float)((c >> s) & 0xFF) * k;
        return (u32)clampv(v, 0.0f, 255.0f);
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

inline u32 mix_color(u32 a, u32 b, float t) {
    auto ch = [&](int s) {
        float va = (float)((a >> s) & 0xFF), vb = (float)((b >> s) & 0xFF);
        return (u32)clampv(va + (vb - va) * t, 0.0f, 255.0f);
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

}  // namespace icarus
