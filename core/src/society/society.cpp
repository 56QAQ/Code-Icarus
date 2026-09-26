#include "icarus/society/society.h"

#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/economy/farming.h"
#include "icarus/sim/clock.h"
#include "icarus/util/log.h"

namespace icarus {

const char* crisis_name_zh(CrisisKind k) {
    switch (k) {
        case CrisisKind::Food: return "粮食短缺";
        case CrisisKind::Water: return "缺水";
        case CrisisKind::Logistics: return "交通中断";
        case CrisisKind::Unrest: return "民众不满";
        case CrisisKind::Disaster: return "灾害";
        case CrisisKind::War: return "战争";
        case CrisisKind::Housing: return "居所不足";
        default: return "?";
    }
}

Society::Society(SimContext& ctx) : ctx_(ctx) {}

void Society::reset(u64 seed) {
    rng_.seed(seed, 0x5061E7);
    polities_.assign(1, Polity{});
    projects_.assign(1, Project{});
}

u16 Society::create_polity(const std::string& name, u32 color, u16 parent) {
    Polity p;
    p.id = (u16)polities_.size();
    p.alive = true;
    p.name = name;
    p.color = color;
    p.founded = ctx_.now;
    p.parent = parent;
    p.techs = {"stone_tools", "fire", "gathering", "farming", "thatching"};
    polities_.push_back(p);
    return p.id;
}

std::string Society::title(u16 id) const {
    const Polity* p = polity(id);
    if (!p) return "";
    const Character* r = ctx_.agents->get(p->ruler);
    if (!r || !r->is_girl()) return p->name;
    std::string dn = r->girl->drive;
    for (const Json& d : ctx_.reg->doc("drives")["drives"].items())
        if (d.str("key") == r->girl->drive) dn = d.str("name", dn);
    return dn + "的文明，" + p->name;
}

void Society::set_ruler(u16 id, EntityId girl, const std::string& how, EventId cause) {
    Polity* p = polity(id);
    if (!p) return;
    std::string old_title = title(id);
    if (!p->reigns.empty() && p->reigns.back().to == 0) p->reigns.back().to = ctx_.now;
    p->ruler = girl;
    Character* g = ctx_.agents->get(girl);
    if (g && g->girl) g->girl->role = "ruler";
    Reign r;
    r.ruler = girl;
    r.ruler_name = g ? g->name : "";
    r.drive = (g && g->girl) ? g->girl->drive : "";
    r.from = ctx_.now;
    r.how = how;
    p->reigns.push_back(r);
    if (how != "founding") {
        Event e;
        e.type = EventType::RulerChanged;
        e.severity = 5;
        e.polity = id;
        e.actor = girl;
        e.causes[0] = cause;
        e.text = strfmt("「%s」更名为「%s」", old_title.c_str(), title(id).c_str());
        ctx_.chron->emit(std::move(e));
    }
}

std::vector<StoreId> Society::public_stores(u16 id) const {
    std::vector<StoreId> out;
    for (const Store& s : ctx_.econ->stores()) {
        if (!s.alive || s.polity != id) continue;
        if (s.kind != StoreKind::Stockpile && s.kind != StoreKind::Workshop) continue;
        if (s.building) {
            const Building* b = ctx_.buildings->get(s.building);
            if (b && !b->functional) continue;  // a ruined storehouse can't be used
        }
        out.push_back(s.id);
    }
    return out;
}

float Society::public_food(u16 id) const {
    float n = 0;
    for (StoreId s : public_stores(id)) n += ctx_.econ->food_nutrition_in(s);
    return n;
}

u32 Society::add_project(Project p) {
    p.id = (u32)projects_.size();
    p.alive = true;
    p.created = ctx_.now;
    projects_.push_back(p);
    Event e;
    e.type = EventType::ProjectStarted;
    e.severity = 2;
    e.polity = p.polity;
    e.actor = p.sponsor;
    e.pos = p.target;
    e.causes[0] = p.cause;
    e.text = "开工：" + p.title;
    ctx_.chron->emit(std::move(e));
    return p.id;
}

void Society::finish_project(u32 id, bool success, EventId cause) {
    Project* p = project(id);
    if (!p || p->status != 0) return;
    p->status = success ? 1 : 2;
    p->progress = success ? 1.0f : p->progress;
    Event e;
    e.type = success ? EventType::ProjectCompleted : EventType::ProjectAbandoned;
    e.severity = 3;
    e.polity = p->polity;
    e.actor = p->sponsor;
    e.pos = p->target;
    e.causes[0] = cause ? cause : p->cause;
    e.text = (success ? "竣工：" : "放弃：") + p->title;
    ctx_.chron->emit(std::move(e));
}

void Society::step(Tick now) {
    if (now % kTicksPerHour == 0) hourly(now);
    if (now % kTicksPerDay == 0) daily(now);
}

void Society::hourly(Tick now) {
    for (auto& p : polities_) {
        if (!p.alive) continue;
        compute_stats(p);
        update_support(p);
        update_crises(p);
        p.history.push_back(p.stats);
        if (p.history.size() > 24 * 60) p.history.erase(p.history.begin());
    }
    update_projects();
    (void)now;
}

void Society::daily(Tick now) {
    ctx_.econ->spoil(rng_);
    ctx_.agents->day = {};
    (void)now;
}

void Society::compute_stats(Polity& p) {
    PolityStats s;
    s.tick = ctx_.now;
    int residents = 0, fed = 0, watered = 0;
    float mood = 0, sup = 0;
    for (const auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed || cp->polity != p.id) continue;
        const Character& c = *cp;
        s.population++;
        if (c.is_girl()) {
            s.girls++;
            continue;
        }
        residents++;
        if (c.needs.food > 0.3f) fed++;
        if (c.needs.water > 0.3f) watered++;
        mood += c.mood;
        sup += c.support_for(p.ruler);
        if (c.task.type == TaskType::Protest && c.task.step == 2) s.protesters++;
    }
    s.food_stock = public_food(p.id);
    float daily_need = std::max(1.0f, (float)s.population * ctx_.agents->tune.food_per_day);
    s.food_days = s.food_stock / daily_need;
    s.food_access = residents ? (float)fed / (float)residents : 1.0f;
    s.water_access = residents ? (float)watered / (float)residents : 1.0f;
    s.mood = residents ? mood / (float)residents : 0.5f;
    s.ruler_support = residents ? sup / (float)residents : 0.0f;
    s.deaths = p.deaths_total;
    s.harvest_today = ctx_.agents->day.harvested;
    float protest_share = residents ? (float)s.protesters / (float)residents : 0.0f;
    s.stability = clampv(0.5f * s.mood + 0.35f * (0.5f + 0.5f * s.ruler_support) + 0.15f * s.food_access - protest_share, 0.0f, 1.0f);
    for (auto& r : p.research) s.knowledge += r.second;
    s.knowledge += (float)p.techs.size();
    p.stats = s;
}

