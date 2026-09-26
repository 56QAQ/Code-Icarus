// Shared access to every subsystem for code that crosses system boundaries
// (agents, society, decisions). Owned and wired by Simulation.
#pragma once

#include "icarus/data/registry.h"
#include "icarus/util/rng.h"

namespace icarus {

class World;
class Chronicle;
class Physics;
class Economy;
class Buildings;
class Farming;
class JobBoard;
class Nav;
class Agents;
class Society;
class Magic;
class Decisions;

struct SimContext {
    const Registry* reg = nullptr;
    World* world = nullptr;
    Chronicle* chron = nullptr;
    Physics* physics = nullptr;
    Economy* econ = nullptr;
    Buildings* buildings = nullptr;
    Farming* farming = nullptr;
    JobBoard* jobs = nullptr;
    Nav* nav = nullptr;
    Agents* agents = nullptr;
    Society* society = nullptr;
    Magic* magic = nullptr;
    Decisions* decisions = nullptr;
    Tick now = 0;
};

}  // namespace icarus
