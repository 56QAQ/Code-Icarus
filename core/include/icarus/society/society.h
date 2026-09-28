// Society: polities, their statistics, crises, projects and political dynamics.
#pragma once

#include <string>
#include <vector>

#include "icarus/economy/economy.h"
#include "icarus/sim/context.h"
#include "icarus/society/polity.h"

namespace icarus {

// What a polity could give away and what it lacks, judged from its public stores:
// food beyond five days of eating, building materials beyond what is needed, tools and
// arms beyond one for everyone who would use them; and the other way round.
struct TradeBook {
    std::vector<std::pair<ItemId, i32>> spare;  // units, most plentiful first
    std::vector<std::pair<ItemId, i32>> want;   // units, most needed first
    i32 spare_of(ItemId it) const {
        for (auto& [i, n] : spare)
            if (i == it) return n;
        return 0;
    }
    i32 want_of(ItemId it) const {
        for (auto& [i, n] : want)
            if (i == it) return n;
        return 0;
    }
};

// One caravan's exchange at the partner's storehouse.
struct TradeDeal {
    ItemId out = kNoItem, in = kNoItem;
    i32 out_n = 0, in_n = 0;
    float value = 0;
};

class Society {
public:
    explicit Society(SimContext& ctx);

    void reset(u64 seed);
    void step(Tick now);

    // The knowledge a people starts with in an era (techs.json start_eras): wild,
    // tribal or village (the default).
    std::vector<std::string> start_techs(const std::string& era) const;
    u16 create_polity(const std::string& name, u32 color, u16 parent = 0, const std::string& era = "village");
    Polity* polity(u16 id) { return (id > 0 && id < polities_.size() && polities_[id].alive) ? &polities_[id] : nullptr; }
    const Polity* polity(u16 id) const {
        return (id > 0 && id < polities_.size() && polities_[id].alive) ? &polities_[id] : nullptr;
    }
    const std::vector<Polity>& polities() const { return polities_; }
    std::string title(u16 id) const;  // "美食的文明，拉米娅"
    void set_ruler(u16 id, EntityId girl, const std::string& how, EventId cause);

    // Public stores of a polity (stockpiles and workshops).
    std::vector<StoreId> public_stores(u16 polity) const;
    float public_food(u16 polity) const;  // nutrition units
    // Still living mostly by gathering and hunting: too few fields to feed everyone.
    bool foraging_band(const Polity& p) const;

    u32 add_project(Project p);
    Project* project(u32 id) { return (id > 0 && id < projects_.size() && projects_[id].alive) ? &projects_[id] : nullptr; }
    const std::vector<Project>& projects() const { return projects_; }
    void finish_project(u32 id, bool success, EventId cause);

    void compute_stats(Polity& p);
    // The steward's plan (planner.cpp): drawn up every two hours; the civilisation index.
    void draw_plan(Polity& p);
    CivIndex civ_index(const Polity& p) const;
    // The whole island's index (the sum over living polities), one a day.
    const std::vector<float>& island_history() const { return island_history_; }
    // Technology.
    const Json* tech(const std::string& key) const;
    bool tech_available(const Polity& p, const std::string& key) const;  // requirements met, not known
    // Techs of the era before `era` a people knows, and how many they need to know
    // before techs of `era` open up.
    std::pair<int, int> era_foundation(const Polity& p, int era) const;
    // The tech of learning that opens `era` ("" for the first era).
    std::string era_gate(int era) const;
    // Whether a tech can only be advanced by scholars at a research building (the
    // techs after the wild era).
    bool needs_scholars(const std::string& key) const;
    std::vector<std::string> available_techs(const Polity& p) const;
    float tech_effect(u16 polity, const std::string& effect) const;
    int era(const Polity& p) const;
    // Adds research points to the current target; discovers it when complete. Only
    // scholars (`scholarly`) advance the techs after the wild era.
    void add_research(u16 polity, float points, EntityId by, bool scholarly = false);
    // Research buildings of a people: seats for scholars and knowledge a day at full
    // strength (for estimates).
    int scholar_seats(u16 polity) const;
    float research_per_day(u16 polity, bool scholarly) const;
    void discover(Polity& p, const std::string& key, EntityId by, EventId cause, bool by_practice = false);
    // Learning by doing: everyday work (activity keys such as "forage", "chop", "hunt")
    // slowly teaches the techs whose "practice" lists it, whatever is being researched.
    void practice(u16 polity, const std::string& activity, float amount, EntityId by);
    // Knowledge given from outside (a god's revelation) goes to the current research,
    // or the cheapest open tech when nobody is researching ("" if nothing is open).
    std::string grant_target(u16 polity) const;
    void grant_research(u16 polity, const std::string& key, float points, EventId cause);
    // War and peace.
    EventId declare_war(u16 attacker, u16 defender, const std::string& aim, EntityId by, EventId cause);
    EventId make_peace(u16 a, u16 b, const std::string& how, EventId cause);
    // A new undertaking of the army (raid / conquest) against an enemy already at war.
    void start_operation(u16 polity, u16 enemy, const std::string& aim, EventId cause);
    // Food (units) a raid could carry off from one store.
    int food_in(StoreId store) const;
    // Where an army coming from `from` forms up before it falls on `objective`: open
    // ground some way short of it on the road in.
    Vec3i stage_point(const Vec3i& from, const Vec3i& objective);
    bool at_war(u16 a, u16 b) const;
    // Drafts up to n residents (strongest, most combative first); returns how many serve.
    int draft(u16 polity, int n, EventId cause);
    // The magical girls with battle magic who go with the army (at most two).
    int enlist_champions(u16 polity, EventId cause);
    void discharge(u16 polity);
    int soldiers(u16 polity) const;
    int draftable(u16 polity) const;  // residents fit to serve who are not yet soldiers
    // The winner absorbs the loser: people, fields, buildings and stores change hands.
    void annex(u16 winner, u16 loser, EventId cause, const std::string& how = "");

