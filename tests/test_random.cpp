// Version 3 world: the random layout (terrain, stepped rivers, lakes, the sea, sites).
#include <set>
#include <vector>

#include "icarus/fauna/fauna.h"
#include "icarus/sim/simulation.h"
#include "icarus/util/binio.h"
#include "icarus/world/world.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
WorldConfig random_island(u64 seed, bool sea = true) {
    WorldConfig c = WorldConfig::for_layout(WorldLayout::Random, seed);
    c.sea = sea;
    c.sized();
    return c;
}

// A water column of the kind asked for within `r` of p (nearest first), or false.
bool find_column(const WorldGen& g, const Vec3i& p, int r, bool want_sea, bool want_weir, Vec3i& out) {
    for (int d = 0; d <= r; ++d)
        for (int dz = -d; dz <= d; ++dz)
            for (int dx = -d; dx <= d; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != d) continue;
                const ColumnInfo c = g.column(p.x + dx, p.z + dz);
                if (!c.land) continue;
                if (want_weir ? c.weir : (c.water_top >= 0 && c.sea == want_sea && !c.frozen)) {
                    out = Vec3i{p.x + dx, want_weir ? c.top : c.water_top, p.z + dz};
                    return true;
                }
            }
    return false;
}
}  // namespace

TEST("random island: deterministic and order independent") {
    const Registry& reg = test_registry();
    WorldGen g1, g2;
    g1.init(random_island(7), reg);
    g2.init(random_island(7), reg);
    const Vec3i cc = cell_of(g1.features().sites[0].water);
    std::vector<Voxel> a(kCellVol), b(kCellVol);
    g1.generate_cell(cc, a.data());
    g2.generate_cell(cc + Vec3i{1, 0, -1}, b.data());
    g2.generate_cell(cc, b.data());
    CHECK(a == b);
    // Another seed is another island.
    WorldGen g3;
    g3.init(random_island(8), reg);
    CHECK(g3.features().sites[0].center != g1.features().sites[0].center);
}

TEST("random island: four sites, level, by fresh water and far apart") {
    const Registry& reg = test_registry();
    for (u64 seed : {1ull, 2ull, 3ull, 4ull}) {
        for (bool sea : {true, false}) {
            WorldGen g;
            g.init(random_island(seed, sea), reg);
            const auto& sites = g.features().sites;
            CHECK_EQ((int)sites.size(), 4);
            for (size_t i = 0; i < sites.size(); ++i) {
                const Site& s = sites[i];
                const ColumnInfo c = g.column(s.center.x, s.center.z);
                CHECK(c.land);
                CHECK(c.water_top < 0);
                int lo = 9999, hi = -9999;
                for (int dz = -8; dz <= 8; dz += 2)
                    for (int dx = -8; dx <= 8; dx += 2) {
                        const ColumnInfo q = g.column(s.center.x + dx, s.center.z + dz);
                        lo = std::min<int>(lo, q.top);
                        hi = std::max<int>(hi, q.top);
                    }
                CHECK(hi - lo <= 2);
                const ColumnInfo w = g.column(s.water.x, s.water.z);
                CHECK(w.water_top >= 0 && !w.sea);
                CHECK(s.water.dist2(s.center) < 70 * 70);
                for (size_t j = i + 1; j < sites.size(); ++j) CHECK(s.center.dist2(sites[j].center) > 150 * 150);
            }
            CHECK_EQ(g.sea_level() >= 0, sea);
        }
    }
}

