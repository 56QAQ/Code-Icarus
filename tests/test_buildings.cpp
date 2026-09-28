// Buildings as they are used: furniture in the blueprints, the beds, seats and work
// places found in them, and people lying in the beds and sitting at the desks.
#include <algorithm>
#include <cmath>
#include <set>

#include "icarus/agents/jobs.h"
#include "icarus/decision/decisions.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/sim/simulation.h"
#include "icarus/society/society.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig village(u64 seed) {
    GameConfig c;
    c.world = WorldConfig::for_layout(WorldLayout::Continent, seed);
    c.scenario = "village";
    return c;
}
Polity& first(Simulation& sim) {
    for (const Polity& p : sim.society().polities())
        if (p.alive) return *sim.society().polity(p.id);
    return *sim.society().polity(1);
}
}  // namespace

TEST("buildings: every blueprint's beds, seats and work places are free, reachable and clear of the door") {
    Simulation sim(test_registry());
    sim.new_game(village(4));
    Polity& p = first(sim);
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    const Vec3i near = seat->entrance;
    const std::map<std::string, int> beds{{"lean_to", 2}, {"hut", 3}, {"longhouse", 6}, {"herbalist", 1}};
    int placed = 0;
    for (const BuildingDef& d : sim.buildings().defs()) {
        if (d.key == "campfire" || d.key == "watchtower") continue;
        // Every blueprint says where it is used, and houses sleep as many as they are for.
        if (beds.count(d.key)) CHECK_EQ(d.beds, beds.at(d.key));
        int seats = 0, work = 0, store = 0;
        for (const BuildingSlot& s : d.slots) {
            seats += s.kind == SlotKind::Seat;
            work += s.kind == SlotKind::Work;
            store += s.kind == SlotKind::Store;
        }
        if (d.scholars > 0) CHECK(seats >= d.scholars);
        if (!d.workstation.empty() && d.workstation != "research") CHECK(work >= 1);
        if (d.storage > 0 && d.workstation.empty()) CHECK(store >= 3);
        Vec3i origin;
        u8 rot = 0;
        if (!sim.buildings().find_site(d.key, near, 60, origin, rot)) continue;
        const u32 id = sim.buildings().place_complete(d.key, origin, rot, p.id, 0);
        const Building* b = sim.buildings().get(id);
        REQUIRE(b != nullptr);
        ++placed;
        CHECK_EQ(b->slots.size(), d.slots.size());
        CHECK_EQ(b->beds, d.beds);
        std::set<Vec3i> at;
        const Vec3i door = b->entrance + Vec3i{(b->inside.x > b->entrance.x) - (b->inside.x < b->entrance.x), 0,
                                               (b->inside.z > b->entrance.z) - (b->inside.z < b->entrance.z)};
        for (const BuildingSlot& s : b->slots) {
            // One body to a place, none in the doorway or the cube inside it (a lean-to's
            // mats, open to the front, are walked over).
            CHECK(at.insert(s.pos).second);
            if (s.what != "mat") CHECK(s.pos != door);
            if (s.kind != SlotKind::Bed) CHECK(s.pos != b->inside);
            // In the building, and reached from the doorstep on foot.
            CHECK(b->contains(s.access));
            CHECK(sim.nav().standable(s.access));
            Path path;
            CHECK(sim.nav().find_path(b->entrance, s.access, false, path, 20000));
            // The furniture is really there.
            if (s.kind == SlotKind::Bed || s.kind == SlotKind::Seat)
                CHECK(!sim.world().material(s.pos).furniture.empty());
            else
                CHECK(!sim.world().material(s.face).furniture.empty());
        }
        // The doorway itself stays walkable.
        CHECK(sim.nav().standable(b->inside));
    }
    CHECK(placed >= 10);
}

