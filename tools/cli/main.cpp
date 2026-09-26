// icarus_cli: headless runner for the Code:Icarus simulation kernel.
//   icarus_cli map  --seed N --out map.png          top-down map of the generated world
#include <cstdio>
#include <cstring>
#include <string>

#include "icarus/data/registry.h"
#include "icarus/util/image.h"
#include "icarus/util/log.h"
#include "icarus/world/world.h"

using namespace icarus;

namespace {

struct Args {
    std::string cmd;
    u64 seed = 1;
    std::string out = "map.png";
    std::string data = "game/data";
    int scale = 1;
};

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc > 1) a.cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--seed") a.seed = std::stoull(next());
        else if (k == "--out") a.out = next();
        else if (k == "--data") a.data = next();
        else if (k == "--scale") a.scale = std::stoi(next());
    }
    return a;
}

int cmd_map(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    World w(reg);
    WorldConfig cfg;
    cfg.seed = a.seed;
    w.init(cfg);
    const int W = w.size_x(), D = w.size_z();
    Image img(W, D);
    for (int z = 0; z < D; ++z) {
        for (int x = 0; x < W; ++x) {
            int top = -1;
            u32 col = 0x101826;
            // Scan from top using peek: renderer-style access, no simulation side effects.
            ColumnInfo ci = w.gen().column(x, z);
            int start = ci.land ? std::min(w.size_y() - 1, (int)ci.top + 16) : -1;
            for (int y = start; y >= 0; --y) {
                Voxel v = w.peek({x, y, z});
                MatId m = vmat(v);
                if (m == 0) continue;
                const Material& mat = reg.mat(m);
                top = y;
                col = mat.color;
                if (mat.fluid) col = mix_color(col, 0x1a3a70, 0.25f);
                break;
            }
            if (top >= 0) {
                float k = 0.55f + 0.45f * (float)(top - 100) / 110.0f;
                col = shade_color(col, clampv(k, 0.35f, 1.25f));
            }
            img.set(x, z, col);
        }
    }
    const IslandFeatures& f = w.gen().features();
    auto mark = [&](const Vec3i& p, u32 c) {
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) img.set(p.x + dx, p.z + dz, c);
    };
    mark(f.village, 0xff3030);
    mark(f.farms, 0xffd000);
    mark(f.bridge_a, 0xffffff);
    mark(f.bridge_b, 0xffffff);
    mark(f.spring, 0x00ffff);
    mark(f.ravine_end, 0xff00ff);
    if (!write_png(a.out, img)) {
        std::fprintf(stderr, "cannot write %s\n", a.out.c_str());
        return 1;
    }
    WorldStats st = w.stats();
    std::printf("map written: %s (%dx%d) cells: ungenerated=%d dormant=%d active=%d mem=%zuKB\n", a.out.c_str(), W, D,
                st.ungenerated, st.dormant, st.active, st.bytes / 1024);
    std::printf("village=%s farms=%s lake=%s spring=%s bridge=%s->%s mountain=%s\n", f.village.str().c_str(),
                f.farms.str().c_str(), f.lake.str().c_str(), f.spring.str().c_str(), f.bridge_a.str().c_str(),
                f.bridge_b.str().c_str(), f.mountain.str().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    try {
        if (a.cmd == "map") return cmd_map(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    std::fprintf(stderr, "usage: icarus_cli <map|run> [--seed N] [--out FILE] [--data DIR]\n");
    return 1;
}