void Society::update_support(Polity& p) {
    // Hourly drift of every resident's support for every magical girl of the polity.
    std::vector<Character*> girls;
    for (auto& cp : ctx_.agents->all())
        if (cp && cp->alive && !cp->departed && cp->is_girl() && cp->polity == p.id) girls.push_back(cp.get());
    const Policies& pol = p.policies;
    const Crisis* food = p.crisis(CrisisKind::Food);
    const Crisis* water = p.crisis(CrisisKind::Water);
    for (auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed || cp->is_girl() || cp->polity != p.id) continue;
        Character& c = *cp;
        for (Character* g : girls) {
            float& s = c.support_ref(g->id);
            float d = 0;
            if (g->id == p.ruler) {
                d += 0.035f * (c.mood - 0.5f);                                       // credit / blame
                d -= 0.03f * std::max(0.0f, 1.0f - pol.ration) * (0.6f + c.pers.altruism);  // rationing
                d -= 0.035f * pol.punishment * (1.0f - c.pers.conformity);          // harshness
                d += 0.01f * pol.punishment * c.pers.conformity;                    // order-lovers
                d -= 0.04f * pol.requisition * (0.5f + c.pers.ambition);
                if (food && food->active) d -= 0.02f * food->severity * (1.0f - 0.5f * c.pers.conformity);
                if (water && water->active) d -= 0.02f * water->severity;
                d += 0.01f * pol.wage * c.pers.ambition;
            } else {
                d += 0.004f * (float)g->girl->level;
            }
            // Personal memories about this girl.
            for (const Memory& m : c.memories)
                if (m.subject == g->id && ctx_.now - m.tick < kTicksPerHour) d += m.valence * 0.3f;
            s = clampv(s * 0.998f + d, -1.0f, 1.0f);
        }
    }
}

