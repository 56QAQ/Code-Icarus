// Small self-contained JSON value, parser and serializer.
// Objects keep insertion order so that serialised output is deterministic.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace icarus {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& m) : std::runtime_error(m) {}
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : type_(Type::Number), n_(v) {}
    Json(unsigned v) : type_(Type::Number), n_(v) {}
    Json(long v) : type_(Type::Number), n_((double)v) {}
    Json(unsigned long v) : type_(Type::Number), n_((double)v) {}
    Json(long long v) : type_(Type::Number), n_((double)v) {}
    Json(unsigned long long v) : type_(Type::Number), n_((double)v) {}
    Json(float v) : type_(Type::Number), n_(v) {}
    Json(double v) : type_(Type::Number), n_(v) {}
    Json(const char* s) : type_(Type::String), s_(s) {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    static Json parse(const std::string& text);  // throws JsonError
    std::string dump(int indent = -1) const;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double as_num(double def = 0.0) const { return type_ == Type::Number ? n_ : def; }
    float as_float(float def = 0.0f) const { return type_ == Type::Number ? (float)n_ : def; }
    int as_int(int def = 0) const { return type_ == Type::Number ? (int)n_ : def; }
    const std::string& as_str() const;
    std::string as_str(const std::string& def) const { return type_ == Type::String ? s_ : def; }

    // Object access. operator[] on a missing key returns a shared null value.
    const Json& operator[](const std::string& key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;
    Json& set(const std::string& key, Json value);  // converts null to object
    const std::vector<Member>& members() const { return o_; }

    // Array access.
    const Json& operator[](size_t idx) const;
    const Json& operator[](int idx) const { return (*this)[(size_t)idx]; }
    Json& push(Json value);  // converts null to array
    const std::vector<Json>& items() const { return a_; }
    std::vector<Json>& items_mut() { return a_; }

    size_t size() const;

    // Convenience getters with defaults.
    double num(const std::string& key, double def = 0.0) const { return (*this)[key].as_num(def); }
    float flt(const std::string& key, float def = 0.0f) const { return (*this)[key].as_float(def); }
    int integer(const std::string& key, int def = 0) const { return (*this)[key].as_int(def); }
    bool boolean(const std::string& key, bool def = false) const { return (*this)[key].as_bool(def); }
    std::string str(const std::string& key, const std::string& def = "") const { return (*this)[key].as_str(def); }

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0.0;
    std::string s_;
    std::vector<Json> a_;
    std::vector<Member> o_;
};

std::string json_escape(const std::string& s);

}  // namespace icarus
