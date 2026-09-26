// Chronicle: the world's event log with causal links.
// Every significant change is recorded as an Event that names up to four causes, so the
// UI can walk backwards from "the civilisation split" to "the player dropped a meteor".
#pragma once

#include <string>
#include <vector>

#include "icarus/util/binio.h"
#include "icarus/util/json.h"
#include "icarus/util/types.h"

namespace icarus {

using EventId = u32;
constexpr EventId kNoEvent = 0;

enum class EventType : u16 {
    None = 0,
    AdminAction,
    MeteorImpact,
    FireStarted,
    FireSpread,
    Flood,
    Collapse,
    DebrisLanded,
    StructureDamaged,
    StructureDestroyed,
    WaterSourceLost,
    PathBlocked,
    LogisticsDisrupted,
    Shortage,
    ShortageResolved,
    Starvation,
    Injury,
    Death,
    CropFailure,
    Harvest,
    DecisionRequested,
    DecisionMade,
    PolicyChanged,
    ProjectStarted,
    ProjectCompleted,
    ProjectAbandoned,
    SupportShift,
    Protest,
    Punishment,
    Refusal,
    Rebellion,
    Secession,
    Coup,
    RulerChanged,
    WarDeclared,
    Battle,
    Peace,
    TechDiscovered,
    LevelUp,
    SpellCast,
    Migration,
    Theft,
    Rescue,
    Construction,
    Info,
    Unification,  // one polity holds the whole island
    Trade,        // pacts, caravans, cut trade roads
    Hunt,         // game taken, beasts attacking people
    Count
};

const char* event_type_key(EventType t);

struct Event {
    EventId id = 0;
    Tick tick = 0;
    EventType type = EventType::None;
    u8 severity = 1;  // 0 trivial .. 5 historic
    Vec3i pos;
    EntityId actor = kNoEntity;
    EntityId target = kNoEntity;
    u16 polity = 0;
    EventId causes[4] = {0, 0, 0, 0};
    std::string text;  // human readable (zh)
    Json data;         // structured details for UI
};

class Chronicle {
public:
    EventId emit(Event e);  // assigns id/tick, returns id
    const Event* get(EventId id) const;
    const std::vector<Event>& events() const { return events_; }
    void set_now(Tick t) { now_ = t; }
    Tick now() const { return now_; }

    // Ancestors of an event (breadth-first, bounded), for the causality viewer.
    std::vector<EventId> causes_of(EventId id, int max_depth = 8, int max_nodes = 64) const;
    // Direct consequences of an event.
    std::vector<EventId> effects_of(EventId id, size_t max = 64) const;

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;
    void clear() {
        events_.clear();
        next_id_ = 1;
    }

private:
    std::vector<Event> events_;
    EventId next_id_ = 1;
    Tick now_ = 0;
};

}  // namespace icarus
