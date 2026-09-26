// icarus_cli: headless runner for the Code:Icarus simulation kernel.
//   icarus_cli map  --seed N [--layout continent] --out map.png   top-down map of the generated world
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/economy/buildings.h"
#include "icarus/util/binio.h"
#include "icarus/util/image.h"
#include "icarus/util/log.h"
#include "icarus/sim/simulation.h"
#include "icarus/render/mesher.h"
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
    std::string era;  // 开局时代 override (wild / tribal / village)
    int civs = 0;
    std::string layout = "classic";  // classic | continent
    std::string save;
    bool verbose = false;
    int every = 1;                 // print stats every N hours
    bool events = false;           // stream notable events
    std::vector<std::string> admin; // "HOURS:type:json" or shortcuts "HOURS:break_bridge"
    std::string seeds = "1-6";      // experiment: seed range "a-b" or list "1,4,9"
    std::string report;             // experiment: markdown report path
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
        else if (k == "--era") a.era = next();
        else if (k == "--civs") a.civs = std::stoi(next());
        else if (k == "--layout" || k == "--island") a.layout = next();
        else if (k == "--save") a.save = next();
        else if (k == "-v" || k == "--verbose") a.verbose = true;
        else if (k == "--every") a.every = std::max(1, std::stoi(next()));
        else if (k == "--events") a.events = true;
        else if (k == "--admin") a.admin.push_back(next());
        else if (k == "--seeds") a.seeds = next();
        else if (k == "--report") a.report = next();
    }
    return a;
}

