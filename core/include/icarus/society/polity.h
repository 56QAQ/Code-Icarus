// Polity (国家): identity, culture, ruler, policies, projects, knowledge and history are
// stored separately, so a change of ruler renames the civilisation's title without
// erasing its people, industry, institutions or past.
#pragma once

#include <string>
#include <vector>

#include "icarus/sim/chronicle.h"
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
    std::vector<std::string> techs;
    std::vector<std::pair<std::string, float>> research;
    std::vector<Crisis> crises;
    PolityStats stats;
    std::vector<PolityStats> history;
    int deaths_total = 0;
    Tick forage_until = 0;   // organised foraging campaign
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
    bool has_tech(const std::string& t) const {
        for (auto& x : techs)
            if (x == t) return true;
        return false;
    }
};

}  // namespace icarus
