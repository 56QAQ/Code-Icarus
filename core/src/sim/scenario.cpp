#include "icarus/sim/scenario.h"

#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {

struct Frame {
    float cx, cz, ux, uz, vx, vz;
    void to_world(float lu, float lv, int& x, int& z) const {
        x = (int)std::lround(cx + lu * ux + lv * vx);
        z = (int)std::lround(cz + lu * uz + lv * vz);
    }
    void to_local(float x, float z, float& lu, float& lv) const {
        float dx = x - cx, dz = z - cz;
        lu = dx * ux + dz * uz;
        lv = dx * vx + dz * vz;
    }
};

std::string pick(const Json& arr, Rng& rng, std::vector<std::string>* used = nullptr) {
    if (!arr.is_array() || arr.size() == 0) return "?";
    for (int tries = 0; tries < 50; ++tries) {
        std::string s = arr[(size_t)rng.below((u32)arr.size())].as_str();
        if (!used || std::find(used->begin(), used->end(), s) == used->end()) {
            if (used) used->push_back(s);
            return s;
        }
    }
    return arr[0].as_str();
}

std::string temperament_of(const Personality& p) {
    // Two most pronounced traits (high or low), rendered as a short description.
    struct T {
        float dev;
        std::string text;
    };
    std::vector<T> ts = {
        {p.caution - 0.5f, p.caution > 0.5f ? "谨慎" : "鲁莽"},
        {p.altruism - 0.5f, p.altruism > 0.5f ? "体恤他人" : "冷漠"},
        {p.ambition - 0.5f, p.ambition > 0.5f ? "野心勃勃" : "淡泊"},
        {p.diligence - 0.5f, p.diligence > 0.5f ? "勤勉" : "懒散"},
        {p.sociability - 0.5f, p.sociability > 0.5f ? "开朗" : "孤僻"},
        {p.conformity - 0.5f, p.conformity > 0.5f ? "守规矩" : "叛逆"},
        {p.aggression - 0.5f, p.aggression > 0.5f ? "好斗" : "温和"},
        {p.idealism - 0.5f, p.idealism > 0.5f ? "理想主义" : "务实"},
    };
    std::stable_sort(ts.begin(), ts.end(), [](const T& a, const T& b) { return std::fabs(a.dev) > std::fabs(b.dev); });
    return ts[0].text + "、" + ts[1].text;
}

}  // namespace

void make_girl(SimContext& ctx, EntityId id, const std::string& drive, Rng& rng, const Json& ov) {
    Character* c = ctx.agents->get(id);
    if (!c || !c->girl) return;
    GirlData& g = *c->girl;
    g.drive = drive;
    g.level = 2 + (int)rng.below(2);
    g.xp = 0;
    g.mana = 1.0f;
    for (int i = 0; i < Personality::kCount; ++i) g.persona.at(i) = rng.uniform(0.08f, 0.92f);
    static const char* keys[] = {"caution", "altruism", "ambition", "diligence", "sociability", "conformity", "aggression", "idealism"};
    for (int i = 0; i < Personality::kCount; ++i)
        if (ov.has(keys[i])) g.persona.at(i) = clampv(ov[keys[i]].as_float(), 0.0f, 1.0f);
    c->pers = g.persona;
    g.temperament = temperament_of(g.persona);
    const Json* dd = nullptr;
    for (const Json& d : ctx.reg->doc("drives")["drives"].items())
        if (d.str("key") == drive) dd = &d;
    c->look.skin = 0xF4DCC8;
    c->look.long_hair = true;
    c->look.dress = true;
    c->look.ribbon = true;
    if (dd) {
        const Json& co = (*dd)["costume"];
        c->look.cloth = parse_color(co.str("cloth", "#ffffff"));
        c->look.accent = parse_color(co.str("accent", "#ffcc00"));
        c->look.hair = parse_color(co.str("hair", "#553322"));
        if (dd->str("category") == "combat") c->skills[kCombat] = 0.8f;
        else c->skills[kCombat] = 0.5f;
    }
    // Eyes in her colour, deepened so they read as eyes on a pale face.
    auto deepen = [](u32 a) {
        auto ch = [](u32 v, u32 d) { return (v * 45 + d * 55) / 100; };
        return (ch((a >> 16) & 0xFF, 0x1E) << 16) | (ch((a >> 8) & 0xFF, 0x18) << 8) | ch(a & 0xFF, 0x30);
    };
    c->look.eyes = deepen(c->look.accent);
    c->body.build(c->look);
    c->skills[kResearch] = std::max(c->skills[kResearch], 0.4f);
}