int cmd_map(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    World w(reg);
    WorldConfig cfg = WorldConfig::for_layout(layout_from_key(a.layout), a.seed);
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
                // Hill shading: light from the north-west.
                ColumnInfo nw = w.gen().column(x - 1, z - 1);
                if (nw.land) k *= clampv(1.0f + 0.07f * (float)(ci.top - nw.top), 0.6f, 1.4f);
                col = shade_color(col, clampv(k, 0.3f, 1.35f));
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
    if (cfg.layout == WorldLayout::Classic) {
        mark(f.bridge_a, 0xffffff);
        mark(f.bridge_b, 0xffffff);
        mark(f.ravine_end, 0xff00ff);
    }
    for (const Site& s : f.sites) {
        mark(s.center, 0xff3030);
        mark(s.farms, 0xffd000);
    }
    for (const Vec3i& sp : f.springs) mark(sp, 0x00ffff);
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
    for (const Site& s : f.sites)
        std::printf("site %s (%s) farms=%s water=%s\n", s.center.str().c_str(), biome_name_zh(s.biome),
                    s.farms.str().c_str(), s.water.str().c_str());
    // Biome census over land columns.
    int counts[(int)Biome::Count] = {0};
    int land = 0;
    for (int z = 0; z < D; z += 2)
        for (int x = 0; x < W; x += 2) {
            ColumnInfo ci = w.gen().column(x, z);
            if (!ci.land) continue;
            ++land;
            counts[(int)ci.biome]++;
        }
    std::printf("land columns: %d (x4)\n", land);
    {
        int wet = 0, wet_water = 0, hist[8] = {0};
        for (int z = 0; z < D; z += 2)
            for (int x = 0; x < W; x += 2) {
                ColumnInfo ci = w.gen().column(x, z);
                if (!ci.land || ci.biome != Biome::Wetland) continue;
                ++wet;
                if (ci.water_top >= 0) ++wet_water;
                hist[std::min(7, ci.moist / 32)]++;
            }
        std::printf("wetland columns %d, with water %d; moisture:", wet, wet_water);
        for (int i = 0; i < 8; ++i) std::printf(" %d", hist[i]);
        std::printf("\n");
    }
    for (int b = 1; b < (int)Biome::Count; ++b)
        if (counts[b]) std::printf("  %-10s %5.1f%%\n", biome_key((Biome)b), 100.0 * counts[b] / std::max(1, land));
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
        } else if (type == "bless_seat") {
            // Food from the sky at the first polity's hall (favouring one civilisation).
            s.cmd.type = "bless_food";
            s.cmd.params = Json::object();
            Vec3i at = f.village;
            if (const Polity* p = sim.society().polity(1))
                if (const Building* hall = sim.buildings().get(p->seat)) at = hall->entrance;
            s.cmd.params.set("pos", vec(at));
            s.cmd.params.set("amount", c2 == std::string::npos ? 200 : std::stoi(rest.substr(c2 + 1)));
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

// Generates and meshes every cell the renderer would ask for, and reports the cost.
int cmd_meshbench(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    World w(reg);
    w.init(WorldConfig::for_layout(layout_from_key(a.layout), a.seed));
    Mesher mesher(w);
    CellMesh out;
    int cells = 0, nonempty = 0;
    size_t verts = 0;
    double gen_ms = 0, mesh_ms = 0;
    for (int y = 0; y < w.cells_y(); ++y)
        for (int z = 0; z < w.cells_z(); ++z)
            for (int x = 0; x < w.cells_x(); ++x) {
                const Vec3i c{x, y, z};
                if (!w.gen().cell_maybe_nonempty(c)) continue;
                ++cells;
                auto t0 = std::chrono::steady_clock::now();
                w.peek(Vec3i{x * kCellSize, y * kCellSize, z * kCellSize});
                auto t1 = std::chrono::steady_clock::now();
                mesher.build_cell(c, out);
                auto t2 = std::chrono::steady_clock::now();
                gen_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                mesh_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
                const size_t v = out.opaque.vertex_count() + out.water.vertex_count() + out.foliage.vertex_count() +
                                 out.decor.vertex_count() + out.crops.vertex_count();
                if (v) ++nonempty;
                verts += v;
            }
    std::printf("cells %d (with geometry %d), vertices %zu, generate %.0f ms, mesh %.0f ms\n", cells, nonempty, verts,
                gen_ms, mesh_ms);
    return 0;
}

// Debug: where is water flowing after a run? Lists cells with the most non-full water.
int cmd_waterdump(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    GameConfig cfg;
    cfg.world = WorldConfig::for_layout(layout_from_key(a.layout), a.seed);
    cfg.scenario = a.scenario;
    if (!a.era.empty()) cfg.era = a.era;
    if (a.civs > 0) cfg.civs = a.civs;
    Simulation sim(reg);
    sim.new_game(cfg);
    sim.run((Tick)(a.days * (double)kTicksPerDay));
    World& w = const_cast<World&>(sim.world());
    std::map<Vec3i, int> cells;
    for (int cy = 0; cy < w.cells_y(); ++cy)
        for (int cz = 0; cz < w.cells_z(); ++cz)
            for (int cx = 0; cx < w.cells_x(); ++cx) {
                const Cell* c = w.cell({cx, cy, cz});
                if (!c || c->state != CellState::Active) continue;
                for (int y = 0; y < kCellSize; ++y)
                    for (int z = 0; z < kCellSize; ++z)
                        for (int x = 0; x < kCellSize; ++x) {
                            Voxel v = w.peek({cx * kCellSize + x, cy * kCellSize + y, cz * kCellSize + z});
                            if (vmat(v) == reg.m().water && vlevel(v) < kFluidFull) cells[{cx, cy, cz}]++;
                        }
            }
    std::vector<std::pair<int, Vec3i>> list;
    for (auto& [c, n] : cells) list.push_back({n, c});
    std::sort(list.rbegin(), list.rend());
    for (size_t i = 0; i < list.size() && i < 12; ++i)
        std::printf("cell %s: %d partial water cubes\n", list[i].second.str().c_str(), list[i].first);
    if (!list.empty()) {
        // Top-down view of the busiest cell: the highest water / solid material per column.
        const Vec3i c = list[0].second;
        for (int z = 0; z < kCellSize; ++z) {
            std::string row;
            for (int x = 0; x < kCellSize; ++x) {
                char ch = ' ';
                for (int y = kCellSize - 1; y >= 0; --y) {
                    Voxel v = w.peek({c.x * kCellSize + x, c.y * kCellSize + y, c.z * kCellSize + z});
                    MatId m = vmat(v);
                    if (m == 0) continue;
                    if (m == reg.m().water) ch = vlevel(v) < kFluidFull ? '~' : 'W';
                    else if (!reg.mat(m).solid) continue;
                    else ch = reg.mat(m).key[0];
                    if (ch != ' ') {
                        if (y > 0 && ch != '~' && ch != 'W') ch = (char)(ch - 32 * (y >= 1));
                        break;
                    }
                }
                row += ch;
            }
            std::printf("  %s\n", row.c_str());
        }
    }
    const IslandFeatures& f = sim.world().gen().features();
    for (const SpeciesDef& sp : sim.fauna().species())
        std::printf("%s %d  ", sp.key.c_str(), sim.fauna().count_alive(sp.id));
    std::printf("\n");
    for (auto& [k, n] : sim.fauna().deaths) std::printf("  died %s x%d\n", k.c_str(), n);
    const PhysicsStats& ps = sim.physics().stats();
    std::printf("physics us: water %.0f evap %.0f fire %.0f granular %.0f support %.0f other %.0f\n", ps.us_water,
                ps.us_evaporation, ps.us_fire, ps.us_granular, ps.us_support, ps.us_other);
    std::printf("village %s lake %s pond %s spring %s\n", f.village.str().c_str(), f.lake.str().c_str(),
                f.pond.str().c_str(), f.spring.str().c_str());
    return 0;
}

int cmd_run(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    Simulation sim(reg);
    GameConfig cfg;
    cfg.world = WorldConfig::for_layout(layout_from_key(a.layout), a.seed);
    cfg.scenario = a.scenario;
    if (!a.era.empty()) cfg.era = a.era;
    if (a.civs > 0) cfg.civs = a.civs;
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
            if (c.task.job)
                if (const Job* j = sim.jobs().get(c.task.job)) std::printf("      job %s step %d\n", job_name_zh(j->type), c.task.step);
        }
        for (const Event& e : sim.chronicle().events())
            if (e.severity >= 2) std::printf("  [%s] %s\n", format_time_zh(e.tick).c_str(), e.text.c_str());
        for (const Store& st : sim.economy().stores()) {
            if (!st.alive || st.items.empty() || (st.kind != StoreKind::Stockpile && st.kind != StoreKind::Workshop &&
                                                  st.kind != StoreKind::Pile))
                continue;
            std::printf("  store %u %s (%d,%d,%d):", st.id, store_kind_key(st.kind), st.pos.x, st.pos.y, st.pos.z);
            for (const ItemStack& is : st.items) std::printf(" %s=%d", sim.reg().item(is.item).key.c_str(), is.count);
            std::printf("\n");
        }
        std::map<std::string, int> open_jobs;
        for (const Job& j : sim.jobs().all())
            if (j.alive) open_jobs[std::string(job_name_zh(j.type)) + (j.claimed_by ? "*" : "")]++;
        std::printf("  jobs:");
        for (const auto& [k, v] : open_jobs) std::printf(" %s=%d", k.c_str(), v);
        std::printf("\n");
        // Where the items came from and went (the ledger's sources and sinks).
        std::printf("  ledger:");
        for (const auto& [k, v] : sim.economy().reasons()) std::printf(" %s=%lld", k.c_str(), (long long)v);
        std::printf("\n");
    }
    std::printf("final hash %016llx, events %zu\n", (unsigned long long)sim.state_hash(), sim.chronicle().events().size());
    if (!a.save.empty()) {
        auto blob = sim.save();
        write_file(a.save, blob.data(), blob.size());
        std::printf("saved %s (%zu KB)\n", a.save.c_str(), blob.size() / 1024);
    }
    return 0;
}

