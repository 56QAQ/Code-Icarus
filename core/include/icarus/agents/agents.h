// Agents: every character's needs, health, perception, utility-based choices, tasks
// and movement. Residents are autonomous: plans and policies change what options are
// worth, never what they are forced to do.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "icarus/agents/character.h"
#include "icarus/agents/region_map.h"
#include "icarus/agents/jobs.h"
#include "icarus/sim/context.h"
#include "icarus/sim/physics.h"

namespace icarus {

struct Polity;
struct Building;

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
    // What a character can carry: more with a cart.
    float carry_capacity(const Character& c) const;
    // The nearest cube within `radius` that residents can walk to from a settlement.
    bool nearest_walkable(const Vec3i& p, int radius, Vec3i& out) const;
    // A hazard residents will run from for a while (explosions, a god's wrath).
    void add_danger(const Vec3f& p, float radius) { dangers_.push_back({p, radius}); }

    // Utility: random personality/appearance/skills.
    void randomize(Character& c, Rng& rng);

    // The course of a life (agents_life.cpp). Ages are in years (see life.json).
    float age_years(const Character& c) const;
    bool is_child(const Character& c) const;
    bool is_elder(const Character& c) const;
    // A first-generation age for someone who arrives grown (a pure hash of who they are).
    void set_first_age(Character& c);
    // Once a day: old age, partnerships, births, coming of age, awakenings.
    void daily_life();
    // A magical girl awakens among the people of a polity (false: nobody could).
    bool awaken(const Polity& p, EventId cause);
    // The children of a character (living ones).
    std::vector<EntityId> children_of(EntityId id) const;

    // Job generation (fields, piles, sites, kitchens...).
    void generate_jobs();
    // A wild food plant in column (x, z) within reach from the ground: a bush, grain,
    // mushrooms on the surface, or fruit in the lowest layers of a crown.
    bool food_plant_at(int x, int z, Vec3i& out);
    // The nearest wild food plant within radius of p (not already someone's job).
    bool wild_food_near(const Vec3i& p, int radius, Vec3i& out);
    // A standable place by any water within radius (pools and streams in the wild).
    bool wild_water_near(const Vec3i& p, int radius, Vec3i& stand);

    // Equipment (agents_equipment.cpp). The tool a job is done with ("" = none helps).
    std::string tool_kind_for(const Job& j) const;
    // Speed of work of that kind with what c holds (1 = a stone tool of the kind; bare
    // hands are much slower at felling, quarrying, tilling...).
    float tool_factor(const Character& c, const std::string& kind) const;
    // A public store not far out of the way holding a tool of that kind (reserved), or 0.
    StoreId tool_store_for(Character& c, const std::string& kind, const Vec3i& work);
    // Take the best tool of the kind from the store, handing back the one in hand.
    bool swap_tool(Character& c, StoreId sid, const std::string& kind);
    // A store not far out of the way with a hunting weapon (reserved), or 0; take it.
    StoreId hunting_weapon_store(Character& c, const Vec3i& work);
    bool take_hunting_weapon(Character& c, StoreId sid);
    // One use of the tool in hand; a tool used up breaks.
    void wear_tool(Character& c);
    // What each occupation keeps in hand between jobs.
    static const char* occupation_tool(const std::string& occupation);
    // Cold, wet and dark against what c wears: 0 = comfortable .. 1 = freezing.
    float exposure(const Character& c) const;
    bool near_campfire(const Vec3i& p) const;