void build_village_scenario(SimContext& ctx, const GameConfig& cfg, Rng& rng) {
    World& w = *ctx.world;
    const Registry& reg = *ctx.reg;
    const CoreMats& M = reg.m();
    const IslandFeatures& f = w.gen().features();
    const IslandDef& is = w.gen().islands()[0];
    Frame fr{is.cx, is.cz, f.axis_u_x, f.axis_u_z, f.axis_v_x, f.axis_v_z};
    const Json& names = reg.doc("names");

    Event se;
    se.type = EventType::Info;
    se.severity = 3;
    se.pos = f.village;
    se.text = "人造人在空岛上建立了村落";
    EventId ev = ctx.chron->emit(std::move(se));

    // Polity.
    std::string pname = pick(names["polity_names"], rng);
    u32 pcolor = parse_color(pick(names["polity_colors"], rng));
    u16 pid = ctx.society->create_polity(pname, pcolor);
    Polity* pol = ctx.society->polity(pid);

    // ---- buildings around the village plaza
    float vu, vv;
    fr.to_local((float)f.village.x, (float)f.village.z, vu, vv);
    auto place = [&](const std::string& key, float du, float dv, float face_du, float face_dv) -> u32 {
        const BuildingDef* d = ctx.buildings->def(key);
        int cx, cz, fx, fz;
        fr.to_world(vu + du, vv + dv, cx, cz);
        fr.to_world(vu + face_du, vv + face_dv, fx, fz);
        float dx = (float)(fx - cx), dz = (float)(fz - cz);
        u8 rot = 0;
        if (std::fabs(dx) > std::fabs(dz)) rot = dx > 0 ? 3 : 1;
        else rot = dz > 0 ? 0 : 2;
        int fw = (rot & 1) ? d->d : d->w, fd = (rot & 1) ? d->w : d->d;
        int ground = w.surface_y(cx, cz);
        Vec3i origin{cx - fw / 2, ground + 1, cz - fd / 2};
        return ctx.buildings->place_complete(key, origin, rot, pid, ev);
    };
    // Face toward the bridge side of the plaza.
    float bu, bv;
    fr.to_local((float)f.bridge_a.x, (float)f.bridge_a.z, bu, bv);
    float to_bu = (bu - vu), to_bv = (bv - vv);
    float bl = std::sqrt(to_bu * to_bu + to_bv * to_bv);
    to_bu /= bl;
    to_bv /= bl;
    u32 hall = place("hall", 0, 0, to_bu * 20, to_bv * 20);
    pol->seat = hall;
    auto ring = [&](float ang_deg, float r, float& du, float& dv) {
        float a = ang_deg * 3.14159265f / 180.0f;
        // Angles measured from the plaza->bridge direction.
        float ca = std::cos(a), sa = std::sin(a);
        du = (to_bu * ca - to_bv * sa) * r;
        dv = (to_bu * sa + to_bv * ca) * r;
    };
    float du, dv;
    ring(40, 16, du, dv);
    u32 store = place("storehouse", du, dv, 0, 0);
    ring(-40, 15, du, dv);
    u32 kitchen = place("kitchen", du, dv, 0, 0);
    std::vector<u32> huts;
    int hut_count = std::max(1, (cfg.residents + 2) / 3);
    for (int i = 0; i < hut_count; ++i) {
        float ang = 95.0f + (float)i * (170.0f / (float)std::max(1, hut_count - 1));
        ring(ang, 21.0f + (float)(i % 2) * 3.0f, du, dv);
        huts.push_back(place("hut", du, dv, 0, 0));
    }

    // Initial public stock (tribal level).
    {
        const Building* sb = ctx.buildings->get(store);
        StoreId s = sb->store;
        auto give = [&](const char* item, int n) { ctx.econ->add(s, reg.item_id(item), n, "initial"); };
        give("grain", 220);
        give("berries", 30);
        give("wood", 80);
        give("planks", 50);
        give("stone", 40);
        give("fiber", 30);
        give("clay", 10);
        // Spare tools of every trade and a few changes of clothes.
        give("stone_axe", 2);
        give("stone_pick", 2);
        give("stone_hoe", 1);
        give("stone_hammer", 1);
        give("flint_sickle", 2);
        give("flint_knife", 1);
        give("linen_clothes", 4);
        const Building* kb = ctx.buildings->get(kitchen);
        if (kb && kb->store) ctx.econ->add(kb->store, reg.item_id("bread"), 20, "initial");
    }

    // ---- the bridge over the ravine
    Vec3i fa{f.bridge_a.x, w.surface_y(f.bridge_a.x, f.bridge_a.z) + 1, f.bridge_a.z};
    Vec3i fb{f.bridge_b.x, w.surface_y(f.bridge_b.x, f.bridge_b.z) + 1, f.bridge_b.z};
    ctx.buildings->place_bridge(fa, fb, pid, true, ev);

    // ---- paths
    auto path_line = [&](Vec3i a, Vec3i b) {
        int x0 = a.x, z0 = a.z, x1 = b.x, z1 = b.z;
        int dx = std::abs(x1 - x0), dz = std::abs(z1 - z0);
        int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1;
        int err = dx - dz;
        for (int guard = 0; guard < 2000; ++guard) {
            for (int ox = 0; ox <= 1; ++ox)
                for (int oz = 0; oz <= 1; ++oz) {
                    int x = x0 + ox, z = z0 + oz;
                    int y = w.surface_y(x, z);
                    Vec3i p{x, y, z};
                    if (ctx.buildings->at(p) || ctx.buildings->at(p + Vec3i{0, 1, 0})) continue;
                    MatId m = w.mat(p);
                    if (m == M.grass || m == M.dirt || m == M.sand) {
                        w.set(p, make_voxel(M.path), ev);
                        Vec3i up = p + Vec3i{0, 1, 0};
                        MatId um = w.mat(up);
                        if (um != M.air && !w.material(up).solid && !w.material(up).fluid)
                            w.set(up, make_voxel(M.air), ev);
                    }
                }
            if (x0 == x1 && z0 == z1) break;
            int e2 = 2 * err;
            if (e2 > -dz) {
                err -= dz;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                z0 += sz;
            }
        }
    };
    const Building* hb = ctx.buildings->get(hall);
    path_line(hb->entrance, fa);
    path_line(f.pond, hb->entrance);
    for (u32 h : huts) path_line(ctx.buildings->get(h)->entrance, hb->entrance);
    path_line(ctx.buildings->get(store)->entrance, hb->entrance);
    path_line(ctx.buildings->get(kitchen)->entrance, hb->entrance);

    // ---- irrigated fields by the lake
    float lu_l, lv_l;
    fr.to_local((float)f.lake.x, (float)f.lake.z, lu_l, lv_l);
    const int lake_wt = f.lake.y;
    float lv_shore = lv_l - (float)f.lake_radius;
    float lv_first = lv_shore - 17.0f, lv_last = lv_shore - 6.0f;
    // Ditch from the lake down through the fields. Dug as a 4-connected line so water,
    // which flows cube-to-cube, can run along it.
    {
        int x0, z0, x1, z1;
        fr.to_world(lu_l, lv_l, x0, z0);
        fr.to_world(lu_l, lv_first - 2.0f, x1, z1);
        auto dig = [&](int x, int z) {
            int top = w.surface_y(x, z);
            if (w.mat({x, top, z}) == M.water) return;
            for (int y = top; y >= lake_wt; --y) w.set({x, y, z}, make_voxel(M.air), ev);
            // Packed clay banks keep sand from sliding in and silting the channel.
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    for (int y = top + 1; y >= lake_wt - 1; --y) {
                        Vec3i q{x + dx, y, z + dz};
                        if (reg.mat(w.mat(q)).granular) w.set(q, make_voxel(M.clay), ev);
                    }
            Vec3i floor{x, lake_wt - 1, z};
            if (reg.mat(w.mat(floor)).granular || !w.material(floor).solid) w.set(floor, make_voxel(M.clay), ev);
        };
        int dx = std::abs(x1 - x0), dz = std::abs(z1 - z0);
        int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1;
        int err = dx - dz;
        int x = x0, z = z0;
        for (int guard = 0; guard < 4000; ++guard) {
            dig(x, z);
            if (x == x1 && z == z1) break;
            int e2 = 2 * err;
            if (e2 > -dz) {
                err -= dz;
                x += sx;
                dig(x, z);  // step one axis at a time
            }
            if (e2 < dx) {
                err += dx;
                z += sz;
            }
        }
    }
    std::vector<Vec3i> plots;
    for (float lv = lv_first; lv <= lv_last; lv += 1.0f) {
        for (int side : {-1, 1}) {
            for (int k = 1; k <= 4; ++k) {
                int x, z;
                fr.to_world(lu_l + (float)(side * k), lv, x, z);
                int top = w.surface_y(x, z);
                Vec3i g{x, top, z};
                if (w.mat(g) == M.water || w.mat(g) == M.air) continue;
                if (std::find(plots.begin(), plots.end(), g) != plots.end()) continue;
                bool nearby_same = false;
                for (auto& q : plots)
                    if (q.x == x && q.z == z) nearby_same = true;
                if (nearby_same) continue;
                w.set(g, make_voxel(M.farmland), ev);
                Vec3i up = g + Vec3i{0, 1, 0};
                if (w.material(up).solid || (w.mat(up) != M.air && !w.material(up).fluid))
                    w.set(up, make_voxel(M.air), ev);
                if (rng.chance(0.75f)) w.set(up, make_voxel(M.crop, (u8)rng.range(1, 7)), ev);
                plots.push_back(g);
            }
        }
    }
    u32 farm_id = ctx.farming->create(pid, "湖畔田地", plots);
    if (Farm* farm = ctx.farming->get(farm_id)) {
        for (auto& p : farm->plots) {
            Voxel v = w.get(p.ground + Vec3i{0, 1, 0});
            if (vmat(v) == M.crop) p.growth = (float)vlevel(v) / 7.0f;
            p.irrigated = true;
        }
    }
    // Path from the bridge to the fields.
    int fx, fz;
    fr.to_world(lu_l - 5.0f, (lv_first + lv_last) * 0.5f, fx, fz);
    path_line(fb, Vec3i{fx, 0, fz});

    // ---- people
    std::vector<std::string> used;
    auto spawn_near = [&](const Vec3i& p, CharKind kind, const std::string& name, bool female) {
        Vec3i foot;
        if (!ctx.nav->find_standable_near(p, foot, 4)) foot = p;
        return ctx.agents->spawn(kind, name, female, foot, pid);
    };
    // Magical girls.
    std::vector<std::string> all_drives;
    for (const Json& d : reg.doc("drives")["drives"].items()) all_drives.push_back(d.str("key"));
    std::vector<std::string> drives = cfg.girl_drives;
    while (drives.size() < 3) {
        std::string d = all_drives[rng.below((u32)all_drives.size())];
        if (std::find(drives.begin(), drives.end(), d) == drives.end()) drives.push_back(d);
    }
    std::vector<EntityId> girls;
    for (size_t i = 0; i < drives.size() && i < 3; ++i) {
        std::string nm = pick(names["girl_names"], rng, &used);
        EntityId id = spawn_near(hb->inside, CharKind::MagicalGirl, nm, true);
        Character* c = ctx.agents->get(id);
        ctx.agents->randomize(*c, rng);
        Json ov = i < cfg.girl_personalities.size() ? cfg.girl_personalities[i] : Json();
        make_girl(ctx, id, drives[i], rng, ov);
        c->home = hall;
        c->girl->role = i == 0 ? "ruler" : (i == 1 ? "minister" : "none");
        c->girl->domain = i == 1 ? "agriculture" : "";
        c->girl->loyalty = 0.55f + rng.uniform(-0.1f, 0.2f);
        girls.push_back(id);
    }
    ctx.society->set_ruler(pid, girls[0], "founding", ev);
    // Policies follow the founding ruler's drive.
    for (const Json& d : reg.doc("drives")["drives"].items()) {
        if (d.str("key") != drives[0]) continue;
        pol->policies.punishment = d.flt("punish_bias", 0.3f);
        int valence = d.integer("valence", 1);
        pol->policies.distribution = valence < 0 ? 2 : (drives[0] == "light" ? 1 : 0);
        pol->policies.work_hours = valence < 0 ? 10.0f : 9.0f;
    }
    // Residents.
    for (int i = 0; i < cfg.residents; ++i) {
        bool female = rng.chance(0.5f);
        std::string nm = pick(names[female ? "female_names" : "male_names"], rng, &used);
        u32 home = huts[(size_t)i % huts.size()];
        const Building* hbld = ctx.buildings->get(home);
        EntityId id = spawn_near(hbld->entrance + Vec3i{rng.range(-2, 2), 0, rng.range(-2, 2)}, CharKind::Resident, nm, female);
        Character* c = ctx.agents->get(id);
        ctx.agents->randomize(*c, rng);
        c->home = home;
        ctx.buildings->get(home)->residents.push_back(id);
        int top = 0;
        for (int s = 1; s < kSkillCount; ++s)
            if (c->skills[s] > c->skills[top]) top = s;
        c->occupation = (top == kFarming || top == kCooking) ? "food" : (top == kBuilding ? "build" : "gather");
        for (size_t gi = 0; gi < girls.size(); ++gi) {
            float base = gi == 0 ? 0.35f : 0.1f;
            c->support_ref(girls[gi]) = clampv(rng.normalish(base, 0.12f), -1.0f, 1.0f);
        }
        // Village folk own the stone tool of their trade and wear linen.
        const std::string trade = Agents::occupation_tool(c->occupation);
        const ItemId tool = reg.find_item(trade == "hoe" ? "stone_hoe" : trade == "hammer" ? "stone_hammer" : "stone_axe");
        if (tool != kNoItem && ctx.econ->add(c->inv, tool, 1, "initial") == 1) c->tool = tool;
        const ItemId cloth = reg.find_item("linen_clothes");
        if (cloth != kNoItem && ctx.econ->add(c->inv, cloth, 1, "initial") == 1) c->clothes = cloth;
    }
    // Housemates start as acquaintances.
    for (u32 h : huts) {
        const Building* b = ctx.buildings->get(h);
        for (EntityId a : b->residents)
            for (EntityId bb : b->residents)
                if (a != bb) ctx.agents->get(a)->affinity_ref(bb) = rng.uniform(0.1f, 0.4f);
    }
    // Girls know each other; attitude depends on drive compatibility (randomised).
    for (EntityId a : girls)
        for (EntityId b : girls)
            if (a != b) ctx.agents->get(a)->affinity_ref(b) = rng.uniform(-0.2f, 0.4f);
    ctx.society->compute_stats(*pol);
}

}  // namespace icarus
