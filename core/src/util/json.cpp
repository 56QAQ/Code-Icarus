#include "icarus/util/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace icarus {

namespace {

const Json& null_json() {
    static const Json n;
    return n;
}

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}

    Json parse_document() {
        skip_ws();
        Json v = parse_value(0);
        skip_ws();
        if (p_ != t_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) {
        int line = 1, col = 1;
        for (size_t i = 0; i < p_ && i < t_.size(); ++i) {
            if (t_[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        throw JsonError("JSON parse error at " + std::to_string(line) + ":" + std::to_string(col) + ": " + msg);
    }

    void skip_ws() {
        while (p_ < t_.size()) {
            char c = t_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
            } else if (c == '/' && p_ + 1 < t_.size() && t_[p_ + 1] == '/') {
                // Allow // line comments in data files.
                while (p_ < t_.size() && t_[p_] != '\n') ++p_;
            } else {
                break;
            }
        }
    }

    Json parse_value(int depth) {
        if (depth > 256) fail("nesting too deep");
        if (p_ >= t_.size()) fail("unexpected end");
        char c = t_[p_];
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return Json(parse_string());
        if (c == 't') { expect("true"); return Json(true); }
        if (c == 'f') { expect("false"); return Json(false); }
        if (c == 'n') { expect("null"); return Json(); }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail(std::string("unexpected character '") + c + "'");
    }

    void expect(const char* lit) {
        size_t n = std::strlen(lit);
        if (t_.compare(p_, n, lit) != 0) fail(std::string("expected ") + lit);
        p_ += n;
    }

    Json parse_number() {
        size_t start = p_;
        if (t_[p_] == '-') ++p_;
        while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' ||
                                  t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-'))
            ++p_;
        std::string s = t_.substr(start, p_ - start);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') fail("bad number '" + s + "'");
        return Json(v);
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    unsigned parse_hex4() {
        if (p_ + 4 > t_.size()) fail("bad unicode escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }

    std::string parse_string() {
        ++p_;  // opening quote
        std::string out;
        while (true) {
            if (p_ >= t_.size()) fail("unterminated string");
            char c = t_[p_++];
            if (c == '"') break;
            if (c == '\\') {
                if (p_ >= t_.size()) fail("bad escape");
                char e = t_[p_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = parse_hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (p_ + 2 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                                p_ += 2;
                                unsigned lo = parse_hex4();
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Json parse_array(int depth) {
        ++p_;
        Json arr = Json::array();
        skip_ws();
        if (p_ < t_.size() && t_[p_] == ']') { ++p_; return arr; }
        while (true) {
            skip_ws();
            arr.push(parse_value(depth + 1));
            skip_ws();
            if (p_ >= t_.size()) fail("unterminated array");
            char c = t_[p_++];
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']'");
            skip_ws();
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; break; }  // tolerate trailing comma
        }
        return arr;
    }

    Json parse_object(int depth) {
        ++p_;
        Json obj = Json::object();
        skip_ws();
        if (p_ < t_.size() && t_[p_] == '}') { ++p_; return obj; }
        while (true) {
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != '"') fail("expected key");
            std::string key = parse_string();
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != ':') fail("expected ':'");
            ++p_;
            skip_ws();
            obj.set(key, parse_value(depth + 1));
            skip_ws();
            if (p_ >= t_.size()) fail("unterminated object");
            char c = t_[p_++];
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}'");
            skip_ws();
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; break; }
        }
        return obj;
    }

    const std::string& t_;
    size_t p_ = 0;
};

void format_number(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "null"; return; }
    if (v == std::floor(v) && std::fabs(v) < 9.007199254740992e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", (long long)v);
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    if (std::strtod(buf, nullptr) != v) std::snprintf(buf, sizeof buf, "%.17g", v);
    out += buf;
}

}  // namespace

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

Json Json::parse(const std::string& text) { return Parser(text).parse_document(); }

const std::string& Json::as_str() const {
    static const std::string empty;
    return type_ == Type::String ? s_ : empty;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return null_json();
    for (const auto& m : o_)
        if (m.first == key) return m.second;
    return null_json();
}

bool Json::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& m : o_)
        if (m.first == key) return true;
    return false;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw JsonError("set() on non-object");
    for (auto& m : o_) {
        if (m.first == key) {
            m.second = std::move(value);
            return m.second;
        }
    }
    o_.emplace_back(key, std::move(value));
    return o_.back().second;
}

const Json& Json::operator[](size_t idx) const {
    if (type_ != Type::Array || idx >= a_.size()) return null_json();
    return a_[idx];
}

Json& Json::push(Json value) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw JsonError("push() on non-array");
    a_.push_back(std::move(value));
    return a_.back();
}

size_t Json::size() const {
    if (type_ == Type::Array) return a_.size();
    if (type_ == Type::Object) return o_.size();
    return 0;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: format_number(out, n_); break;
        case Type::String:
            out += '"';
            out += json_escape(s_);
            out += '"';
            break;
        case Type::Array: {
            out += '[';
            if (a_.empty()) { out += ']'; break; }
            for (size_t i = 0; i < a_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                a_[i].dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            if (o_.empty()) { out += '}'; break; }
            for (size_t i = 0; i < o_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                out += '"';
                out += json_escape(o_[i].first);
                out += indent >= 0 ? "\": " : "\":";
                o_[i].second.dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out += '}';
            break;
        }
    }
}

}  // namespace icarus