void Society::update_crises(Polity& p) {
    auto recent_cause = [&](std::initializer_list<EventType> types, Tick window) -> EventId {
        const auto& ev = ctx_.chron->events();
        for (auto it = ev.rbegin(); it != ev.rend(); ++it) {
            if (ctx_.now - it->tick > window) break;
            for (EventType t : types)
                if (it->type == t) return it->id;
        }
        return 0;
    };
    auto declare = [&](CrisisKind k, float sev, EventId cause, const std::string& text) {
        Crisis* c = p.crisis(k);
        if (!c) {
            p.crises.push_back(Crisis{});
            c = &p.crises.back();
            c->kind = k;
        }
        if (c->active) {
            c->severity = sev;
            return;
        }
        c->active = true;
        c->severity = sev;
        c->since = ctx_.now;
        c->cause = cause;
        c->decision = 0;
        Event e;
        e.type = k == CrisisKind::Food ? EventType::Shortage
                 : k == CrisisKind::Logistics ? EventType::LogisticsDisrupted
                 : k == CrisisKind::Unrest ? EventType::Protest : EventType::Shortage;
        e.severity = 4;
        e.polity = p.id;
        e.causes[0] = cause;
        e.text = text;
        e.data.set("crisis", crisis_name_zh(k));
        c->event = ctx_.chron->emit(std::move(e));
    };
    auto resolve = [&](CrisisKind k, const std::string& text) {
        Crisis* c = p.crisis(k);
        if (!c || !c->active) return;
        c->active = false;
        Event e;
        e.type = EventType::ShortageResolved;
        e.severity = 3;
        e.polity = p.id;
        e.causes[0] = c->event;
        e.text = text;
        ctx_.chron->emit(std::move(e));
    };
    const PolityStats& s = p.stats;
    const Tick day = kTicksPerDay;

    // Logistics: a broken bridge or many failed routes.
    const Building* broken_bridge = nullptr;
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.is_bridge && (b.complete || b.completed_tick > 0) && !b.functional && b.polity == p.id)
            broken_bridge = &b;  // broken, or still under repair
    if (broken_bridge) {
        declare(CrisisKind::Logistics, 0.8f, broken_bridge->last_event, "桥梁中断，两岸的物流与通勤受阻");
    } else if (ctx_.agents->day.path_failures > std::max(10, s.population * 3)) {
        declare(CrisisKind::Logistics, 0.5f,
                recent_cause({EventType::StructureDestroyed, EventType::Collapse, EventType::MeteorImpact}, day),
                "道路受阻，许多居民无法抵达目的地");
    } else {
        resolve(CrisisKind::Logistics, "交通恢复畅通");
    }

    // Food.
    const Crisis* lg = p.crisis(CrisisKind::Logistics);
    if (s.food_days < 0.8f || s.food_access < 0.65f) {
        float sev = clampv(std::max((1.2f - s.food_days) / 1.2f, (0.85f - s.food_access) / 0.85f), 0.1f, 1.0f);
        EventId cause = recent_cause({EventType::CropFailure, EventType::WaterSourceLost, EventType::StructureDestroyed,
                                      EventType::MeteorImpact, EventType::FireStarted},
                                     day * 3);
        if (lg && lg->active) cause = lg->event;
        declare(CrisisKind::Food, sev, cause,
                s.food_access < 0.95f ? strfmt("粮食短缺：公共存粮仅够 %.1f 天，%.0f%% 的居民吃不饱", s.food_days, (1.0f - s.food_access) * 100.0f)
                                      : strfmt("粮食短缺：公共存粮仅够 %.1f 天", s.food_days));
    } else if (s.food_days > 1.5f && s.food_access > 0.85f) {
        resolve(CrisisKind::Food, "粮食短缺缓解");
    }

    // Water.
    if (s.water_access < 0.6f) {
        declare(CrisisKind::Water, clampv((0.8f - s.water_access) / 0.8f, 0.1f, 1.0f),
                recent_cause({EventType::WaterSourceLost, EventType::StructureDestroyed}, day * 3),
                strfmt("缺水：%.0f%% 的居民严重口渴", (1.0f - s.water_access) * 100.0f));
    } else if (s.water_access > 0.85f) {
        resolve(CrisisKind::Water, "饮水恢复");
    }

    // Unrest.
    int residents = s.population - s.girls;
    if (residents > 0 && s.protesters >= std::max(3, residents / 5)) {
        declare(CrisisKind::Unrest, clampv((float)s.protesters / (float)residents * 2.0f, 0.2f, 1.0f),
                recent_cause({EventType::Shortage, EventType::PolicyChanged, EventType::Punishment}, day * 2),
                strfmt("%d 名居民聚集在议事厅前抗议", s.protesters));
    } else if (s.protesters == 0 && s.mood > 0.45f) {
        resolve(CrisisKind::Unrest, "抗议平息");
    }
}

