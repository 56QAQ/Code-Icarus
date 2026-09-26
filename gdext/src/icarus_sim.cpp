#include "icarus_sim.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "convert.h"
#include "icarus/util/log.h"
#include "icarus/world/query.h"

using namespace godot;

IcarusSim::IcarusSim() {
    icarus::set_log_sink([](icarus::LogLevel lvl, const std::string& msg) {
        String s = to_gd("[icarus] " + msg);
        if (lvl >= icarus::LogLevel::Error) UtilityFunctions::push_error(s);
        else if (lvl >= icarus::LogLevel::Warn) UtilityFunctions::push_warning(s);
        else UtilityFunctions::print(s);
    });
}
IcarusSim::~IcarusSim() = default;

void IcarusSim::_bind_methods() {
    ClassDB::bind_method(D_METHOD("kernel_version"), &IcarusSim::kernel_version);
    ClassDB::bind_method(D_METHOD("load_rules", "files"), &IcarusSim::load_rules);
    ClassDB::bind_method(D_METHOD("last_error"), &IcarusSim::last_error);
    ClassDB::bind_method(D_METHOD("rules_doc", "name"), &IcarusSim::rules_doc);
    ClassDB::bind_method(D_METHOD("new_game", "config"), &IcarusSim::new_game);
    ClassDB::bind_method(D_METHOD("has_game"), &IcarusSim::has_game);
    ClassDB::bind_method(D_METHOD("step", "ticks"), &IcarusSim::step);
    ClassDB::bind_method(D_METHOD("get_tick"), &IcarusSim::get_tick);
    ClassDB::bind_method(D_METHOD("time_string"), &IcarusSim::time_string);
    ClassDB::bind_method(D_METHOD("clock_info"), &IcarusSim::clock_info);
    ClassDB::bind_method(D_METHOD("world_info"), &IcarusSim::world_info);
    ClassDB::bind_method(D_METHOD("world_stats"), &IcarusSim::world_stats);
    ClassDB::bind_method(D_METHOD("perf"), &IcarusSim::perf);
    ClassDB::bind_method(D_METHOD("take_dirty_cells"), &IcarusSim::take_dirty_cells);
    ClassDB::bind_method(D_METHOD("render_cells"), &IcarusSim::render_cells);
    ClassDB::bind_method(D_METHOD("build_cell_mesh", "cell"), &IcarusSim::build_cell_mesh);
    ClassDB::bind_method(D_METHOD("raycast", "origin", "dir", "max_dist"), &IcarusSim::raycast);
    ClassDB::bind_method(D_METHOD("cube_info", "cube"), &IcarusSim::cube_info);
    ClassDB::bind_method(D_METHOD("debris_list"), &IcarusSim::debris_list);
    ClassDB::bind_method(D_METHOD("debris_mesh", "id"), &IcarusSim::debris_mesh);
    ClassDB::bind_method(D_METHOD("meteors"), &IcarusSim::meteors);
    ClassDB::bind_method(D_METHOD("admin", "type", "params"), &IcarusSim::admin);
    ClassDB::bind_method(D_METHOD("last_event_id"), &IcarusSim::last_event_id);
    ClassDB::bind_method(D_METHOD("events_since", "after_id", "max_count"), &IcarusSim::events_since);
    ClassDB::bind_method(D_METHOD("event", "id"), &IcarusSim::event);
    ClassDB::bind_method(D_METHOD("event_causes", "id"), &IcarusSim::event_causes);
    ClassDB::bind_method(D_METHOD("event_effects", "id"), &IcarusSim::event_effects);
    ClassDB::bind_method(D_METHOD("save_bytes"), &IcarusSim::save_bytes);
    ClassDB::bind_method(D_METHOD("load_bytes", "data"), &IcarusSim::load_bytes);
    ClassDB::bind_method(D_METHOD("state_hash"), &IcarusSim::state_hash);
}

String IcarusSim::kernel_version() const { return "icarus-kernel 0.2"; }

bool IcarusSim::load_rules(const Dictionary& files) {
    std::map<std::string, std::string> m;
    Array keys = files.keys();
    for (int i = 0; i < keys.size(); ++i) {
        String k = keys[i];
        String v = files[k];
        m[to_std(k)] = to_std(v);
    }
    try {
        auto reg = std::make_unique<icarus::Registry>();
        reg->load(m);
        sim_.reset();
        mesher_.reset();
        reg_ = std::move(reg);
        return true;
    } catch (const std::exception& e) {
        last_error_ = String::utf8(e.what());
        return false;
    }
}

