// Polity (国家): identity, culture, ruler, policies, projects, knowledge and history are
// stored separately, so a change of ruler renames the civilisation's title without
// erasing its people, industry, institutions or past.
#pragma once

#include <string>
#include <vector>

#include "icarus/sim/chronicle.h"
#include "icarus/society/plan.h"
#include "icarus/util/json.h"
#include "icarus/util/types.h"

namespace icarus {

struct Policies {
    float ration = 1.0f;       // portion residents may take from public food (0.5 .. 1.4)
    float punishment = 0.3f;   // severity of penalties for shirking/theft (0 .. 1)
    float work_hours = 9.0f;   // expected daily work hours
    float requisition = 0.0f;  // share of private food seized into public stores
    u8 distribution = 0;       // 0 equal, 1 workers first, 2 elite first
    float pri_food = 1.0f, pri_build = 1.0f, pri_gather = 0.7f, pri_research = 0.4f, pri_military = 0.3f;
    float wage = 0.0f;         // extra food paid to diligent workers (0 .. 1)
    std::string research;      // current research target
    int army = 0;              // soldiers wanted (drafted residents)
};

struct Culture {
    float collectivism = 0.5f;
    float militarism = 0.3f;
    float tradition = 0.5f;
    float openness = 0.5f;
};

struct Reign {
    EntityId ruler = kNoEntity;
    std::string ruler_name;
    std::string drive;
    Tick from = 0, to = 0;
    std::string how;  // founding / succession / coup / secession
};

struct PolityStats {
    Tick tick = 0;
    int population = 0;
    int girls = 0;
    float food_stock = 0;       // nutrition units in public stores
    float food_days = 0;        // days of food at current population
    float food_access = 0;      // share of residents whose food need is fine
    float water_access = 0;
    float mood = 0;
    float ruler_support = 0;
    float stability = 0;
    float knowledge = 0;
    int protesters = 0;
    int deaths = 0;
    int harvest_today = 0;
    float ecology = 0;
};

enum class CrisisKind : u8 { None = 0, Food, Water, Logistics, Unrest, Disaster, War, Housing };
const char* crisis_name_zh(CrisisKind k);

struct Crisis {
    CrisisKind kind = CrisisKind::None;
    float severity = 0;      // 0..1
    Tick since = 0;
    EventId event = 0;       // the event that declared it
    EventId cause = 0;       // best known root cause
    u32 decision = 0;        // decision addressing it (if any)
    bool active = false;
};

struct Project {
    u32 id = 0;
    bool alive = false;
    u16 polity = 0;
    std::string kind;        // construct, repair, bridge, gather, dig, farm, relocate
    std::string title;
    u32 building = 0;
    Vec3i target;
    float priority = 1.0f;
    Tick created = 0;
    EntityId sponsor = kNoEntity;
    EventId cause = 0;
    u32 decision = 0;
    u8 status = 0;           // 0 active, 1 done, 2 abandoned
    Json params;
    float progress = 0;
};

struct War {
    u16 enemy = 0;
    bool attacker = false;
    std::string aim;         // raid / conquest (attacker), defend, ally (joined for an ally)
    Tick since = 0;
    EventId event = 0;       // declaration
    int kills = 0, losses = 0;
    int loot = 0;            // food carried off from them
    int razed = 0;           // cubes of their buildings broken
    int refused = 0;         // our demands they turned down
    int repulsed = 0;        // our raids beaten back in a row, with nothing carried off
    Tick last_offer = 0;     // our last offer of peace
    Tick active_at = 0;      // the last fighting, raiding or marching in this war
    // How the war stands for us: blood, plunder and ruin.
    float score() const { return (float)(kills - losses) + 0.1f * (float)loot + 0.15f * (float)razed; }
};

// How a polity stands with another beyond war and trade.
struct Diplo {
    u16 other = 0;
    Tick truce_until = 0;    // no war may be declared on them before this
    bool allied = false;     // each defends the other
    Tick allied_since = 0;
    float grievance = 0;     // raids suffered, trespass on our land, dead kin (decays)
    Tick last_incident = 0;  // the last border incident noted
    Tick envoy_at = 0;       // when we last sent them an envoy with gifts
};

// The army's current undertaking (at most one per polity).
struct Operation {
    bool active = false;
    u16 enemy = 0;
    std::string aim;         // raid / conquest / defend
    Vec3i rally, objective;
    u8 phase = 0;            // 0 muster, 1 march, 2 at the objective, 3 return
    Tick since = 0;
    int party = 0, lost = 0;
    EventId event = 0;
    bool engaged = false;    // a battle event has been recorded
    int loot = 0;            // units of food carried off (raids)
    Tick held_since = 0;     // conquest: since when no defender has stood by their hall (0: contested)
    Vec3i stage;             // where the army forms up outside the enemy's village before the assault
    Tick staged_since = 0;   // when the first soldiers reached it (0: nobody yet)
};

// A trade pact between two polities at peace. Each side's carriers take what it can
// spare to the other's storehouse and bring back goods of equal value; nothing is
// created or lost on the way, and a cut road stops the caravans.
struct TradePact {
    u16 partner = 0;
    Tick since = 0;
    EventId event = 0;         // the pact
    int trips = 0;             // our caravans that made an exchange
    float sent = 0, received = 0;  // exchange value, all time
    Tick blocked_until = 0;    // a caravan could not get there: none until then
    EventId blocked = 0;       // "the trade road is cut" (0 while it is open)
    // Today's exchanges by our caravans (for the evening summary).
    std::vector<std::pair<ItemId, i32>> out_today, in_today;
};

struct Polity {
    u16 id = 0;
    bool alive = false;
    std::string name;        // display name, never changes
    u32 color = 0xC9B27A;
    EntityId ruler = kNoEntity;
    Culture culture;
    Policies policies;
    u32 seat = 0;            // hall building
    Tick founded = 0;
    u16 parent = 0;
    std::vector<Reign> reigns;
    std::vector<std::pair<u16, float>> attitude;
    std::vector<u16> at_war;
    std::vector<War> wars;
    Operation op;
    std::vector<TradePact> pacts;
    std::vector<Diplo> diplo;
    u16 overlord = 0;         // a vassal pays tribute to its overlord and follows it to war
    std::vector<Vec3i> outposts;  // new villages founded away from the seat
    Tick tribute_next = 0;    // when the next tribute is due (vassals)
    std::vector<std::string> techs;
    std::vector<std::pair<std::string, float>> research;
    std::vector<Crisis> crises;
    PolityStats stats;
    std::vector<PolityStats> history;
    PolityPlan plan;                   // the steward's plan (内政规划)
    std::vector<CivIndex> civ_history;  // the civilisation index, one a day
    int deaths_total = 0;
    Tick forage_until = 0;   // organised foraging campaign
    Tick emergency_until = 0;  // emergency work priorities or rations lapse then
    // Passive spells of the polity's magical girls, summed by effect (refreshed hourly).
    std::vector<std::pair<std::string, float>> passives;
    float passive(const std::string& effect) const {
        for (auto& e : passives)
            if (e.first == effect) return e.second;
        return 0.0f;
    }

