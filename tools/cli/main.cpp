// icarus_cli: headless runner for the Code:Icarus simulation kernel.
//   icarus_cli map  --seed N --out map.png          top-down map of the generated world
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/util/binio.h"
#include "icarus/util/image.h"
#include "icarus/util/log.h"
#include "icarus/sim/simulation.h"
#include "icarus/world/world.h"

using namespace icarus;

namespace {

struct Args {
    std::string cmd;
    u64 seed = 1;
    std::string out = "map.png";
    std::string data = "game/data";
    int scale = 1;
    double days = 1.0;
    std::string scenario = "village";
    std::string save;
    bool verbose = false;
    int every = 1;                 // print stats every N hours
    bool events = false;           // stream notable events
    std::vector<std::string> admin; // "HOURS:type:json" or shortcuts "HOURS:break_bridge"
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
        else if (k == "--days") a.days = std::stod(next());
        else if (k == "--scenario") a.scenario = next();
        else if (k == "--save") a.save = next();
        else if (k == "-v" || k == "--verbose") a.verbose = true;
        else if (k == "--every") a.every = std::max(1, std::stoi(next()));
        else if (k == "--events") a.events = true;
        else if (k == "--admin") a.admin.push_back(next());
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

// Scheduled administrator interventions. Shortcuts target the island's features.
struct Scheduled {
    Tick at = 0;
    AdminCommand cmd;
};

std::vector<Scheduled> parse_admin(const Args& a, Simulation& sim) {
    std::vector<Scheduled> out;
    const IslandFeatures& f = sim.world().gen().features();
    auto vec = [](const Vec3i& p) {
        Json j = Json::array();
        j.push(p.x);
        j.push(p.y);
        j.push(p.z);
        return j;
    };
    for (const std::string& spec : a.admin) {
        size_t c1 = spec.find(':');
        if (c1 == std::string::npos) continue;
        Scheduled s;
        s.at = sim.now() + (Tick)(std::stod(spec.substr(0, c1)) * (double)kTicksPerHour);
        std::string rest = spec.substr(c1 + 1);
        size_t c2 = rest.find(':');
        std::string type = rest.substr(0, c2);
        if (type == "break_bridge") {
            // Blow out the middle of the bridge deck.
            Vec3i mid{(f.bridge_a.x + f.bridge_b.x) / 2, (f.bridge_a.y + f.bridge_b.y) / 2, (f.bridge_a.z + f.bridge_b.z) / 2};
            s.cmd.type = "dig";
            s.cmd.params = Json::object();
            s.cmd.params.set("pos", vec(mid));
            s.cmd.params.set("radius", 4.5);
        } else if (type == "kill_spring") {
            // Bury the spring under stone: the lake stops being fed.
            s.cmd.type = "place";
            s.cmd.params = Json::object();
            s.cmd.params.set("pos", vec(f.spring));
            s.cmd.params.set("radius", 2.5);
            s.cmd.params.set("material", "stone");
        } else if (type == "drain_lake") {
            s.cmd.type = "dig";
            s.cmd.params = Json::object();
            Vec3i p = f.lake;
            p.y -= 3;
            s.cmd.params.set("pos", vec(p));
            s.cmd.params.set("radius", 6.0);
        } else {
            s.cmd.type = type;
            s.cmd.params = c2 == std::string::npos ? Json::object() : Json::parse(rest.substr(c2 + 1));
        }
        out.push_back(s);
    }
    return out;
}

int cmd_run(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    Simulation sim(reg);
    GameConfig cfg;
    cfg.world.seed = a.seed;
    cfg.scenario = a.scenario;
    sim.new_game(cfg);
    std::vector<Scheduled> sched = parse_admin(a, sim);
    size_t shown = sim.chronicle().events().size();
    Tick total = (Tick)(a.days * (double)kTicksPerDay);
    double max_us = 0, sum_us = 0, sum_phys = 0, sum_ag = 0, sum_soc = 0, sum_dec = 0;
    for (Tick t = 0; t < total; ++t) {
        for (const Scheduled& s : sched)
            if (s.at == sim.now()) sim.queue_admin(s.cmd);
        sim.step();
        if (a.events) {
            const auto& ev = sim.chronicle().events();
            for (; shown < ev.size(); ++shown) {
                const Event& e = ev[shown];
                if (e.severity < 3 && e.type != EventType::DecisionMade) continue;
                std::printf("    >> [%s] %s\n", format_time_zh(e.tick).c_str(), e.text.c_str());
                if (e.type == EventType::DecisionMade && e.data.has("rationale")) {
                    for (const Json& pj : e.data["proposals"].items())
                        std::printf("       %s建议「%s」%s\n", pj.str("name").c_str(), pj.str("option").c_str(),
                                    pj.boolean("adopted") ? "（采纳）" : "");
                    std::printf("       理由(%s)：%s\n", e.data.str("source").c_str(), e.data.str("rationale").c_str());
                }
            }
        }
        const auto& pr = sim.profile();
        max_us = std::max(max_us, pr.total_us);
        sum_us += pr.total_us;
        sum_phys += pr.physics_us;
        sum_ag += pr.agents_us;
        sum_soc += pr.society_us;
        sum_dec += pr.decisions_us;
        if (sim.now() % (kTicksPerHour * (Tick)a.every) == 0) {
            const double per = (double)(kTicksPerHour * (Tick)a.every);
            const PhysicsStats& ps = sim.physics().stats();
            std::printf("%s | water %zu fire %zu | ", format_time_zh(sim.now()).c_str(), ps.water_active, ps.fire_active);
            for (const Polity& p : sim.society().polities()) {
                if (!p.alive) continue;
                const PolityStats& st = p.stats;
                FarmStats fs = sim.farming().stats_polity(p.id);
                std::printf("%s pop %d food %.0f (%.1fd) fed %.0f%% water %.0f%% mood %.2f sup %.2f prot %d farm %d/%d irr %d | ",
                            sim.society().title(p.id).c_str(), st.population, st.food_stock, st.food_days,
                            st.food_access * 100, st.water_access * 100, st.mood, st.ruler_support, st.protesters,
                            fs.growing + fs.mature, fs.plots, fs.irrigated);
            }
            const auto& d = sim.agents().day;
            std::printf("jobs %zu harv %d drinks %d pathfail %d nofood %d nowater %d | avg %.0fus (phys %.0f ag %.0f soc %.0f dec %.0f) max %.0fus\n",
                        sim.jobs().open_count(), d.harvested, d.drinks, d.path_failures, d.hungry_no_food,
                        d.thirsty_no_water, sum_us / per, sum_phys / per, sum_ag / per, sum_soc / per, sum_dec / per, max_us);
            sum_us = sum_phys = sum_ag = sum_soc = sum_dec = 0;
            max_us = 0;
        }
    }
    if (a.verbose) {
        for (const auto& cp : sim.agents().all()) {
            if (!cp) continue;
            const Character& c = *cp;
            std::printf("  #%u %-6s %s food %.2f water %.2f rest %.2f mood %.2f task %s [%s] %s\n", c.id, c.name.c_str(),
                        c.alive ? "" : "(dead)", c.needs.food, c.needs.water, c.needs.rest, c.mood,
                        task_name_zh(c.task.type), c.status_text.c_str(), c.task.label.c_str());
        }
        for (const Event& e : sim.chronicle().events())
            if (e.severity >= 2) std::printf("  [%s] %s\n", format_time_zh(e.tick).c_str(), e.text.c_str());
    }
    std::printf("final hash %016llx, events %zu\n", (unsigned long long)sim.state_hash(), sim.chronicle().events().size());
    if (!a.save.empty()) {
        auto blob = sim.save();
        write_file(a.save, blob.data(), blob.size());
        std::printf("saved %s (%zu KB)\n", a.save.c_str(), blob.size() / 1024);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    try {
        if (a.cmd == "map") return cmd_map(a);
        if (a.cmd == "run") return cmd_run(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    std::fprintf(stderr, "usage: icarus_cli <map|run> [--seed N] [--out FILE] [--data DIR]\n");
    return 1;
}