Dictionary IcarusSim::rules_doc(const String& name) const {
    if (!reg_) return Dictionary();
    return json_to_variant(reg_->doc(to_std(name)));
}

bool IcarusSim::new_game(const Dictionary& config) {
    if (!reg_) {
        last_error_ = "rules not loaded";
        return false;
    }
    try {
        icarus::Json j = variant_to_json(config);
        icarus::GameConfig cfg;
        cfg.world.seed = (uint64_t)j.num("seed", 1);
        cfg.world.island_radius = j.flt("island_radius", cfg.world.island_radius);
        cfg.scenario = j.str("scenario", cfg.scenario);
        cfg.residents = j.integer("residents", cfg.residents);
        for (const auto& d : j["girl_drives"].items()) cfg.girl_drives.push_back(d.as_str());
        sim_ = std::make_unique<icarus::Simulation>(*reg_);
        sim_->new_game(cfg);
        mesher_ = std::make_unique<icarus::Mesher>(sim_->world());
        return true;
    } catch (const std::exception& e) {
        last_error_ = String::utf8(e.what());
        sim_.reset();
        return false;
    }
}

void IcarusSim::step(int ticks) {
    if (!sim_) return;
    for (int i = 0; i < ticks; ++i) sim_->step();
}

int64_t IcarusSim::get_tick() const { return sim_ ? (int64_t)sim_->now() : 0; }

String IcarusSim::time_string() const { return sim_ ? to_gd(icarus::format_time_zh(sim_->now())) : String(); }

Dictionary IcarusSim::clock_info() const {
    Dictionary d;
    if (!sim_) return d;
    icarus::Tick t = sim_->now();
    d["tick"] = (int64_t)t;
    d["day"] = icarus::day_of(t);
    d["hour"] = icarus::hour_of(t);
    d["season"] = icarus::season_of(t);
    d["year"] = icarus::year_of(t);
    d["night"] = icarus::is_night(t);
    d["ticks_per_day"] = (int64_t)icarus::kTicksPerDay;
    d["ticks_per_second"] = (int64_t)icarus::kTicksPerSecond;
    d["text"] = to_gd(icarus::format_time_zh(t));
    return d;
}

Dictionary IcarusSim::world_info() const {
    Dictionary d;
    if (!sim_) return d;
    const icarus::World& w = sim_->world();
    d["size"] = Vector3i(w.size_x(), w.size_y(), w.size_z());
    d["cells"] = Vector3i(w.cells_x(), w.cells_y(), w.cells_z());
    d["cell_size"] = icarus::kCellSize;
    const icarus::IslandFeatures& f = w.gen().features();
    Dictionary feat;
    feat["village"] = to_gd(f.village);
    feat["farms"] = to_gd(f.farms);
    feat["lake"] = to_gd(f.lake);
    feat["pond"] = to_gd(f.pond);
    feat["spring"] = to_gd(f.spring);
    feat["mountain"] = to_gd(f.mountain);
    feat["bridge_a"] = to_gd(f.bridge_a);
    feat["bridge_b"] = to_gd(f.bridge_b);
    feat["ravine_end"] = to_gd(f.ravine_end);
    d["features"] = feat;
    Array islands;
    for (const auto& is : w.gen().islands()) {
        Dictionary id;
        id["center"] = Vector3(is.cx, is.base_h, is.cz);
        id["radius"] = is.radius;
        id["main"] = is.main;
        islands.push_back(id);
    }
    d["islands"] = islands;
    d["seed"] = (int64_t)w.config().seed;
    return d;
}

Dictionary IcarusSim::world_stats() const {
    Dictionary d;
    if (!sim_) return d;
    icarus::WorldStats s = sim_->world().stats();
    d["ungenerated"] = s.ungenerated;
    d["dormant"] = s.dormant;
    d["active"] = s.active;
    d["bytes"] = (int64_t)s.bytes;
    const icarus::PhysicsStats& p = sim_->physics().stats();
    d["water_active"] = (int64_t)p.water_active;
    d["fire_active"] = (int64_t)p.fire_active;
    d["debris"] = (int64_t)p.debris;
    return d;
}

