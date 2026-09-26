// Conversions between Godot Variants and kernel types.
#pragma once

#include <string>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include "icarus/render/mesher.h"
#include "icarus/util/json.h"
#include "icarus/util/types.h"

namespace godot {

inline std::string to_std(const String& s) {
    CharString cs = s.utf8();
    return std::string(cs.get_data(), (size_t)cs.length());
}
inline String to_gd(const std::string& s) { return String::utf8(s.c_str(), (int)s.size()); }

inline Vector3i to_gd(const icarus::Vec3i& v) { return Vector3i(v.x, v.y, v.z); }
inline Vector3 to_gd(const icarus::Vec3f& v) { return Vector3(v.x, v.y, v.z); }
inline icarus::Vec3i to_ic(const Vector3i& v) { return {v.x, v.y, v.z}; }
inline icarus::Vec3f to_ic(const Vector3& v) { return {(float)v.x, (float)v.y, (float)v.z}; }

icarus::Json variant_to_json(const Variant& v);
Variant json_to_variant(const icarus::Json& j);

// Builds an Array for ArrayMesh::add_surface_from_arrays, or an empty Array.
Array mesh_to_arrays(const icarus::MeshData& m);

}  // namespace godot