TEST("buildings: at night the household lies in its own beds, each in one, inside the house") {
    Simulation sim(test_registry());
    sim.new_game(village(1));
    // The middle of the first night.
    sim.run(kTicksPerDay - kTicksPerHour * 5);
    int housed = 0, in_bed = 0, outside = 0;
    std::set<Vec3i> beds;
    for (const auto& cp : sim.agents().all()) {
        if (!cp || !cp->alive || cp->departed || cp->task.type != TaskType::Sleep || cp->task.step < 2) continue;
        const Building* h = sim.buildings().get(cp->home);
        if (!h || !h->functional || h->beds <= 0) continue;
        ++housed;
        if (const BuildingSlot* s = h->slot_at(cp->foot); s && s->kind == SlotKind::Bed) {
            ++in_bed;
            CHECK(beds.insert(cp->foot).second);  // one to a bed
            CHECK(cp->sleeping);
            // Lying along the bed with her head at its head end.
            const float hx = -std::cos(cp->yaw), hz = std::sin(cp->yaw);
            const float ax = (float)(s->face.x - s->pos.x), az = (float)(s->face.z - s->pos.z);
            if (ax != 0.0f || az != 0.0f) CHECK(hx * ax + hz * az > 0.9f);
        }
        if (!sim.agents().at_home(*cp)) ++outside;
        else CHECK(h->contains(cp->foot));
    }
    CHECK(housed >= 6);
    CHECK(in_bed * 10 >= housed * 8);
    CHECK(outside * 10 <= housed);
    // In the morning they get up out of bed: nobody awake is left inside a bed.
    sim.run(kTicksPerHour * 8);
    for (const auto& cp : sim.agents().all()) {
        if (!cp || !cp->alive || cp->departed || cp->sleeping) continue;
        const Material& m = sim.world().material(cp->foot);
        CHECK(!(m.solid && !m.furniture.empty()));
    }
}

TEST("buildings: scholars sit on the stools at the desks, facing them, not in the doorway") {
    Simulation sim(test_registry());
    sim.new_game(village(3));
    sim.decisions().mode = "remote";
    sim.decisions().remote_budget_per_day = 1 << 20;
    sim.decisions().remote_deadline = kTicksPerDay * 100;
    sim.run(kTicksPerHour);
    Polity& p = first(sim);
    Society& soc = sim.society();
    REQUIRE(soc.needs_scholars("pottery"));
    p.policies.research = "pottery";
    p.policies.pri_research = 0.6f;
    int studying = 0, seated = 0, facing = 0, in_door = 0;
    for (int k = 0; k < 60; ++k) {
        sim.run(kTicksPerHour / 4);
        for (const auto& cp : sim.agents().all()) {
            if (!cp || !cp->alive || cp->task.type != TaskType::Work || cp->task.step != 1) continue;
            const Job* j = sim.jobs().get(cp->task.job);
            if (!j || j->type != JobType::Research) continue;
            const Building* b = sim.buildings().get(j->building);
            if (!b || b->slots.empty()) continue;
            ++studying;
            if (cp->foot == b->inside) ++in_door;
            const BuildingSlot* s = b->slot_at(cp->foot);
            if (!s || s->kind != SlotKind::Seat) continue;
            ++seated;
            const float fx = (float)s->face.x + 0.5f - cp->pos.x, fz = (float)s->face.z + 0.5f - cp->pos.z;
            const float len = std::sqrt(fx * fx + fz * fz);
            if (len > 0.0f && (std::sin(cp->yaw) * fx + std::cos(cp->yaw) * fz) / len > 0.9f) ++facing;
        }
    }
    CHECK(studying >= 10);
    CHECK_EQ(seated, studying);
    CHECK_EQ(facing, seated);
    CHECK_EQ(in_door, 0);
}