// ------------------------------------------------------------------------------ experiment
// Runs the same scenario (and the same shocks) on many seeds and reports how each
// civilisation responded and how it ended up. Phase-1 asks for divergent histories:
// same shock, different girls, different choices, different fates.

std::vector<u64> parse_seeds(const std::string& spec) {
    std::vector<u64> out;
    size_t dash = spec.find('-');
    if (dash != std::string::npos && spec.find(',') == std::string::npos) {
        u64 a = std::stoull(spec.substr(0, dash)), b = std::stoull(spec.substr(dash + 1));
        for (u64 s = a; s <= b; ++s) out.push_back(s);
        return out;
    }
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        out.push_back(std::stoull(spec.substr(pos, comma - pos)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

struct RunSummary {
    u64 seed = 0;
    std::string ruler, drive, girls;
    std::vector<std::string> responses;  // first crisis decisions: "who: choice (proposals)"
    int pop_start = 0, pop_end = 0, deaths = 0, polities = 1, coups = 0, secessions = 0, wars = 0, pacts = 0, trips = 0, aid = 0;
    float food_end = 0, mood_end = 0, support_end = 0, forest_end = 1;
    bool unified = false;
    std::string outcome;
    std::vector<std::string> timeline;
};

std::string drive_name(const Registry& reg, const std::string& key) {
    for (const Json& d : reg.doc("drives")["drives"].items())
        if (d.str("key") == key) return d.str("name", key);
    return key;
}

int cmd_experiment(const Args& a) {
    Registry reg;
    reg.load_from_dir(a.data);
    std::vector<RunSummary> runs;
    for (u64 seed : parse_seeds(a.seeds)) {
        Simulation sim(reg);
        GameConfig cfg;
        cfg.world = WorldConfig::for_layout(layout_from_key(a.layout), seed);
        cfg.scenario = a.scenario;
        if (!a.era.empty()) cfg.era = a.era;
        if (a.civs > 0) cfg.civs = a.civs;
    if (!a.era.empty()) cfg.era = a.era;
    if (a.civs > 0) cfg.civs = a.civs;
        sim.new_game(cfg);
        std::vector<Scheduled> sched = parse_admin(a, sim);
        RunSummary r;
        r.seed = seed;
        const Polity* p0 = sim.society().polity(1);
        if (const Character* ru = p0 ? sim.agents().get(p0->ruler) : nullptr) {
            r.ruler = ru->name;
            r.drive = drive_name(reg, ru->girl->drive);
        }
        for (const auto& cp : sim.agents().all())
            if (cp && cp->is_girl())
                r.girls += (r.girls.empty() ? "" : "、") + cp->name + "(" + drive_name(reg, cp->girl->drive) + "·" + cp->girl->temperament + ")";
        r.pop_start = sim.agents().count_alive(1);
        Tick first_shock = sched.empty() ? 0 : sched.front().at;
        Tick total = (Tick)(a.days * (double)kTicksPerDay);
        for (Tick t = 0; t < total; ++t) {
            for (const Scheduled& s : sched)
                if (s.at == sim.now()) sim.queue_admin(s.cmd);
            sim.step();
        }
        // Responses: the first crisis decisions after the shock.
        for (const Decision& d : sim.decisions().all()) {
            if (!d.id || d.kind != "crisis" || d.chosen < 0 || d.created < first_shock) continue;
            if (r.responses.size() >= 3) break;
            const Character* g = sim.agents().get(d.girl);
            std::string line = (g ? g->name : std::string("?")) + "「" + d.options[(size_t)d.chosen].title + "」";
            std::string props;
            for (const Proposal& pr : d.proposals) {
                const Character* pg = sim.agents().get(pr.girl);
                std::string t = pr.key;
                for (const DecisionOption& o : d.options)
                    if (o.key == pr.key) t = o.title;
                props += (props.empty() ? "" : "；") + (pg ? pg->name : std::string("?")) + "主张" + t;
            }
            if (!props.empty()) line += "（" + props + "）";
            r.responses.push_back(line);
        }
        int pols = 0;
        for (const Polity& p : sim.society().polities()) {
            if (!p.alive) continue;
            ++pols;
            r.pop_end += p.stats.population;
            r.deaths += p.deaths_total;
        }
        r.polities = pols;
        // The first polity, or whoever holds the island after it fell.
        const Polity* main_polity = sim.society().polity(1);
        for (const Polity& p : sim.society().polities())
            if (!main_polity && p.alive) main_polity = &p;
        if (const Polity* p = main_polity) {
            r.food_end = p->stats.food_days;
            r.mood_end = p->stats.mood;
            r.support_end = p->stats.ruler_support;
        }
        for (const Event& e : sim.chronicle().events()) {
            if (e.type == EventType::Coup && e.text.find("失败") == std::string::npos) r.coups++;
            if (e.type == EventType::Secession) r.secessions++;
            if (e.type == EventType::WarDeclared) r.wars++;
            if (e.type == EventType::Trade && e.text.find("缔结") != std::string::npos) r.pacts++;
            if (e.type == EventType::Trade && e.text.find("送出援粮") != std::string::npos) r.aid++;
            bool key = e.severity >= 4 || e.type == EventType::DecisionMade || e.type == EventType::Death;
            if (key && e.tick >= first_shock && r.timeline.size() < 40)
                r.timeline.push_back(format_time_zh(e.tick) + " " + e.text);
        }
        const bool crises_left = [&]() {
            if (const Polity* p = main_polity)
                for (const Crisis& c : p->crises)
                    if (c.active && c.severity >= 0.5f) return true;
            return false;
        }();
        r.unified = sim.society().unification_event() != 0;
        for (const Polity& p : sim.society().polities())
            if (p.alive)
                for (const TradePact& t : p.pacts) r.trips += t.trips;
        r.forest_end = sim.forest().ratio();
        if (r.unified) r.outcome = r.wars > 0 ? "分裂、战争后重归统一" : "分裂后重归统一";
        else if (r.secessions > 0) r.outcome = r.wars > 0 ? "分裂并交战" : "分裂";
        else if (r.coups > 0) r.outcome = "政变";
        else if (r.pop_end < r.pop_start * 3 / 4 || r.mood_end < 0.4f) r.outcome = "衰落";
        else if (!crises_left && r.deaths == 0) r.outcome = "恢复";
        else if (!crises_left) r.outcome = "恢复（有伤亡）";
        else r.outcome = "僵持";
        std::printf("seed %llu: %s（%s）→ %s | %s | pop %d→%d deaths %d polities %d wars %d pacts %d trips %d aid %d food %.1fd mood %.2f support %+.2f forest %.0f%%\n",
                    (unsigned long long)seed, r.ruler.c_str(), r.drive.c_str(), r.outcome.c_str(),
                    r.responses.empty() ? "-" : r.responses.front().c_str(), r.pop_start, r.pop_end, r.deaths, r.polities,
                    r.wars, r.pacts, r.trips, r.aid, r.food_end, r.mood_end, r.support_end, 100.0f * r.forest_end);
        std::fflush(stdout);
        runs.push_back(std::move(r));
    }
    // Divergence summary.
    std::map<std::string, int> outcomes, first;
    for (const RunSummary& r : runs) {
        outcomes[r.outcome]++;
        if (!r.responses.empty()) first[r.responses.front().substr(r.responses.front().find("「"))]++;
    }
    std::printf("\noutcomes:");
    for (auto& [k, v] : outcomes) std::printf(" %s×%d", k.c_str(), v);
    std::printf("\nfirst responses:");
    for (auto& [k, v] : first) std::printf(" %s×%d", k.c_str(), v);
    std::printf("\n");
    if (!a.report.empty()) {
        std::string md = "# 分歧历史实验报告\n\n";
        md += strfmt("场景：%s · 每个种子模拟 %.1f 天 · 冲击：", a.scenario.c_str(), a.days);
        for (const std::string& s : a.admin) md += "`" + s + "` ";
        md += "\n\n| 种子 | 统治者 | 首要应对 | 结局 | 人口 | 死亡 | 国家数 | 战争 | 通商与援助 | 存粮(天) | 心情 | 支持 | 林木 |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        for (const RunSummary& r : runs) {
            std::string trade = r.pacts ? strfmt("通商 %d（商队 %d 次）", r.pacts, r.trips) : std::string();
            if (r.aid) trade += (trade.empty() ? "" : "，") + strfmt("援助 %d 次", r.aid);
            md += strfmt("| %llu | %s（%s） | %s | **%s** | %d→%d | %d | %d | %d | %s | %.1f | %.2f | %+.2f | %.0f%% |\n",
                         (unsigned long long)r.seed, r.ruler.c_str(), r.drive.c_str(),
                         r.responses.empty() ? "-" : r.responses.front().c_str(), r.outcome.c_str(), r.pop_start, r.pop_end,
                         r.deaths, r.polities, r.wars, trade.empty() ? "-" : trade.c_str(), r.food_end, r.mood_end,
                         r.support_end, 100.0f * r.forest_end);
        }
        md += "\n## 各种子的经过\n";
        for (const RunSummary& r : runs) {
            md += strfmt("\n### 种子 %llu — %s\n\n魔法少女：%s\n\n", (unsigned long long)r.seed, r.outcome.c_str(), r.girls.c_str());
            for (const std::string& resp : r.responses) md += "- 应对：" + resp + "\n";
            md += "\n";
            for (const std::string& line : r.timeline) md += "    " + line + "\n";
        }
        write_file(a.report, md.data(), md.size());
        std::printf("report written: %s\n", a.report.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    try {
        if (a.cmd == "map") return cmd_map(a);
        if (a.cmd == "meshbench") return cmd_meshbench(a);
        if (a.cmd == "waterdump") return cmd_waterdump(a);
        if (a.cmd == "run") return cmd_run(a);
        if (a.cmd == "experiment") return cmd_experiment(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    std::fprintf(stderr, "usage: icarus_cli <map|run> [--seed N] [--out FILE] [--data DIR]\n");
    return 1;
}
