#include "icarus/sim/chronicle.h"

#include <algorithm>
#include <deque>
#include <set>

namespace icarus {

const char* event_type_key(EventType t) {
    static const char* names[] = {"none", "admin_action", "meteor_impact", "fire_started", "fire_spread", "flood",
                                  "collapse", "debris_landed", "structure_damaged", "structure_destroyed",
                                  "water_source_lost", "path_blocked", "logistics_disrupted", "shortage",
                                  "shortage_resolved", "starvation", "injury", "death", "crop_failure", "harvest",
                                  "decision_requested", "decision_made", "policy_changed", "project_started",
                                  "project_completed", "project_abandoned", "support_shift", "protest", "punishment",
                                  "refusal", "rebellion", "secession", "coup", "ruler_changed", "war_declared",
                                  "battle", "peace", "tech_discovered", "level_up", "spell_cast", "migration", "theft",
                                  "rescue", "construction", "info", "unification", "trade", "hunt", "life", "awakening"};
    static_assert(sizeof(names) / sizeof(names[0]) == (size_t)EventType::Count, "event names out of sync");
    size_t i = (size_t)t;
    return i < (size_t)EventType::Count ? names[i] : "unknown";
}

EventId Chronicle::emit(Event e) {
    e.id = next_id_++;
    e.tick = now_;
    events_.push_back(std::move(e));
    return events_.back().id;
}

const Event* Chronicle::get(EventId id) const {
    if (id == 0 || events_.empty()) return nullptr;
    // ids are dense and increasing; events are never removed.
    size_t idx = id - events_.front().id;
    if (idx < events_.size() && events_[idx].id == id) return &events_[idx];
    auto it = std::lower_bound(events_.begin(), events_.end(), id,
                               [](const Event& e, EventId v) { return e.id < v; });
    return (it != events_.end() && it->id == id) ? &*it : nullptr;
}

std::vector<EventId> Chronicle::causes_of(EventId id, int max_depth, int max_nodes) const {
    std::vector<EventId> out;
    std::set<EventId> seen{id};
    std::deque<std::pair<EventId, int>> q{{id, 0}};
    while (!q.empty() && (int)out.size() < max_nodes) {
        auto [cur, d] = q.front();
        q.pop_front();
        const Event* e = get(cur);
        if (!e || d >= max_depth) continue;
        for (EventId c : e->causes) {
            if (c == 0 || seen.count(c)) continue;
            seen.insert(c);
            out.push_back(c);
            q.push_back({c, d + 1});
        }
    }
    return out;
}

std::vector<EventId> Chronicle::effects_of(EventId id, size_t max) const {
    std::vector<EventId> out;
    const Event* e = get(id);
    if (!e) return out;
    size_t start = (size_t)(e - events_.data());
    for (size_t i = start + 1; i < events_.size() && out.size() < max; ++i)
        for (EventId c : events_[i].causes)
            if (c == id) {
                out.push_back(events_[i].id);
                break;
            }
    return out;
}

void Chronicle::save(BinWriter& w) const {
    size_t s = w.begin_section("CHRN");
    w.u32v(next_id_);
    w.u64v(now_);
    w.varu(events_.size());
    for (const Event& e : events_) {
        w.u32v(e.id);
        w.u64v(e.tick);
        w.u16v((u16)e.type);
        w.u8v(e.severity);
        w.vec3i(e.pos);
        w.u32v(e.actor);
        w.u32v(e.target);
        w.u16v(e.polity);
        for (EventId c : e.causes) w.u32v(c);
        w.str(e.text);
        w.str(e.data.is_null() ? std::string() : e.data.dump());
    }
    w.end_section(s);
}

void Chronicle::load(BinReader& outer) {
    BinReader r = outer.section("CHRN");
    next_id_ = r.u32v();
    now_ = r.u64v();
    u64 n = r.varu();
    events_.clear();
    events_.reserve((size_t)n);
    for (u64 i = 0; i < n; ++i) {
        Event e;
        e.id = r.u32v();
        e.tick = r.u64v();
        e.type = (EventType)r.u16v();
        e.severity = r.u8v();
        e.pos = r.vec3i();
        e.actor = r.u32v();
        e.target = r.u32v();
        e.polity = r.u16v();
        for (EventId& c : e.causes) c = r.u32v();
        e.text = r.str();
        std::string d = r.str();
        if (!d.empty()) e.data = Json::parse(d);
        events_.push_back(std::move(e));
    }
}

u64 Chronicle::hash() const {
    u64 h = 1469598103934665603ull;
    for (const Event& e : events_) {
        h = fnv1a64(&e.id, sizeof e.id, h);
        h = fnv1a64(&e.tick, sizeof e.tick, h);
        u16 t = (u16)e.type;
        h = fnv1a64(&t, sizeof t, h);
        h = fnv1a64(e.text.data(), e.text.size(), h);
    }
    return h;
}

}  // namespace icarus
