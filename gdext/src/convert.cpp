#include "convert.h"

#include <cstring>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

icarus::Json variant_to_json(const Variant& v) {
    switch (v.get_type()) {
        case Variant::NIL: return icarus::Json();
        case Variant::BOOL: return icarus::Json((bool)v);
        case Variant::INT: return icarus::Json((double)(int64_t)v);
        case Variant::FLOAT: return icarus::Json((double)v);
        case Variant::STRING:
        case Variant::STRING_NAME: return icarus::Json(to_std(String(v)));
        case Variant::VECTOR3: {
            Vector3 p = v;
            icarus::Json a = icarus::Json::array();
            a.push((double)p.x);
            a.push((double)p.y);
            a.push((double)p.z);
            return a;
        }
        case Variant::VECTOR3I: {
            Vector3i p = v;
            icarus::Json a = icarus::Json::array();
            a.push(p.x);
            a.push(p.y);
            a.push(p.z);
            return a;
        }
        case Variant::ARRAY: {
            Array arr = v;
            icarus::Json a = icarus::Json::array();
            for (int i = 0; i < arr.size(); ++i) a.push(variant_to_json(arr[i]));
            return a;
        }
        case Variant::PACKED_STRING_ARRAY: {
            PackedStringArray arr = v;
            icarus::Json a = icarus::Json::array();
            for (int i = 0; i < arr.size(); ++i) a.push(to_std(arr[i]));
            return a;
        }
        case Variant::DICTIONARY: {
            Dictionary d = v;
            icarus::Json o = icarus::Json::object();
            Array keys = d.keys();
            for (int i = 0; i < keys.size(); ++i) o.set(to_std(String(keys[i])), variant_to_json(d[keys[i]]));
            return o;
        }
        default: return icarus::Json(to_std(String(v)));
    }
}

Variant json_to_variant(const icarus::Json& j) {
    switch (j.type()) {
        case icarus::Json::Type::Null: return Variant();
        case icarus::Json::Type::Bool: return j.as_bool();
        case icarus::Json::Type::Number: {
            double n = j.as_num();
            if (n == (double)(int64_t)n && n > -9e15 && n < 9e15) return (int64_t)n;
            return n;
        }
        case icarus::Json::Type::String: return to_gd(j.as_str());
        case icarus::Json::Type::Array: {
            Array a;
            for (const auto& e : j.items()) a.push_back(json_to_variant(e));
            return a;
        }
        case icarus::Json::Type::Object: {
            Dictionary d;
            for (const auto& [k, e] : j.members()) d[to_gd(k)] = json_to_variant(e);
            return d;
        }
    }
    return Variant();
}

Array mesh_to_arrays(const icarus::MeshData& m) {
    Array out;
    if (m.empty()) return out;
    out.resize(Mesh::ARRAY_MAX);
    const int64_t nv = (int64_t)m.vertex_count();
    PackedVector3Array pos;
    pos.resize(nv);
    PackedVector3Array nrm;
    nrm.resize(nv);
    PackedColorArray col;
    col.resize(nv);
    Vector3* pp = pos.ptrw();
    Vector3* np = nrm.ptrw();
    Color* cp = col.ptrw();
    for (int64_t i = 0; i < nv; ++i) {
        pp[i] = Vector3(m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]);
        np[i] = Vector3(m.normals[i * 3], m.normals[i * 3 + 1], m.normals[i * 3 + 2]);
        cp[i] = Color(m.colors[i * 4], m.colors[i * 4 + 1], m.colors[i * 4 + 2], m.colors[i * 4 + 3]);
    }
    PackedInt32Array idx;
    idx.resize((int64_t)m.indices.size());
    std::memcpy(idx.ptrw(), m.indices.data(), m.indices.size() * sizeof(int32_t));
    out[Mesh::ARRAY_VERTEX] = pos;
    out[Mesh::ARRAY_NORMAL] = nrm;
    out[Mesh::ARRAY_COLOR] = col;
    if (m.uvs.size() == (size_t)nv * 2) {
        PackedVector2Array uv;
        uv.resize(nv);
        Vector2* up = uv.ptrw();
        for (int64_t i = 0; i < nv; ++i) up[i] = Vector2(m.uvs[i * 2], m.uvs[i * 2 + 1]);
        out[Mesh::ARRAY_TEX_UV] = uv;
    }
    out[Mesh::ARRAY_INDEX] = idx;
    return out;
}

}  // namespace godot
