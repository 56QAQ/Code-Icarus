#include "icarus/sim/simulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "icarus/sim/scenario.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
constexpr u32 kSaveVersion = 1;

Vec3i json_vec3i(const Json& j) {
    return {(i32)std::lround(j[0].as_num()), (i32)std::lround(j[1].as_num()), (i32)std::lround(j[2].as_num())};
}

Json config_to_json(const GameConfig& c) {
    Json j = Json::object();
    Json w = Json::object();
    w.set("layout", layout_key(c.world.layout));
    w.set("seed", (double)c.world.seed);
    w.set("cells_x", c.world.cells_x);
    w.set("cells_y", c.world.cells_y);
    w.set("cells_z", c.world.cells_z);
    w.set("island_radius", c.world.island_radius);
    w.set("base_height", c.world.base_height);
    w.set("islet_count", c.world.islet_count);
    j.set("world", w);
    j.set("scenario", c.scenario);
    j.set("residents", c.residents);
    Json drives = Json::array();
    for (auto& d : c.girl_drives) drives.push(d);
    j.set("girl_drives", drives);
    Json pers = Json::array();
    for (auto& p : c.girl_personalities) pers.push(p);
    j.set("girl_personalities", pers);
    j.set("personality_seed", (double)c.personality_seed);
    j.set("lifecycle_idle_ticks", (double)c.lifecycle_idle_ticks);
    return j;
}

GameConfig config_from_json(const Json& j) {
    GameConfig c;
    const Json& w = j["world"];
    c.world.layout = layout_from_key(w.str("layout", "classic"));
    c.world.seed = (u64)w.num("seed", 1);
    c.world.cells_x = w.integer("cells_x", 32);
    c.world.cells_y = w.integer("cells_y", 8);
    c.world.cells_z = w.integer("cells_z", 32);
    c.world.island_radius = w.flt("island_radius", 112.0f);
    c.world.base_height = w.integer("base_height", 160);
    c.world.islet_count = w.integer("islet_count", 4);
    c.scenario = j.str("scenario", "village");
    c.residents = j.integer("residents", 18);
    for (const Json& d : j["girl_drives"].items()) c.girl_drives.push_back(d.as_str());
    for (const Json& p : j["girl_personalities"].items()) c.girl_personalities.push_back(p);
    c.personality_seed = (u64)j.num("personality_seed", 0);
    c.lifecycle_idle_ticks = (Tick)j.num("lifecycle_idle_ticks", 1200);
    return c;
}
}  // namespace

Simulation::Simulation(const Registry& reg)
    : reg_(&reg),
      world_(reg),
      physics_(world_, chronicle_),
      econ_(reg),
      buildings_(world_, econ_, chronicle_),
      farming_(world_, chronicle_),
      nav_(world_),
      agents_(ctx_),
      society_(ctx_),
      decisions_(ctx_),
      ecology_(world_, reg) {
    world_.on_wake = [this](Cell& c, Tick last, Tick now) { on_cell_wake(c, last, now); };
    ctx_.reg = reg_;
    ctx_.world = &world_;
    ctx_.chron = &chronicle_;
    ctx_.physics = &physics_;
    ctx_.econ = &econ_;
    ctx_.buildings = &buildings_;
    ctx_.farming = &farming_;
    ctx_.jobs = &jobs_;
    ctx_.nav = &nav_;
    ctx_.agents = &agents_;
    ctx_.society = &society_;
    ctx_.decisions = &decisions_;
    ctx_.ecology = &ecology_;
    buildings_.load_defs(reg);
    buildings_.set_physics(&physics_);
    farming_.irrigation_bonus = [this](u16 polity) { return (int)society_.tech_effect(polity, "irrigation_radius"); };
}

Simulation::~Simulation() = default;

