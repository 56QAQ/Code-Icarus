// Godot binding for the simulation kernel. Thin: converts types and forwards calls.
#pragma once

#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "icarus/data/registry.h"
#include "icarus/render/mesher.h"
#include "icarus/sim/simulation.h"

namespace godot {

class IcarusSim : public RefCounted {
    GDCLASS(IcarusSim, RefCounted)

public:
    IcarusSim();
    ~IcarusSim() override;

    String kernel_version() const;
    bool load_rules(const Dictionary& files);  // name -> json text
    String last_error() const { return last_error_; }
    Dictionary rules_doc(const String& name) const;

    bool new_game(const Dictionary& config);
    bool has_game() const { return (bool)sim_; }
    void step(int ticks);
    int64_t get_tick() const;
    String time_string() const;
    Dictionary clock_info() const;
    Dictionary world_info() const;
    Dictionary world_stats() const;
    Dictionary perf() const;

    // Rendering.
    PackedInt32Array take_dirty_cells();
    PackedInt32Array render_cells() const;  // all cells that may contain matter
    Array material_table() const;           // by material id: key, name, colour, kind flags
    PackedInt32Array take_fx();             // [kind, x, y, z, material] per broken / landed cube
    PackedVector3Array fire_spots(int64_t max_count) const;  // burning cubes
    Array build_cell_mesh(const Vector3i& cell);
    Array build_lod_mesh(const Vector2i& column, int step);
    Dictionary raycast(const Vector3& origin, const Vector3& dir, double max_dist) const;
    Dictionary cube_info(const Vector3i& cube) const;
    Array debris_list() const;
    Array debris_mesh(int64_t id) const;
    Array meteors() const;

    // Characters.
    Array characters() const;               // light per-frame snapshot
    // Wild animals near a point (per frame), their species' looks, one animal's card.
    Array animals(const Vector3& center, double radius) const;
    Array animal_species() const;
    Dictionary animal_info(int64_t id) const;
    Array character_body(int64_t id) const; // 6 parts: {mesh, pivot}
    Dictionary character_info(int64_t id) const;
    Dictionary polity_info(int64_t id) const;
    Array tech_tree(int64_t polity) const;  // every tech with its state for the polity
    Dictionary round_state() const;         // {unified, event, text, time, polity}
    Array polities() const;
    Array piles() const;
    Dictionary building_at(const Vector3i& cube) const;
    Array buildings() const;

    // Commands.
    void admin(const String& type, const Dictionary& params);

    // Decisions (Jev pipeline).
    Array decisions(int64_t after_id, int64_t max_count) const;  // summaries, newest last
    Dictionary decision(int64_t id) const;                       // full record
    int64_t last_decision_id() const;
    String decision_mode() const;
    void set_decision_mode(const String& mode, int64_t budget_per_day, int64_t deadline_ticks);
    PackedInt32Array awaiting_remote() const;
    Dictionary remote_request(int64_t id) const;                 // {system, user, schema (json text)}
    Dictionary submit_decision(int64_t id, const String& key, const String& rationale, const String& source);
    void remote_failed(int64_t id, const String& why);
    String export_decision_log() const;
    bool load_decision_replay(const String& json_text);

    // Chronicle.
    int64_t last_event_id() const;
    Array events_since(int64_t after_id, int64_t max_count) const;
    Dictionary event(int64_t id) const;
    PackedInt32Array event_causes(int64_t id) const;
    PackedInt32Array event_effects(int64_t id) const;
    // Newest-first events filtered by category ("" = all) and minimum severity.
    Array recent_events(const String& category, int64_t min_severity, int64_t max_count, int64_t polity) const;
    static String event_category(int type);

    // Persistence.
    PackedByteArray save_bytes() const;
    bool load_bytes(const PackedByteArray& data);
    int64_t state_hash() const;

protected:
    static void _bind_methods();

private:
    icarus::ForestStats forest() const;
    mutable icarus::ForestStats forest_;
    mutable int64_t forest_tick_ = -1;
    Dictionary event_to_dict(const icarus::Event& e) const;
    Dictionary decision_summary(const icarus::Decision& d) const;

    std::unique_ptr<icarus::Registry> reg_;
    std::unique_ptr<icarus::Simulation> sim_;
    std::unique_ptr<icarus::Mesher> mesher_;
    icarus::CellMesh scratch_;
    String last_error_;
};

}  // namespace godot
