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
    // Texture lookup per vertex: u = material id (-1: plain vertex colour), v = face
    // kind (0 top, 1 side, 2 bottom) + 3 * damage. The shader turns these into a layer
    // of the material texture array; colours then carry only shading (AO, variation).
    std::vector<float> uvs;
    std::vector<int> indices;
    size_t vertex_count() const { return positions.size() / 3; }
    bool empty() const { return indices.empty(); }
    void clear() {
        positions.clear();
        normals.clear();
        colors.clear();
        uvs.clear();
        indices.clear();
    }
};

struct CellMesh {
    MeshData opaque;
    MeshData water;
    MeshData foliage;  // leaves, crops, bushes (alpha-scissor-friendly, double sided)
    // Decoration sprites (grass tufts, flowers): crossed quads, u = layer + x (0..1),
    // v = y (0 at the top) in the decoration texture array.
    MeshData decor;
    MeshData crops;  // wheat, drawn like decoration but at any distance
};

class Mesher {
public:
    explicit Mesher(const World& world) : w_(world) {}
    // Builds the mesh of one cell in world coordinates.
    void build_cell(const Vec3i& cc, CellMesh& out);
    // Coarse stand-in for a whole column of cells seen from afar: the surface sampled
    // every `step` cubes as flat-topped blocks with walls and an underside, textured
    // like the cubes (opaque) plus lake surfaces (water).
    void build_lod_column(int cx, int cz, int step, CellMesh& out);
    // Cutaway views: the cubes in these boxes (a building's roof and the top of its
    // walls) are left out of the meshes, as if they were air, so what is inside shows.
    // Presentation only: the world is untouched.
    struct Cut {
        Vec3i lo, hi;  // inclusive
    };
    void set_cuts(std::vector<Cut> cuts) { cuts_ = std::move(cuts); }
    const std::vector<Cut>& cuts() const { return cuts_; }

private:
    Voxel at(int x, int y, int z) const {  // padded local coords -1..32
        return pad_[((y + 1) * 34 + (z + 1)) * 34 + (x + 1)];
    }
    void load_padded(const Vec3i& cc);

    const World& w_;
    std::vector<Cut> cuts_;
    std::vector<Voxel> pad_ = std::vector<Voxel>(34 * 34 * 34);
    std::vector<Voxel> tmp_ = std::vector<Voxel>(kCellVol);
};

// The colour (0xRRGGBB) of the blanket on the bed whose head is at `head` (the mesher
// draws it; the renderer covers a sleeper with the same one).
u32 bed_blanket_rgb(const Vec3i& head);

// Body-part voxel mesh (character parts, debris). vox is sx*sy*sz palette indices
// (0 = empty), palette holds 0xRRGGBB colors indexed by value-1.
// `vary` (optional, per palette slot): how much each voxel's colour varies (a weave of
// leaves or fur rather than flat cloth).
void build_voxel_model(const u8* vox, int sx, int sy, int sz, const std::vector<u32>& palette, float scale,
                       MeshData& out, const std::vector<float>* vary = nullptr);

// Debris body mesh (world material cubes at offsets).
void build_debris_mesh(const Registry& reg, const std::vector<std::pair<Vec3i, Voxel>>& cubes, MeshData& out);

}  // namespace icarus
