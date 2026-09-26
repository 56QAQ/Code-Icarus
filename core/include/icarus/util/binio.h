// Binary serialisation helpers used by the save format.
// Little-endian, explicit widths, varints for counts. Readers are bounds-checked.
#pragma once

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "icarus/util/types.h"

namespace icarus {

class BinError : public std::runtime_error {
public:
    explicit BinError(const std::string& m) : std::runtime_error(m) {}
};

class BinWriter {
public:
    void u8v(u8 v) { buf_.push_back(v); }
    void u16v(u16 v) { raw(&v, 2); }
    void u32v(u32 v) { raw(&v, 4); }
    void u64v(u64 v) { raw(&v, 8); }
    void i32v(i32 v) { raw(&v, 4); }
    void i64v(i64 v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void f64(double v) { raw(&v, 8); }
    void boolean(bool b) { u8v(b ? 1 : 0); }
    void varu(u64 v) {
        while (v >= 0x80) {
            buf_.push_back((u8)(v | 0x80));
            v >>= 7;
        }
        buf_.push_back((u8)v);
    }
    void vari(i64 v) { varu(((u64)v << 1) ^ (u64)(v >> 63)); }
    void str(const std::string& s) {
        varu(s.size());
        raw(s.data(), s.size());
    }
    void bytes(const std::vector<u8>& b) {
        varu(b.size());
        raw(b.data(), b.size());
    }
    void vec3i(const Vec3i& v) { vari(v.x); vari(v.y); vari(v.z); }
    void vec3f(const Vec3f& v) { f32(v.x); f32(v.y); f32(v.z); }
    void raw(const void* p, size_t n) {
        const u8* b = (const u8*)p;
        buf_.insert(buf_.end(), b, b + n);
    }
    // Section tag helpers: writes a 4-char tag + placeholder length; end_section patches it.
    size_t begin_section(const char tag[4]) {
        raw(tag, 4);
        size_t at = buf_.size();
        u64v(0);
        return at;
    }
    void end_section(size_t at) {
        u64 len = (u64)(buf_.size() - at - 8);
        std::memcpy(&buf_[at], &len, 8);
    }
    const std::vector<u8>& data() const { return buf_; }
    std::vector<u8>& data_mut() { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    std::vector<u8> buf_;
};

class BinReader {
public:
    BinReader(const u8* data, size_t size) : d_(data), n_(size) {}
    explicit BinReader(const std::vector<u8>& v) : d_(v.data()), n_(v.size()) {}

    u8 u8v() { need(1); return d_[p_++]; }
    u16 u16v() { u16 v; rd(&v, 2); return v; }
    u32 u32v() { u32 v; rd(&v, 4); return v; }
    u64 u64v() { u64 v; rd(&v, 8); return v; }
    i32 i32v() { i32 v; rd(&v, 4); return v; }
    i64 i64v() { i64 v; rd(&v, 8); return v; }
    float f32() { float v; rd(&v, 4); return v; }
    double f64() { double v; rd(&v, 8); return v; }
    bool boolean() { return u8v() != 0; }
    u64 varu() {
        u64 v = 0;
        int shift = 0;
        while (true) {
            u8 b = u8v();
            v |= (u64)(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 63) throw BinError("varint overflow");
        }
        return v;
    }
    i64 vari() {
        u64 u = varu();
        return (i64)(u >> 1) ^ -(i64)(u & 1);
    }
    std::string str() {
        u64 n = varu();
        need((size_t)n);
        std::string s((const char*)d_ + p_, (size_t)n);
        p_ += (size_t)n;
        return s;
    }
    std::vector<u8> bytes() {
        u64 n = varu();
        need((size_t)n);
        std::vector<u8> b(d_ + p_, d_ + p_ + n);
        p_ += (size_t)n;
        return b;
    }
    Vec3i vec3i() {
        Vec3i v;
        v.x = (i32)vari();
        v.y = (i32)vari();
        v.z = (i32)vari();
        return v;
    }
    Vec3f vec3f() {
        Vec3f v;
        v.x = f32();
        v.y = f32();
        v.z = f32();
        return v;
    }
    // Reads a section header; returns a sub-reader limited to the section body.
    BinReader section(const char tag[4]) {
        need(12);
        if (std::memcmp(d_ + p_, tag, 4) != 0)
            throw BinError(std::string("expected section ") + std::string(tag, 4));
        p_ += 4;
        u64 len = u64v();
        need((size_t)len);
        BinReader sub(d_ + p_, (size_t)len);
        p_ += (size_t)len;
        return sub;
    }
    bool at_end() const { return p_ >= n_; }
    size_t pos() const { return p_; }
    size_t remaining() const { return n_ - p_; }

private:
    void need(size_t k) {
        if (p_ + k > n_) throw BinError("unexpected end of data");
    }
    void rd(void* out, size_t k) {
        need(k);
        std::memcpy(out, d_ + p_, k);
        p_ += k;
    }
    const u8* d_;
    size_t n_;
    size_t p_ = 0;
};

// Run-length encoding for u16 voxel arrays: (count varint, value u16) pairs.
std::vector<u8> rle_encode_u16(const u16* data, size_t n);
bool rle_decode_u16(const std::vector<u8>& in, u16* out, size_t n);

bool read_file(const std::string& path, std::string& out);
bool read_file_bytes(const std::string& path, std::vector<u8>& out);
bool write_file(const std::string& path, const void* data, size_t n);

// 64-bit FNV-1a over bytes, used for state hashing.
inline u64 fnv1a64(const void* data, size_t n, u64 h = 1469598103934665603ull) {
    const u8* p = (const u8*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace icarus
