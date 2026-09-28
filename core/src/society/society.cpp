#include "icarus/society/society.h"

#include "icarus/sim/physics.h"

#include <algorithm>
#include <cmath>
#include <set>

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
    most_polities_ = 1;
    last_merge_ = unification_ = 0;
    island_history_.clear();
}

std::vector<std::string> Society::start_techs(const std::string& era) const {
    std::vector<std::string> out;
    for (const Json& t : ctx_.reg->doc("techs")["start_eras"][era].items()) out.push_back(t.as_str());
    if (out.empty()) out = {"gathering", "fire", "stone_tools", "hunting", "shelter", "hide_working",
                            "weaving", "farming", "thatching", "storage"};
    return out;
}

u16 Society::create_polity(const std::string& name, u32 color, u16 parent, const std::string& era) {
    Polity p;
    p.id = (u16)polities_.size();
    p.alive = true;
    p.name = name;
    p.color = color;
    p.founded = ctx_.now;
    p.parent = parent;
    p.techs = start_techs(era);
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

bool Society::foraging_band(const Polity& p) const {
    if (!p.has_tech("farming")) return true;
    int plots = 0;
    for (const Farm& f : ctx_.farming->all())
        if (f.alive && f.polity == p.id) plots += (int)f.plots.size();
    // Fields for fewer than half its people: it still lives mostly from what it finds.
    return plots * 2 < std::max(1, p.stats.population);
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
    EventId ev = ctx_.chron->emit(std::move(e));
    // Projects of the polity's own initiative trace back to their start.
    if (!projects_.back().cause) projects_.back().cause = ev;
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
        update_wars(p);
        tidy_pacts(p);
        // A people that has built a hall moves its seat there from the campfire.
        if (const Building* seat = ctx_.buildings->get(p.seat); seat && seat->def == "campfire")
            for (const Building& b : ctx_.buildings->all())
                if (b.alive && b.complete && b.functional && b.polity == p.id && b.def == "hall") {
                    p.seat = b.id;
                    Event e;
                    e.type = EventType::Construction;
                    e.severity = 3;
                    e.polity = p.id;
                    e.pos = b.entrance;
                    e.text = "「" + p.name + "」的魔法少女们从篝火旁迁入了新建的议事厅";
                    ctx_.chron->emit(std::move(e));
                    for (auto& cp : ctx_.agents->all())
                        if (cp && cp->alive && cp->is_girl() && cp->polity == p.id && cp->home == 0) cp->home = b.id;
                    break;
                }
        // A polity without any magical girl cannot hold together: after a few hours its
        // people rejoin the polity it came from (or the nearest other).
        bool has_girl = false;
        for (const auto& cp : ctx_.agents->all())
            if (cp && cp->alive && !cp->departed && cp->is_girl() && cp->polity == p.id) has_girl = true;
        if (!has_girl && ctx_.now > p.founded + kTicksPerHour * 6) {
            u16 into = 0;
            if (const Polity* par = polity(p.parent); par && par->id != p.id) into = par->id;
            for (const Polity& o : polities_)
                if (!into && o.alive && o.id != p.id) into = o.id;
            if (into) {
                EventId why = p.reigns.empty() ? 0 : 0;
                for (const auto& cp : ctx_.agents->all())
                    if (cp && cp->id == p.ruler && cp->death_event) why = cp->death_event;
                annex(into, p.id, why, "群龙无首，重新并入");
                continue;
            }
        }
        p.history.push_back(p.stats);
        if (p.history.size() > 24 * 60) p.history.erase(p.history.begin());
        // The steward looks ahead every two hours (the first time at once).
        if (p.plan.at == 0 || (now / kTicksPerHour) % 2 == 0) draw_plan(p);
    }
    update_projects();
    check_unification();
    (void)now;
}

void Society::check_unification() {
    int alive = 0;
    const Polity* last = nullptr;
    for (const Polity& p : polities_)
        if (p.alive) {
            ++alive;
            last = &p;
        }
    most_polities_ = std::max(most_polities_, alive);
    if (unification_ || alive != 1 || most_polities_ < 2 || !last) return;
    // Victory belongs to the ruler and her drive; whether the island is happy or
    // sustainable is another matter (see the outcome measures).
    const Character* r = ctx_.agents->get(last->ruler);
    std::string drive;
    if (r && r->girl)
        for (const Json& d : ctx_.reg->doc("drives")["drives"].items())
            if (d.str("key") == r->girl->drive) drive = d.str("name");
    Event e;
    e.type = EventType::Unification;
    e.severity = 5;
    e.polity = last->id;
    e.actor = r ? r->id : kNoEntity;
    e.causes[0] = last_merge_;
    e.text = r ? strfmt("「%s」统一了空岛：象征%s的魔法少女%s与她的源动力「%s」赢得了本轮", title(last->id).c_str(),
                        drive.c_str(), r->name.c_str(), drive.c_str())
               : strfmt("「%s」统一了空岛", title(last->id).c_str());
    unification_ = ctx_.chron->emit(std::move(e));
}

