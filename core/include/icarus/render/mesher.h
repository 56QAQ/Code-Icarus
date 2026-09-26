// Cell mesher: turns cubes into triangle arrays for the presentation layer.
// Engine independent (plain float/int arrays) so it can be tested headless.
// Uses World::peek*, so meshing never changes simulation state.
#pragma once

#include <vector>

#include "icarus/world/world.h"

namespace icarus {

struct MeshData {
    std::vector<float> positions;  // xyz
    std::vector<float> normals;    // xyz
    std::vector<float> colors;     // rgba (a = emission strength)
    std::vector<int> indices;
    size_t vertex_count() const { return positions.size() / 3; }
    bool empty() const { return indices.empty(); }
    void clear() {
        positions.clear();
        normals.clear();
        colors.clear();
        indices.clear();
    }
};

struct CellMesh {
    MeshData opaque;
    MeshData water;
    MeshData foliage;  // leaves, crops, bushes (alpha-scissor-friendly, double sided)
};

class Mesher {
public:
    explicit Mesher(const World& world) : w_(world) {}
    // Builds the mesh of one cell in world coordinates.
    void build_cell(const Vec3i& cc, CellMesh& out);

private:
    Voxel at(int x, int y, int z) const {  // padded local coords -1..32
        return pad_[((y + 1) * 34 + (z + 1)) * 34 + (x + 1)];
    }
    void load_padded(const Vec3i& cc);

    const World& w_;
    std::vector<Voxel> pad_ = std::vector<Voxel>(34 * 34 * 34);
    std::vector<Voxel> tmp_ = std::vector<Voxel>(kCellVol);
};

// Body-part voxel mesh (character parts, debris). vox is sx*sy*sz palette indices
// (0 = empty), palette holds 0xRRGGBB colors indexed by value-1.
void build_voxel_model(const u8* vox, int sx, int sy, int sz, const std::vector<u32>& palette, float scale,
                       MeshData& out);

// Debris body mesh (world material cubes at offsets).
void build_debris_mesh(const Registry& reg, const std::vector<std::pair<Vec3i, Voxel>>& cubes, MeshData& out);

}  // namespace icarus