    Crisis* crisis(CrisisKind k) {
        for (auto& c : crises)
            if (c.kind == k) return &c;
        return nullptr;
    }
    const Crisis* crisis(CrisisKind k) const {
        for (auto& c : crises)
            if (c.kind == k) return &c;
        return nullptr;
    }
    War* war_with(u16 other) {
        for (auto& w : wars)
            if (w.enemy == other) return &w;
        return nullptr;
    }
    const War* war_with(u16 other) const {
        for (auto& w : wars)
            if (w.enemy == other) return &w;
        return nullptr;
    }
    TradePact* pact_with(u16 other) {
        for (auto& t : pacts)
            if (t.partner == other) return &t;
        return nullptr;
    }
    const TradePact* pact_with(u16 other) const {
        for (auto& t : pacts)
            if (t.partner == other) return &t;
        return nullptr;
    }
    Diplo& diplo_ref(u16 other) {
        for (auto& d : diplo)
            if (d.other == other) return d;
        diplo.push_back(Diplo{});
        diplo.back().other = other;
        return diplo.back();
    }
    const Diplo* diplo_of(u16 other) const {
        for (auto& d : diplo)
            if (d.other == other) return &d;
        return nullptr;
    }
    bool allied_with(u16 other) const {
        const Diplo* d = diplo_of(other);
        return d && d->allied;
    }
    float attitude_to(u16 other) const {
        for (auto& a : attitude)
            if (a.first == other) return a.second;
        return 0.0f;
    }
    float& attitude_ref(u16 other) {
        for (auto& a : attitude)
            if (a.first == other) return a.second;
        attitude.push_back({other, 0.0f});
        return attitude.back().second;
    }
    bool has_tech(const std::string& t) const {
        for (auto& x : techs)
            if (x == t) return true;
        return false;
    }
};

}  // namespace icarus
