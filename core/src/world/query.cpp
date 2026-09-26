#include "icarus/world/query.h"

#include <cmath>
#include <limits>

namespace icarus {

RayHit raycast(const World& w, Vec3f origin, Vec3f dir, float max_dist, bool hit_fluids) {
    RayHit res;
    dir = dir.normalized();
    if (dir.length() < 0.5f) return res;
    Vec3i cell = origin.floor_i();
    const float inf = std::numeric_limits<float>::infinity();
    int step[3];
    float tmax[3], tdelta[3];
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {dir.x, dir.y, dir.z};
    int c[3] = {cell.x, cell.y, cell.z};
    for (int a = 0; a < 3; ++a) {
        if (d[a] > 0) {
            step[a] = 1;
            tmax[a] = ((float)c[a] + 1.0f - o[a]) / d[a];
            tdelta[a] = 1.0f / d[a];
        } else if (d[a] < 0) {
            step[a] = -1;
            tmax[a] = (o[a] - (float)c[a]) / -d[a];
            tdelta[a] = 1.0f / -d[a];
        } else {
            step[a] = 0;
            tmax[a] = inf;
            tdelta[a] = inf;
        }
    }
    Vec3i last_normal{0, 0, 0};
    float t = 0;
    const Registry& reg = w.reg();
    for (int iter = 0; iter < 4096 && t <= max_dist; ++iter) {
        Vec3i p{c[0], c[1], c[2]};
        if (w.in_bounds(p)) {
            Voxel v = w.peek(p);
            MatId m = vmat(v);
            if (m != 0) {
                const Material& mat = reg.mat(m);
                if (mat.solid || mat.passable || (hit_fluids && mat.fluid) || (!mat.fluid && !mat.solid)) {
                    res.hit = true;
                    res.cube = p;
                    res.normal = last_normal;
                    res.distance = t;
                    res.voxel = v;
                    return res;
                }
            }
        } else {
            // Outside the world: stop when moving further away.
            bool leaving = (p.y < 0 && step[1] <= 0) || (p.y >= w.size_y() && step[1] >= 0) ||
                           (p.x < 0 && step[0] <= 0) || (p.x >= w.size_x() && step[0] >= 0) ||
                           (p.z < 0 && step[2] <= 0) || (p.z >= w.size_z() && step[2] >= 0);
            if (leaving) break;
        }
        int a = (tmax[0] < tmax[1]) ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        t = tmax[a];
        tmax[a] += tdelta[a];
        c[a] += step[a];
        last_normal = {0, 0, 0};
        if (a == 0) last_normal.x = -step[0];
        if (a == 1) last_normal.y = -step[1];
        if (a == 2) last_normal.z = -step[2];
    }
    return res;
}

}  // namespace icarus