TEST("buildings: a lean-to's two mats are crawled onto from the front and slept on, under its roof") {
    Simulation sim(test_registry());
    GameConfig c;
    c.world = WorldConfig::for_layout(WorldLayout::Continent, 5);
    c.scenario = "wild";
    c.era = "wild";
    sim.new_game(c);
    Polity& p = first(sim);
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    Vec3i origin;
    u8 rot = 0;
    REQUIRE(sim.buildings().find_site("lean_to", seat->entrance, 40, origin, rot));
    const u32 id = sim.buildings().place_complete("lean_to", origin, rot, p.id, 0);
    const Building* b = sim.buildings().get(id);
    REQUIRE(b != nullptr);
    REQUIRE(b->beds == 2);
    for (const BuildingSlot& s : b->slots) {
        CHECK(s.what == "mat");
        // A low roof over each mat (lain on, not stood on), crawled onto from the open front.
        CHECK(!sim.nav().standable(s.pos));
        CHECK(sim.nav().standable(s.access));
        CHECK(s.access != s.pos);
    }
    // Two of the band move in and sleep on the mats.
    sim.run(kTicksPerHour);
    int moved = 0;
    for (const auto& cp : sim.agents().all())
        if (cp && cp->alive && !cp->is_girl() && cp->home == id) ++moved;
    REQUIRE(moved >= 1);
    sim.run(kTicksPerDay - sim.now() % kTicksPerDay + kTicksPerHour * 1);
    int on_mats = 0;
    for (const auto& cp : sim.agents().all()) {
        if (!cp || !cp->alive || cp->home != id || !cp->sleeping) continue;
        const BuildingSlot* s = b->slot_at(cp->foot);
        if (s && s->kind == SlotKind::Bed) ++on_mats;
        CHECK(sim.agents().at_home(*cp));
    }
    CHECK(on_mats >= 1);
    // Up in the morning and out from under the roof.
    sim.run(kTicksPerHour * 9);
    for (const auto& cp : sim.agents().all())
        if (cp && cp->alive && cp->home == id && !cp->sleeping) CHECK(sim.nav().standable(cp->foot) || cp->in_boat);
}

TEST("buildings: old saved houses without furniture still work (floor beds, near the door)") {
    // A house as it stands in the world is what counts: a plan without furniture has no
    // slots and keeps the old ways.
    const Registry& reg = test_registry();
    std::vector<Vec3i> pos;
    std::vector<MatId> mats;
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 5; ++x) {
            pos.push_back({x, 0, z});
            mats.push_back((x == 0 || x == 4 || z == 0 || z == 4) && !(x == 2 && z == 4) ? reg.mat_id("log") : 0);
        }
    CHECK(derive_slots(reg, pos, mats, 0, {}).empty());
}

TEST("buildings: a site built from exactly its bill of materials uses every unit (thatch: a bundle for two cubes)") {
    Simulation sim(test_registry());
    sim.new_game(village(2));
    Polity& p = first(sim);
    const Building* seat = sim.buildings().get(p.seat);
    REQUIRE(seat != nullptr);
    for (const char* key : {"hut", "lean_to", "study"}) {
        Vec3i origin;
        u8 rot = 0;
        REQUIRE(sim.buildings().find_site(key, seat->entrance, 60, origin, rot));
        const u32 id = sim.buildings().start_site(key, origin, rot, p.id, 0);
        Building* b = sim.buildings().get(id);
        REQUIRE(b != nullptr);
        // The bill matches the blueprint's cost, and a bundle of straw goes two cubes.
        const BuildingDef* d = sim.buildings().def(key);
        const auto bill = sim.buildings().remaining_cost(*b);
        CHECK(bill == d->cost);
        const ItemId fiber = test_registry().find_item("fiber");
        int thatch = 0;
        for (const Voxel v : b->plan_vox) thatch += vmat(v) == test_registry().mat_id("thatch");
        CHECK_EQ(bill.count(fiber) ? bill.at(fiber) : 0,
                 (thatch + 1) / 2 + (d->key == "lean_to" ? 6 : 0));  // (the lean-to's mats: a unit each)
        for (auto& [it, n] : bill) sim.economy().add(b->site, it, n, "admin_create");
        // Lay it all (whatever can be laid, round and round).
        for (int round = 0; round < 40 && !sim.buildings().site_done(*b); ++round)
            for (int k = 0; k < (int)b->plan_pos.size(); ++k) {
                const int idx = sim.buildings().next_buildable(*b, k);
                if (idx < 0) break;
                const Material& here = sim.world().material(b->plan_pos[(size_t)idx]);
                if (here.solid && vmat(b->plan_vox[(size_t)idx]) != sim.world().mat(b->plan_pos[(size_t)idx])) {
                    sim.world().set(b->plan_pos[(size_t)idx], make_voxel(0), 0);  // (clear the ground first)
                    continue;
                }
                CHECK(sim.buildings().place_cell(*b, idx, 0));
            }
        CHECK(sim.buildings().site_done(*b));
        const Store* site = sim.economy().store(b->site);
        REQUIRE(site != nullptr);
        CHECK(site->empty());
    }
}