void Society::daily(Tick now) {
    trade_daily();
    for (auto& p : polities_)
        if (p.alive) {
            update_diplomacy(p);
            p.weary *= 0.85f;  // war weariness fades over a week or two
        }
    // The civilisation index, per polity and for the island. The island's people and what
    // stands built add up; what is known counts once, wherever on the island it is known
    // (one people taking in another loses no knowledge).
    float island = 0;
    std::set<std::string> known;
    for (auto& p : polities_) {
        if (!p.alive) continue;
        p.civ_history.push_back(civ_index(p));
        island += p.civ_history.back().total - kCivKnownWeight * p.civ_history.back().known;
        known.insert(p.techs.begin(), p.techs.end());
    }
    float k = 0;
    for (const std::string& t : known)
        if (const Json* tj = tech(t)) k += 1.0f + (float)tj->integer("era", 0);
    island_history_.push_back(island + kCivKnownWeight * k);
    ctx_.econ->spoil(rng_, [this](u16 polity) { return 1.0f - std::min(0.9f, passive(polity, "preserve")); });
    ctx_.agents->day = {};
    (void)now;
}

const Json* Society::tech(const std::string& key) const {
    for (const Json& t : ctx_.reg->doc("techs")["techs"].items())
        if (t.str("key") == key) return &t;
    return nullptr;
}

bool Society::tech_available(const Polity& p, const std::string& key) const {
    const Json* t = tech(key);
    if (!t || p.has_tech(key)) return false;
    for (const Json& r : (*t)["requires"].items())
        if (!p.has_tech(r.as_str())) return false;
    // An era is built on the one before it: a people must know at least half of the
    // previous era's techs before anything of the next era can be studied or stumbled on.
    const int e = t->integer("era", 0);
    if (e > 0) {
        const auto [known, needed] = era_foundation(p, e);
        if (known < needed) return false;
        // ...and the tech of learning that opens it.
        const std::string gate = era_gate(e);
        if (!gate.empty() && !p.has_tech(gate)) return false;
    }
    return true;
}

std::string Society::era_gate(int era) const {
    const Json& g = ctx_.reg->doc("techs")["era_gates"];
    return g.str(std::to_string(era), "");
}

bool Society::needs_scholars(const std::string& key) const {
    const Json* t = tech(key);
    return t && t->integer("era", 0) > 0;
}

int Society::scholar_seats(u16 id) const {
    int n = 0;
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.functional && b.polity == id)
            if (const BuildingDef* d = ctx_.buildings->def(b.def)) n += d->scholars;
    return n;
}

float Society::research_per_day(u16 id, bool scholarly) const {
    const float speed = 1.0f + tech_effect(id, "research_speed");
    if (!scholarly) return 2.0f * 7.0f * 1.2f * speed;  // two at the fire or the hall
    float pts = 0.0f;
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.functional && b.polity == id)
            if (const BuildingDef* d = ctx_.buildings->def(b.def)) pts += (float)d->scholars * 7.0f * 1.0f * d->research_rate;
    return pts * speed;
}

std::pair<int, int> Society::era_foundation(const Polity& p, int era) const {
    int total = 0, known = 0;
    for (const Json& t : ctx_.reg->doc("techs")["techs"].items())
        if (t.integer("era", 0) == era - 1) {
            ++total;
            if (p.has_tech(t.str("key"))) ++known;
        }
    return {known, (total + 1) / 2};
}

std::vector<std::string> Society::available_techs(const Polity& p) const {
    std::vector<std::string> out;
    for (const Json& t : ctx_.reg->doc("techs")["techs"].items())
        if (tech_available(p, t.str("key"))) out.push_back(t.str("key"));
    return out;
}

float Society::tech_effect(u16 polity, const std::string& effect) const {
    const Polity* p = this->polity(polity);
    if (!p) return 0.0f;
    float v = 0;
    for (const std::string& k : p->techs)
        if (const Json* t = tech(k)) v += (*t)["effects"].flt(effect, 0.0f);
    return v;
}

int Society::era(const Polity& p) const {
    int e = 0;
    for (const std::string& k : p.techs)
        if (const Json* t = tech(k)) e = std::max(e, t->integer("era", 0));
    return e;
}

