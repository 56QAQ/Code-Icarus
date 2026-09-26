#include "icarus_sim.h"

#include <map>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

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
    ClassDB::bind_method(D_METHOD("characters"), &IcarusSim::characters);
    ClassDB::bind_method(D_METHOD("character_body", "id"), &IcarusSim::character_body);
    ClassDB::bind_method(D_METHOD("character_info", "id"), &IcarusSim::character_info);
    ClassDB::bind_method(D_METHOD("polity_info", "id"), &IcarusSim::polity_info);
    ClassDB::bind_method(D_METHOD("tech_tree", "polity"), &IcarusSim::tech_tree);
    ClassDB::bind_method(D_METHOD("round_state"), &IcarusSim::round_state);
    ClassDB::bind_method(D_METHOD("polities"), &IcarusSim::polities);
    ClassDB::bind_method(D_METHOD("piles"), &IcarusSim::piles);
    ClassDB::bind_method(D_METHOD("building_at", "cube"), &IcarusSim::building_at);
    ClassDB::bind_method(D_METHOD("buildings"), &IcarusSim::buildings);
    ClassDB::bind_method(D_METHOD("last_event_id"), &IcarusSim::last_event_id);
    ClassDB::bind_method(D_METHOD("events_since", "after_id", "max_count"), &IcarusSim::events_since);
    ClassDB::bind_method(D_METHOD("event", "id"), &IcarusSim::event);
    ClassDB::bind_method(D_METHOD("event_causes", "id"), &IcarusSim::event_causes);
    ClassDB::bind_method(D_METHOD("event_effects", "id"), &IcarusSim::event_effects);
    ClassDB::bind_method(D_METHOD("recent_events", "category", "min_severity", "max_count", "polity"), &IcarusSim::recent_events);
    ClassDB::bind_method(D_METHOD("save_bytes"), &IcarusSim::save_bytes);
    ClassDB::bind_method(D_METHOD("load_bytes", "data"), &IcarusSim::load_bytes);
    ClassDB::bind_method(D_METHOD("state_hash"), &IcarusSim::state_hash);
    ClassDB::bind_method(D_METHOD("decisions", "after_id", "max_count"), &IcarusSim::decisions);
    ClassDB::bind_method(D_METHOD("decision", "id"), &IcarusSim::decision);
    ClassDB::bind_method(D_METHOD("last_decision_id"), &IcarusSim::last_decision_id);
    ClassDB::bind_method(D_METHOD("decision_mode"), &IcarusSim::decision_mode);
    ClassDB::bind_method(D_METHOD("set_decision_mode", "mode", "budget_per_day", "deadline_ticks"),
                         &IcarusSim::set_decision_mode);
    ClassDB::bind_method(D_METHOD("awaiting_remote"), &IcarusSim::awaiting_remote);
    ClassDB::bind_method(D_METHOD("remote_request", "id"), &IcarusSim::remote_request);
    ClassDB::bind_method(D_METHOD("submit_decision", "id", "key", "rationale", "source"), &IcarusSim::submit_decision);
    ClassDB::bind_method(D_METHOD("remote_failed", "id", "why"), &IcarusSim::remote_failed);
    ClassDB::bind_method(D_METHOD("export_decision_log"), &IcarusSim::export_decision_log);
    ClassDB::bind_method(D_METHOD("load_decision_replay", "json_text"), &IcarusSim::load_decision_replay);
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
        forest_tick_ = -1;
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
    d["category"] = event_category((int)e.type);
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

String IcarusSim::event_category(int type) {
    using T = icarus::EventType;
    switch ((T)type) {
        case T::DecisionRequested: case T::DecisionMade: case T::PolicyChanged: case T::Secession: case T::Coup:
        case T::RulerChanged: case T::Protest: case T::Punishment: case T::Refusal: case T::Rebellion:
        case T::SupportShift: case T::LevelUp: case T::WarDeclared: case T::Battle: case T::Peace:
        case T::Unification:
            return "politics";
        case T::MeteorImpact: case T::FireStarted: case T::FireSpread: case T::Flood: case T::Collapse:
        case T::DebrisLanded: case T::StructureDamaged: case T::StructureDestroyed: case T::WaterSourceLost:
        case T::PathBlocked: case T::LogisticsDisrupted: case T::CropFailure:
            return "disaster";
        case T::Shortage: case T::ShortageResolved: case T::Harvest: case T::ProjectStarted: case T::ProjectCompleted:
        case T::ProjectAbandoned: case T::Construction: case T::Theft: case T::TechDiscovered:
            return "economy";
        case T::Death: case T::Injury: case T::Starvation: case T::Rescue: case T::SpellCast: case T::Migration:
            return "life";
        case T::AdminAction:
            return "admin";
        default:
            return "other";
    }
}

