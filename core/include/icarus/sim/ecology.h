// Ecology: the living world between the voxels. Once a day, near trees that still
// stand, a few saplings take root on open grass; after some days a sapling grows into a
// tree (a real trunk and crown). Berry bushes re-sprout near the woods. So a forest
// felled faster than it regrows shrinks, and one left alone slowly returns.
//
// Only places the simulation has already touched are seeded: an island nobody has
// visited keeps its generated forest untouched and costs nothing.
//
// On the continent, trees come back as the species of their biome (pines in the cold,
// acacias on the savanna...), wild grain, mushrooms and reeds re-sprout where they
// belong, and picked fruit trees bear again after a few days.
#pragma once

#include <vector>

#include "icarus/util/binio.h"
#include "icarus/util/rng.h"
#include "icarus/world/world.h"

namespace icarus {

class Buildings;

class Ecology {
public:
    Ecology(World& world, const Registry& reg) : w_(world), reg_(&reg) {}

    void reset(u64 seed);
    void daily(Tick now, const Buildings& buildings);

    // Trees that grew from saplings and still stand.
    int regrown_standing() const;
    size_t saplings() const { return saplings_.size(); }
    // A wild plant was gathered here (fruit picked, a bush stripped, grain cut); it
    // grows back after a few days if the spot is still free.
    void picked(const Vec3i& p, Tick now, MatId plant);

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    struct Sapling {
        Vec3i pos;
        Tick planted = 0;
    };
    bool open_grass(const Vec3i& above, const Buildings& buildings);
    void grow_tree(const Vec3i& base);
    MatId wild_plant_for(const Vec3i& p);

    World& w_;
    const Registry* reg_;
    Rng rng_;
    std::vector<Sapling> saplings_;
    std::vector<Vec3i> regrown_;
    struct Picked {
        Vec3i pos;
        Tick when = 0;
        MatId plant = 0;
    };
    std::vector<Picked> picked_;  // gathered plants waiting to grow back
};

}  // namespace icarus
