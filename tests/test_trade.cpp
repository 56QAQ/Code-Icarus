// Trade between polities: pacts, caravans that carry real goods both ways, cut roads.
#include <algorithm>

#include "icarus/economy/buildings.h"
#include "icarus/sim/simulation.h"
#include "test_framework.h"
#include "test_support.h"

using namespace icarus;

namespace {
GameConfig village(u64 seed) {
    GameConfig c;
    c.world.seed = seed;
    c.scenario = "village";
    return c;
}

// A neighbour led by one of the girls, with a third of the residents and a storehouse
// of its own `dx` cubes east of the hall (or on a levistone ledge high above it).
u16 make_neighbour(Simulation& sim, int dx, bool out_of_reach = false) {
    Polity* home = sim.society().polity(1);
    Character* rival = nullptr;
    for (auto& cp : sim.agents().all())
        if (cp && cp->is_girl() && cp->id != home->ruler) rival = cp.get();
    if (!rival) return 0;
    const u16 nid = sim.society().create_polity("邻邦", 0x88AA66, 1);
    home = sim.society().polity(1);
    rival->polity = nid;
    sim.society().set_ruler(nid, rival->id, "secession", 0);
    sim.society().polity(nid)->seat = home->seat;
    int n = 0;
    for (auto& cp : sim.agents().all())
        if (cp && !cp->is_girl() && cp->polity == 1 && (n++ % 3 == 0)) cp->polity = nid;
    const Building* hall = sim.buildings().get(home->seat);
    Vec3i at = hall->entrance + Vec3i{dx, 0, 0};
    if (out_of_reach) {
        at.y += 14;
        for (int x = -1; x <= 1; ++x)
            for (int z = -1; z <= 1; ++z)
                sim.world().set(at + Vec3i{x, -1, z}, make_voxel(sim.reg().mat_id("levistone")), 0);
    } else {
        Vec3i st;
        if (sim.nav().find_standable_near(at + Vec3i{0, 1, 0}, st, 8)) at = st;
    }
    sim.economy().create_store(StoreKind::Stockpile, at, nid, kNoEntity, 600.0f);
    return nid;
}

StoreId store_of(Simulation& sim, u16 polity) {
    for (const Store& s : sim.economy().stores())
        if (s.alive && s.polity == polity && s.kind == StoreKind::Stockpile) return s.id;
    return kNoStore;
}

void check_ledger(Simulation& sim) {
    const Registry& reg = sim.reg();
    for (size_t i = 0; i < reg.item_count(); ++i) {
        const LedgerLine& l = sim.economy().ledger((ItemId)i);
        CHECK_EQ(sim.economy().total((ItemId)i), l.produced - l.consumed);
    }
}
}  // namespace

TEST("trade: a hungry neighbour and a full granary make a trade prospect, and the books say why") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    sim.run(kTicksPerHour);
    const u16 nid = make_neighbour(sim, 18);
    REQUIRE(nid != 0);
    const Registry& reg = sim.reg();
    const ItemId grain = reg.find_item("grain"), copper = reg.find_item("copper");
    const StoreId home = store_of(sim, 1), theirs = store_of(sim, nid);
    REQUIRE(home != kNoStore);
    REQUIRE(theirs != kNoStore);
    sim.economy().add(home, grain, 400, "admin_bless");
    sim.economy().add(theirs, copper, 40, "admin_create");
    sim.run(kTicksPerHour);  // statistics refresh
    const TradeBook ours = sim.society().trade_book(1), neighbour = sim.society().trade_book(nid);
    CHECK(ours.spare_of(grain) > 0);
    CHECK(neighbour.want_of(grain) > 0);
    CHECK(neighbour.spare_of(copper) > 0);
    i32 n = 0;
    CHECK_EQ(sim.society().trade_export(1, nid, &n), grain);
    CHECK(n > 0);
    CHECK(sim.society().trade_prospect(1, nid));
}

