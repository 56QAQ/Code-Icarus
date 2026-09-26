// Godot binding for the simulation kernel. Thin: converts types and forwards calls.
#pragma once

#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "icarus/data/registry.h"
#include "icarus/world/world.h"

namespace godot {

class IcarusSim : public RefCounted {
    GDCLASS(IcarusSim, RefCounted)

public:
    IcarusSim();
    ~IcarusSim() override;

    String kernel_version() const;
    bool load_rules(const Dictionary& files);  // name -> json text
    String last_error() const { return last_error_; }
    bool new_world(int64_t seed);
    Dictionary world_stats() const;

protected:
    static void _bind_methods();

private:
    std::unique_ptr<icarus::Registry> reg_;
    std::unique_ptr<icarus::World> world_;
    String last_error_;
};

}  // namespace godot
