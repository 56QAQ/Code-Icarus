#include "icarus/util/binio.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace icarus {

std::vector<u8> rle_encode_u16(const u16* data, size_t n) {
    BinWriter w;
    size_t i = 0;
    while (i < n) {
        u16 v = data[i];
        size_t j = i + 1;
        while (j < n && data[j] == v) ++j;
        w.varu(j - i);
        w.u16v(v);
        i = j;
    }
    return std::move(w.data_mut());
}

bool rle_decode_u16(const std::vector<u8>& in, u16* out, size_t n) {
    try {
        BinReader r(in);
        size_t i = 0;
        while (!r.at_end()) {
            u64 cnt = r.varu();
            u16 v = r.u16v();
            if (i + cnt > n) return false;
            for (u64 k = 0; k < cnt; ++k) out[i++] = v;
        }
        return i == n;
    } catch (const BinError&) {
        return false;
    }
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool read_file_bytes(const std::string& path, std::vector<u8>& out) {
    std::string s;
    if (!read_file(path, s)) return false;
    out.assign(s.begin(), s.end());
    return true;
}

bool write_file(const std::string& path, const void* data, size_t n) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)data, (std::streamsize)n);
    return (bool)f;
}

std::string Vec3i::str() const {
    return "(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ")";
}

}  // namespace icarus