TEST("trade: caravans carry goods both ways, nothing is created, and war ends the pact") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    sim.run(kTicksPerHour);
    const u16 nid = make_neighbour(sim, 18);
    REQUIRE(nid != 0);
    const Registry& reg = sim.reg();
    const ItemId grain = reg.find_item("grain"), copper = reg.find_item("copper");
    const StoreId home = store_of(sim, 1), theirs = store_of(sim, nid);
    sim.economy().add(home, grain, 400, "admin_bless");
    sim.economy().add(theirs, copper, 40, "admin_create");
    const EventId pact = sim.society().open_trade(1, nid, sim.society().polity(1)->ruler, 0);
    REQUIRE(pact != 0);
    REQUIRE(sim.society().polity(nid)->pact_with(1) != nullptr);
    const i32 their_grain0 = sim.economy().store(theirs)->count(grain);
    for (int h = 0; h < 30; ++h) sim.run(kTicksPerHour);
    const TradePact* t = sim.society().polity(1)->pact_with(nid);
    REQUIRE(t != nullptr);
    CHECK(t->trips > 0);
    CHECK(t->sent > 0.0f);
    CHECK(t->received > 0.0f);
    CHECK(sim.economy().store(theirs)->count(grain) > their_grain0);
    i64 copper_home = 0;
    for (StoreId sid : sim.society().public_stores(1)) copper_home += sim.economy().store(sid)->count(copper);
    CHECK(copper_home > 0);
    check_ledger(sim);
    // The day's exchanges are summed up in the chronicle, traced to the pact.
    bool summary = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Trade && e.causes[0] == pact && e.polity == 1 && e.text.find("商队") != std::string::npos)
            summary = true;
    CHECK(summary);
    // Save, load and carry on: the same as never stopping.
    {
        std::vector<u8> bytes = sim.save();
        Simulation loaded(test_registry());
        loaded.load(bytes);
        REQUIRE(loaded.society().polity(1)->pact_with(nid) != nullptr);
        CHECK_EQ(loaded.society().polity(1)->pact_with(nid)->trips, t->trips);
        sim.run(kTicksPerHour * 3);
        loaded.run(kTicksPerHour * 3);
        CHECK_EQ(loaded.state_hash(), sim.state_hash());
    }
    // War cuts the trade. (A ruler may have cut it already of her own accord.)
    if (!sim.society().polity(1)->pact_with(nid)) REQUIRE(sim.society().open_trade(1, nid, 0, 0) != 0);
    const EventId war = sim.society().declare_war(nid, 1, "raid", sim.society().polity(nid)->ruler, 0);
    REQUIRE(war != 0);
    CHECK(sim.society().polity(1)->pact_with(nid) == nullptr);
    CHECK(sim.society().polity(nid)->pact_with(1) == nullptr);
    bool ended = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Trade && e.causes[0] == war) ended = true;
    CHECK(ended);
}

TEST("trade: a caravan that cannot reach the partner turns back and the cut road is on record") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    sim.run(kTicksPerHour);
    const u16 nid = make_neighbour(sim, 12, true);
    REQUIRE(nid != 0);
    const Registry& reg = sim.reg();
    const ItemId grain = reg.find_item("grain"), copper = reg.find_item("copper");
    sim.economy().add(store_of(sim, 1), grain, 400, "admin_bless");
    sim.economy().add(store_of(sim, nid), copper, 40, "admin_create");
    const EventId pact = sim.society().open_trade(1, nid, sim.society().polity(1)->ruler, 0);
    REQUIRE(pact != 0);
    for (int h = 0; h < 24 && !sim.society().polity(1)->pact_with(nid)->blocked; ++h) sim.run(kTicksPerHour);
    const TradePact* t = sim.society().polity(1)->pact_with(nid);
    REQUIRE(t != nullptr);
    CHECK(t->blocked != 0);
    CHECK(t->blocked_until > sim.now());
    CHECK_EQ(t->trips, 0);
    const Event* e = sim.chronicle().get(t->blocked);
    REQUIRE(e != nullptr);
    CHECK(e->type == EventType::Trade);
    CHECK(e->causes[1] == pact);
    check_ledger(sim);
}

TEST("trade: a hungry neighbour's ruler proposes trade; the other ruler decides by her own values") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    sim.run(kTicksPerHour);
    const u16 nid = make_neighbour(sim, 18);
    REQUIRE(nid != 0);
    const Registry& reg = sim.reg();
    sim.economy().add(store_of(sim, 1), reg.find_item("grain"), 400, "admin_bless");
    sim.economy().add(store_of(sim, nid), reg.find_item("copper"), 40, "admin_create");
    sim.society().polity(1)->attitude_ref(nid) = 0.3f;
    sim.society().polity(nid)->attitude_ref(1) = 0.3f;
    const Decision* offer = nullptr;
    for (int h = 0; h < 24 * 4 && !offer; ++h) {
        sim.run(kTicksPerHour);
        for (const Decision& d : sim.decisions().all())
            if (d.id && d.kind == "trade_offer" && d.polity == 1) offer = &d;
    }
    REQUIRE(offer != nullptr);
    // The offer came from the neighbour's ruler choosing to propose.
    const Decision* proposal = nullptr;
    for (const Decision& d : sim.decisions().all())
        if (d.id && d.kind == "diplomacy" && d.polity == nid && d.chosen >= 0 &&
            d.options[(size_t)d.chosen].key == "propose_trade")
            proposal = &d;
    REQUIRE(proposal != nullptr);
    bool accept = false, refuse = false;
    for (const DecisionOption& o : offer->options) {
        accept |= o.key == "accept_trade";
        refuse |= o.key == "refuse_trade";
    }
    CHECK(accept);
    CHECK(refuse);
    CHECK(offer->chosen >= 0);
    // Whatever she chose is what happened.
    const bool accepted = offer->chosen >= 0 && offer->options[(size_t)offer->chosen].key == "accept_trade";
    CHECK_EQ(sim.society().polity(1)->pact_with(nid) != nullptr, accepted);
}