void Society::update_projects() {
    for (auto& pr : projects_) {
        if (!pr.alive || pr.status != 0) continue;
        if (pr.building) {
            const Building* b = ctx_.buildings->get(pr.building);
            if (!b) {
                finish_project(pr.id, false, 0);
                continue;
            }
            pr.progress = b->integrity;
            if (b->complete && (b->functional || b->integrity > 0.95f)) finish_project(pr.id, true, b->last_event);
        }
    }
}

// ------------------------------------------------------------------------------ persistence

namespace {
void save_stats(BinWriter& w, const PolityStats& s) {
    w.u64v(s.tick);
    w.vari(s.population);
    w.vari(s.girls);
    w.f32(s.food_stock);
    w.f32(s.food_days);
    w.f32(s.food_access);
    w.f32(s.water_access);
    w.f32(s.mood);
    w.f32(s.ruler_support);
    w.f32(s.stability);
    w.f32(s.knowledge);
    w.vari(s.protesters);
    w.vari(s.deaths);
    w.vari(s.harvest_today);
    w.f32(s.ecology);
}
PolityStats load_stats(BinReader& r) {
    PolityStats s;
    s.tick = r.u64v();
    s.population = (int)r.vari();
    s.girls = (int)r.vari();
    s.food_stock = r.f32();
    s.food_days = r.f32();
    s.food_access = r.f32();
    s.water_access = r.f32();
    s.mood = r.f32();
    s.ruler_support = r.f32();
    s.stability = r.f32();
    s.knowledge = r.f32();
    s.protesters = (int)r.vari();
    s.deaths = (int)r.vari();
    s.harvest_today = (int)r.vari();
    s.ecology = r.f32();
    return s;
}
}  // namespace

void Society::save(BinWriter& w) const {
    size_t sec = w.begin_section("SOCI");
    w.u64v(rng_.state());
    w.u64v(rng_.inc());
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        const Polity& p = polities_[i];
        w.boolean(p.alive);
        w.str(p.name);
        w.u32v(p.color);
        w.u32v(p.ruler);
        w.f32(p.culture.collectivism);
        w.f32(p.culture.militarism);
        w.f32(p.culture.tradition);
        w.f32(p.culture.openness);
        const Policies& q = p.policies;
        w.f32(q.ration);
        w.f32(q.punishment);
        w.f32(q.work_hours);
        w.f32(q.requisition);
        w.u8v(q.distribution);
        w.f32(q.pri_food);
        w.f32(q.pri_build);
        w.f32(q.pri_gather);
        w.f32(q.pri_research);
        w.f32(q.pri_military);
        w.f32(q.wage);
        w.str(q.research);
        w.u32v(p.seat);
        w.u64v(p.founded);
        w.u16v(p.parent);
        w.varu(p.reigns.size());
        for (auto& r : p.reigns) {
            w.u32v(r.ruler);
            w.str(r.ruler_name);
            w.str(r.drive);
            w.u64v(r.from);
            w.u64v(r.to);
            w.str(r.how);
        }
        w.varu(p.attitude.size());
        for (auto& a : p.attitude) {
            w.u16v(a.first);
            w.f32(a.second);
        }
        w.varu(p.at_war.size());
        for (u16 x : p.at_war) w.u16v(x);
        w.varu(p.techs.size());
        for (auto& t : p.techs) w.str(t);
        w.varu(p.research.size());
        for (auto& r : p.research) {
            w.str(r.first);
            w.f32(r.second);
        }
        w.varu(p.crises.size());
        for (auto& c : p.crises) {
            w.u8v((u8)c.kind);
            w.f32(c.severity);
            w.u64v(c.since);
            w.u32v(c.event);
            w.u32v(c.cause);
            w.u32v(c.decision);
            w.boolean(c.active);
        }
        save_stats(w, p.stats);
        w.varu(p.history.size());
        for (auto& h : p.history) save_stats(w, h);
        w.vari(p.deaths_total);
        w.u64v(p.forage_until);
    }
    w.varu(projects_.size());
    for (size_t i = 1; i < projects_.size(); ++i) {
        const Project& p = projects_[i];
        w.boolean(p.alive);
        w.u16v(p.polity);
        w.str(p.kind);
        w.str(p.title);
        w.u32v(p.building);
        w.vec3i(p.target);
        w.f32(p.priority);
        w.u64v(p.created);
        w.u32v(p.sponsor);
        w.u32v(p.cause);
        w.u32v(p.decision);
        w.u8v(p.status);
        w.str(p.params.dump());
        w.f32(p.progress);
    }
    w.end_section(sec);
}

