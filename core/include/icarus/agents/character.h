// Characters: ordinary artificial humans (residents) and magical girls share this
// structure. Personality is separate from a magical girl's drive (源动力).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "icarus/agents/body.h"
#include "icarus/agents/nav.h"
#include "icarus/economy/economy.h"
#include "icarus/sim/chronicle.h"

namespace icarus {

enum class CharKind : u8 { Resident = 0, MagicalGirl = 1 };

struct Personality {
    float caution = 0.5f;      // flees earlier, avoids risk
    float altruism = 0.5f;     // helps others, shares, rescues
    float ambition = 0.5f;     // values reward and status
    float diligence = 0.5f;    // intrinsic motivation to work
    float sociability = 0.5f;  // social need weight
    float conformity = 0.5f;   // responds to authority, fears punishment
    float aggression = 0.5f;   // theft, violence, rebellion
    float idealism = 0.5f;     // values fairness/principles over pragmatism
    static constexpr int kCount = 8;
    float& at(int i) { return (&caution)[i]; }
    float at(int i) const { return (&caution)[i]; }
};
const char* trait_name_zh(int i);

enum Skill : u8 { kFarming = 0, kBuilding, kMining, kHauling, kCrafting, kResearch, kCombat, kMedicine, kCooking, kSkillCount };
const char* skill_name_zh(int s);

struct Needs {
    float food = 0.9f;
    float water = 0.9f;
    float rest = 0.9f;
    float social = 0.7f;
    float safety = 1.0f;
    float comfort = 0.6f;
};

struct Relation {
    EntityId other = kNoEntity;
    float affinity = 0.0f;  // -1..1
};

struct Support {
    EntityId girl = kNoEntity;  // a magical girl
    float value = 0.0f;         // -1..1
};

enum class MemoryKind : u8 {
    None = 0, AteWell, Hungry, Thirsty, Injured, SawDeath, FriendDied, Punished, Rewarded, Helped, Saved,
    Disaster, HomeLost, Rationed, Feast, Protested, Insulted, LostJob, Healed, Blessed, Cursed, Migrated,
    Partnered, ChildBorn, Bereaved
};
const char* memory_kind_zh(MemoryKind k);

struct Memory {
    Tick tick = 0;
    MemoryKind kind = MemoryKind::None;
    EntityId subject = kNoEntity;  // who caused / was involved
    float valence = 0;             // mood impact (-1..1), decays
    EventId event = 0;
};

enum class TaskType : u8 {
    None = 0, Idle, Wander, Eat, Drink, Sleep, Socialize, Work, Flee, Protest, Steal, Heal, Govern, Cast, Fight, Leave, Escape
};
const char* task_name_zh(TaskType t);

// Utility scoring trace: why the character chose what it is doing.
struct Consideration {
    std::string label;
    float score = 0;
    std::string why;
};

struct Task {
    TaskType type = TaskType::None;
    u8 step = 0;
    u32 job = 0;
    Vec3i target;
    Vec3i target2;       // secondary position (e.g. the cube being dug)
    StoreId store = kNoStore;
    ItemId item = kNoItem;
    i32 count = 0;
    EntityId other = kNoEntity;
    Tick started = 0;
    Tick until = 0;
    float utility = 0;
    int fails = 0;
    std::string label;
    u8 resume = 0;       // step to go back to after fetching a tool (step 9)
};

// A tie between two magical girls, as she sees it.
enum class BondKind : u8 { None = 0, Friend, Rival, Mentor, Student, Nemesis };
const char* bond_name_zh(BondKind k);
struct Bond {
    EntityId other = kNoEntity;
    BondKind kind = BondKind::None;
    Tick since = 0;
    EventId event = 0;  // what made it
};

struct GirlData {
    std::string drive;        // drive key (see drives.json)
    int level = 1;
    float xp = 0;
    float mana = 1.0f;        // 0..1 of max
    Personality persona;      // decision persona (for Jev), separate from the drive
    std::string temperament;  // short description used in prompts/UI
    std::string role = "none";  // ruler / minister / governor / general / none
    std::string domain;         // area of responsibility (agriculture, logistics, military...)
    float loyalty = 0.6f;       // toward the current ruler
    float ambition_pressure = 0;
    std::vector<u32> spell_cooldowns = std::vector<u32>(4, 0);
    Tick last_decision = 0;
    std::vector<u32> decisions;  // decision record ids
    std::string stance = "loyal";  // loyal / critical / defiant / rebel
    std::vector<std::pair<std::string, float>> experience;  // option key -> how it worked out
    EntityId grudge = kNoEntity;   // someone who wronged her politically
    Tick awakened = 0;             // when she awoke among the people (0: one of the first)
    // The drama of her life (magic_drama.cpp).
    std::vector<Bond> bonds;
    float trauma = 0;              // grief, defeat, famine: toward a darker drive
    float solace = 0;              // kindness received, triumph, friendship: toward a brighter one
    std::vector<EventId> marks;    // the experiences behind them (the latest few)
    std::string born_drive;        // the drive she awoke with ("" = unchanged)
    Tick drive_changed = 0;
    EntityId duel = kNoEntity;     // an enemy girl she is locked in a duel with
    Tick duel_since = 0;
    const Bond* bond_with(EntityId o) const {
        for (const Bond& b : bonds)
            if (b.other == o) return &b;
        return nullptr;
    }
    float experience_of(const std::string& k) const {
        for (auto& e : experience)
            if (e.first == k) return e.second;
        return 0.0f;
    }
};

struct Character {
    EntityId id = kNoEntity;
    CharKind kind = CharKind::Resident;
    std::string name;
    bool female = true;
    bool alive = true;
    bool departed = false;
    Tick born = 0;
    float age0 = 25.0f;             // age in years at `born` (the first generation arrives grown)
    EntityId partner = kNoEntity;
    EntityId parents[2] = {kNoEntity, kNoEntity};
    Tick last_child = 0;
    Tick died = 0;
    std::string death_cause;
    EventId death_event = 0;
    u16 polity = 0;

