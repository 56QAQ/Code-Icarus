// Scenario setup: the starting village on the main island.
#pragma once

#include "icarus/sim/context.h"
#include "icarus/util/json.h"

namespace icarus {

struct GameConfig;

// Builds the phase-1 prototype: one polity, three magical girls, residents, a village of
// huts around a hall, a storehouse and kitchen, irrigated fields across the ravine by the
// lake, and the plank bridge that connects them.
void build_village_scenario(SimContext& ctx, const GameConfig& cfg, Rng& rng);

// Default persona for a magical girl of a drive (randomised around the drive's temper).
void make_girl(SimContext& ctx, EntityId id, const std::string& drive, Rng& rng, const Json& persona_override);

}  // namespace icarus