void Society::add_research(u16 id, float points, EntityId by, bool scholarly) {
    Polity* p = polity(id);
    if (!p || p->policies.research.empty()) return;
    const std::string key = p->policies.research;
    const Json* t = tech(key);
    if (!t || p->has_tech(key)) {
        p->policies.research.clear();
        return;
    }
    if (!scholarly && needs_scholars(key)) return;
    float* prog = nullptr;
    for (auto& r : p->research)
        if (r.first == key) prog = &r.second;
    if (!prog) {
        p->research.push_back({key, 0.0f});
        prog = &p->research.back().second;
    }
    *prog += points * (1.0f + tech_effect(id, "research_speed"));
    if (*prog >= t->flt("cost", 100.0f)) discover(*p, key, by, 0);
}

std::string Society::grant_target(u16 id) const {
    const Polity* p = polity(id);
    if (!p) return "";
    if (!p->policies.research.empty() && !p->has_tech(p->policies.research) && tech(p->policies.research))
        return p->policies.research;
    std::string key;
    float best = 1e30f;
    for (const std::string& k : available_techs(*p))
        if (const Json* t = tech(k); t && t->flt("cost", 100.0f) < best) {
            best = t->flt("cost", 100.0f);
            key = k;
        }
    return key;
}

void Society::grant_research(u16 id, const std::string& key, float points, EventId cause) {
    Polity* p = polity(id);
    const Json* t = key.empty() ? nullptr : tech(key);
    if (!p || !t || p->has_tech(key)) return;
    float* prog = nullptr;
    for (auto& r : p->research)
        if (r.first == key) prog = &r.second;
    if (!prog) {
        p->research.push_back({key, 0.0f});
        prog = &p->research.back().second;
    }
    *prog += points;
    if (*prog >= t->flt("cost", 100.0f)) discover(*p, key, kNoEntity, cause);
}

void Society::practice(u16 id, const std::string& activity, float amount, EntityId by) {
    Polity* p = polity(id);
    if (!p) return;
    for (const Json& t : ctx_.reg->doc("techs")["techs"].items()) {
        const float per = t["practice"].flt(activity, 0.0f);
        if (per <= 0.0f) continue;
        const std::string key = t.str("key");
        if (p->has_tech(key) || !tech_available(*p, key)) continue;
        float* prog = nullptr;
        for (auto& r : p->research)
            if (r.first == key) prog = &r.second;
        if (!prog) {
            p->research.push_back({key, 0.0f});
            prog = &p->research.back().second;
        }
        const float cost = t.flt("cost", 100.0f);
        // Past the wild era, experience only prepares the ground for the scholars.
        if (needs_scholars(key)) {
            const float cap = cost * (float)ctx_.reg->doc("techs").flt("practice_cap", 0.25f);
            *prog = std::max(*prog, std::min(cap, *prog + per * amount));
            continue;
        }
        *prog += per * amount;
        if (*prog >= cost) discover(*p, key, by, 0, true);
    }
}

void Society::discover(Polity& p, const std::string& key, EntityId by, EventId cause, bool by_practice) {
    if (p.has_tech(key)) return;
    const Json* t = tech(key);
    int era_before = era(p);
    p.techs.push_back(key);
    if (p.policies.research == key) p.policies.research.clear();
    Event e;
    e.type = EventType::TechDiscovered;
    e.severity = 3;
    e.polity = p.id;
    e.actor = by;
    e.causes[0] = cause;
    e.text = strfmt("「%s」%s掌握了%s：%s", p.name.c_str(), by_practice ? "在劳作中摸索" : "",
                    t ? t->str("name").c_str() : key.c_str(), t ? t->str("desc").c_str() : "");
    e.data.set("tech", key);
    if (by_practice) e.data.set("practice", true);
    EventId ev = ctx_.chron->emit(std::move(e));
    int era_after = era(p);
    if (era_after > era_before) {
        Event a;
        a.type = EventType::TechDiscovered;
        a.severity = 4;
        a.polity = p.id;
        a.causes[0] = ev;
        const Json& eras = ctx_.reg->doc("techs")["eras"];
        a.text = strfmt("「%s」迈入了%s", p.name.c_str(),
                        era_after < (int)eras.size() ? eras[(size_t)era_after].as_str().c_str() : "新的时代");
        ctx_.chron->emit(std::move(a));
    }
}

