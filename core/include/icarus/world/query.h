// Read-only world queries for the presentation layer (picking, inspection).
#pragma once

#include "icarus/world/world.h"

namespace icarus {

struct RayHit {
    bool hit = false;
    Vec3i cube;      // the cube that was hit
    Vec3i normal;    // face normal of the hit
    float distance = 0;
    Voxel voxel = 0;
};

// DDA ray march using renderer-safe peek access. Hits solids and (optionally) fluids.
RayHit raycast(const World& w, Vec3f origin, Vec3f dir, float max_dist, bool hit_fluids);

}  // namespace icarus