    AgentTuning tune;
    // Diagnostics (not saved): recent failed routes (who, from, to).
    struct PathFail { EntityId who; Vec3i from, to; Tick tick; };
    std::vector<PathFail> debug_path_failures;
    // Magic made visible: casts since the last take. Presentation only: not saved, never
    // read by the simulation, and bounded when nobody takes them.
    struct SpellFx {
        std::string effect, name, drive;
        EntityId caster = kNoEntity, target = kNoEntity;
        Vec3f from, to;
        float radius = 0;
    };
    void note_spell(const Character& caster, const std::string& effect, const std::string& name, const Vec3f& to,
                    EntityId target = kNoEntity, float radius = 0);
    std::vector<SpellFx> take_spells() {
        std::vector<SpellFx> out;
        out.swap(spell_fx_);
        return out;
    }
    // Strength factor of a character's blows right now (war cry, frenzy).
    float empowerment(const Character& c) const { return now_ < c.empowered_until ? c.empowered : 1.0f; }
    // Magic cast on her own initiative (agents_magic.cpp): healing the injured, quenching
    // fire, battle magic, and the rituals.
    struct SpellPick {
        int effect = 0;  // the spell's code while casting (Task::count)
        Vec3i pos;
        EntityId who = kNoEntity;
        float mana = 0;
        std::string name;
    };
    float pick_spell(Character& c, SpellPick& out, std::string& why);
    // The drama among magical girls (agents_drama.cpp): ties, what weighs on them, and
    // drives that turn.
    void daily_drama();
    void mark_girl(EntityId id, float trauma, float solace, EventId ev);
    void set_bond(Character& a, Character& b, BondKind ka, BondKind kb, EventId ev);
    void drop_bond(Character& a, Character& b);
    void take_student(Character& girl, EventId cause);
    void girl_died(Character& dead, EventId ev);
    void girl_felled(Character& victor, Character& fallen, EventId cause);
    void turn_drive(Character& c, const std::string& to, bool darker);
    EventId tell(EventType type, u8 severity, const Character& a, EntityId other, EventId cause, std::string text);
    // A blow in war (agents_war.cpp): wounds, the tally of the dead, grievances.
    void strike(Character& attacker, Character& target, float power, const std::string& how, EventId cause);
    // Two enemy magical girls turning their magic on each other (told once per duel).
    void begin_duel(Character& a, Character& b, EventId cause);
    // The rituals among her spells: war cry, frenzy, discord, withering, devouring.
    void cast_ritual(Character& c, int code, const std::string& name, const Json* sp, const Vec3i& at, Character* who,
                     Event& e);
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
    // Failed routes over the last 24 hours (a rolling window, unlike `day`).
    int path_failures_24h() const {
        int n = 0;
        for (int v : fail_ring_) n += v;
        return n;
    }

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
    // Where to lie down: a bed spot of its own inside the home (by rank among the
    // household), or a free cube near `near` when sleeping out; never on a cube another
    // sleeper already lies on or is heading for.
    Vec3i sleep_spot(const Character& c, const Building* home, const Vec3i& near);
    bool task_social(Character& c);
    bool task_wander(Character& c);
    bool task_work(Character& c);
    bool task_flee(Character& c);
    bool task_protest(Character& c);
    bool task_steal(Character& c);
    bool task_govern(Character& c);
    bool task_cast(Character& c);
    // Trapped (e.g. fell into a ravine): dig a staircase toward reachable ground.
    bool trapped(const Character& c) const;
    // War (agents_war.cpp).
    // Nearest living enemy (at war); fighters_only = soldiers and magical girls,
    // soldiers_only = drafted residents only.
    Character* nearest_enemy(const Character& c, float radius, bool fighters_only, bool soldiers_only = false);
    bool task_fight(Character& c);
    bool task_escape(Character& c);
    bool task_leave(Character& c);
    bool task_treat(Character& c);
    // How badly a resident needs their wounds seen to (0 = not at all).
    float treatment_need(const Character& c) const;
    // Migration: how strongly an unhappy resident is drawn to another polity (0 if not).
    float migration_pull(const Character& c, const Polity& own, const Polity*& dest, std::string& why) const;
    void defect(Character& c, u16 to, EventId cause);
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
    // Set down everything carried except what is worn or held as equipment.
    void drop_cargo(Character& c);
    void deposit_all(Character& c, StoreId to);
    // Keeps tool/weapon/armour slots consistent with the inventory and picks up better
    // gear from nearby public stores (weapons and armour only when drafted).
    void update_equipment(Character& c);
    void assign_homes();
    // Craft, chop, quarry and mine jobs from what the polity needs (agents_production.cpp).
    void production_jobs();
    void say(Character& c, const std::string& s) { c.status_text = s; }

    SimContext& ctx_;
    Rng rng_;
    std::vector<std::unique_ptr<Character>> chars_ = std::vector<std::unique_ptr<Character>>(1);
    Tick now_ = 0;
    std::vector<std::pair<Vec3f, float>> dangers_;  // recent hazards (pos, radius)
    std::vector<SpellFx> spell_fx_;
    // Path search nodes expanded this tick (long searches beyond the budget wait a tick).
    static constexpr u64 kPathTickBudget = 40000;
    u64 path_spent_ = 0;
    std::array<int, 24> fail_ring_{};                // path failures per hour, last 24 h
    int fail_ring_pos_ = 0;
    std::vector<Vec3i> water_spots_;                // standable places next to drinkable water
    std::vector<u16> water_regions_;                // walkable region of each spot (0 = unknown)
    // Walkable regions flooded from settlement anchors. Rebuilt when event-driven terrain
    // changes happen (or daily after minor settling); saved so reloads stay deterministic.
    RegionMap region_map_;
    std::vector<u8> region_open_;  // per region id: 1 if its flood was cut short (may reach further)
    std::vector<Vec3i> region_anchors_;
    Tick region_built_ = 0;
    void refresh_water_spots();
};

}  // namespace icarus
