// The steward's plan (内政规划): every couple of hours each polity takes stock of what it
// has, what the land around it can give for good, what it will need, and decides how
// its working hands should be shared out, whether families can afford more children,
// and what its ruler should be told. The plan is what makes the people look ahead
// instead of doing only what seems most urgent this minute.
#pragma once

#include <string>
#include <vector>

#include "icarus/util/types.h"

namespace icarus {

// Food sources the plan keeps apart.
enum FoodSource : int { kSrcFarm = 0, kSrcForage, kSrcHunt, kSrcFish, kFoodSources };
const char* food_source_zh(int s);

// A trade (职业) the plan staffs: a share of the grown hands that are free to work.
enum Trade : int { kTradeFood = 0, kTradeBuild, kTradeGather, kTrades };
const char* trade_key(int t);  // "food" / "build" / "gather" (the occupation strings)
const char* trade_zh(int t);

// Something the steward recommends to the ruler (it biases her choices; she decides).
struct Advice {
    std::string key;    // the decision option it supports (found_farm, expand_farms,
                        // build_housing, build_<def>, research_<tech>, prioritize_food,
                        // ration, forage, found_outpost)
    float urgency = 0;  // 0..1
    std::string text;   // why, in the steward's words
};

struct PolityPlan {
    Tick at = 0;  // when it was last drawn up (0: never)

    // --- Taking stock.
    int workers = 0;          // grown residents fit to work (not soldiers, not girls)
    int children = 0, elders = 0, people = 0;
    float need = 0;           // food eaten a day
    float stock = 0;          // food in the public stores
    float seed = 0;           // grain kept for sowing
    float income = 0;         // food brought in a day, lately (all sources)
    float income_by[kFoodSources] = {0};
    float potential = 0;      // food a day the land around can give for good, at full effort
    float potential_by[kFoodSources] = {0};
    float capacity = 0;       // people that potential would feed
    int beds = 0, plots = 0, irrigated = 0;
    int sites = 0;            // buildings under construction

    // --- Looking ahead.
    float target_days = 3;    // days of food to keep in store
    float balance = 0;        // income - need, a day
    float days_left = 99;     // until the stores run dry at this balance (99: they do not)
    int plots_needed = 0;     // fields to feed everyone (farmers)
    int beds_needed = 0;      // beds for everyone, and for the children to come

    // --- Decisions.
    float want[kTrades] = {0.5f, 0.25f, 0.25f};  // shares of the free hands by trade
    int staff[kTrades] = {0};                     // how many hold each trade now
    float birth = 1.0f;       // births allowed: 0 (none) .. 1.5 (encouraged)
    bool renewing = false;    // ...to replace the grown folk who will be old a generation on
    std::string birth_why;
    std::vector<Advice> advice;  // most urgent first
    std::string summary;         // a line for the council and the player

    // --- Bookkeeping: looks at the food ledger over the last three days (one each time
    // the plan is drawn up), for what came in a day and how many worked for it.
    struct Look {
        Tick at = 0;
        double food[kFoodSources] = {0};  // the ledger's totals
        float hands = 0;                  // people in the food trade
    };
    std::vector<Look> looks;
    Tick first = 0;   // the first plan
    float hands = 0;  // people in the food trade, on average over the looks
    // Learnt from the stores: how far the reckoned food share has to be leaned (up while
    // the stores stay below their target, down while they overflow).
    float food_lean = 0;

    // How strongly the plan backs a decision option (0 when it does not).
    float backing(const std::string& key) const {
        for (const Advice& a : advice)
            if (a.key == key) return a.urgency;
        return 0.0f;
    }
    const Advice* top() const { return advice.empty() ? nullptr : &advice.front(); }
};

// Weights of what stands built and what is known in the civilisation index (people living
// well count one each; see Society::civ_index).
constexpr float kCivBuiltWeight = 0.3f;
constexpr float kCivKnownWeight = 0.8f;

// The island's state at a glance (文明指数): how many live, how well, how secure, how
// housed, how much is built and known, and how content. Recorded daily per polity.
struct CivIndex {
    float people = 0;     // population (children count half)
    float fed = 0;        // share fed (0..1)
    float secure = 0;     // food security: stock against the target (0..1)
    float housed = 0;     // share with a bed (0..1)
    float built = 0;      // standing buildings' worth
    float known = 0;      // techs known, weighted by era
    float content = 0;    // mood and stability (0..1)
    float total = 0;      // the composite
};

}  // namespace icarus