Dictionary IcarusSim::perf() const {
    Dictionary d;
    if (!sim_) return d;
    d["physics_us"] = sim_->profile().physics_us;
    d["total_us"] = sim_->profile().total_us;
    return d;
}

PackedInt32Array IcarusSim::take_dirty_cells() {
    PackedInt32Array out;
    if (!sim_) return out;
    for (const icarus::Vec3i& c : sim_->world().take_dirty_cells()) {
        out.push_back(c.x);
        out.push_back(c.y);
        out.push_back(c.z);
    }
    return out;
}

PackedInt32Array IcarusSim::render_cells() const {
    PackedInt32Array out;
    if (!sim_) return out;
    const icarus::World& w = sim_->world();
    for (int y = 0; y < w.cells_y(); ++y)
        for (int z = 0; z < w.cells_z(); ++z)
            for (int x = 0; x < w.cells_x(); ++x) {
                icarus::Vec3i c{x, y, z};
                const icarus::Cell* cell = w.cell(c);
                bool maybe = w.gen().cell_maybe_nonempty(c) || (cell && !cell->pristine);
                if (!maybe) continue;
                out.push_back(x);
                out.push_back(y);
                out.push_back(z);
            }
    return out;
}

Array IcarusSim::build_cell_mesh(const Vector3i& cell) {
    Array out;
    if (!sim_ || !mesher_) return out;
    mesher_->build_cell(to_ic(cell), scratch_);
    out.push_back(mesh_to_arrays(scratch_.opaque));
    out.push_back(mesh_to_arrays(scratch_.water));
    out.push_back(mesh_to_arrays(scratch_.foliage));
    return out;
}

Dictionary IcarusSim::raycast(const Vector3& origin, const Vector3& dir, double max_dist) const {
    Dictionary d;
    if (!sim_) return d;
    icarus::RayHit h = icarus::raycast(sim_->world(), to_ic(origin), to_ic(dir), (float)max_dist, true);
    d["hit"] = h.hit;
    if (h.hit) {
        d["cube"] = to_gd(h.cube);
        d["normal"] = to_gd(h.normal);
        d["distance"] = h.distance;
        const icarus::Material& m = reg_->mat(icarus::vmat(h.voxel));
        d["material"] = to_gd(m.key);
        d["material_name"] = to_gd(m.name);
    }
    return d;
}

Dictionary IcarusSim::cube_info(const Vector3i& cube) const {
    Dictionary d;
    if (!sim_) return d;
    const icarus::World& w = sim_->world();
    icarus::Vec3i p = to_ic(cube);
    icarus::Voxel v = w.peek(p);
    const icarus::Material& m = reg_->mat(icarus::vmat(v));
    d["material"] = to_gd(m.key);
    d["material_name"] = to_gd(m.name);
    d["level"] = icarus::vlevel(v);
    d["damage"] = icarus::vdamage(v);
    d["burning"] = icarus::vburning(v);
    icarus::Vec3i cc = icarus::cell_of(p);
    const icarus::Cell* c = w.cell(cc);
    if (c) {
        Dictionary cd;
        cd["coord"] = to_gd(cc);
        static const char* states[] = {"ungenerated", "dormant", "active"};
        cd["state"] = states[(int)c->state];
        cd["pristine"] = c->pristine;
        cd["biome"] = to_gd(icarus::biome_name_zh(c->biome));
        cd["owner"] = c->owner;
        cd["last_touch"] = (int64_t)c->last_touch_tick;
        d["cell"] = cd;
    }
    return d;
}

Array IcarusSim::debris_list() const {
    Array out;
    if (!sim_) return out;
    for (const icarus::DebrisBody& b : sim_->physics().debris()) {
        Dictionary d;
        d["id"] = (int64_t)b.id;
        d["pos"] = to_gd(b.pos);
        d["count"] = (int64_t)b.voxels.size();
        out.push_back(d);
    }
    return out;
}

Array IcarusSim::debris_mesh(int64_t id) const {
    if (!sim_) return Array();
    for (const icarus::DebrisBody& b : sim_->physics().debris()) {
        if ((int64_t)b.id != id) continue;
        std::vector<std::pair<icarus::Vec3i, icarus::Voxel>> cubes;
        cubes.reserve(b.voxels.size());
        for (const auto& dv : b.voxels) cubes.push_back({dv.off, dv.v});
        icarus::MeshData m;
        icarus::build_debris_mesh(*reg_, cubes, m);
        return mesh_to_arrays(m);
    }
    return Array();
}