TEST("random island: rivers, lakes and the sea stand still") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world = random_island(3);
    cfg.scenario = "empty";
    sim.new_game(cfg);
    World& w = sim.world();
    const WorldGen& g = w.gen();
    sim.physics().evaporation = 0.0f;
    sim.physics().evaporation_samples = 0;
    const MatId water = sim.reg().m().water;
    // Boxes around each site's water, a weir and a stretch of coast.
    std::vector<std::pair<Vec3i, Vec3i>> boxes;
    auto box = [&](const Vec3i& p, int r) { boxes.push_back({p - Vec3i{r, 10, r}, p + Vec3i{r, 4, r}}); };
    int weirs = 0, coasts = 0;
    for (const Site& s : g.features().sites) {
        box(s.water, 28);
        Vec3i q;
        if (find_column(g, s.water, 90, false, true, q)) box(q, 12), ++weirs;
        if (find_column(g, s.center, 240, true, false, q)) box(q, 16), ++coasts;
    }
    CHECK(weirs >= 2);
    CHECK(coasts >= 2);
    // Wake every water cube in them, and every cube around.
    std::vector<VoxelChange> wake;
    std::vector<Voxel> before;
    for (const auto& [a, b] : boxes)
        for (int y = a.y; y <= b.y; ++y)
            for (int z = a.z; z <= b.z; ++z)
                for (int x = a.x; x <= b.x; ++x) {
                    const Vec3i p{x, y, z};
                    const Voxel v = w.get(p);
                    before.push_back(v);
                    if (vmat(v) == water) wake.push_back({p, v, v, 0});
                }
    CHECK(wake.size() > 1000);
    sim.physics().on_changes(wake);
    sim.run(300);
    size_t i = 0, changed = 0;
    for (const auto& [a, b] : boxes)
        for (int y = a.y; y <= b.y; ++y)
            for (int z = a.z; z <= b.z; ++z)
                for (int x = a.x; x <= b.x; ++x)
                    if (w.get({x, y, z}) != before[i++]) ++changed;
    CHECK_EQ((int)changed, 0);
    CHECK_EQ((int)sim.physics().stats().water_units_to_void, 0);
}

TEST("random island: the sea keeps its level and its wall") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world = random_island(2);
    cfg.scenario = "empty";
    sim.new_game(cfg);
    World& w = sim.world();
    const WorldGen& g = w.gen();
    const CoreMats& M = sim.reg().m();
    sim.physics().evaporation_samples = 0;
    const int sl = g.sea_level();
    CHECK(sl > 0);
    // A hole through the sea floor: the sea pours through it, yet stays level.
    Vec3i q;
    CHECK(find_column(g, g.features().sites[0].center, 300, true, false, q));
    const ColumnInfo c = g.column(q.x, q.z);
    for (int y = c.bottom; y <= c.top; ++y) w.set({q.x, y, q.z}, make_voxel(M.air));
    sim.run(200);
    CHECK(sim.physics().stats().water_units_sea_out > 0);
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            const Vec3i p{q.x + dx, sl, q.z + dz};
            if (dx == 0 && dz == 0) continue;
            if (!g.column(p.x, p.z).sea) continue;
            CHECK_EQ((int)vlevel(w.get(p)), (int)kFluidFull);
        }
    // Water poured on the sea above its level runs off.
    Vec3i pour{q.x + 4, sl + 1, q.z + 4};
    for (int k = 0; k < 64 && !g.column(pour.x, pour.z).sea; ++k) pour = Vec3i{q.x + 4 - (k % 8), sl + 1, q.z + 4 - (k / 8)};
    CHECK(g.column(pour.x, pour.z).sea);
    w.set(pour, make_voxel(M.water, kFluidFull));
    sim.run(20);
    CHECK(sim.physics().stats().water_units_sea_in > 0);
    // Nothing crosses the wall at the rim.
    int x = g.config().cells_x * kCellSize / 2, z = x;
    while (!g.sea_wall(x + 1, z)) ++x;
    CHECK(g.column(x, z).sea);
    CHECK(!g.column(x + 1, z).land);
    w.set({x, sl + 1, z}, make_voxel(M.water, kFluidFull));
    sim.run(30);
    for (int y = sl - 16; y <= sl + 2; ++y) CHECK(vmat(w.get({x + 1, y, z})) != M.water);
}