void Society::load(BinReader& outer) {
    BinReader r = outer.section("SOCI");
    u64 st = r.u64v(), inc = r.u64v();
    rng_.set_raw(st, inc);
    u64 n = r.varu();
    polities_.assign((size_t)n, Polity{});
    for (size_t i = 1; i < (size_t)n; ++i) {
        Polity& p = polities_[i];
        p.id = (u16)i;
        p.alive = r.boolean();
        p.name = r.str();
        p.color = r.u32v();
        p.ruler = r.u32v();
        p.culture.collectivism = r.f32();
        p.culture.militarism = r.f32();
        p.culture.tradition = r.f32();
        p.culture.openness = r.f32();
        Policies& q = p.policies;
        q.ration = r.f32();
        q.punishment = r.f32();
        q.work_hours = r.f32();
        q.requisition = r.f32();
        q.distribution = r.u8v();
        q.pri_food = r.f32();
        q.pri_build = r.f32();
        q.pri_gather = r.f32();
        q.pri_research = r.f32();
        q.pri_military = r.f32();
        q.wage = r.f32();
        q.research = r.str();
        p.seat = r.u32v();
        p.founded = r.u64v();
        p.parent = r.u16v();
        u64 nr = r.varu();
        for (u64 k = 0; k < nr; ++k) {
            Reign rg;
            rg.ruler = r.u32v();
            rg.ruler_name = r.str();
            rg.drive = r.str();
            rg.from = r.u64v();
            rg.to = r.u64v();
            rg.how = r.str();
            p.reigns.push_back(rg);
        }
        u64 na = r.varu();
        for (u64 k = 0; k < na; ++k) {
            u16 o = r.u16v();
            float a = r.f32();
            p.attitude.push_back({o, a});
        }
        u64 nw = r.varu();
        for (u64 k = 0; k < nw; ++k) p.at_war.push_back(r.u16v());
        u64 nt = r.varu();
        for (u64 k = 0; k < nt; ++k) p.techs.push_back(r.str());
        u64 nrs = r.varu();
        for (u64 k = 0; k < nrs; ++k) {
            std::string key = r.str();
            float v = r.f32();
            p.research.push_back({key, v});
        }
        u64 nc = r.varu();
        for (u64 k = 0; k < nc; ++k) {
            Crisis c;
            c.kind = (CrisisKind)r.u8v();
            c.severity = r.f32();
            c.since = r.u64v();
            c.event = r.u32v();
            c.cause = r.u32v();
            c.decision = r.u32v();
            c.active = r.boolean();
            p.crises.push_back(c);
        }
        p.stats = load_stats(r);
        u64 nh = r.varu();
        for (u64 k = 0; k < nh; ++k) p.history.push_back(load_stats(r));
        p.deaths_total = (int)r.vari();
        p.forage_until = r.u64v();
    }
    u64 np = r.varu();
    projects_.assign((size_t)np, Project{});
    for (size_t i = 1; i < (size_t)np; ++i) {
        Project& p = projects_[i];
        p.id = (u32)i;
        p.alive = r.boolean();
        p.polity = r.u16v();
        p.kind = r.str();
        p.title = r.str();
        p.building = r.u32v();
        p.target = r.vec3i();
        p.priority = r.f32();
        p.created = r.u64v();
        p.sponsor = r.u32v();
        p.cause = r.u32v();
        p.decision = r.u32v();
        p.status = r.u8v();
        std::string pj = r.str();
        p.params = pj.empty() ? Json() : Json::parse(pj);
        p.progress = r.f32();
    }
}

u64 Society::hash() const {
    u64 h = hash_combine(rng_.state(), rng_.inc());
    for (auto& p : polities_) {
        if (!p.alive) continue;
        h = hash_combine(h, p.ruler);
        h = hash_combine(h, (u64)(p.stats.food_stock * 10.0f));
        h = hash_combine(h, (u64)(p.policies.ration * 1000.0f));
    }
    return h;
}

}  // namespace icarus