void Simulation::new_game(const GameConfig& cfg) {
    cfg_ = cfg;
    // The world begins on the morning of the first day.
    tick_ = kTicksPerHour * 8;
    world_.init(cfg.world);
    world_.set_now(tick_);
    chronicle_.clear();
    chronicle_.set_now(tick_);
    physics_.reset(hash_combine(cfg.world.seed, 0xF1));
    econ_.reset();
    buildings_.reset();
    farming_.reset();
    jobs_.reset();
    u64 pseed = cfg.personality_seed ? cfg.personality_seed : hash_combine(cfg.world.seed, 0xB0);
    agents_.reset(hash_combine(cfg.world.seed, 0xA9));
    society_.reset(hash_combine(cfg.world.seed, 0x50));
    decisions_.reset(hash_combine(cfg.world.seed, 0xDE));
    ecology_.reset(hash_combine(cfg.world.seed, 0xEC));
    scenario_rng_.seed(pseed, 0x5CE7);
    ctx_.now = tick_;
    econ_.set_now(tick_);
    admin_queue_.clear();
    const IslandFeatures& f = world_.gen().features();
    for (const Vec3i& sp : f.springs)
        if (sp.y > 0) physics_.add_spring(sp);
    Event e;
    e.type = EventType::Info;
    e.severity = 3;
    e.text = "空岛纪元开始";
    chronicle_.emit(std::move(e));
    if (cfg.scenario == "village") build_village_scenario(ctx_, cfg_, scenario_rng_);
    dispatch_changes();
    fx_.clear();  // building the starting village is not an event to animate
}

void Simulation::on_cell_wake(Cell& c, Tick last, Tick now) {
    // Slow processes of dormant cells are summarised by systems that track them (crops,
    // regrowth). The cell itself only needs its timestamp updated, which World does.
    (void)c;
    (void)last;
    (void)now;
}

void Simulation::dispatch_changes() {
    std::vector<VoxelChange> changes;
    changes.swap(world_.changes());
    if (changes.empty()) return;
    note_fx(changes);
    physics_.on_changes(changes);
    buildings_.on_changes(changes);
    nav_.on_changes(changes);
}

void Simulation::note_fx(const std::vector<VoxelChange>& changes) {
    // Granular matter sliding cube by cube and water flowing are shown by the world
    // itself; breaking and landing solids get a burst of chips or dust.
    constexpr size_t kMaxFx = 4096;
    for (const VoxelChange& c : changes) {
        if (fx_.size() >= kMaxFx) break;
        const MatId b = vmat(c.before), a = vmat(c.after);
        if (b == a) continue;
        const Material& mb = reg_->mat(b);
        const Material& ma = reg_->mat(a);
        // What burns away falls as ash, not as chips of what it was.
        if (mb.solid && !mb.fluid && !mb.granular && !ma.solid)
            fx_.push_back({VisualFx::Break, c.p, vburning(c.before) ? reg_->m().ash : b});
        else if (!mb.solid && ma.solid && !ma.granular && !ma.fluid) fx_.push_back({VisualFx::Land, c.p, a});
        // Sand or gravel settling on firm ground raises a little dust.
        else if (!mb.solid && ma.granular && reg_->mat(vmat(world_.peek(c.p - Vec3i{0, 1, 0}))).solid &&
                 !reg_->mat(vmat(world_.peek(c.p - Vec3i{0, 1, 0}))).granular)
            fx_.push_back({VisualFx::Land, c.p, a});
    }
}

