// Agents and the steward's plan: trades assigned from the plan's shares of hands.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/society/society.h"

namespace icarus {

namespace {
// How well someone suits a trade: what they are good at, and a little of their temper
// (the diligent build, the sociable and strong-willed range the wild).
float fitness(const Character& c, int trade) {
    switch (trade) {
        case kTradeFood: return c.skills[kFarming] + 0.5f * c.skills[kCooking] + 0.1f * c.pers.altruism;
        case kTradeBuild: return c.skills[kBuilding] + 0.3f * c.skills[kCrafting] + 0.1f * c.pers.diligence;
        default: return c.skills[kMining] + 0.4f * c.skills[kHauling] + 0.1f * c.pers.ambition;
    }
}
int trade_of_occupation(const std::string& o) {
    for (int t = 0; t < kTrades; ++t)
        if (o == trade_key(t)) return t;
    return -1;
}
}  // namespace

void Agents::assign_trades(u16 polity, const PolityPlan& plan) {
    std::vector<Character*> pool;
    for (auto& cp : chars_) {
        Character* c = cp.get();
        if (!c || !c->alive || c->departed || c->polity != polity || c->is_girl() || c->drafted) continue;
        if (is_child(*c) || !c->body.can_hold() || c->occupation == "research") continue;
        pool.push_back(c);
    }
    const int n = (int)pool.size();
    if (n == 0) return;
    // Hands wanted per trade (largest remainders, so they add up to everyone).
    int want[kTrades] = {0}, have[kTrades] = {0};
    float frac[kTrades];
    int given = 0;
    for (int t = 0; t < kTrades; ++t) {
        const float x = plan.want[t] * (float)n;
        want[t] = (int)std::floor(x);
        frac[t] = x - (float)want[t];
        given += want[t];
    }
    while (given < n) {
        int best = 0;
        for (int t = 1; t < kTrades; ++t)
            if (frac[t] > frac[best]) best = t;
        want[best]++;
        frac[best] = -1.0f;
        ++given;
    }
    // Anyone without a trade takes the most short-handed one.
    for (Character* c : pool)
        if (trade_of_occupation(c->occupation) < 0) {
            int best = 0;
            for (int t = 1; t < kTrades; ++t)
                if (want[t] - have[t] > want[best] - have[best]) best = t;
            c->occupation = trade_key(best);
        }
    for (Character* c : pool) have[trade_of_occupation(c->occupation)]++;
    // A few at a time move from trades with hands to spare to short-handed ones, those who
    // suit the new trade best (and the old one least) first.
    int moves = std::max(1, n / 4);
    while (moves-- > 0) {
        int shortest = -1, fullest = -1;
        for (int t = 0; t < kTrades; ++t) {
            if (want[t] - have[t] > 0 && (shortest < 0 || want[t] - have[t] > want[shortest] - have[shortest])) shortest = t;
            if (have[t] - want[t] > 0 && (fullest < 0 || have[t] - want[t] > have[fullest] - want[fullest])) fullest = t;
        }
        if (shortest < 0 || fullest < 0) break;
        Character* pick = nullptr;
        float bs = -1e9f;
        for (Character* c : pool) {
            if (trade_of_occupation(c->occupation) != fullest) continue;
            const float s = fitness(*c, shortest) - fitness(*c, fullest);
            if (s > bs || (s == bs && pick && c->id < pick->id)) {
                bs = s;
                pick = c;
            }
        }
        if (!pick) break;
        pick->occupation = trade_key(shortest);
        have[fullest]--;
        have[shortest]++;
    }
}

}  // namespace icarus