TEST("trade: an accepted offer opens the pact, traced to the decisions") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    sim.run(kTicksPerHour);
    const u16 nid = make_neighbour(sim, 18);
    REQUIRE(nid != 0);
    const Registry& reg = sim.reg();
    sim.economy().add(store_of(sim, 1), reg.find_item("grain"), 400, "admin_bless");
    sim.economy().add(store_of(sim, nid), reg.find_item("copper"), 40, "admin_create");
    sim.society().polity(1)->attitude_ref(nid) = 0.3f;
    sim.society().polity(nid)->attitude_ref(1) = 0.3f;
    sim.decisions().mode = "remote";
    std::string err;
    u32 proposed = 0, accepted = 0;
    for (int h = 0; h < 24 * 4 && !accepted; ++h) {
        sim.run(kTicksPerHour);
        for (u32 id : sim.decisions().awaiting_remote()) {
            const Decision* d = sim.decisions().get(id);
            for (const DecisionOption& o : d->options) {
                if (!proposed && d->kind == "diplomacy" && o.key == "propose_trade" && o.feasible) {
                    REQUIRE(sim.decisions().submit(id, o.key, "互通有无", "remote", err));
                    proposed = id;
                    break;
                }
                if (proposed && d->kind == "trade_offer" && o.key == "accept_trade") {
                    REQUIRE(sim.decisions().submit(id, o.key, "好", "remote", err));
                    accepted = id;
                    break;
                }
            }
            if (accepted) break;
        }
    }
    REQUIRE(proposed != 0);
    REQUIRE(accepted != 0);
    const Polity* a = sim.society().polity(1);
    const Polity* b = sim.society().polity(nid);
    REQUIRE(a->pact_with(nid) != nullptr);
    REQUIRE(b->pact_with(1) != nullptr);
    const Event* e = sim.chronicle().get(a->pact_with(nid)->event);
    REQUIRE(e != nullptr);
    CHECK(e->type == EventType::Trade);
    CHECK(e->causes[0] == sim.decisions().get(accepted)->decision_event);
}

TEST("aid: food to spare goes to a starving neighbour, carried over and asked nothing for") {
    Simulation sim(test_registry());
    sim.new_game(village(6));
    sim.run(kTicksPerHour);
    const u16 nid = make_neighbour(sim, 18);
    REQUIRE(nid != 0);
    const Registry& reg = sim.reg();
    const ItemId grain = reg.find_item("grain");
    sim.economy().add(store_of(sim, 1), grain, 400, "admin_bless");
    sim.society().polity(1)->attitude_ref(nid) = 0.3f;
    sim.society().polity(nid)->attitude_ref(1) = 0.3f;
    sim.decisions().mode = "remote";
    std::string err;
    u32 chosen = 0;
    for (int h = 0; h < 24 * 4 && !chosen; ++h) {
        sim.run(kTicksPerHour);
        for (u32 id : sim.decisions().awaiting_remote()) {
            const Decision* d = sim.decisions().get(id);
            if (d->kind != "diplomacy" || d->polity != 1) continue;
            for (const DecisionOption& o : d->options)
                if (o.key == "send_aid" && o.feasible) {
                    REQUIRE(sim.decisions().submit(id, o.key, "邻人挨饿", "remote", err));
                    chosen = id;
                    break;
                }
            if (chosen) break;
        }
    }
    REQUIRE(chosen != 0);
    sim.decisions().mode = "local";
    const float att0 = sim.society().polity(nid)->attitude_to(1);
    int delivered = 0;
    for (int h = 0; h < 36 && delivered == 0; ++h) {
        sim.run(kTicksPerHour);
        for (const Event& e : sim.chronicle().events())
            if (e.type == EventType::Trade && e.text.find("援粮送抵") != std::string::npos) ++delivered;
    }
    CHECK(delivered > 0);
    CHECK(sim.society().polity(nid)->attitude_to(1) > att0);
    // The gift is traced to the ruler's decision, and nothing came back.
    const Decision* d = sim.decisions().get(chosen);
    bool traced = false;
    for (const Event& e : sim.chronicle().events())
        if (e.type == EventType::Trade && e.causes[0] == d->decision_event) traced = true;
    CHECK(traced);
    CHECK(sim.society().polity(1)->pact_with(nid) == nullptr);
    check_ledger(sim);
}
