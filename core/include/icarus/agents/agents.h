// Agents: every character's needs, health, perception, utility-based choices, tasks
// and movement. Residents are autonomous: plans and policies change what options are
// worth, never what they are forced to do.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "icarus/agents/character.h"
#include "icarus/agents/jobs.h"
#include "icarus/sim/context.h"
#include "icarus/sim/physics.h"

namespace icarus {

struct AgentTuning {
    float walk_speed = 0.38f;       // cubes per tick at full mobility (days are compressed)
    float food_per_day = 1.0f;
    float water_per_day = 1.6f;
    float rest_per_day = 1.5f;      // awake 16h drains ~1.0
    float social_per_day = 0.5f;
    float carry_capacity = 12.0f;   // weight units
    Tick think_interval = 30;
};

class Agents {
public:
    explicit Agents(SimContext& ctx);

    void reset(u64 seed);
    void step(Tick now);

    EntityId spawn(CharKind kind, const std::string& name, bool female, const Vec3i& foot, u16 polity);
    Character* get(EntityId id) {
        return (id > 0 && id < chars_.size() && chars_[id]) ? chars_[id].get() : nullptr;
    }
    const Character* get(EntityId id) const {
        return (id > 0 && id < chars_.size() && chars_[id]) ? chars_[id].get() : nullptr;
    }
    const std::vector<std::unique_ptr<Character>>& all() const { return chars_; }
    std::vector<Character*> living();
    int count_alive(u16 polity) const;

    void kill(Character& c, const std::string& cause, EventId ev_cause);
    void apply_area_damage(const AreaDamage& d);
    void damage(Character& c, float fraction, int part, const std::string& what, EventId cause);

    // Utility: random personality/appearance/skills.
    void randomize(Character& c, Rng& rng);

    // Job generation (fields, piles, sites, kitchens...).
    void generate_jobs();

    AgentTuning tune;
    // Diagnostics (not saved): recent failed routes (who, from, to).
    struct PathFail { EntityId who; Vec3i from, to; Tick tick; };
    std::vector<PathFail> debug_path_failures;
    Rng& rng() { return rng_; }
    bool find_water(Character& c, Vec3i& stand, Vec3i& water);

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

    // Counters for polity statistics (reset daily by society).
    struct DayCounters {
        int harvested = 0, ate_public = 0, refused_food = 0, thefts = 0, path_failures = 0, drinks = 0,
            hungry_no_food = 0, thirsty_no_water = 0;
    } day;

private:
    // core
    void update_needs(Character& c);
    void update_health(Character& c);
    void update_physics(Character& c);
    void hourly(Character& c);
    // ai
    void think(Character& c);
    float work_score(Character& c, const Job& j, std::string& why);
    u32 best_job(Character& c, float& score, std::string& why);
    // tasks
    void run_task(Character& c);
    void end_task(Character& c, bool success);
    void start_task(Character& c, TaskType t, float utility, const std::string& label);
    bool task_eat(Character& c);
    bool task_drink(Character& c);
    bool task_sleep(Character& c);
    bool task_social(Character& c);
    bool task_wander(Character& c);
    bool task_work(Character& c);
    bool task_flee(Character& c);
    bool task_protest(Character& c);
    bool task_steal(Character& c);
    bool task_govern(Character& c);
    // Magic cast on her own initiative (healing the injured, quenching fire).
    struct SpellPick {
        int effect = 0;  // 1 heal, 2 quench
        Vec3i pos;
        EntityId who = kNoEntity;
        float mana = 0;
        std::string name;
    };
    float pick_spell(Character& c, SpellPick& out, std::string& why);
    bool task_cast(Character& c);
    // movement
    enum class Move { Moving, Arrived, Failed };
    Move move_to(Character& c, const Vec3i& goal, bool adjacent_ok);
    void place_at(Character& c, const Vec3i& foot);
    bool blacklisted(Character& c, const Vec3i& p);
    void blacklist(Character& c, const Vec3i& p, Tick duration);
    // helpers
    StoreId find_food_store(Character& c, bool public_only, bool allow_over_ration);
    StoreId nearest_storage(u16 polity, const Vec3i& from, ItemId item_for_capacity);
    float danger_at(const Character& c) const;
    bool is_work_time(const Character& c) const;
    float carried_weight(const Character& c) const;
    void deposit_all(Character& c, StoreId to);
    void say(Character& c, const std::string& s) { c.status_text = s; }

    SimContext& ctx_;
    Rng rng_;
    std::vector<std::unique_ptr<Character>> chars_ = std::vector<std::unique_ptr<Character>>(1);
    Tick now_ = 0;
    std::vector<std::pair<Vec3f, float>> dangers_;  // recent hazards (pos, radius)
    std::vector<Vec3i> water_spots_;                // standable places next to drinkable water
    std::vector<u16> water_regions_;                // walkable region of each spot (0 = unknown)
    // Walkable regions flooded from settlement anchors. Rebuilt when event-driven terrain
    // changes happen (or daily after minor settling); saved so reloads stay deterministic.
    std::unordered_map<Vec3i, u16, Vec3iHash> region_map_;
    std::vector<Vec3i> region_anchors_;
    Tick region_built_ = 0;
    void refresh_water_spots();
};

}  // namespace icarus
