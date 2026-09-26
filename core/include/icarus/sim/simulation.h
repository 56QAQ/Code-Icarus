// Top-level simulation: owns every subsystem and advances the world clock.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/agents/agents.h"
#include "icarus/agents/jobs.h"
#include "icarus/agents/nav.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/economy.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/chronicle.h"
#include "icarus/sim/context.h"
#include "icarus/society/society.h"
#include "icarus/decision/decisions.h"
#include "icarus/sim/clock.h"
#include "icarus/fauna/fauna.h"
#include "icarus/sim/ecology.h"
#include "icarus/sim/physics.h"
#include "icarus/util/json.h"
#include "icarus/util/rng.h"
#include "icarus/world/world.h"

namespace icarus {


struct GameConfig {
    WorldConfig world;
    // "village": the settlement(s) in `era`; "wild" = village in the wild era;
    // "three_realms" = three civilisations; anything else: an empty world.
    std::string scenario = "village";
    std::string era = "village";  // 开局时代: wild / tribal / village
    int civs = 1;                 // civilisations at the start (one per site)
    int residents = 18;           // per civilisation
    // Optional overrides for the three starting magical girls (drive keys).
    std::vector<std::string> girl_drives;
    std::vector<Json> girl_personalities;
    u64 personality_seed = 0;  // 0 = derive from world seed
    Tick lifecycle_idle_ticks = 1200;
};

// A player (administrator) command. Applied at the start of the next tick and logged.
struct AdminCommand {
    std::string type;
    Json params;
};

// A change worth animating: a cube broken out of the world or something landing in
// place. Presentation only (never saved or hashed); the renderer takes them.
struct VisualFx {
    enum Kind : u8 { Break = 0, Land = 1 };
    u8 kind = Break;
    Vec3i pos;
    MatId mat = 0;
};

class Simulation {
public:
    explicit Simulation(const Registry& reg);
    ~Simulation();

    void new_game(const GameConfig& cfg);
    void step();
    void run(Tick ticks) {
        for (Tick i = 0; i < ticks; ++i) step();
    }

    Tick now() const { return tick_; }
    const Registry& reg() const { return *reg_; }
    const GameConfig& config() const { return cfg_; }
    World& world() { return world_; }
    const World& world() const { return world_; }
    Chronicle& chronicle() { return chronicle_; }
    const Chronicle& chronicle() const { return chronicle_; }
    Physics& physics() { return physics_; }
    Economy& economy() { return econ_; }
    Buildings& buildings() { return buildings_; }
    Farming& farming() { return farming_; }
    JobBoard& jobs() { return jobs_; }
    Nav& nav() { return nav_; }
    Agents& agents() { return agents_; }
    Society& society() { return society_; }
    const Agents& agents() const { return agents_; }
    const Society& society() const { return society_; }
    const Economy& economy() const { return econ_; }
    const Buildings& buildings() const { return buildings_; }
    Decisions& decisions() { return decisions_; }
    const Ecology& ecology() const { return ecology_; }
    Fauna& fauna() { return fauna_; }
    const Fauna& fauna() const { return fauna_; }
    // The islands' forest: generated trees still standing plus those that grew back.
    ForestStats forest() const {
        ForestStats f = world_.forest();
        f.regrown = ecology_.regrown_standing();
        return f;
    }
    const Decisions& decisions() const { return decisions_; }
    SimContext& ctx() { return ctx_; }

    void queue_admin(AdminCommand cmd) { admin_queue_.push_back(std::move(cmd)); }
    // Applies immediately (used by tests and scenario scripts). Returns the event id.
    EventId apply_admin(const AdminCommand& cmd);

    // Persistence.
    std::vector<u8> save() const;
    void load(const std::vector<u8>& data);
    u64 state_hash() const;

    // Per-tick profiling (microseconds, last tick).
    struct Profile {
        double physics_us = 0, agents_us = 0, society_us = 0, decisions_us = 0, fauna_us = 0, total_us = 0;
    };
    const Profile& profile() const { return profile_; }

    // Visual effects since the last call (bounded; the oldest are dropped).
    std::vector<VisualFx> take_fx() {
        std::vector<VisualFx> out;
        out.swap(fx_);
        return out;
    }

private:
    void on_cell_wake(Cell& c, Tick last, Tick now);
    void dispatch_changes();
    void note_fx(const std::vector<VoxelChange>& changes);

    std::vector<VisualFx> fx_;
    const Registry* reg_;
    GameConfig cfg_;
    World world_;
    Chronicle chronicle_;
    Physics physics_;
    Economy econ_;
    Buildings buildings_;
    Farming farming_;
    JobBoard jobs_;
    Nav nav_;
    SimContext ctx_;
    Agents agents_;
    Society society_;
    Decisions decisions_;
    Ecology ecology_;
    Fauna fauna_;
    Rng scenario_rng_;
    std::vector<AdminCommand> admin_queue_;
    Tick tick_ = 0;
    Profile profile_;
};

}  // namespace icarus