Array IcarusSim::meteors() const {
    Array out;
    if (!sim_) return out;
    for (const icarus::Meteor& m : sim_->physics().meteors()) {
        Dictionary d;
        d["id"] = (int64_t)m.id;
        d["pos"] = to_gd(m.pos);
        d["vel"] = to_gd(m.vel);
        d["radius"] = m.radius;
        out.push_back(d);
    }
    return out;
}

void IcarusSim::admin(const String& type, const Dictionary& params) {
    if (!sim_) return;
    icarus::AdminCommand c;
    c.type = to_std(type);
    c.params = variant_to_json(params);
    sim_->queue_admin(std::move(c));
}

int64_t IcarusSim::last_event_id() const {
    if (!sim_ || sim_->chronicle().events().empty()) return 0;
    return sim_->chronicle().events().back().id;
}

Dictionary IcarusSim::event_to_dict(const icarus::Event& e) const {
    Dictionary d;
    d["id"] = (int64_t)e.id;
    d["tick"] = (int64_t)e.tick;
    d["type"] = to_gd(icarus::event_type_key(e.type));
    d["severity"] = e.severity;
    d["pos"] = to_gd(e.pos);
    d["actor"] = (int64_t)e.actor;
    d["target"] = (int64_t)e.target;
    d["polity"] = e.polity;
    PackedInt32Array causes;
    for (icarus::EventId c : e.causes)
        if (c) causes.push_back((int32_t)c);
    d["causes"] = causes;
    d["text"] = to_gd(e.text);
    d["time"] = to_gd(icarus::format_time_zh(e.tick));
    if (!e.data.is_null()) d["data"] = json_to_variant(e.data);
    return d;
}

Array IcarusSim::events_since(int64_t after_id, int64_t max_count) const {
    Array out;
    if (!sim_) return out;
    const auto& ev = sim_->chronicle().events();
    // Events have dense ids; find the start quickly.
    size_t start = 0;
    if (!ev.empty() && after_id >= (int64_t)ev.front().id) start = (size_t)(after_id - ev.front().id + 1);
    for (size_t i = start; i < ev.size() && (int64_t)out.size() < max_count; ++i) out.push_back(event_to_dict(ev[i]));
    return out;
}

Dictionary IcarusSim::event(int64_t id) const {
    if (!sim_) return Dictionary();
    const icarus::Event* e = sim_->chronicle().get((icarus::EventId)id);
    return e ? event_to_dict(*e) : Dictionary();
}

PackedInt32Array IcarusSim::event_causes(int64_t id) const {
    PackedInt32Array out;
    if (!sim_) return out;
    for (icarus::EventId c : sim_->chronicle().causes_of((icarus::EventId)id)) out.push_back((int32_t)c);
    return out;
}

PackedInt32Array IcarusSim::event_effects(int64_t id) const {
    PackedInt32Array out;
    if (!sim_) return out;
    for (icarus::EventId c : sim_->chronicle().effects_of((icarus::EventId)id)) out.push_back((int32_t)c);
    return out;
}

PackedByteArray IcarusSim::save_bytes() const {
    PackedByteArray out;
    if (!sim_) return out;
    std::vector<uint8_t> blob = sim_->save();
    out.resize((int64_t)blob.size());
    std::memcpy(out.ptrw(), blob.data(), blob.size());
    return out;
}

bool IcarusSim::load_bytes(const PackedByteArray& data) {
    if (!reg_) {
        last_error_ = "rules not loaded";
        return false;
    }
    try {
        std::vector<uint8_t> blob(data.ptr(), data.ptr() + data.size());
        auto sim = std::make_unique<icarus::Simulation>(*reg_);
        sim->load(blob);
        sim_ = std::move(sim);
        mesher_ = std::make_unique<icarus::Mesher>(sim_->world());
        return true;
    } catch (const std::exception& e) {
        last_error_ = String::utf8(e.what());
        return false;
    }
}

int64_t IcarusSim::state_hash() const { return sim_ ? (int64_t)sim_->state_hash() : 0; }