void Society::refresh_passives(Polity& p) {
    p.passives.clear();
    const Json& drives = ctx_.reg->doc("drives")["drives"];
    for (const auto& cp : ctx_.agents->all()) {
        if (!cp || !cp->alive || cp->departed || !cp->is_girl() || cp->polity != p.id) continue;
        for (const Json& d : drives.items()) {
            if (d.str("key") != cp->girl->drive) continue;
            for (const Json& sp : d["spells"].items()) {
                if (sp.str("type") != "passive" || cp->girl->level < sp.integer("level", 1)) continue;
                std::string eff = sp.str("effect");
                float amt = sp.flt("amount", 1.0f);
                bool found = false;
                for (auto& e : p.passives)
                    if (e.first == eff) {
                        e.second += amt;
                        found = true;
                    }
                if (!found) p.passives.push_back({eff, amt});
            }
        }
    }
}

void Society::compute_stats(Polity& p) {
    refresh_passives(p);
    PolityStats s;
    s.tick = ctx_.now;
    int residents = 0, fed = 0, watered = 0, grown = 0;
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
        // Children have no say in politics.
        if (c.age0 >= 14.0f || !ctx_.agents->is_child(c)) {
            sup += c.support_for(p.ruler);
            ++grown;
        }
        if (c.task.type == TaskType::Protest && c.task.step == 2) s.protesters++;
    }
    s.food_stock = public_food(p.id);
    float daily_need = std::max(1.0f, (float)s.population * ctx_.agents->tune.food_per_day);
    s.food_days = s.food_stock / daily_need;
    s.food_access = residents ? (float)fed / (float)residents : 1.0f;
    s.water_access = residents ? (float)watered / (float)residents : 1.0f;
    s.mood = residents ? mood / (float)residents : 0.5f;
    s.ruler_support = grown ? sup / (float)grown : 0.0f;
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
                // The other girls are known for what they do; renown fades without it
                // (it settles around 0.15 per level for one in office, half that without).
                d += 0.0015f * (float)g->girl->level * (g->girl->role != "none" ? 1.0f : 0.5f);
            }
            // Personal memories about this girl.
            for (const Memory& m : c.memories)
                if (m.subject == g->id && ctx_.now - m.tick < kTicksPerHour) d += m.valence * 0.3f;
            s = clampv(s * (g->id == p.ruler ? 0.998f : 0.99f) + d, -1.0f, 1.0f);
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
                 : k == CrisisKind::Unrest ? EventType::Protest
                 : k == CrisisKind::War    ? EventType::Battle : EventType::Shortage;
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
    } else if (ctx_.agents->path_failures_24h() > std::max(10, s.population * 3)) {
        declare(CrisisKind::Logistics, 0.5f,
                recent_cause({EventType::StructureDestroyed, EventType::Collapse, EventType::MeteorImpact}, day),
                "道路受阻，许多居民无法抵达目的地");
    } else {
        resolve(CrisisKind::Logistics, "交通恢复畅通");
    }

    // Food.
    const Crisis* lg = p.crisis(CrisisKind::Logistics);
    // Foragers live from day to day: an empty larder is normal for them, hungry people not.
    const bool band = foraging_band(p);
    const float low_days = band ? 0.2f : 0.8f, ok_days = band ? 0.5f : 1.5f;
    if (s.food_days < low_days || s.food_access < 0.65f) {
        float sev = clampv(std::max((1.2f - s.food_days) / 1.2f, (0.85f - s.food_access) / 0.85f), 0.1f, 1.0f);
        EventId cause = recent_cause({EventType::CropFailure, EventType::WaterSourceLost, EventType::StructureDestroyed,
                                      EventType::MeteorImpact, EventType::FireStarted},
                                     day * 3);
        if (lg && lg->active) cause = lg->event;
        declare(CrisisKind::Food, sev, cause,
                s.food_access < 0.95f ? strfmt("粮食短缺：公共存粮仅够 %.1f 天，%.0f%% 的居民吃不饱", s.food_days, (1.0f - s.food_access) * 100.0f)
                                      : strfmt("粮食短缺：公共存粮仅够 %.1f 天", s.food_days));
    } else if (s.food_days > ok_days && s.food_access > 0.85f) {
        const Crisis* fc = p.crisis(CrisisKind::Food);
        const bool was = fc && fc->active;
        resolve(CrisisKind::Food, "粮食短缺缓解");
        // The emergency measures end with the emergency.
        if (was && p.emergency_until) p.emergency_until = ctx_.now;
    }
    // Emergency measures (everyone to the food, building halted, short rations; a
    // foraging campaign; a rush on a site) are for days, not for good: then the usual
    // division of work and full portions again, until the ruler orders otherwise.
    Policies& q = p.policies;
    if ((p.emergency_until && ctx_.now >= p.emergency_until) || (p.forage_until && ctx_.now >= p.forage_until)) {
        const bool lifted = q.pri_food > 1.0f || q.pri_build != 1.0f || q.pri_gather != 0.7f || q.ration < 1.0f;
        q.pri_food = std::min(q.pri_food, 1.0f);
        q.pri_build = 1.0f;
        q.pri_gather = 0.7f;
        q.ration = std::max(q.ration, 1.0f);
        p.emergency_until = 0;
        p.forage_until = 0;
        if (lifted) {
            Event e;
            e.type = EventType::PolicyChanged;
            e.severity = 2;
            e.polity = p.id;
            const Crisis* fc = p.crisis(CrisisKind::Food);
            e.causes[0] = fc ? fc->event : 0;
            e.text = "应急措施到期：人手回到平常的分工，口粮恢复足额";
            ctx_.chron->emit(std::move(e));
        }
    }

    // Water: people thirsting, a sealed spring feeding the land, or fields losing irrigation.
    EventId sealed = 0;
    {
        const Building* seat = ctx_.buildings->get(p.seat);
        const auto& sp = ctx_.physics->springs();
        const auto& ss = ctx_.physics->spring_states();
        for (size_t i = 0; i < sp.size() && i < ss.size(); ++i)
            if (!ss[i].flowing && seat && sp[i].dist2(seat->entrance) < 200LL * 200LL) sealed = ss[i].lost_event;
    }
    FarmStats fs = ctx_.farming->stats_polity(p.id);
    // Ditches take a while to fill after founding; judge irrigation only after a day.
    const bool fields_dry = ctx_.now > p.founded + day && fs.plots >= 8 && (float)fs.irrigated < 0.5f * (float)fs.plots;
    if (s.water_access < 0.6f) {
        declare(CrisisKind::Water, clampv((0.8f - s.water_access) / 0.8f, 0.1f, 1.0f),
                recent_cause({EventType::WaterSourceLost, EventType::StructureDestroyed}, day * 3),
                strfmt("缺水：%.0f%% 的居民严重口渴", (1.0f - s.water_access) * 100.0f));
    } else if (sealed || fields_dry) {
        EventId cause = sealed ? sealed : recent_cause({EventType::WaterSourceLost, EventType::StructureDestroyed}, day * 6);
        declare(CrisisKind::Water, fields_dry ? 0.7f : 0.45f, cause,
                fields_dry ? strfmt("农田缺水：%d 块田中只有 %d 块还能灌溉", fs.plots, fs.irrigated)
                           : std::string("泉眼断流，湖水将逐渐干涸"));
    } else if (s.water_access > 0.85f) {
        resolve(CrisisKind::Water, "水源危机解除");
    }

    // War: attacked by a neighbour.
    {
        const War* attacked = nullptr;
        for (const War& w : p.wars)
            if (!w.attacker) attacked = &w;
        if (attacked) {
            const Polity* enemy = polity(attacked->enemy);
            declare(CrisisKind::War, 0.8f, attacked->event,
                    strfmt("遭到「%s」的进攻", enemy ? enemy->name.c_str() : "?"));
        } else if (p.wars.empty()) {
            resolve(CrisisKind::War, "战事平息");
        }
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
void save_plan(BinWriter& w, const PolityPlan& P) {
    w.u64v(P.at);
    for (int v : {P.workers, P.children, P.elders, P.people, P.beds, P.plots, P.irrigated, P.sites, P.plots_needed, P.beds_needed})
        w.vari(v);
    for (float v : {P.need, P.stock, P.seed, P.income, P.potential, P.capacity, P.target_days, P.balance, P.days_left, P.birth,
                    P.hands})
        w.f32(v);
    for (int s = 0; s < kFoodSources; ++s) {
        w.f32(P.income_by[s]);
        w.f32(P.potential_by[s]);
    }
    for (int t = 0; t < kTrades; ++t) {
        w.f32(P.want[t]);
        w.vari(P.staff[t]);
    }
    w.str(P.birth_why);
    w.str(P.summary);
    w.u64v(P.first);
    w.varu(P.looks.size());
    for (const PolityPlan::Look& l : P.looks) {
        w.u64v(l.at);
        for (double d : l.food) w.f64(d);
        w.f32(l.hands);
    }
    w.varu(P.advice.size());
    for (const Advice& a : P.advice) {
        w.str(a.key);
        w.f32(a.urgency);
        w.str(a.text);
    }
}
PolityPlan load_plan(BinReader& r) {
    PolityPlan P;
    P.at = r.u64v();
    for (int* v : {&P.workers, &P.children, &P.elders, &P.people, &P.beds, &P.plots, &P.irrigated, &P.sites, &P.plots_needed,
                   &P.beds_needed})
        *v = (int)r.vari();
    for (float* v : {&P.need, &P.stock, &P.seed, &P.income, &P.potential, &P.capacity, &P.target_days, &P.balance,
                     &P.days_left, &P.birth, &P.hands})
        *v = r.f32();
    for (int s = 0; s < kFoodSources; ++s) {
        P.income_by[s] = r.f32();
        P.potential_by[s] = r.f32();
    }
    for (int t = 0; t < kTrades; ++t) {
        P.want[t] = r.f32();
        P.staff[t] = (int)r.vari();
    }
    P.birth_why = r.str();
    P.summary = r.str();
    P.first = r.u64v();
    const u64 nl = r.varu();
    for (u64 k = 0; k < nl; ++k) {
        PolityPlan::Look l;
        l.at = r.u64v();
        for (double& d : l.food) d = r.f64();
        l.hands = r.f32();
        P.looks.push_back(l);
    }
    const u64 na = r.varu();
    for (u64 k = 0; k < na; ++k) {
        Advice a;
        a.key = r.str();
        a.urgency = r.f32();
        a.text = r.str();
        P.advice.push_back(std::move(a));
    }
    return P;
}
void save_civ(BinWriter& w, const CivIndex& c) {
    for (float v : {c.people, c.fed, c.secure, c.housed, c.built, c.known, c.content, c.total}) w.f32(v);
}
CivIndex load_civ(BinReader& r) {
    CivIndex c;
    for (float* v : {&c.people, &c.fed, &c.secure, &c.housed, &c.built, &c.known, &c.content, &c.total}) *v = r.f32();
    return c;
}

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
        w.vari(q.army);
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
        w.varu(p.wars.size());
        for (const War& wr : p.wars) {
            w.u16v(wr.enemy);
            w.boolean(wr.attacker);
            w.str(wr.aim);
            w.u64v(wr.since);
            w.u32v(wr.event);
            w.vari(wr.kills);
            w.vari(wr.losses);
        }
        w.boolean(p.op.active);
        w.u16v(p.op.enemy);
        w.str(p.op.aim);
        w.vec3i(p.op.rally);
        w.vec3i(p.op.objective);
        w.u8v(p.op.phase);
        w.u64v(p.op.since);
        w.vari(p.op.party);
        w.vari(p.op.lost);
        w.u32v(p.op.event);
        w.boolean(p.op.engaged);
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
        w.varu(p.passives.size());
        for (auto& [k, v] : p.passives) {
            w.str(k);
            w.f32(v);
        }
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
    w.vari(most_polities_);
    w.u64v(last_merge_);
    w.u64v(unification_);
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) w.vari(polities_[i].op.loot);
    // Trade pacts (added later; older saves have none).
    auto save_goods = [&](const std::vector<std::pair<ItemId, i32>>& g) {
        w.varu(g.size());
        for (auto& [it, n] : g) {
            w.u16v(it);
            w.vari(n);
        }
    };
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        w.varu(polities_[i].pacts.size());
        for (const TradePact& t : polities_[i].pacts) {
            w.u16v(t.partner);
            w.u64v(t.since);
            w.u32v(t.event);
            w.vari(t.trips);
            w.f32(t.sent);
            w.f32(t.received);
            w.u64v(t.blocked_until);
            w.u32v(t.blocked);
            save_goods(t.out_today);
            save_goods(t.in_today);
        }
    }
    // Strategy (added in version 2): truces, alliances, grievances, vassals, war tallies.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        const Polity& p = polities_[i];
        w.varu(p.diplo.size());
        for (const Diplo& d : p.diplo) {
            w.u16v(d.other);
            w.u64v(d.truce_until);
            w.boolean(d.allied);
            w.u64v(d.allied_since);
            w.f32(d.grievance);
            w.u64v(d.last_incident);
        }
        w.u16v(p.overlord);
        w.u64v(p.tribute_next);
        w.varu(p.wars.size());
        for (const War& wr : p.wars) {
            w.vari(wr.loot);
            w.vari(wr.razed);
            w.u64v(wr.last_offer);
            w.u64v(wr.active_at);
            w.vari(wr.refused);
        }
        w.varu(p.outposts.size());
        for (const Vec3i& o : p.outposts) w.vec3i(o);
    }
    // Strategy, version 3: envoys, sieges.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        const Polity& p = polities_[i];
        w.varu(p.diplo.size());
        for (const Diplo& d : p.diplo) w.u64v(d.envoy_at);
        w.u64v(p.op.held_since);
    }
    // Version 4: where armies form up.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        w.vec3i(polities_[i].op.stage);
        w.u64v(polities_[i].op.staged_since);
    }
    // Version 5: raids beaten back in each war.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        w.varu(polities_[i].wars.size());
        for (const War& wr : polities_[i].wars) w.vari(wr.repulsed);
    }
    // Version 6: the stewards' plans and the civilisation index.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) {
        save_plan(w, polities_[i].plan);
        w.varu(polities_[i].civ_history.size());
        for (const CivIndex& c : polities_[i].civ_history) save_civ(w, c);
    }
    w.varu(island_history_.size());
    for (float v : island_history_) w.f32(v);
    // Version 7: what the stewards have learnt about their food shares.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) w.f32(polities_[i].plan.food_lean);
    // Version 8: whether the plan calls for children to replace the old.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) w.boolean(polities_[i].plan.renewing);
    // Version 9: when emergency measures lapse.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) w.u64v(polities_[i].emergency_until);
    // Version 10: how tired of war each people is.
    w.varu(polities_.size());
    for (size_t i = 1; i < polities_.size(); ++i) w.f32(polities_[i].weary);
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
        q.army = (int)r.vari();
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
        u64 nwars = r.varu();
        for (u64 k = 0; k < nwars; ++k) {
            War wr;
            wr.enemy = r.u16v();
            wr.attacker = r.boolean();
            wr.aim = r.str();
            wr.since = r.u64v();
            wr.event = r.u32v();
            wr.kills = (int)r.vari();
            wr.losses = (int)r.vari();
            p.wars.push_back(wr);
        }
        p.op.active = r.boolean();
        p.op.enemy = r.u16v();
        p.op.aim = r.str();
        p.op.rally = r.vec3i();
        p.op.objective = r.vec3i();
        p.op.phase = r.u8v();
        p.op.since = r.u64v();
        p.op.party = (int)r.vari();
        p.op.lost = (int)r.vari();
        p.op.event = r.u32v();
        p.op.engaged = r.boolean();
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
        p.passives.clear();
        u64 npv = r.varu();
        for (u64 k = 0; k < npv; ++k) {
            std::string key = r.str();
            float v = r.f32();
            p.passives.push_back({key, v});
        }
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
    most_polities_ = 1;
    last_merge_ = unification_ = 0;
    if (!r.at_end()) {
        most_polities_ = (int)r.vari();
        last_merge_ = r.u64v();
        unification_ = r.u64v();
        if (!r.at_end()) {
            const u64 np2 = r.varu();
            for (size_t i = 1; i < (size_t)np2 && i < polities_.size(); ++i) polities_[i].op.loot = (int)r.vari();
        }
        if (!r.at_end()) {
            auto load_goods = [&](std::vector<std::pair<ItemId, i32>>& g) {
                const u64 n = r.varu();
                for (u64 k = 0; k < n; ++k) {
                    ItemId it = r.u16v();
                    g.push_back({it, (i32)r.vari()});
                }
            };
            const u64 np3 = r.varu();
            for (size_t i = 1; i < (size_t)np3; ++i) {
                const u64 nt = r.varu();
                for (u64 k = 0; k < nt; ++k) {
                    TradePact t;
                    t.partner = r.u16v();
                    t.since = r.u64v();
                    t.event = r.u32v();
                    t.trips = (int)r.vari();
                    t.sent = r.f32();
                    t.received = r.f32();
                    t.blocked_until = r.u64v();
                    t.blocked = r.u32v();
                    load_goods(t.out_today);
                    load_goods(t.in_today);
                    if (i < polities_.size()) polities_[i].pacts.push_back(std::move(t));
                }
            }
            if (!r.at_end()) {
                const u64 np4 = r.varu();
                for (size_t i = 1; i < (size_t)np4; ++i) {
                    Polity dummy;
                    Polity& p = i < polities_.size() ? polities_[i] : dummy;
                    const u64 nd = r.varu();
                    p.diplo.clear();
                    for (u64 k = 0; k < nd; ++k) {
                        Diplo d;
                        d.other = r.u16v();
                        d.truce_until = r.u64v();
                        d.allied = r.boolean();
                        d.allied_since = r.u64v();
                        d.grievance = r.f32();
                        d.last_incident = r.u64v();
                        p.diplo.push_back(d);
                    }
                    p.overlord = r.u16v();
                    p.tribute_next = r.u64v();
                    const u64 nw = r.varu();
                    for (u64 k = 0; k < nw; ++k) {
                        const int loot = (int)r.vari(), razed = (int)r.vari();
                        const Tick last = r.u64v(), active = r.u64v();
                        const int refused = (int)r.vari();
                        if (k < p.wars.size()) {
                            p.wars[k].loot = loot;
                            p.wars[k].razed = razed;
                            p.wars[k].last_offer = last;
                            p.wars[k].active_at = active;
                            p.wars[k].refused = refused;
                        }
                    }
                    const u64 no = r.varu();
                    p.outposts.clear();
                    for (u64 k = 0; k < no; ++k) p.outposts.push_back(r.vec3i());
                }
            }
            if (!r.at_end()) {
                const u64 np5 = r.varu();
                for (size_t i = 1; i < (size_t)np5; ++i) {
                    const u64 nd = r.varu();
                    for (u64 k = 0; k < nd; ++k) {
                        const Tick at = r.u64v();
                        if (i < polities_.size() && k < polities_[i].diplo.size()) polities_[i].diplo[k].envoy_at = at;
                    }
                    const Tick held = r.u64v();
                    if (i < polities_.size()) polities_[i].op.held_since = held;
                }
            }
            if (!r.at_end()) {
                const u64 np6 = r.varu();
                for (size_t i = 1; i < (size_t)np6; ++i) {
                    const Vec3i stage = r.vec3i();
                    const Tick since = r.u64v();
                    if (i < polities_.size()) {
                        polities_[i].op.stage = stage;
                        polities_[i].op.staged_since = since;
                    }
                }
            }
            if (!r.at_end()) {
                const u64 np7 = r.varu();
                for (size_t i = 1; i < (size_t)np7; ++i) {
                    const u64 nw = r.varu();
                    for (u64 k = 0; k < nw; ++k) {
                        const int rep = (int)r.vari();
                        if (i < polities_.size() && k < polities_[i].wars.size()) polities_[i].wars[k].repulsed = rep;
                    }
                }
            }
            island_history_.clear();
            if (!r.at_end()) {
                const u64 np8 = r.varu();
                for (size_t i = 1; i < (size_t)np8; ++i) {
                    PolityPlan plan = load_plan(r);
                    std::vector<CivIndex> civ;
                    const u64 nc = r.varu();
                    for (u64 k = 0; k < nc; ++k) civ.push_back(load_civ(r));
                    if (i < polities_.size()) {
                        polities_[i].plan = std::move(plan);
                        polities_[i].civ_history = std::move(civ);
                    }
                }
                const u64 ni = r.varu();
                for (u64 k = 0; k < ni; ++k) island_history_.push_back(r.f32());
            }
            if (!r.at_end()) {
                const u64 np9 = r.varu();
                for (size_t i = 1; i < (size_t)np9; ++i) {
                    const float lean = r.f32();
                    if (i < polities_.size()) polities_[i].plan.food_lean = lean;
                }
            }
            if (!r.at_end()) {
                const u64 np10 = r.varu();
                for (size_t i = 1; i < (size_t)np10; ++i) {
                    const bool renewing = r.boolean();
                    if (i < polities_.size()) polities_[i].plan.renewing = renewing;
                }
            }
            if (!r.at_end()) {
                const u64 np11 = r.varu();
                for (size_t i = 1; i < (size_t)np11; ++i) {
                    const Tick until = r.u64v();
                    if (i < polities_.size()) polities_[i].emergency_until = until;
                }
            }
            if (!r.at_end()) {
                const u64 np12 = r.varu();
                for (size_t i = 1; i < (size_t)np12; ++i) {
                    const float weary = r.f32();
                    if (i < polities_.size()) polities_[i].weary = weary;
                }
            }
        }
    }
}

u64 Society::hash() const {
    u64 h = hash_combine(rng_.state(), rng_.inc());
    for (auto& p : polities_) {
        if (!p.alive) continue;
        h = hash_combine(h, p.ruler);
        h = hash_combine(h, (u64)(p.stats.food_stock * 10.0f));
        h = hash_combine(h, (u64)(p.policies.ration * 1000.0f));
        for (const TradePact& t : p.pacts) h = hash_combine(h, ((u64)t.partner << 32) ^ (u64)t.trips ^ ((u64)t.blocked << 16));
        for (const Diplo& d : p.diplo)
            h = hash_combine(h, ((u64)d.other << 48) ^ d.truce_until ^ ((u64)d.allied << 40) ^ (u64)(d.grievance * 1000.0f));
        h = hash_combine(h, ((u64)p.overlord << 32) ^ p.tribute_next);
    }
    return h;
}

}  // namespace icarus