    Vec3f pos;         // feet position (world units)
    Vec3i foot;        // standable cube currently occupied
    float yaw = 0;
    float fall_speed = 0;
    float walk_phase = 0;
    bool moving = false;

    Body body;
    Appearance look;
    Needs needs;
    float mood = 0.6f;
    float stress = 0.0f;
    float fear = 0.0f;          // fear of punishment / authority pressure
    float work_debt = 0.0f;     // hours of shirked duty
    Personality pers;
    float skills[kSkillCount] = {0};
    std::vector<Relation> relations;
    std::vector<Support> support;
    std::vector<Memory> memories;

    StoreId inv = kNoStore;     // carried items
    ItemId tool = kNoItem, weapon = kNoItem, armor = kNoItem, cart = kNoItem;
    ItemId clothes = kNoItem;   // worn (leaf wrap, fur cloak, linen...)
    u16 tool_wear = 0;          // uses of the tool in hand since it was taken up
    Tick treated_until = 0;     // wounds dressed: they heal faster until then
    bool treated_well = false;  // ...and faster still when treated at a 药庐
    Tick empowered_until = 0;   // a war cry or battle frenzy: stronger and fearless until then
    float empowered = 1.0f;     // strength factor while empowered
    u32 home = 0;               // building id
    std::string occupation;     // preferred work category
    Task task;
    Path path;
    Vec3i path_goal{-99999, 0, 0};
    Vec3i water_spot{-1, -1, -1};  // remembered place to drink
    u16 region = 0;                // walkable region (refreshed with the water index; 0 = unknown)
    Tick next_think = 0;
    Tick last_ate = 0, last_drank = 0, last_slept = 0, last_social = 0, last_punished = 0;
    Tick no_food_until = 0;  // found nothing to eat anywhere near: not looking again before this
    bool in_boat = false;    // afloat in a boat (fishing out on the water): no ground needed
    std::vector<Consideration> trace;  // last decision breakdown
    std::vector<std::pair<Vec3i, Tick>> unreachable;  // temporarily blacklisted targets
    bool drafted = false;
    bool rebel = false;
    bool sleeping = false;
    std::string status_text;    // what am I doing, in words

    std::unique_ptr<GirlData> girl;

    bool is_girl() const { return kind == CharKind::MagicalGirl && girl != nullptr; }
    float support_for(EntityId g) const {
        for (auto& s : support)
            if (s.girl == g) return s.value;
        return 0.0f;
    }
    float& support_ref(EntityId g) {
        for (auto& s : support)
            if (s.girl == g) return s.value;
        support.push_back({g, 0.0f});
        return support.back().value;
    }
    float affinity(EntityId o) const {
        for (auto& r : relations)
            if (r.other == o) return r.affinity;
        return 0.0f;
    }
    float& affinity_ref(EntityId o) {
        for (auto& r : relations)
            if (r.other == o) return r.affinity;
        relations.push_back({o, 0.0f});
        return relations.back().affinity;
    }
    void remember(Tick now, MemoryKind k, EntityId subject, float valence, EventId ev) {
        memories.push_back({now, k, subject, valence, ev});
        if (memories.size() > 24) memories.erase(memories.begin());
    }
};

}  // namespace icarus
