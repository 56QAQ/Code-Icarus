// Fauna: the wild animals of the islands (game/data/animals.json). Every animal is a
// deterministic, saved entity: it grazes and wanders near its herd, flees from people
// and predators (tiring as it runs), predators hunt their prey, herds breed up to what
// the land carried at first, and animals age and die. Hunters strike them and butcher
// the carcass for meat, hide and bone.
//
// Animals near people are simulated every tick; the rest every kFarStep ticks with the
// same rules in bigger steps. Movement only looks at the world (peek), so animals never
// wake cells or change the terrain.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "icarus/agents/character.h"
#include "icarus/economy/economy.h"
#include "icarus/sim/chronicle.h"
#include "icarus/sim/context.h"
#include "icarus/util/binio.h"
#include "icarus/util/rng.h"
#include "icarus/world/world.h"

namespace icarus {

enum class Temper : u8 { Shy = 0, Defensive, Predator, Territorial };
enum class AnimalState : u8 { Idle = 0, Graze, Wander, Flee, Chase, Attack, Eat, Dead };

struct SpeciesDef {
    u16 id = 0;
    std::string key, name;
    Vec3f size{1, 1, 0.5f};
    float speed = 0.2f, run = 0.5f, hp = 0.5f, flee = 10.0f, attack = 0.0f, guard = 0.0f;
    int stamina = 300;
    Temper temper = Temper::Shy;
    bool tall = false;  // needs two cubes of headroom
    bool aquatic = false;       // lives in water (fish): swims in lakes, dies stranded
    float water_density = 0.0f; // aquatic: chance of a school per 8x8 patch of deep water
    std::vector<std::pair<Biome, float>> biomes;
    int herd_min = 1, herd_max = 1, litter_min = 1, litter_max = 1;
    float lifespan_days = 100.0f, breed_days = 10.0f;
    std::vector<u16> prey;
    std::vector<std::pair<ItemId, int>> yield;
    std::vector<u32> colors;
    std::vector<std::string> look;
    float density(Biome b) const {
        for (auto& [bb, d] : biomes)
            if (bb == b) return d;
        return 0.0f;
    }
};

struct Animal {
    u32 id = 0;
    u16 species = 0;
    bool alive = true;
    bool female = false;
    bool butchered = false;
    Vec3f pos;
    Vec3i foot;
    Vec3i next;          // cube being walked to
    float yaw = 0;
    float hp = 1;        // of the species' hp
    float hunger = 0;    // predators: 0 fed .. 1 hungry (dies past 3)
    float tired = 0;     // 0 fresh .. 1 spent (running tires, rest restores)
    float phase = 0;     // walk cycle (presentation)
    bool moving = false;
    AnimalState state = AnimalState::Idle;
    u32 herd = 0;        // id of the herd's leader (itself for the leader)
    Vec3i home;          // where the herd ranges
    Vec3i goal;
    u32 target = 0;      // prey animal, or a character (target_char)
    bool target_char = false;
    u32 threat = 0;      // what it flees from (animal id, or character with threat_char)
    bool threat_char = false;
    EntityId hunted_by = kNoEntity;
    EntityId attacker = kNoEntity;  // who struck it last (defensive animals turn on them)
    Tick born = 0, died = 0, next_think = 0, state_until = 0, last_bite = 0;
    std::string death_cause;
};

class Fauna {
public:
    static constexpr Tick kFarStep = 20;

    explicit Fauna(SimContext& ctx) : ctx_(ctx) {}

    void load_species(const Registry& reg);
    void reset(u64 seed);
    // Herds for the whole world at the start of a game (away from settlement sites).
    void populate();
    void step(Tick now);

    const std::vector<SpeciesDef>& species() const { return species_; }
    const SpeciesDef& spec(u16 s) const { return species_[s]; }
    int species_id(const std::string& key) const;
    const std::vector<Animal>& all() const { return animals_; }
    Animal* get(u32 id);
    const Animal* get(u32 id) const;
    int count_alive(int species = -1) const;

    // Hunting. A blow of `power` (weapon harm); returns true when it killed.
    bool strike(Animal& a, float power, EntityId by, EventId cause);
    // Butcher a carcass: its yield goes into the store. Returns units produced.
    int butcher(Animal& a, StoreId into);
    // The nearest living animal of a huntable kind within radius that nobody is after.
    u32 find_prey(const Vec3i& from, int radius, bool dangerous_too) const;
    // The nearest fish within radius that nobody is after (0 if none).
    u32 find_fish(const Vec3i& from, int radius) const;
    // Kills a fish caught by `by` (a fisher) and puts the catch in `into`; returns units.
    int catch_fish(Animal& a, EntityId by, StoreId into);
    // Whether an animal of species s can be at p (on its feet, or swimming).
    bool fits(const SpeciesDef& s, const Vec3i& p) const;
    // How threatening animals are at p for a person (0..1): predators on the prowl,
    // a bear's ground, a boar that was struck.
    float threat_at(const Vec3f& p) const;
    // The threatening animal nearest to p (0 if none within radius).
    u32 nearest_threat(const Vec3f& p, float radius) const;

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

    // Diagnostics (not saved): deaths by cause since the start of the session.
    std::map<std::string, int> deaths;

private:
    bool walkable(const Vec3i& p, bool tall) const;
    bool swimmable(const Vec3i& p) const;
    // A place for an animal of species s near p (ground, or water for fish).
    bool find_spot(Vec3i& p, const SpeciesDef& s) const;
    void spawn_herd(u16 s, const Vec3i& at, int n, Tick now);
    u32 spawn(u16 s, const Vec3i& foot, u32 herd, Tick now);
    void think(Animal& a, bool near_people);
    void move(Animal& a, float budget);
    Vec3i pick_next(const Animal& a) const;
    void die(Animal& a, const std::string& cause);
    void daily();
    void index_people();
    bool people_near(const Vec3i& p, int radius) const;
    const Character* nearest_person(const Vec3f& p, float radius) const;

    SimContext& ctx_;
    std::vector<SpeciesDef> species_;
    std::vector<Animal> animals_;  // index = id - 1; dead animals stay (carcasses) until pruned
    std::vector<int> capacity_;    // per species: what the land carried at the start
    Rng rng_;
    u32 next_id_ = 1;
    Tick now_ = 0;
    // People by 16-cube column, and which 32-cube cell columns have someone within
    // kNearPeople (rebuilt every few ticks, not saved). Predators on a chase.
    std::vector<std::pair<i64, EntityId>> people_;
    std::vector<u8> near_;
    std::vector<u32> chasers_;
    Tick people_at_ = ~0ull;
    // Far from everyone animals walk on the generator's terrain heights (cheap, and
    // nobody sees them brush through a tree); near people they see every cube.
    mutable bool rough_ = false;
    // Animals that threaten people this tick (pos, radius), built on first use in a tick.
    mutable std::vector<std::pair<Vec3f, float>> threats_;
    mutable Tick threats_at_ = ~0ull;
};

}  // namespace icarus
