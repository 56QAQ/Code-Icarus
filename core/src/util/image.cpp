#include "icarus/util/image.h"

#include "icarus/util/binio.h"

namespace icarus {

namespace {
u32 crc_table[256];
bool crc_init = false;
u32 crc32(const u8* d, size_t n, u32 c = 0xFFFFFFFFu) {
    if (!crc_init) {
        for (u32 i = 0; i < 256; ++i) {
            u32 k = i;
            for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
        crc_init = true;
    }
    for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<u8>& o, u32 v) {
    o.push_back((u8)(v >> 24));
    o.push_back((u8)(v >> 16));
    o.push_back((u8)(v >> 8));
    o.push_back((u8)v);
}
void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
    be32(out, (u32)data.size());
    std::vector<u8> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
}  // namespace

bool write_png(const std::string& path, const Image& img) {
    std::vector<u8> raw;
    raw.reserve((size_t)img.h * (img.w * 3 + 1));
    for (int y = 0; y < img.h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), img.rgb.begin() + (size_t)y * img.w * 3, img.rgb.begin() + (size_t)(y + 1) * img.w * 3);
    }
    // zlib stream with stored blocks.
    std::vector<u8> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((u8)(n & 0xFF));
        z.push_back((u8)(n >> 8));
        z.push_back((u8)(~n & 0xFF));
        z.push_back((u8)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    u32 a = 1, b = 0;
    for (u8 c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);

    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    be32(ihdr, (u32)img.w);
    be32(ihdr, (u32)img.h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return write_file(path, out.data(), out.size());
}

}  // namespace icarus