void Simulation::step() {
    auto t0 = std::chrono::steady_clock::now();
    world_.set_now(tick_);
    chronicle_.set_now(tick_);

    if (!admin_queue_.empty()) {
        std::vector<AdminCommand> q;
        q.swap(admin_queue_);
        for (const AdminCommand& c : q) apply_admin(c);
    }
    ctx_.now = tick_;
    econ_.set_now(tick_);
    dispatch_changes();
    auto t1 = std::chrono::steady_clock::now();
    physics_.step(tick_);
    dispatch_changes();
    farming_.step(tick_, agents_.rng());
    auto t2 = std::chrono::steady_clock::now();
    agents_.step(tick_);
    physics_.damage_queue().clear();
    dispatch_changes();
    auto t3 = std::chrono::steady_clock::now();
    society_.step(tick_);
    dispatch_changes();
    auto t4 = std::chrono::steady_clock::now();
    decisions_.step(tick_);
    dispatch_changes();
    auto t4b = std::chrono::steady_clock::now();

    if (tick_ % kTicksPerDay == kTicksPerDay / 2 && tick_ > 0) {
        ecology_.daily(tick_, buildings_);
        dispatch_changes();
    }
    if (tick_ % 100 == 0) world_.update_lifecycle(tick_, cfg_.lifecycle_idle_ticks);

    auto t5 = std::chrono::steady_clock::now();
    profile_.physics_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
    profile_.agents_us = std::chrono::duration<double, std::micro>(t3 - t2).count();
    profile_.society_us = std::chrono::duration<double, std::micro>(t4 - t3).count();
    profile_.decisions_us = std::chrono::duration<double, std::micro>(t4b - t4).count();
    profile_.total_us = std::chrono::duration<double, std::micro>(t5 - t0).count();
    ++tick_;
}

