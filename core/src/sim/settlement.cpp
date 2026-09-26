// Settlements at the world's sites, in a chosen starting era (开局时代):
//   wild    — a campfire in the wilderness: no shelter, no store, no tools; people in leaf
//             wraps sleep on the ground by the fire and live off what they gather.
//   tribal  — a camp: the campfire, lean-to shelters, stone tools, hides and a spear or two.
//   village — a hall, huts, a storehouse, a kitchen and sown fields by the water.
// One to three civilisations, each at its own site (the continent has three).
#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/scenario.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"

namespace icarus {

namespace {

std::string pick_name(const Json& arr, Rng& rng, std::vector<std::string>& used) {
    if (!arr.is_array() || arr.size() == 0) return "?";
    for (int tries = 0; tries < 60; ++tries) {
        std::string s = arr[(size_t)rng.below((u32)arr.size())].as_str();
        if (std::find(used.begin(), used.end(), s) == used.end()) {
            used.push_back(s);
            return s;
        }
    }
    return arr[0].as_str();
}

// Fell every tree whose trunk stands within radius of c (the whole tree goes: trunk and
// the crown attached to it), so buildings are not placed into a forest.
void clear_trees(World& w, const Registry& reg, const Vec3i& c, int radius, EventId ev) {
    for (int dz = -radius; dz <= radius; ++dz)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int x = c.x + dx, z = c.z + dz;
            const ColumnInfo col = w.gen().column(x, z);
            if (!col.land) continue;
            for (int y = col.top - 2; y <= col.top + 16; ++y) {
                const Vec3i p{x, y, z};
                if (!reg.mat(w.mat(p)).trunk) continue;
                std::deque<Vec3i> q{p};
                std::set<Vec3i> seen{p};
                int n = 0;
                while (!q.empty() && n < 500) {
                    const Vec3i a = q.front();
                    q.pop_front();
                    const Material& m = reg.mat(w.mat(a));
                    if (!m.trunk && !m.foliage) continue;
                    w.set(a, make_voxel(0), ev);
                    ++n;
                    for (int k = 0; k < 6; ++k) {
                        const Vec3i b = a + kDir6[k];
                        if (b.dist2(p) < 8 * 8 && seen.insert(b).second) q.push_back(b);
                    }
                }
                break;
            }
            // Small plants in the way are trampled.
            const Vec3i up{x, col.top + 1, z};
            const Material& um = w.material(up);
            if (w.mat(up) != 0 && !um.solid && !um.fluid) w.set(up, make_voxel(0), ev);
        }
}

// A building centred at (x, z), its front facing toward `face`.
u32 place_building(SimContext& ctx, const std::string& key, int x, int z, const Vec3i& face, u16 pid, EventId ev) {
    World& w = *ctx.world;
    const BuildingDef* d = ctx.buildings->def(key);
    if (!d) return 0;
    const float dx = (float)(face.x - x), dz = (float)(face.z - z);
    u8 rot = 0;
    if (std::fabs(dx) > std::fabs(dz)) rot = dx > 0 ? 3 : 1;
    else rot = dz > 0 ? 0 : 2;
    const int fw = (rot & 1) ? d->d : d->w, fd = (rot & 1) ? d->w : d->d;
    clear_trees(w, *ctx.reg, Vec3i{x, 0, z}, std::max(fw, fd) / 2 + 3, ev);
    const int ground = w.surface_y(x, z);
    const Vec3i origin{x - fw / 2, ground + 1, z - fd / 2};
    return ctx.buildings->place_complete(key, origin, rot, pid, ev);
}

// A trodden path between two points (grass, dirt and sand become path).
void path_line(SimContext& ctx, Vec3i a, Vec3i b, EventId ev) {
    World& w = *ctx.world;
    const CoreMats& M = ctx.reg->m();
    int x0 = a.x, z0 = a.z;
    const int x1 = b.x, z1 = b.z;
    const int dx = std::abs(x1 - x0), dz = std::abs(z1 - z0);
    const int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1;
    int err = dx - dz;
    for (int guard = 0; guard < 2000; ++guard) {
        const int y = w.surface_y(x0, z0);
        const Vec3i p{x0, y, z0};
        if (!ctx.buildings->at(p) && !ctx.buildings->at(p + Vec3i{0, 1, 0})) {
            const MatId m = w.mat(p);
            if (m == M.grass || m == M.dirt || m == M.sand || (m != M.air && (m == M.dry_grass || m == M.snow))) {
                w.set(p, make_voxel(M.path), ev);
                const Vec3i up = p + Vec3i{0, 1, 0};
                if (w.mat(up) != M.air && !w.material(up).solid && !w.material(up).fluid) w.set(up, make_voxel(M.air), ev);
            }
        }
        if (x0 == x1 && z0 == z1) break;
        const int e2 = 2 * err;
        if (e2 > -dz) {
            err -= dz;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            z0 += sz;
        }
    }
}

u16 found_settlement(SimContext& ctx, const GameConfig& cfg, Rng& rng, const Site& site, int index,
                     std::vector<std::string>& used) {
    World& w = *ctx.world;
    const Registry& reg = *ctx.reg;
    const Json& names = reg.doc("names");
    const std::string era = cfg.era;
    const bool wild = era == "wild", tribal = era == "tribal", village = !wild && !tribal;

    Event se;
    se.type = EventType::Info;
    se.severity = 3;
    se.pos = site.center;
    se.text = wild ? "一群衣不蔽体的人造人在荒野中生起了篝火"
                   : (tribal ? "人造人在火边搭起窝棚，结成了部落" : "人造人在空岛上建立了村落");
    const EventId ev = ctx.chron->emit(std::move(se));

    const std::string pname = pick_name(names["polity_names"], rng, used);
    const u32 pcolor = parse_color(names["polity_colors"][(size_t)((rng.below(64) + (u32)index * 3) %
                                                                  std::max<size_t>(1, names["polity_colors"].size()))]
                                       .as_str());
    const u16 pid = ctx.society->create_polity(pname, pcolor, 0, village ? "village" : era);
    Polity* pol = ctx.society->polity(pid);

    // The heart of the place faces its water.
    const Vec3i c = site.center;
    const Vec3i water = site.water;
    Vec3i dir{water.x - c.x, 0, water.z - c.z};
    const float dl = std::max(1.0f, std::sqrt((float)(dir.x * dir.x + dir.z * dir.z)));
    const float ux = (float)dir.x / dl, uz = (float)dir.z / dl;
    auto ring = [&](float ang_deg, float r) {
        const float a = ang_deg * 3.14159265f / 180.0f;
        const float ca = std::cos(a), sa = std::sin(a);
        return Vec3i{c.x + (int)std::lround((ux * ca - uz * sa) * r), 0, c.z + (int)std::lround((ux * sa + uz * ca) * r)};
    };
    if (!village) clear_trees(w, reg, c, 7, ev);  // a clearing around the fire
    const u32 seat = place_building(ctx, village ? "hall" : "campfire", c.x, c.z, water, pid, ev);
    pol->seat = seat;
    const Building* sb = ctx.buildings->get(seat);

    std::vector<u32> homes;
    u32 store = 0, kitchen = 0;
    if (village) {
        Vec3i p = ring(40, 16);
        store = place_building(ctx, "storehouse", p.x, p.z, c, pid, ev);
        p = ring(-40, 15);
        kitchen = place_building(ctx, "kitchen", p.x, p.z, c, pid, ev);
        const int n = std::max(1, (cfg.residents + 2) / 3);
        for (int i = 0; i < n; ++i) {
            p = ring(95.0f + (float)i * (170.0f / (float)std::max(1, n - 1)), 21.0f + (float)(i % 2) * 3.0f);
            homes.push_back(place_building(ctx, "hut", p.x, p.z, c, pid, ev));
        }
    } else if (tribal) {
        const int n = std::max(1, (cfg.residents + 1) / 2);
        for (int i = 0; i < n; ++i) {
            const Vec3i p = ring(60.0f + (float)i * (240.0f / (float)std::max(1, n - 1)), 10.0f + (float)(i % 2) * 2.5f);
            homes.push_back(place_building(ctx, "lean_to", p.x, p.z, c, pid, ev));
        }
    }
    homes.erase(std::remove(homes.begin(), homes.end(), 0u), homes.end());

    // What they start with.
    const StoreId stash = village ? (store ? ctx.buildings->get(store)->store : kNoStore) : (sb ? sb->store : kNoStore);
    auto give = [&](const char* item, int n) {
        const ItemId it = reg.find_item(item);
        if (stash && it != kNoItem) ctx.econ->add(stash, it, n, "initial");
    };
    if (wild) {
        give("berries", 20);
        give("fruit", 10);
        give("fiber", 12);
    } else if (tribal) {
        give("berries", 20);
        give("meat", 12);
        give("fiber", 20);
        give("wood", 20);
        give("stone", 10);
        give("flint", 6);
        give("hide", 4);
        give("stone_axe", 2);
        give("flint_knife", 2);
        give("wooden_spear", 3);
    } else {
        give("grain", 220);
        give("berries", 30);
        give("wood", 80);
        give("planks", 50);
        give("stone", 40);
        give("fiber", 30);
        give("clay", 10);
        give("stone_axe", 2);
        give("stone_pick", 2);
        give("stone_hoe", 1);
        give("stone_hammer", 1);
        give("flint_sickle", 2);
        give("flint_knife", 1);
        give("linen_clothes", 4);
        if (const Building* kb = ctx.buildings->get(kitchen); kb && kb->store)
            ctx.econ->add(kb->store, reg.item_id("bread"), 20, "initial");
    }

    // Village fields: sown plots by the water, with a path to them.
    if (village) {
        const u32 farm_id = ctx.farming->found(pid, pname + "的田地", water, 20, 40);
        if (Farm* farm = ctx.farming->get(farm_id)) {
            const CoreMats& M = reg.m();
            for (Plot& p : farm->plots) {
                w.set(p.ground, make_voxel(M.farmland), ev);
                const Vec3i up = p.ground + Vec3i{0, 1, 0};
                if (w.mat(up) != M.air) w.set(up, make_voxel(M.air), ev);
                if (rng.chance(0.75f)) {
                    const u8 stage = (u8)rng.range(1, 7);
                    w.set(up, make_voxel(M.crop, stage), ev);
                    p.growth = (float)stage / 7.0f;
                }
            }
            path_line(ctx, sb->entrance, farm->plots.front().ground, ev);
        }
        for (u32 h : homes) path_line(ctx, ctx.buildings->get(h)->entrance, sb->entrance, ev);
        if (store) path_line(ctx, ctx.buildings->get(store)->entrance, sb->entrance, ev);
        if (kitchen) path_line(ctx, ctx.buildings->get(kitchen)->entrance, sb->entrance, ev);
    }

    // People.
    auto spawn_near = [&](const Vec3i& p, CharKind kind, const std::string& name, bool female) {
        Vec3i foot;
        if (!ctx.nav->find_standable_near(p, foot, 5)) foot = p;
        return ctx.agents->spawn(kind, name, female, foot, pid);
    };
    std::vector<std::string> all_drives;
    for (const Json& d : reg.doc("drives")["drives"].items()) all_drives.push_back(d.str("key"));
    std::vector<std::string> drives;
    for (size_t k = (size_t)index * 3; k < cfg.girl_drives.size() && drives.size() < 3; ++k) drives.push_back(cfg.girl_drives[k]);
    while (drives.size() < 3) {
        const std::string d = all_drives[rng.below((u32)all_drives.size())];
        if (std::find(drives.begin(), drives.end(), d) == drives.end()) drives.push_back(d);
    }
    const Vec3i gather = sb ? sb->inside : c;
    std::vector<EntityId> girls;
    for (size_t i = 0; i < 3; ++i) {
        const std::string nm = pick_name(names["girl_names"], rng, used);
        const EntityId id = spawn_near(gather + Vec3i{rng.range(-3, 3), 0, rng.range(-3, 3)}, CharKind::MagicalGirl, nm, true);
        Character* g = ctx.agents->get(id);
        ctx.agents->randomize(*g, rng);
        const size_t pi = (size_t)index * 3 + i;
        const Json ov = pi < cfg.girl_personalities.size() ? cfg.girl_personalities[pi] : Json();
        make_girl(ctx, id, drives[i], rng, ov);
        g->home = village ? seat : 0;
        g->girl->role = i == 0 ? "ruler" : (i == 1 ? "minister" : "none");
        g->girl->domain = i == 1 ? "agriculture" : "";
        g->girl->loyalty = 0.55f + rng.uniform(-0.1f, 0.2f);
        girls.push_back(id);
    }
    ctx.society->set_ruler(pid, girls[0], "founding", ev);
    for (const Json& d : reg.doc("drives")["drives"].items()) {
        if (d.str("key") != drives[0]) continue;
        pol->policies.punishment = d.flt("punish_bias", 0.3f);
        const int valence = d.integer("valence", 1);
        pol->policies.distribution = valence < 0 ? 2 : (drives[0] == "light" ? 1 : 0);
        pol->policies.work_hours = valence < 0 ? 10.0f : 9.0f;
    }
    const ItemId leaf = reg.find_item("leaf_wrap"), fur = reg.find_item("fur_cloak"), linen = reg.find_item("linen_clothes");
    for (int i = 0; i < cfg.residents; ++i) {
        const bool female = rng.chance(0.5f);
        const std::string nm = pick_name(names[female ? "female_names" : "male_names"], rng, used);
        const u32 home = homes.empty() ? 0u : homes[(size_t)i % homes.size()];
        const Building* hb = home ? ctx.buildings->get(home) : nullptr;
        const Vec3i at = hb ? hb->entrance : gather + Vec3i{rng.range(-5, 5), 0, rng.range(-5, 5)};
        const EntityId id = spawn_near(at, CharKind::Resident, nm, female);
        Character* r = ctx.agents->get(id);
        ctx.agents->randomize(*r, rng);
        r->home = home;
        if (hb) ctx.buildings->get(home)->residents.push_back(id);
        int top = 0;
        for (int s = 1; s < kSkillCount; ++s)
            if (r->skills[s] > r->skills[top]) top = s;
        r->occupation = (top == kFarming || top == kCooking) ? "food" : (top == kBuilding ? "build" : "gather");
        for (size_t gi = 0; gi < girls.size(); ++gi)
            r->support_ref(girls[gi]) = clampv(rng.normalish(gi == 0 ? 0.35f : 0.1f, 0.12f), -1.0f, 1.0f);
        // What they wear and carry.
        const ItemId clothes = wild ? leaf : (tribal ? (rng.chance(0.5f) ? fur : leaf) : linen);
        if (clothes != kNoItem && ctx.econ->add(r->inv, clothes, 1, "initial") == 1) r->clothes = clothes;
        if (!wild && (village || rng.chance(0.5f))) {
            const std::string trade = Agents::occupation_tool(r->occupation);
            const ItemId tool =
                reg.find_item(trade == "hoe" ? (village ? "stone_hoe" : "digging_stick") : trade == "hammer" ? "stone_hammer" : "stone_axe");
            if (tool != kNoItem && ctx.econ->add(r->inv, tool, 1, "initial") == 1) r->tool = tool;
        }
    }
    // Housemates start as acquaintances, the band as a whole a little closer.
    for (u32 h : homes) {
        const Building* b = ctx.buildings->get(h);
        for (EntityId a : b->residents)
            for (EntityId bb : b->residents)
                if (a != bb) ctx.agents->get(a)->affinity_ref(bb) = rng.uniform(0.1f, 0.4f);
    }
    for (EntityId a : girls)
        for (EntityId b : girls)
            if (a != b) ctx.agents->get(a)->affinity_ref(b) = rng.uniform(-0.2f, 0.4f);
    ctx.society->compute_stats(*pol);
    return pid;
}

}  // namespace

void build_settlements(SimContext& ctx, const GameConfig& cfg, Rng& rng) {
    const IslandFeatures& f = ctx.world->gen().features();
    std::vector<Site> sites = f.sites;
    if (sites.empty()) sites.push_back({f.village, f.farms, f.pond, Biome::Grassland});
    const int n = std::clamp(cfg.civs, 1, (int)sites.size());
    std::vector<std::string> used;
    std::vector<u16> pids;
    for (int i = 0; i < n; ++i) pids.push_back(found_settlement(ctx, cfg, rng, sites[(size_t)i], i, used));
    // Neighbours begin as strangers who distrust each other.
    for (u16 a : pids)
        for (u16 b : pids)
            if (a != b) ctx.society->polity(a)->attitude_ref(b) = -0.5f;
}

}  // namespace icarus