    // Strategy (strategy.cpp): how strong a polity is in the field, and how a war on a
    // neighbour looks from here.
    struct Assessment {
        float ours = 0, theirs = 0, ratio = 1;  // fighting strength (theirs with allies)
        float motive = 0;       // grievance, hunger against their plenty, rivalry
        float opportunity = 0;  // their famine, unrest, other wars
        int armed = 0;          // weapons for our fighters
        bool settled = true;    // both past the first days of building up
        bool truce = false;     // a truce still holds
        std::string why;        // in words, for the decision
    };
    float strength(u16 polity) const;
    Assessment assess(u16 us, u16 them) const;
    // The ruling girl's drive traits (drives.json): how readily she seeks a fight, and how
    // warmly she answers an envoy. 0 / 0.5 without a ruling girl.
    float war_appetite(u16 polity) const;
    // Days a polity spends building up before it will start a war (sooner for a ruler
    // who looks for a fight).
    float settle_days(u16 polity) const;
    float cooperation(u16 polity) const;
    // Daily: grievances along the borders, alliances that sour, tribute that falls due.
    void update_diplomacy(Polity& p);
    void set_truce(u16 a, u16 b, float days);
    void make_alliance(u16 a, u16 b, EventId cause);
    void end_alliance(u16 a, u16 b, const std::string& why, EventId cause);
    // Food carried from one polity's stores to another's (tribute, reparations); returns
    // the units sent on their way.
    i32 send_tribute(u16 from, u16 to, i32 food, const std::string& why, EventId cause);
    void make_vassal(u16 vassal, u16 overlord, EventId cause);
    void add_grievance(u16 who, u16 against, float amount);
    // A place for a new village: water 45-140 cubes from the seat, clear of everyone's
    // buildings. Returns false when there is none.
    bool outpost_site(u16 polity, Vec3i& center, Vec3i& water) const;
    // How close (cubes) the nearest buildings of two polities are.
    int border_distance(u16 a, u16 b) const;
    // Trade.
    TradeBook trade_book(u16 polity) const;
    // Goods `from` would carry to `to` (an item it spares that the other wants, or one
    // the other would take in payment for something `from` wants); kNoItem if none.
    ItemId trade_export(u16 from, u16 to, i32* amount = nullptr) const;
    bool trade_prospect(u16 a, u16 b) const;  // either side has a reason to trade
    EventId open_trade(u16 a, u16 b, EntityId by, EventId cause);
    void end_trade(u16 a, u16 b, const std::string& why, EventId cause);
    // A caravan of `from` at `at` (a store of `to`) hands over up to `count` of its load
    // from `inv` and takes goods of equal value back, as much as `carry` (weight)
    // allows. Units only move between stores.
    TradeDeal exchange(u16 from, u16 to, StoreId inv, StoreId at, ItemId load, i32 count, float carry);
    void trade_road_blocked(u16 from, u16 to);
    // Aid: food `from` can spare for a hungry `to` (kNoItem if none), and its delivery.
    ItemId aid_food(u16 from, u16 to, i32* amount = nullptr) const;
    void aid_delivered(u16 from, u16 to, ItemId item, i32 n, EntityId carrier, EventId cause, bool tribute = false);
    void refresh_passives(Polity& p);
    // Set once one polity holds the whole island after there had been several.
    EventId unification_event() const { return unification_; }
    // Convenience for systems that only know a polity id.
    float passive(u16 polity, const std::string& effect) const {
        const Polity* p = this->polity(polity);
        return p ? p->passive(effect) : 0.0f;
    }

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

    Rng& rng() { return rng_; }

private:
    void hourly(Tick now);
    void daily(Tick now);
    void update_crises(Polity& p);
    void update_support(Polity& p);
    void update_projects();
    void update_wars(Polity& p);

    void check_unification();
    void trade_daily();
    void tidy_pacts(Polity& p);

    SimContext& ctx_;
    Rng rng_;
    std::vector<Polity> polities_ = std::vector<Polity>(1);
    std::vector<Project> projects_ = std::vector<Project>(1);
    int most_polities_ = 1;     // most polities alive at once so far
    std::vector<float> island_history_;  // the island's civilisation index, a day each
    EventId last_merge_ = 0;    // latest annexation
    EventId unification_ = 0;   // the island was unified (once per round)
};

}  // namespace icarus
