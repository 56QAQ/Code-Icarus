#include "icarus_sim.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

using namespace godot;

namespace {
std::string to_std(const String& s) {
    CharString cs = s.utf8();
    return std::string(cs.get_data(), (size_t)cs.length());
}
}  // namespace

IcarusSim::IcarusSim() = default;
IcarusSim::~IcarusSim() = default;

void IcarusSim::_bind_methods() {
    ClassDB::bind_method(D_METHOD("kernel_version"), &IcarusSim::kernel_version);
    ClassDB::bind_method(D_METHOD("load_rules", "files"), &IcarusSim::load_rules);
    ClassDB::bind_method(D_METHOD("last_error"), &IcarusSim::last_error);
    ClassDB::bind_method(D_METHOD("new_world", "seed"), &IcarusSim::new_world);
    ClassDB::bind_method(D_METHOD("world_stats"), &IcarusSim::world_stats);
}

String IcarusSim::kernel_version() const { return "icarus-kernel 0.1"; }

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
        reg_ = std::move(reg);
        world_.reset();
        return true;
    } catch (const std::exception& e) {
        last_error_ = String::utf8(e.what());
        return false;
    }
}

bool IcarusSim::new_world(int64_t seed) {
    if (!reg_) {
        last_error_ = "rules not loaded";
        return false;
    }
    icarus::WorldConfig cfg;
    cfg.seed = (uint64_t)seed;
    world_ = std::make_unique<icarus::World>(*reg_);
    world_->init(cfg);
    return true;
}

Dictionary IcarusSim::world_stats() const {
    Dictionary d;
    if (!world_) return d;
    icarus::WorldStats s = world_->stats();
    d["ungenerated"] = s.ungenerated;
    d["dormant"] = s.dormant;
    d["active"] = s.active;
    d["bytes"] = (int64_t)s.bytes;
    d["size_x"] = world_->size_x();
    d["size_y"] = world_->size_y();
    d["size_z"] = world_->size_z();
    return d;
}
