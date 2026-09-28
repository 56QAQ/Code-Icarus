// Where bodies lie on the ground, for keeping them apart. A sleeper lies on her side
// along her bed (three cubes in a row, about 2.6 cubes long and one wide); someone
// sleeping rough without a bed covers her own cube.
#pragma once

#include <cstdlib>

#include "icarus/util/types.h"

namespace icarus {

struct BedPrint {
    float x0, z0, x1, z1;  // xz rectangle
    int y;                 // floor height
    bool overlaps(const BedPrint& o) const {
        constexpr float eps = 0.02f;
        return std::abs(y - o.y) < 2 && x0 < o.x1 - eps && o.x0 < x1 - eps && z0 < o.z1 - eps && o.z0 < z1 - eps;
    }
};

// The bed centred on cube `mid` along `axis` (a unit x or z vector; zero for one cube).
inline BedPrint bed_print(const Vec3i& mid, const Vec3i& axis) {
    const float hx = axis.x ? 1.3f : 0.5f, hz = axis.z ? 1.3f : 0.5f;
    const float cx = (float)mid.x + 0.5f, cz = (float)mid.z + 0.5f;
    return {cx - hx, cz - hz, cx + hx, cz + hz, mid.y};
}

}  // namespace icarus