EventId Simulation::apply_admin(const AdminCommand& cmd) {
    const CoreMats& M = reg_->m();
    const Json& p = cmd.params;
    Event e;
    e.type = EventType::AdminAction;
    e.severity = 3;
    e.data = p;
    e.data.set("action", cmd.type);
    Vec3i pos = p.has("pos") ? json_vec3i(p["pos"]) : Vec3i{};
    e.pos = pos;
    float radius = p.flt("radius", 2.0f);

    if (cmd.type == "dig") {
        e.text = strfmt("管理员挖除了 %s 附近的物质（半径 %.0f）", pos.str().c_str(), radius);
        EventId id = chronicle_.emit(std::move(e));
        int r = (int)std::ceil(radius);
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    if ((float)(dx * dx + dy * dy + dz * dz) > radius * radius) continue;
                    Vec3i q = pos + Vec3i{dx, dy, dz};
                    if (world_.in_bounds(q) && world_.mat(q) != M.air) world_.set(q, make_voxel(M.air), id);
                }
        return id;
    }
    if (cmd.type == "place") {
        std::string mk = p.str("material", "stone");
        MatId m = reg_->has_mat(mk) ? reg_->mat_id(mk) : M.stone;
        e.text = strfmt("管理员在 %s 创造了%s（半径 %.0f）", pos.str().c_str(), reg_->mat(m).name.c_str(), radius);
        EventId id = chronicle_.emit(std::move(e));
        int r = (int)std::ceil(radius);
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    if ((float)(dx * dx + dy * dy + dz * dz) > radius * radius) continue;
                    Vec3i q = pos + Vec3i{dx, dy, dz};
                    if (!world_.in_bounds(q)) continue;
                    Voxel cur = world_.get(q);
                    if (vmat(cur) != M.air && !reg_->mat(vmat(cur)).fluid) continue;
                    world_.set(q, reg_->mat(m).fluid ? make_voxel(m, kFluidFull) : make_voxel(m), id);
                }
        return id;
    }
    if (cmd.type == "meteor") {
        e.severity = 4;
        e.text = strfmt("管理员召唤了陨石，目标 %s（半径 %.0f）", pos.str().c_str(), radius);
        EventId id = chronicle_.emit(std::move(e));
        physics_.spawn_meteor(Vec3f((float)pos.x + 0.5f, (float)pos.y + 0.5f, (float)pos.z + 0.5f), radius, id);
        return id;
    }
    if (cmd.type == "explode") {
        e.severity = 4;
        e.text = strfmt("管理员在 %s 引发了爆炸（半径 %.0f）", pos.str().c_str(), radius);
        EventId id = chronicle_.emit(std::move(e));
        physics_.explode(Vec3f((float)pos.x + 0.5f, (float)pos.y + 0.5f, (float)pos.z + 0.5f), radius, id, false);
        return id;
    }
    if (cmd.type == "rain") {
        // Clouds gather over the island: rain refills water surfaces and douses open
        // fires through the ordinary weather rules.
        const int hours = std::clamp(p.integer("hours", 6), 1, 48);
        e.text = strfmt("管理员唤来了降雨（约 %d 小时）", hours);
        EventId id = chronicle_.emit(std::move(e));
        physics_.start_rain((Tick)hours * kTicksPerHour);
        return id;
    }
    if (cmd.type == "ignite") {
        e.text = strfmt("管理员在 %s 点燃了火焰", pos.str().c_str());
        EventId id = chronicle_.emit(std::move(e));
        int r = (int)std::ceil(radius);
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    Vec3i q = pos + Vec3i{dx, dy, dz};
                    if (world_.in_bounds(q)) physics_.ignite(q, id);
                }
        Event f;
        f.type = EventType::FireStarted;
        f.severity = 3;
        f.pos = pos;
        f.causes[0] = id;
        f.text = "火灾开始";
        chronicle_.emit(std::move(f));
        return id;
    }
    if (cmd.type == "whisper") {
        // A voice in a magical girl's ear: nudges her next decision toward an option.
        EntityId gid = (EntityId)p.num("girl");
        std::string opt = p.str("option");
        Character* g = agents_.get(gid);
        if (!g || !g->is_girl() || opt.empty()) {
            e.text = "低语没有找到对象";
            e.severity = 0;
            return chronicle_.emit(std::move(e));
        }
        e.actor = gid;
        e.polity = g->polity;
        e.severity = 2;
        e.text = strfmt("管理员向%s低语：「%s」", g->name.c_str(), p.str("label", opt).c_str());
        EventId id = chronicle_.emit(std::move(e));
        decisions_.whisper(gid, opt, id);
        return id;
    }
    // --- Miracles: powers over people and minds. Like everything the god does, they
    // are recorded and act only through the shared world systems: blessed food is
    // real items in the ledger, fear makes residents run, a whisper shifts what a girl
    // values in her own decisions.
    const Vec3f center((float)pos.x + 0.5f, (float)pos.y + 0.5f, (float)pos.z + 0.5f);
    auto people_near = [&](float r) {
        std::vector<Character*> out;
        for (const auto& cp : agents_.all())
            if (cp && cp->alive && !cp->departed && cp->pos.dist_sq(center) <= r * r) out.push_back(cp.get());
        return out;
    };
    if (cmd.type == "bless_food") {
        ItemId item = reg_->find_item(p.str("item", "grain"));
        if (item == kNoItem) item = reg_->find_item("grain");
        const int n = std::clamp(p.integer("amount", 40), 1, 1000);
        // Gifts land where people can reach them (not on a roof or a ledge).
        Vec3i at = pos;
        if (!agents_.nearest_walkable(pos, 8, at)) {
            at = pos;
            while (world_.in_bounds(at) && world_.mat(at) != M.air && at.y < pos.y + 8) at.y++;
        }
        e.pos = at;
        e.text = strfmt("天降粮食：%d 份%s落在 %s", n, reg_->item(item).name.c_str(), at.str().c_str());
        EventId id = chronicle_.emit(std::move(e));
        econ_.add(econ_.pile_at(at), item, n, "admin_bless");
        for (Character* c : people_near(14.0f)) c->remember(tick_, MemoryKind::Blessed, kNoEntity, 0.3f, id);
        return id;
    }
    if (cmd.type == "inspire" || cmd.type == "terrify") {
        const bool inspire = cmd.type == "inspire";
        const std::vector<Character*> hit = people_near(std::clamp(radius, 1.0f, 30.0f));
        e.text = inspire ? strfmt("神迹显现：%zu 人受到鼓舞", hit.size()) : strfmt("天威降临：%zu 人陷入恐惧", hit.size());
        EventId id = chronicle_.emit(std::move(e));
        for (Character* c : hit) {
            if (inspire) {
                c->fear = std::max(0.0f, c->fear - 0.5f);
                c->stress = std::max(0.0f, c->stress - 0.3f);
                c->remember(tick_, MemoryKind::Blessed, kNoEntity, 0.5f, id);
            } else {
                c->fear = 1.0f;
                c->remember(tick_, MemoryKind::Cursed, kNoEntity, -0.4f, id);
                c->next_think = tick_;
            }
        }
        if (!inspire) agents_.add_danger(center, radius + 4.0f);
        return id;
    }
    if (cmd.type == "heal") {
        // Only the living: no god here brings back the dead.
        const std::vector<Character*> hit = people_near(std::clamp(radius, 1.0f, 20.0f));
        e.text = strfmt("治愈之光：%zu 人的伤势痊愈", hit.size());
        EventId id = chronicle_.emit(std::move(e));
        for (Character* c : hit) {
            int missing = c->body.total_voxels() - c->body.total_alive();
            c->body.regrow(missing, c->look, true);
            c->body.bleeding = 0.0f;
            c->body.vitality = 1.0f;
            c->body.version++;
            c->remember(tick_, MemoryKind::Healed, kNoEntity, 0.4f, id);
        }
        return id;
    }
    if (cmd.type == "smite") {
        e.severity = 4;
        const float r = std::clamp(radius, 1.0f, 6.0f);
        const std::vector<Character*> hit = people_near(r);
        e.text = hit.empty() ? std::string("天雷落下") : strfmt("天雷落下，击中 %zu 人", hit.size());
        EventId id = chronicle_.emit(std::move(e));
        for (Character* c : hit) agents_.damage(*c, 0.8f, -1, "被天雷击中", id);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 2; ++dy)
                    if (world_.in_bounds(pos + Vec3i{dx, dy, dz})) physics_.ignite(pos + Vec3i{dx, dy, dz}, id);
        for (Character* c : people_near(16.0f)) {
            c->fear = std::min(1.0f, c->fear + 0.4f);
            if (c->alive) c->remember(tick_, MemoryKind::Cursed, kNoEntity, -0.2f, id);
        }
        agents_.add_danger(center, r + 5.0f);
        return id;
    }
    if (cmd.type == "enlighten") {
        const u16 pid = (u16)p.integer("polity", 1);
        const float pts = std::clamp(p.flt("points", 40.0f), 1.0f, 500.0f);
        const Polity* pol = society_.polity(pid);
        const std::string key = society_.grant_target(pid);
        const Json* t = key.empty() ? nullptr : society_.tech(key);
        e.polity = pid;
        e.text = strfmt("管理员向「%s」降下启示", pol ? pol->name.c_str() : "?") +
                 (t ? strfmt("：%s +%.0f 研究点", t->str("name").c_str(), pts) : std::string("，却无人能领会"));
        EventId id = chronicle_.emit(std::move(e));
        society_.grant_research(pid, key, pts, id);
        return id;
    }
    if (cmd.type == "empower" || cmd.type == "whisper_value") {
        Character* g = agents_.get((EntityId)p.num("girl"));
        if (!g || !g->is_girl() || !g->alive) {
            e.text = "神迹没有找到对象";
            e.severity = 0;
            return chronicle_.emit(std::move(e));
        }
        e.actor = g->id;
        e.polity = g->polity;
        if (cmd.type == "empower") {
            e.text = strfmt("管理员赐予%s力量", g->name.c_str());
            EventId id = chronicle_.emit(std::move(e));
            g->girl->xp += 40.0f * (float)g->girl->level;
            g->girl->mana = 1.0f;
            g->remember(tick_, MemoryKind::Blessed, kNoEntity, 0.5f, id);
            decisions_.level_ups(*g, id);
            return id;
        }
        int feature = -1;
        for (int f = 0; f < kFeatureCount; ++f)
            if (p.str("feature") == feature_key(f)) feature = f;
        if (feature < 0) {
            e.text = "低语没有内容";
            e.severity = 0;
            return chronicle_.emit(std::move(e));
        }
        const float delta = p.flt("dir", 1.0f) >= 0.0f ? 0.8f : -0.8f;
        e.severity = 2;
        e.text = strfmt("管理员向%s低语：%s「%s」", g->name.c_str(), delta > 0 ? "更在意" : "别再在意", feature_name_zh(feature));
        EventId id = chronicle_.emit(std::move(e));
        decisions_.whisper_value(g->id, feature, delta, id);
        return id;
    }
    e.text = "未知的管理员指令: " + cmd.type;
    e.severity = 0;
    return chronicle_.emit(std::move(e));
}