Array IcarusSim::recent_events(const String& category, int64_t min_severity, int64_t max_count, int64_t polity) const {
    Array out;
    if (!sim_) return out;
    const auto& ev = sim_->chronicle().events();
    for (size_t i = ev.size(); i-- > 0 && out.size() < max_count;) {
        const icarus::Event& e = ev[i];
        if (e.severity < min_severity) continue;
        if (polity > 0 && e.polity != 0 && e.polity != polity) continue;
        String cat = event_category((int)e.type);
        if (!category.is_empty() && cat != category) continue;
        Dictionary d = event_to_dict(e);
        d["category"] = cat;
        out.push_back(d);
    }
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
        forest_tick_ = -1;
        sim_ = std::move(sim);
        mesher_ = std::make_unique<icarus::Mesher>(sim_->world());
        return true;
    } catch (const std::exception& e) {
        last_error_ = String::utf8(e.what());
        return false;
    }
}

int64_t IcarusSim::state_hash() const { return sim_ ? (int64_t)sim_->state_hash() : 0; }

// ---------------------------------------------------------------------------------- characters

namespace {
String drive_name(const icarus::Registry& reg, const std::string& key) {
    for (const icarus::Json& d : reg.doc("drives")["drives"].items())
        if (d.str("key") == key) return to_gd(d.str("name", key));
    return to_gd(key);
}
const icarus::Json* drive_doc(const icarus::Registry& reg, const std::string& key) {
    for (const icarus::Json& d : reg.doc("drives")["drives"].items())
        if (d.str("key") == key) return &d;
    return nullptr;
}
Color col(uint32_t c) { return Color(((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0); }
}  // namespace

Array IcarusSim::characters() const {
    Array out;
    if (!sim_) return out;
    const icarus::Tick now = sim_->now();
    for (const auto& cp : sim_->agents().all()) {
        if (!cp) continue;
        const icarus::Character& c = *cp;
        if (c.departed) continue;
        if (!c.alive && now - c.died > icarus::kTicksPerDay) continue;
        Dictionary d;
        d["id"] = (int64_t)c.id;
        d["name"] = to_gd(c.name);
        d["pos"] = to_gd(c.pos);
        d["yaw"] = c.yaw;
        d["moving"] = c.moving;
        d["phase"] = c.walk_phase;
        d["sleeping"] = c.sleeping;
        d["alive"] = c.alive;
        d["girl"] = c.is_girl();
        d["body_version"] = (int64_t)c.body.version;
        d["polity"] = c.polity;
        d["task"] = to_gd(icarus::task_name_zh(c.task.type));
        d["status"] = to_gd(c.status_text);
        const icarus::Store* inv = sim_->economy().store(c.inv);
        // Cargo, not the tools and gear they always have on them.
        bool cargo = false;
        if (inv)
            for (const auto& st : inv->items) {
                const int kept = (st.item == c.tool || st.item == c.weapon || st.item == c.armor || st.item == c.cart) ? 1 : 0;
                if (st.count > kept) cargo = true;
            }
        d["carrying"] = cargo;
        d["working"] = c.task.type == icarus::TaskType::Work && c.task.until > now && !c.moving;
        d["protest"] = c.task.type == icarus::TaskType::Protest && c.task.step == 2;
        if (c.is_girl()) d["drive"] = drive_name(*reg_, c.girl->drive);
        d["drafted"] = c.drafted;
        if (const icarus::Polity* cp_pol = sim_->society().polity(c.polity)) d["pcolor"] = col(cp_pol->color);
        d["weapon"] = c.weapon != icarus::kNoItem ? to_gd(reg_->item(c.weapon).key) : String();
        d["armor"] = c.armor != icarus::kNoItem ? to_gd(reg_->item(c.armor).key) : String();
        d["cart"] = c.cart != icarus::kNoItem;
        d["fighting"] = c.task.type == icarus::TaskType::Fight && !c.moving && c.alive;
        d["casting"] = c.task.type == icarus::TaskType::Cast && !c.moving && c.alive;
        out.push_back(d);
    }
    return out;
}

Array IcarusSim::character_body(int64_t id) const {
    Array out;
    if (!sim_) return out;
    const icarus::Character* c = sim_->agents().get((icarus::EntityId)id);
    if (!c) return out;
    std::vector<uint32_t> pal = c->look.palette();
    for (int p = 0; p < icarus::kPartCount; ++p) {
        const icarus::PartShape& s = icarus::part_shape(p);
        const icarus::BodyPart& bp = c->body.parts[p];
        icarus::MeshData m;
        icarus::build_voxel_model(bp.vox.data(), s.size.x, s.size.y, s.size.z, pal, icarus::kBodyScale, m);
        // Pivot: hips/shoulders at the top of limbs, neck at the bottom of the head.
        float px = (float)s.origin.x + (float)s.size.x * 0.5f;
        float pz = (float)s.origin.z + (float)s.size.z * 0.5f;
        float py = (p == icarus::kHead || p == icarus::kTorso) ? (float)s.origin.y : (float)(s.origin.y + s.size.y);
        // Offset vertices so the pivot is the mesh origin.
        float ox = ((float)s.origin.x - px) * icarus::kBodyScale;
        float oy = ((float)s.origin.y - py) * icarus::kBodyScale;
        float oz = ((float)s.origin.z - pz) * icarus::kBodyScale;
        for (size_t i = 0; i < m.positions.size(); i += 3) {
            m.positions[i] += ox;
            m.positions[i + 1] += oy;
            m.positions[i + 2] += oz;
        }
        Dictionary d;
        d["mesh"] = mesh_to_arrays(m);
        d["pivot"] = Vector3(px, py, pz) * icarus::kBodyScale;
        d["severed"] = bp.severed;
        out.push_back(d);
    }
    return out;
}

Dictionary IcarusSim::character_info(int64_t id) const {
    Dictionary d;
    if (!sim_) return d;
    const icarus::Character* cp = sim_->agents().get((icarus::EntityId)id);
    if (!cp) return d;
    const icarus::Character& c = *cp;
    const icarus::Tick now = sim_->now();
    d["id"] = (int64_t)c.id;
    d["name"] = to_gd(c.name);
    d["female"] = c.female;
    d["alive"] = c.alive;
    d["death_cause"] = to_gd(c.death_cause);
    d["girl"] = c.is_girl();
    d["polity"] = c.polity;
    d["polity_title"] = to_gd(sim_->society().title(c.polity));
    d["pos"] = to_gd(c.pos);
    d["task"] = to_gd(icarus::task_name_zh(c.task.type));
    d["status"] = to_gd(c.status_text);
    d["reason"] = to_gd(c.task.label);
    Dictionary needs;
    needs["food"] = c.needs.food;
    needs["water"] = c.needs.water;
    needs["rest"] = c.needs.rest;
    needs["social"] = c.needs.social;
    needs["safety"] = c.needs.safety;
    needs["comfort"] = c.needs.comfort;
    d["needs"] = needs;
    d["mood"] = c.mood;
    d["stress"] = c.stress;
    d["fear"] = c.fear;
    d["work_debt"] = c.work_debt;
    Array pers;
    for (int i = 0; i < icarus::Personality::kCount; ++i) {
        Dictionary t;
        t["name"] = to_gd(icarus::trait_name_zh(i));
        t["value"] = c.pers.at(i);
        pers.push_back(t);
    }
    d["personality"] = pers;
    Array skills;
    for (int s = 0; s < icarus::kSkillCount; ++s) {
        Dictionary t;
        t["name"] = to_gd(icarus::skill_name_zh(s));
        t["value"] = c.skills[s];
        skills.push_back(t);
    }
    d["skills"] = skills;
    d["drafted"] = c.drafted;
    Array equip;
    const std::pair<const char*, icarus::ItemId> slots[] = {
        {"工具", c.tool}, {"武器", c.weapon}, {"护甲", c.armor}, {"推车", c.cart}};
    for (const auto& [slot, item] : slots) {
        if (item == icarus::kNoItem) continue;
        Dictionary t;
        t["slot"] = String::utf8(slot);
        t["name"] = to_gd(reg_->item(item).name);
        equip.push_back(t);
    }
    d["equipment"] = equip;
    if (c.treated_until > sim_->now())
        d["treated_hours"] = (double)(c.treated_until - sim_->now()) / (double)icarus::kTicksPerHour;
    Array trace;
    for (const auto& o : c.trace) {
        Dictionary t;
        t["label"] = to_gd(o.label);
        t["score"] = o.score;
        t["why"] = to_gd(o.why);
        trace.push_back(t);
    }
    d["trace"] = trace;
    Array support;
    for (const auto& s : c.support) {
        const icarus::Character* g = sim_->agents().get(s.girl);
        if (!g) continue;
        Dictionary t;
        t["id"] = (int64_t)s.girl;
        t["name"] = to_gd(g->name);
        t["value"] = s.value;
        if (g->girl) t["drive"] = drive_name(*reg_, g->girl->drive);
        support.push_back(t);
    }
    d["support"] = support;
    // Closest relationships.
    std::vector<icarus::Relation> rel = c.relations;
    std::sort(rel.begin(), rel.end(), [](const auto& a, const auto& b) { return std::fabs(a.affinity) > std::fabs(b.affinity); });
    Array rels;
    for (size_t i = 0; i < rel.size() && i < 6; ++i) {
        const icarus::Character* o = sim_->agents().get(rel[i].other);
        if (!o) continue;
        Dictionary t;
        t["id"] = (int64_t)o->id;
        t["name"] = to_gd(o->name);
        t["value"] = rel[i].affinity;
        rels.push_back(t);
    }
    d["relations"] = rels;
    Array mems;
    for (auto it = c.memories.rbegin(); it != c.memories.rend() && mems.size() < 8; ++it) {
        Dictionary t;
        t["text"] = to_gd(icarus::memory_kind_zh(it->kind));
        t["valence"] = it->valence;
        t["time"] = to_gd(icarus::format_time_zh(it->tick));
        t["event"] = (int64_t)it->event;
        const icarus::Character* s = sim_->agents().get(it->subject);
        if (s) t["subject"] = to_gd(s->name);
        mems.push_back(t);
    }
    d["memories"] = mems;
    Array parts;
    for (int p = 0; p < icarus::kPartCount; ++p) {
        Dictionary t;
        t["name"] = to_gd(icarus::body_part_name_zh(p));
        t["integrity"] = c.body.part_integrity(p);
        t["severed"] = c.body.parts[p].severed;
        parts.push_back(t);
    }
    d["body"] = parts;
    d["vitality"] = c.body.vitality;
    d["bleeding"] = c.body.bleeding;
    Array inv;
    if (const icarus::Store* s = sim_->economy().store(c.inv))
        for (const auto& st : s->items) {
            Dictionary t;
            t["item"] = to_gd(reg_->item(st.item).name);
            t["count"] = st.count;
            inv.push_back(t);
        }
    d["inventory"] = inv;
    if (const icarus::Building* h = sim_->buildings().get(c.home)) d["home"] = to_gd(h->name);
    d["occupation"] = to_gd(c.occupation);
    d["age_days"] = (double)(now - c.born) / (double)icarus::kTicksPerDay;
    if (c.is_girl()) {
        const icarus::GirlData& g = *c.girl;
        Dictionary gd;
        gd["drive"] = drive_name(*reg_, g.drive);
        gd["drive_key"] = to_gd(g.drive);
        gd["title"] = String::utf8("象征") + drive_name(*reg_, g.drive) + String::utf8("的魔法少女，") + to_gd(c.name);
        gd["level"] = g.level;
        gd["xp"] = g.xp;
        gd["mana"] = g.mana;
        gd["role"] = to_gd(g.role);
        gd["domain"] = to_gd(g.domain);
        gd["loyalty"] = g.loyalty;
        gd["stance"] = to_gd(g.stance);
        gd["temperament"] = to_gd(g.temperament);
        gd["xp_next"] = 40.0 * (double)g.level;
        PackedInt32Array decs;
        for (uint32_t id : g.decisions) decs.push_back((int32_t)id);
        gd["decisions"] = decs;
        if (const icarus::Character* gr = sim_->agents().get(g.grudge)) gd["grudge"] = to_gd(gr->name);
        float sup = 0;
        int n = 0;
        for (const auto& rp : sim_->agents().all())
            if (rp && rp->alive && !rp->is_girl() && rp->polity == c.polity) {
                sup += rp->support_for(c.id);
                ++n;
            }
        gd["popular_support"] = n ? sup / (float)n : 0.0f;
        int wf = 0;
        float wdelta = 0;
        icarus::Tick wsince = 0;
        if (sim_->decisions().value_whisper(c.id, wf, wdelta, wsince)) {
            Dictionary w;
            w["feature"] = to_gd(icarus::feature_key(wf));
            w["name"] = to_gd(icarus::feature_name_zh(wf));
            w["dir"] = wdelta > 0 ? 1 : -1;
            w["hours_left"] = (double)(wsince + 2 * icarus::kTicksPerDay - now) / (double)icarus::kTicksPerHour;
            gd["whisper"] = w;
        }
        if (const icarus::Json* dd = drive_doc(*reg_, g.drive)) {
            gd["valence"] = dd->integer("valence", 1);
            gd["category"] = to_gd(dd->str("category"));
            Array spells;
            for (const icarus::Json& sp : (*dd)["spells"].items()) {
                Dictionary t;
                t["name"] = to_gd(sp.str("name"));
                t["type"] = to_gd(sp.str("type"));
                t["level"] = sp.integer("level", 1);
                t["unlocked"] = g.level >= sp.integer("level", 1);
                t["desc"] = to_gd(sp.str("desc"));
                spells.push_back(t);
            }
            gd["spells"] = spells;
        }
        d["girl_data"] = gd;
    }
    return d;
}

Array IcarusSim::polities() const {
    Array out;
    if (!sim_) return out;
    for (const auto& p : sim_->society().polities())
        if (p.alive) out.push_back(polity_info(p.id));
    return out;
}

Dictionary IcarusSim::polity_info(int64_t id) const {
    Dictionary d;
    if (!sim_) return d;
    const icarus::Polity* p = sim_->society().polity((uint16_t)id);
    if (!p) return d;
    d["id"] = p->id;
    d["name"] = to_gd(p->name);
    d["title"] = to_gd(sim_->society().title(p->id));
    d["color"] = col(p->color);
    d["ruler"] = (int64_t)p->ruler;
    if (const icarus::Character* r = sim_->agents().get(p->ruler)) d["ruler_name"] = to_gd(r->name);
    const icarus::PolityStats& s = p->stats;
    Dictionary st;
    st["population"] = s.population;
    st["girls"] = s.girls;
    st["food_stock"] = s.food_stock;
    st["food_days"] = s.food_days;
    st["food_access"] = s.food_access;
    st["water_access"] = s.water_access;
    st["mood"] = s.mood;
    st["ruler_support"] = s.ruler_support;
    st["stability"] = s.stability;
    st["knowledge"] = s.knowledge;
    st["protesters"] = s.protesters;
    st["deaths"] = s.deaths;
    d["stats"] = st;
    Dictionary pol;
    pol["ration"] = p->policies.ration;
    pol["punishment"] = p->policies.punishment;
    pol["work_hours"] = p->policies.work_hours;
    pol["requisition"] = p->policies.requisition;
    pol["distribution"] = p->policies.distribution;
    pol["wage"] = p->policies.wage;
    d["policies"] = pol;
    Array crises;
    for (const auto& c : p->crises) {
        if (!c.active) continue;
        Dictionary t;
        t["kind"] = to_gd(icarus::crisis_name_zh(c.kind));
        t["severity"] = c.severity;
        t["event"] = (int64_t)c.event;
        crises.push_back(t);
    }
    d["crises"] = crises;
    Array girls;
    for (const auto& cp : sim_->agents().all()) {
        if (!cp || !cp->alive || cp->departed || !cp->is_girl() || cp->polity != p->id) continue;
        Dictionary g;
        g["id"] = (int64_t)cp->id;
        g["name"] = to_gd(cp->name);
        g["drive"] = drive_name(*reg_, cp->girl->drive);
        g["role"] = to_gd(cp->girl->role);
        g["level"] = cp->girl->level;
        g["loyalty"] = cp->girl->loyalty;
        g["stance"] = to_gd(cp->girl->stance);
        float sup = 0;
        int n = 0;
        for (const auto& rp : sim_->agents().all())
            if (rp && rp->alive && !rp->is_girl() && rp->polity == p->id) {
                sup += rp->support_for(cp->id);
                ++n;
            }
        g["support"] = n ? sup / (float)n : 0.0f;
        girls.push_back(g);
    }
    d["girls"] = girls;
    // History (hourly), compact arrays for charts.
    PackedFloat32Array food, mood, support, pop;
    for (const auto& h : p->history) {
        food.push_back(h.food_days);
        mood.push_back(h.mood);
        support.push_back(h.ruler_support);
        pop.push_back((float)h.population);
    }
    Dictionary hist;
    hist["food_days"] = food;
    hist["mood"] = mood;
    hist["ruler_support"] = support;
    hist["population"] = pop;
    d["history"] = hist;
    Array reigns;
    for (const auto& r : p->reigns) {
        Dictionary t;
        t["ruler"] = to_gd(r.ruler_name);
        t["drive"] = drive_name(*reg_, r.drive);
        t["from"] = to_gd(icarus::format_time_zh(r.from));
        static const std::map<std::string, const char*> hows = {
            {"founding", "建国"}, {"succession", "继位"}, {"secession", "分裂自立"}, {"coup", "政变夺权"}};
        auto hw = hows.find(r.how);
        t["how"] = hw != hows.end() ? String::utf8(hw->second) : to_gd(r.how);
        reigns.push_back(t);
    }
    d["reigns"] = reigns;
    // Technology and war.
    const icarus::Society& soc = sim_->society();
    const int era = soc.era(*p);
    d["era"] = era;
    const icarus::Json& eras = reg_->doc("techs")["eras"];
    d["era_name"] = era < (int)eras.size() ? to_gd(eras[(size_t)era].as_str()) : String();
    d["techs_known"] = (int64_t)p->techs.size();
    Dictionary res;
    if (!p->policies.research.empty())
        if (const icarus::Json* t = soc.tech(p->policies.research)) {
            float prog = 0;
            for (const auto& r : p->research)
                if (r.first == p->policies.research) prog = r.second;
            res["key"] = to_gd(p->policies.research);
            res["name"] = to_gd(t->str("name"));
            res["progress"] = prog;
            res["cost"] = t->flt("cost", 100.0f);
        }
    d["research"] = res;
    d["soldiers"] = soc.soldiers(p->id);
    Array wars;
    for (const auto& w : p->wars) {
        Dictionary t;
        t["enemy"] = w.enemy;
        const icarus::Polity* e = soc.polity(w.enemy);
        t["enemy_name"] = e ? to_gd(e->name) : String("?");
        t["enemy_color"] = e ? col(e->color) : Color(0.5, 0.5, 0.5);
        t["attacker"] = w.attacker;
        t["aim"] = to_gd(w.aim);
        t["since"] = to_gd(icarus::format_time_zh(w.since));
        t["kills"] = w.kills;
        t["losses"] = w.losses;
        t["event"] = (int64_t)w.event;
        wars.push_back(t);
    }
    d["wars"] = wars;
    Dictionary op;
    op["active"] = p->op.active;
    if (p->op.active) {
        static const char* phases[] = {"集结", "行军", "交锋", "撤回"};
        op["aim"] = to_gd(p->op.aim);
        op["phase"] = p->op.phase;
        op["phase_name"] = String::utf8(phases[std::min<int>(p->op.phase, 3)]);
        const icarus::Polity* e = soc.polity(p->op.enemy);
        op["enemy_name"] = e ? to_gd(e->name) : String("?");
        op["party"] = p->op.party;
        op["lost"] = p->op.lost;
        op["objective"] = to_gd(p->op.objective);
        op["event"] = (int64_t)p->op.event;
    }
    d["op"] = op;
    // The measures a round is judged by: unification wins it, these say what it cost.
    Dictionary out;
    int peak = s.population;
    for (const auto& h : p->history) peak = std::max(peak, h.population);
    out["population"] = s.population;
    out["population_peak"] = peak;
    out["living"] = std::clamp(0.45f * s.food_access + 0.2f * s.water_access + 0.35f * s.mood, 0.0f, 1.0f);
    const int total_techs = (int)reg_->doc("techs")["techs"].size();
    out["knowledge"] = total_techs ? (float)p->techs.size() / (float)total_techs : 0.0f;
    const icarus::ForestStats f = forest();
    out["ecology"] = f.ratio();
    out["trees"] = f.standing;
    out["trees_initial"] = f.initial;
    out["stability"] = s.stability;
    d["outcomes"] = out;
    // Who left and who came in the past day (migration is how residents vote).
    int left = 0, came = 0;
    const auto& evs = sim_->chronicle().events();
    for (size_t i = evs.size(); i-- > 0;) {
        const icarus::Event& e = evs[i];
        if (e.tick + icarus::kTicksPerDay < sim_->now()) break;
        if (e.type != icarus::EventType::Migration) continue;
        if (e.polity == p->id) ++left;
        if (e.data.integer("to", 0) == (int)p->id) ++came;
    }
    d["emigrated_day"] = left;
    d["immigrated_day"] = came;
    return d;
}

icarus::ForestStats IcarusSim::forest() const {
    // The tally is cheap but not free; the island's forest changes slowly.
    if (sim_ && (forest_tick_ < 0 || (int64_t)sim_->now() - forest_tick_ >= icarus::kTicksPerHour)) {
        forest_ = sim_->world().forest();
        forest_tick_ = (int64_t)sim_->now();
    }
    return forest_;
}

Dictionary IcarusSim::round_state() const {
    Dictionary d;
    if (!sim_) return d;
    const icarus::EventId u = sim_->society().unification_event();
    d["unified"] = u != 0;
    // The island's toll so far (there are no births: everyone who ever lived is here).
    int ever = 0, dead = 0, girls_dead = 0, left = 0;
    for (const auto& cp : sim_->agents().all()) {
        if (!cp) continue;
        ++ever;
        if (cp->departed) ++left;
        else if (!cp->alive) {
            ++dead;
            if (cp->is_girl()) ++girls_dead;
        }
    }
    d["people_ever"] = ever;
    d["dead"] = dead;
    d["girls_dead"] = girls_dead;
    d["departed"] = left;
    if (u) {
        d["event"] = (int64_t)u;
        if (const icarus::Event* e = sim_->chronicle().get(u)) {
            d["text"] = to_gd(e->text);
            d["time"] = to_gd(icarus::format_time_zh(e->tick));
            d["polity"] = e->polity;
        }
    }
    return d;
}

Array IcarusSim::tech_tree(int64_t polity) const {
    Array out;
    if (!sim_) return out;
    const icarus::Society& soc = sim_->society();
    const icarus::Polity* p = soc.polity((uint16_t)polity);
    // What each tech unlocks: recipes and buildings that name it.
    std::map<std::string, std::vector<std::string>> unlocks;
    for (const icarus::Json& r : reg_->doc("recipes")["recipes"].items())
        if (r.has("tech")) unlocks[r.str("tech")].push_back(r.str("name"));
    for (const icarus::Json& b : reg_->doc("buildings")["buildings"].items())
        if (b.has("tech")) unlocks[b.str("tech")].push_back(b.str("name"));
    for (const icarus::Json& t : reg_->doc("techs")["techs"].items()) {
        Dictionary d;
        const std::string key = t.str("key");
        d["key"] = to_gd(key);
        d["name"] = to_gd(t.str("name"));
        d["era"] = t.integer("era", 0);
        d["cost"] = t.flt("cost", 0.0f);
        d["desc"] = to_gd(t.str("desc"));
        PackedStringArray req;
        if (t.has("requires"))
            for (const icarus::Json& r : t["requires"].items()) req.push_back(to_gd(r.as_str()));
        d["requires"] = req;
        PackedStringArray un;
        for (const std::string& u : unlocks[key]) un.push_back(to_gd(u));
        d["unlocks"] = un;
        static const std::map<std::string, std::pair<const char*, bool>> effect_names = {
            {"craft_speed", {"制作速度", true}},   {"research_speed", {"研究速度", true}},
            {"irrigation_radius", {"灌溉范围", false}}, {"morale", {"士气", true}},
            {"fair_punishment", {"刑罚公正", true}}, {"loyalty_drift", {"忠诚回归", true}}};
        String effects;
        if (t.has("effects"))
            for (const auto& [k, v] : t["effects"].members()) {
                if (!effects.is_empty()) effects += " · ";
                auto it = effect_names.find(k);
                if (it == effect_names.end())
                    effects += to_gd(k) + " +" + String::num(v.as_num(), 2);
                else if (it->second.second)
                    effects += String::utf8(it->second.first) + " +" + String::num_int64((int64_t)std::lround(v.as_num() * 100)) + "%";
                else
                    effects += String::utf8(it->second.first) + " +" + String::num(v.as_num(), 0);
            }
        d["effects"] = effects;
        float prog = 0;
        String state = "locked";
        if (p) {
            for (const auto& r : p->research)
                if (r.first == key) prog = r.second;
            if (p->has_tech(key)) state = "known";
            else if (p->policies.research == key) state = "researching";
            else if (soc.tech_available(*p, key)) state = "available";
        }
        d["progress"] = prog;
        d["state"] = state;
        out.push_back(d);
    }
    return out;
}

Array IcarusSim::piles() const {
    Array out;
    if (!sim_) return out;
    for (const auto& s : sim_->economy().stores()) {
        if (!s.alive || s.kind != icarus::StoreKind::Pile || s.empty()) continue;
        Dictionary d;
        d["pos"] = to_gd(s.pos);
        int n = 0;
        for (auto& st : s.items) n += st.count;
        d["count"] = n;
        out.push_back(d);
    }
    return out;
}

Dictionary IcarusSim::building_at(const Vector3i& cube) const {
    Dictionary d;
    if (!sim_) return d;
    uint32_t id = sim_->buildings().at(to_ic(cube));
    const icarus::Building* b = sim_->buildings().get(id);
    if (!b) return d;
    d["id"] = (int64_t)b->id;
    d["name"] = to_gd(b->name);
    d["integrity"] = b->integrity;
    d["functional"] = b->functional;
    d["complete"] = b->complete;
    d["residents"] = (int64_t)b->residents.size();
    d["beds"] = b->beds;
    if (const icarus::Store* s = sim_->economy().store(b->store)) {
        Array items;
        for (const auto& st : s->items) {
            Dictionary t;
            t["item"] = to_gd(reg_->item(st.item).name);
            t["count"] = st.count;
            items.push_back(t);
        }
        d["items"] = items;
    }
    return d;
}

Array IcarusSim::buildings() const {
    Array out;
    if (!sim_) return out;
    for (const auto& b : sim_->buildings().all()) {
        if (!b.alive) continue;
        Dictionary d;
        d["id"] = (int64_t)b.id;
        d["name"] = to_gd(b.name);
        d["pos"] = to_gd(b.entrance);
        d["functional"] = b.functional;
        d["complete"] = b.complete;
        d["integrity"] = b.integrity;
        out.push_back(d);
    }
    return out;
}

// ------------------------------------------------------------------------------ decisions

namespace godot {

namespace {
const char* status_key(icarus::DecisionStatus s) {
    switch (s) {
        case icarus::DecisionStatus::Pending: return "pending";
        case icarus::DecisionStatus::AwaitingRemote: return "awaiting";
        case icarus::DecisionStatus::Decided: return "decided";
        case icarus::DecisionStatus::Executed: return "executed";
        case icarus::DecisionStatus::Cancelled: return "cancelled";
    }
    return "?";
}
}  // namespace

Dictionary IcarusSim::decision_summary(const icarus::Decision& d) const {
    Dictionary t;
    t["id"] = (int64_t)d.id;
    t["girl"] = (int64_t)d.girl;
    if (const icarus::Character* g = sim_->agents().get(d.girl)) t["girl_name"] = to_gd(g->name);
    t["polity"] = (int64_t)d.polity;
    t["kind"] = to_gd(d.kind);
    t["topic"] = to_gd(d.topic);
    t["status"] = status_key(d.status);
    t["created"] = (int64_t)d.created;
    t["time"] = to_gd(icarus::format_time_zh(d.created));
    t["source"] = to_gd(d.source);
    t["chosen"] = d.chosen >= 0 ? to_gd(d.options[(size_t)d.chosen].title) : String();
    t["chosen_key"] = d.chosen >= 0 ? to_gd(d.options[(size_t)d.chosen].key) : String();
    t["rationale"] = to_gd(d.rationale);
    t["outcome"] = to_gd(d.outcome);
    t["note"] = to_gd(d.note);
    t["options"] = (int64_t)d.options.size();
    t["event"] = (int64_t)d.decision_event;
    return t;
}

Array IcarusSim::decisions(int64_t after_id, int64_t max_count) const {
    Array out;
    if (!sim_) return out;
    const auto& all = sim_->decisions().all();
    size_t start = (size_t)std::max<int64_t>(1, after_id + 1);
    if (max_count > 0 && all.size() > start + (size_t)max_count) start = all.size() - (size_t)max_count;
    for (size_t i = start; i < all.size(); ++i) out.push_back(decision_summary(all[i]));
    return out;
}

Dictionary IcarusSim::decision(int64_t id) const {
    Dictionary t;
    if (!sim_) return t;
    const icarus::Decision* d = sim_->decisions().get((uint32_t)id);
    if (!d) return t;
    t = decision_summary(*d);
    t["situation"] = to_gd(d->situation);
    t["answered"] = (int64_t)d->answered;
    t["cause"] = (int64_t)d->cause;
    Array opts;
    for (size_t i = 0; i < d->options.size(); ++i) {
        const icarus::DecisionOption& o = d->options[i];
        Dictionary od;
        od["key"] = to_gd(o.key);
        od["title"] = to_gd(o.title);
        od["desc"] = to_gd(o.desc);
        od["feasible"] = o.feasible;
        od["why_not"] = to_gd(o.why_not);
        od["chosen"] = (int)i == d->chosen;
        od["score"] = i < d->local_scores.size() && d->local_scores[i] > -1e8f ? Variant(d->local_scores[i]) : Variant();
        Dictionary feats;
        for (int f = 0; f < icarus::kFeatureCount; ++f)
            if (std::fabs(o.f[f]) > 0.01f) feats[to_gd(icarus::feature_name_zh(f))] = o.f[f];
        od["features"] = feats;
        opts.push_back(od);
    }
    t["option_list"] = opts;
    Array props;
    for (const icarus::Proposal& pr : d->proposals) {
        Dictionary pd;
        pd["girl"] = (int64_t)pr.girl;
        if (const icarus::Character* g = sim_->agents().get(pr.girl)) pd["name"] = to_gd(g->name);
        String title = to_gd(pr.key);
        for (const auto& o : d->options)
            if (o.key == pr.key) title = to_gd(o.title);
        pd["option"] = title;
        pd["reason"] = to_gd(pr.reason);
        pd["adopted"] = pr.adopted;
        props.push_back(pd);
    }
    t["proposals"] = props;
    return t;
}

int64_t IcarusSim::last_decision_id() const {
    if (!sim_) return 0;
    return (int64_t)sim_->decisions().all().size() - 1;
}

String IcarusSim::decision_mode() const { return sim_ ? to_gd(sim_->decisions().mode) : String(); }

void IcarusSim::set_decision_mode(const String& mode, int64_t budget_per_day, int64_t deadline_ticks) {
    if (!sim_) return;
    icarus::Decisions& d = sim_->decisions();
    d.mode = to_std(mode);
    if (budget_per_day > 0) d.remote_budget_per_day = (int)budget_per_day;
    if (deadline_ticks > 0) d.remote_deadline = (icarus::Tick)deadline_ticks;
}

PackedInt32Array IcarusSim::awaiting_remote() const {
    PackedInt32Array out;
    if (!sim_) return out;
    for (uint32_t id : sim_->decisions().awaiting_remote()) out.push_back((int32_t)id);
    return out;
}

Dictionary IcarusSim::remote_request(int64_t id) const {
    Dictionary t;
    if (!sim_) return t;
    const icarus::Decisions& d = sim_->decisions();
    t["system"] = to_gd(d.remote_system_prompt((uint32_t)id));
    t["user"] = to_gd(d.remote_user_prompt((uint32_t)id));
    t["schema"] = to_gd(d.remote_schema((uint32_t)id).dump());
    return t;
}

Dictionary IcarusSim::submit_decision(int64_t id, const String& key, const String& rationale, const String& source) {
    Dictionary t;
    if (!sim_) return t;
    std::string err;
    bool ok = sim_->decisions().submit((uint32_t)id, to_std(key), to_std(rationale), to_std(source), err);
    t["ok"] = ok;
    t["error"] = to_gd(err);
    return t;
}

void IcarusSim::remote_failed(int64_t id, const String& why) {
    if (sim_) sim_->decisions().remote_failed((uint32_t)id, to_std(why));
}

String IcarusSim::export_decision_log() const { return sim_ ? to_gd(sim_->decisions().export_log().dump(1)) : String(); }

bool IcarusSim::load_decision_replay(const String& json_text) {
    if (!sim_) return false;
    try {
        sim_->decisions().load_replay(icarus::Json::parse(to_std(json_text)));
        return true;
    } catch (const std::exception& e) {
        last_error_ = to_gd(e.what());
        return false;
    }
}

}  // namespace godot
