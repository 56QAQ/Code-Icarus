// Furniture that spans several cubes (a bed three cubes long, a row of shelves): how a
// piece lies, worked out from the cubes around it. The mesher (to draw it) and the
// buildings (to find the beds in a blueprint) must agree on it, so both use this.
#pragma once

#include <cstdlib>

#include "icarus/data/registry.h"
#include "icarus/util/types.h"

namespace icarus {

struct FurnitureRun {
    Vec3i start, end;  // the two end cubes (start has the lower coordinate)
    Vec3i axis;        // unit x or z
    Vec3i head;        // beds and mats: the end where the head lies
    int length = 1;
};

// The run of like cubes through `p` (`mat_at(Vec3i) -> MatId`): along the longer of its
// x and z extents (x on a tie); a single cube points away from the wall it touches. The
// head lies against a wall: at the start when a wall is there or none is at the end.
template <class MatAt>
FurnitureRun furniture_run(const Registry& reg, const MatAt& mat_at, const Vec3i& p) {
    const MatId mid = mat_at(p);
    auto wall = [&](const Vec3i& q) {
        const Material& m = reg.mat(mat_at(q));
        return m.solid && m.opaque;
    };
    auto span = [&](const Vec3i& ax, Vec3i& lo, Vec3i& hi) {
        lo = p;
        hi = p;
        for (int k = 0; k < 4 && mat_at(lo - ax) == mid; ++k) lo = lo - ax;
        for (int k = 0; k < 4 && mat_at(hi + ax) == mid; ++k) hi = hi + ax;
        return 1 + std::abs(hi.x - lo.x) + std::abs(hi.z - lo.z);
    };
    const Vec3i X{1, 0, 0}, Z{0, 0, 1};
    FurnitureRun r;
    Vec3i xlo, xhi, zlo, zhi;
    const int nx = span(X, xlo, xhi), nz = span(Z, zlo, zhi);
    bool along_x = nx >= nz;
    if (nx == 1 && nz == 1) along_x = wall(p + X) || wall(p - X) || !(wall(p + Z) || wall(p - Z));
    r.axis = along_x ? X : Z;
    r.start = along_x ? xlo : zlo;
    r.end = along_x ? xhi : zhi;
    r.length = along_x ? nx : nz;
    const bool head_at_start = wall(r.start - r.axis) || !wall(r.end + r.axis);
    r.head = head_at_start ? r.start : r.end;
    return r;
}

}  // namespace icarus