std::vector<u8> Simulation::save() const {
    BinWriter w;
    w.raw("ICARUS01", 8);
    w.u32v(kSaveVersion);
    w.u64v(reg_->content_hash());
    w.str(config_to_json(cfg_).dump());
    w.u64v(tick_);
    world_.save(w);
    chronicle_.save(w);
    physics_.save(w);
    econ_.save(w);
    buildings_.save(w);
    farming_.save(w);
    jobs_.save(w);
    agents_.save(w);
    society_.save(w);
    decisions_.save(w);
    w.u64v(scenario_rng_.state());
    w.u64v(scenario_rng_.inc());
    size_t s = w.begin_section("ADMQ");
    w.varu(admin_queue_.size());
    for (const AdminCommand& c : admin_queue_) {
        w.str(c.type);
        w.str(c.params.dump());
    }
    w.end_section(s);
    ecology_.save(w);  // added later: saves without it load with an empty ecology
    return std::move(w.data_mut());
}

void Simulation::load(const std::vector<u8>& data) {
    fx_.clear();
    BinReader r(data);
    char magic[8];
    for (char& c : magic) c = (char)r.u8v();
    if (std::string(magic, 8) != "ICARUS01") throw BinError("not an Icarus save file");
    u32 ver = r.u32v();
    if (ver != kSaveVersion) throw BinError("unsupported save version " + std::to_string(ver));
    u64 content = r.u64v();
    if (content != reg_->content_hash()) log_warn("save was created with different rule data; continuing");
    cfg_ = config_from_json(Json::parse(r.str()));
    tick_ = r.u64v();
    world_.load(r);
    world_.set_now(tick_);
    chronicle_.load(r);
    chronicle_.set_now(tick_);
    physics_.load(r);
    econ_.load(r);
    buildings_.load(r);
    farming_.load(r);
    jobs_.load(r);
    agents_.load(r);
    society_.load(r);
    decisions_.load(r);
    {
        u64 st = r.u64v(), inc = r.u64v();
        scenario_rng_.set_raw(st, inc);
    }
    ctx_.now = tick_;
    econ_.set_now(tick_);
    BinReader q = r.section("ADMQ");
    admin_queue_.clear();
    u64 n = q.varu();
    for (u64 i = 0; i < n; ++i) {
        AdminCommand c;
        c.type = q.str();
        c.params = Json::parse(q.str());
        admin_queue_.push_back(std::move(c));
    }
    if (!r.at_end()) ecology_.load(r);
    else ecology_.reset(hash_combine(cfg_.world.seed, 0xEC));
    world_.changes().clear();
}

u64 Simulation::state_hash() const {
    u64 h = hash_combine(tick_, world_.state_hash());
    h = hash_combine(h, chronicle_.hash());
    h = hash_combine(h, physics_.hash());
    h = hash_combine(h, econ_.hash());
    h = hash_combine(h, buildings_.hash());
    h = hash_combine(h, farming_.hash());
    h = hash_combine(h, jobs_.hash());
    h = hash_combine(h, agents_.hash());
    h = hash_combine(h, society_.hash());
    h = hash_combine(h, decisions_.hash());
    h = hash_combine(h, ecology_.hash());
    return h;
}

}  // namespace icarus