TEST("random island: options change the island, saves keep them") {
    const Registry& reg = test_registry();
    WorldConfig a = random_island(5);
    WorldConfig b = a;
    b.size = 0;
    b.richness = 2;
    b.sized();
    CHECK(b.cells_x < a.cells_x);
    Simulation s1(reg);
    GameConfig cfg;
    cfg.world = b;
    cfg.scenario = "empty";
    s1.new_game(cfg);
    const std::vector<u8> bytes = s1.save();
    Simulation s2(reg);
    s2.load(bytes);
    CHECK(s2.world().config().layout == WorldLayout::Random);
    CHECK_EQ(s2.world().config().size, 0);
    CHECK_EQ(s2.world().config().richness, 2);
    CHECK_EQ(s2.world().config().cells_x, b.cells_x);
    CHECK(s2.world().gen().features().sites[0].center == s1.world().gen().features().sites[0].center);
}

TEST("random island: sea fish near the coast and out at sea, made up when thin") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world = random_island(3);
    cfg.scenario = "empty";
    sim.new_game(cfg);
    Fauna& fauna = sim.fauna();
    const WorldGen& g = sim.world().gen();
    int coastal = 0, offshore = 0;
    for (const FishGround& fg : fauna.grounds()) {
        if (!fauna.spec(fg.species).marine) {
            CHECK(!g.column(fg.at.x, fg.at.z).sea);  // lake and river fish stay in fresh water
            continue;
        }
        CHECK(g.column(fg.at.x, fg.at.z).sea);
        (fg.offshore ? offshore : coastal)++;
    }
    CHECK(coastal >= 5);
    CHECK(offshore >= 10);
    // Empty a coastal ground: within a few days fish swim in from the open sea.
    size_t gi = 0;
    for (size_t i = 0; i < fauna.grounds().size(); ++i)
        if (fauna.spec(fauna.grounds()[i].species).marine && !fauna.grounds()[i].offshore) gi = i;
    const Vec3i at = fauna.grounds()[gi].at;
    const u16 sp = fauna.grounds()[gi].species;
    for (const Animal& a : fauna.all())
        if (a.alive && a.species == sp && a.home.dist2(at) < 30 * 30) fauna.strike(*fauna.get(a.id), 10.0f, kNoEntity, 0);
    fauna.refresh_grounds();
    CHECK_EQ(fauna.grounds()[gi].stock, 0);
    sim.run(kTicksPerDay * 4);
    fauna.refresh_grounds();
    CHECK(fauna.grounds()[gi].stock >= 2);
}

TEST("random island: fishers row boats out to the schools beyond the shore and bring them home") {
    Simulation sim(test_registry());
    GameConfig cfg;
    cfg.world = random_island(3);
    cfg.scenario = "village";
    cfg.civs = 1;
    sim.new_game(cfg);
    Polity& p = *sim.society().polity(1);
    for (const char* k : {"fishing", "carpentry", "boats"})
        if (!p.has_tech(k)) p.techs.push_back(k);
    const ItemId boat = sim.reg().find_item("boat");
    REQUIRE(boat != kNoItem);
    StoreId store = kNoStore;
    for (const Store& s : sim.economy().stores())
        if (s.alive && s.polity == 1 && s.kind == StoreKind::Stockpile) store = s.id;
    REQUIRE(store != kNoStore);
    sim.economy().add(store, boat, 2, "admin_create");
    int caught = 0, afloat = 0;
    for (int h = 0; h < 48 && caught < 2; ++h) {
        sim.run(kTicksPerHour);
        for (auto& cp : sim.agents().all())
            if (cp && cp->alive && cp->in_boat) ++afloat;
        caught = 0;
        for (const Event& e : sim.chronicle().events())
            if (e.text.find("在船上捕到") != std::string::npos) ++caught;
    }
    std::printf("  boats: %d catches, %d character-hours afloat\n", caught, afloat);
    CHECK(caught >= 2);
    // The boats are all still somewhere (the workshop may have made more).
    sim.run(kTicksPerHour * 8);
    const LedgerLine& l = sim.economy().ledger(boat);
    CHECK(sim.economy().total(boat) >= 2);
    CHECK_EQ(sim.economy().total(boat), l.produced - l.consumed);
}
