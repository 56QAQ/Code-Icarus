// Fields of farmland plots. Crops grow as real cubes (stage in the voxel) and need
// irrigation: water within reach at field level. Without water, growth nearly stops
// and young crops wither — which is how losing a water source reaches the harvest.
#pragma once

#include <string>
#include <vector>

#include "icarus/sim/chronicle.h"
#include "icarus/util/rng.h"
#include "icarus/world/world.h"

namespace icarus {

enum class PlotState : u8 { NeedsTill = 0, Empty, Growing, Mature };

struct Plot {
    Vec3i ground;          // the farmland cube; the crop grows in ground + (0,1,0)
    float growth = 0;      // 0..1
    bool irrigated = false;
    Tick dry_since = 0;    // when irrigation was lost (0 = watered)
};

struct Farm {
    u32 id = 0;
    bool alive = false;
    u16 polity = 0;
    std::string name;
    Vec3i center;
    std::vector<Plot> plots;
};

struct FarmStats {
    int plots = 0, needs_till = 0, empty = 0, growing = 0, mature = 0, irrigated = 0;
};

class Farming {
public:
    Farming(World& w, Chronicle& c) : w_(w), chron_(c) {}
    void reset();

    u32 create(u16 polity, const std::string& name, const std::vector<Vec3i>& grounds);
    Farm* get(u32 id) { return (id > 0 && id < farms_.size() && farms_[id].alive) ? &farms_[id] : nullptr; }
    const std::vector<Farm>& all() const { return farms_; }

    PlotState state(const Plot& p);
    bool till(Plot& p, EventId cause);
    bool sow(Plot& p, EventId cause);
    int harvest(Plot& p, EventId cause);  // returns grain yield

    void step(Tick now, Rng& rng);
    // Adds up to n new plots next to existing ones where irrigation reaches. Returns count.
    int expand(u32 farm_id, int n);
    // Moves the share of plots nearest to `toward` into a new farm owned by polity.
    u32 split(u32 farm_id, u16 polity, const Vec3i& toward, float share, const std::string& name);
    // Lays out up to n new plots on fertile, irrigable ground near a point. 0 if none.
    u32 found(u16 polity, const std::string& name, const Vec3i& near, int radius, int n);
    FarmStats stats(u32 farm_id);
    FarmStats stats_polity(u16 polity);

    // Tunables.
    float growth_days = 2.2f;       // irrigated days from sowing to maturity
    float dry_factor = 0.08f;       // growth multiplier without irrigation
    float wither_days = 1.2f;       // unirrigated young crops die after this
    int yield = 5;

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    bool check_irrigation(const Plot& p);

    World& w_;
    Chronicle& chron_;
    std::vector<Farm> farms_ = std::vector<Farm>(1);
    u32 cursor_ = 0;
    int withered_today_ = 0;
    Tick last_wither_report_ = 0;
};

}  // namespace icarus
