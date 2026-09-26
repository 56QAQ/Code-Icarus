// Godot binding for the simulation kernel. Thin: converts types and forwards calls.
#pragma once

#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
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
    Array build_cell_mesh(const Vector3i& cell);
    Dictionary raycast(const Vector3& origin, const Vector3& dir, double max_dist) const;
    Dictionary cube_info(const Vector3i& cube) const;
    Array debris_list() const;
    Array debris_mesh(int64_t id) const;
    Array meteors() const;

    // Characters.
    Array characters() const;               // light per-frame snapshot
    Array character_body(int64_t id) const; // 6 parts: {mesh, pivot}
    Dictionary character_info(int64_t id) const;
    Dictionary polity_info(int64_t id) const;
    Array polities() const;
    Array piles() const;
    Dictionary building_at(const Vector3i& cube) const;
    Array buildings() const;

    // Commands.
    void admin(const String& type, const Dictionary& params);

    // Chronicle.
    int64_t last_event_id() const;
    Array events_since(int64_t after_id, int64_t max_count) const;
    Dictionary event(int64_t id) const;
    PackedInt32Array event_causes(int64_t id) const;
    PackedInt32Array event_effects(int64_t id) const;

    // Persistence.
    PackedByteArray save_bytes() const;
    bool load_bytes(const PackedByteArray& data);
    int64_t state_hash() const;

protected:
    static void _bind_methods();

private:
    Dictionary event_to_dict(const icarus::Event& e) const;

    std::unique_ptr<icarus::Registry> reg_;
    std::unique_ptr<icarus::Simulation> sim_;
    std::unique_ptr<icarus::Mesher> mesher_;
    icarus::CellMesh scratch_;
    String last_error_;
};

}  // namespace godot
