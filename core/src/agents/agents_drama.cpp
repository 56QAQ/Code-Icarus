// Agents: the drama among magical girls. Ties form between them: friends who have lived
// and fought side by side, rivals for the people's support, a mentor and the girl who
// awoke under her, nemeses made on the battlefield. What they live through (a friend
// lost, a people starving, a war lost; kindness, triumph, friendship) weighs on them,
// and can turn a drive darker or brighter. Every change is told in the chronicle with
// its causes.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
const Json* drive_of(const Registry& reg, const std::string& key) {
    for (const Json& d : reg.doc("drives")["drives"].items())
        if (d.str("key") == key) return &d;
    return nullptr;
}
std::string drive_name(const Registry& reg, const std::string& key) {
    const Json* d = drive_of(reg, key);
    return d ? d->str("name", key) : key;
}
constexpr float kTurn = 1.2f;     // trauma or solace at which a drive may turn
constexpr float kFade = 0.97f;    // what is left of both after a day
constexpr size_t kMarks = 4;      // experiences remembered as causes
}  // namespace

void Agents::mark_girl(EntityId id, float trauma, float solace, EventId ev) {
    Character* c = get(id);
    if (!c || !c->girl || !c->alive) return;
    GirlData& g = *c->girl;
    g.trauma = std::min(3.0f, g.trauma + trauma);
    g.solace = std::min(3.0f, g.solace + solace);
    if (ev && std::find(g.marks.begin(), g.marks.end(), ev) == g.marks.end()) {
        g.marks.push_back(ev);
        if (g.marks.size() > kMarks) g.marks.erase(g.marks.begin());
    }
}

void Agents::set_bond(Character& a, Character& b, BondKind ka, BondKind kb, EventId ev) {
    auto put = [&](Character& x, EntityId other, BondKind k) {
        if (!x.girl) return;
        for (Bond& bd : x.girl->bonds)
            if (bd.other == other) {
                bd.kind = k;
                bd.since = now_;
                bd.event = ev;
                return;
            }
        x.girl->bonds.push_back({other, k, now_, ev});
    };
    put(a, b.id, ka);
    put(b, a.id, kb);
}

void Agents::drop_bond(Character& a, Character& b) {
    auto drop = [](Character& x, EntityId other) {
        if (!x.girl) return;
        auto& v = x.girl->bonds;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Bond& bd) { return bd.other == other; }), v.end());
    };
    drop(a, b.id);
    drop(b, a.id);
}

EventId Agents::tell(EventType type, u8 severity, const Character& a, EntityId other, EventId cause, std::string text) {
    Event e;
    e.type = type;
    e.severity = severity;
    e.pos = a.foot;
    e.actor = a.id;
    e.target = other;
    e.polity = a.polity;
    e.causes[0] = cause;
    e.text = std::move(text);
    return ctx_.chron->emit(std::move(e));
}

void Agents::take_student(Character& girl, EventId cause) {
    if (!girl.girl) return;
    // The most experienced magical girl of her people takes her in hand.
    Character* best = nullptr;
    for (auto& cp : chars_) {
        Character* o = cp.get();
        if (!o || o == &girl || !o->alive || o->departed || !o->girl || o->polity != girl.polity) continue;
        if (!best || o->girl->level > best->girl->level || (o->girl->level == best->girl->level && o->id < best->id)) best = o;
    }
    if (!best) return;
    const EventId ev = tell(EventType::Bond, 3, *best, girl.id, cause,
                            strfmt("%s收%s为徒，教她驾驭刚刚觉醒的魔力", best->name.c_str(), girl.name.c_str()));
    set_bond(*best, girl, BondKind::Mentor, BondKind::Student, ev);
    best->affinity_ref(girl.id) = std::max(best->affinity(girl.id), 0.4f);
    girl.affinity_ref(best->id) = std::max(girl.affinity(best->id), 0.5f);
}

void Agents::girl_died(Character& dead, EventId ev) {
    // Those who were close to her grieve; whoever cut her down becomes their nemesis.
    for (auto& cp : chars_) {
        Character* o = cp.get();
        if (!o || !o->alive || !o->girl || o == &dead) continue;
        const Bond* b = o->girl->bond_with(dead.id);
        if (!b) continue;
        if (b->kind == BondKind::Friend || b->kind == BondKind::Mentor || b->kind == BondKind::Student)
            mark_girl(o->id, 0.7f, 0.0f, ev);
        else if (b->kind == BondKind::Nemesis)
            mark_girl(o->id, 0.0f, 0.3f, ev);  // an old enemy gone
    }
    // (Bonds to her stay: a friend lost is still part of their story.)
    for (auto& cp : chars_)
        if (cp && cp->girl && cp->girl->duel == dead.id) cp->girl->duel = kNoEntity;
}

void Agents::girl_felled(Character& victor, Character& fallen, EventId cause) {
    if (!victor.girl || !fallen.girl) return;
    const EventId ev = tell(EventType::Battle, 5, victor, fallen.id, cause,
                            strfmt("%s在对决中击倒了%s", victor.name.c_str(), fallen.name.c_str()));
    mark_girl(victor.id, 0.1f, 0.35f, ev);  // triumph, and a little of the weight of it
    // Her friends, teacher and students will not forget who did it.
    for (auto& cp : chars_) {
        Character* o = cp.get();
        if (!o || !o->alive || !o->girl || o == &fallen || o == &victor || o->polity == victor.polity) continue;
        const Bond* b = o->girl->bond_with(fallen.id);
        if (!b || (b->kind != BondKind::Friend && b->kind != BondKind::Mentor && b->kind != BondKind::Student)) continue;
        const Bond* old = o->girl->bond_with(victor.id);
        if (old && old->kind == BondKind::Nemesis) continue;
        const EventId nev = tell(EventType::Bond, 4, *o, victor.id, ev,
                                 strfmt("%s立誓为%s报仇，%s成了她的宿敌", o->name.c_str(), fallen.name.c_str(), victor.name.c_str()));
        set_bond(*o, victor, BondKind::Nemesis, BondKind::Nemesis, nev);
    }
}

void Agents::daily_drama() {
    now_ = ctx_.now;
    const Registry& reg = *ctx_.reg;
    std::vector<Character*> girls;
    for (auto& cp : chars_)
        if (cp && cp->alive && !cp->departed && cp->girl) girls.push_back(cp.get());

    // Ties between girls of one people: warmth grows between those who get along and
    // share the work (and the fighting); a girl turned against her ruler grows cold.
    for (size_t i = 0; i < girls.size(); ++i)
        for (size_t j = i + 1; j < girls.size(); ++j) {
            Character& a = *girls[i];
            Character& b = *girls[j];
            if (a.polity != b.polity) continue;
            const Polity* p = ctx_.society->polity(a.polity);
            const Json* da = drive_of(reg, a.girl->drive);
            const Json* db = drive_of(reg, b.girl->drive);
            const float coop = 0.5f * ((da ? da->flt("cooperation", 0.5f) : 0.5f) + (db ? db->flt("cooperation", 0.5f) : 0.5f));
            float warm = 0.03f * (coop - 0.4f) + 0.01f * (a.pers.sociability + b.pers.sociability - 1.0f);
            if (a.drafted && b.drafted) warm += 0.06f;  // comrades in arms
            const bool a_rules = p && p->ruler == a.id, b_rules = p && p->ruler == b.id;
            auto defiant = [](const Character& x) { return x.girl->stance == "defiant" || x.girl->stance == "rebel"; };
            if ((a_rules && defiant(b)) || (b_rules && defiant(a))) warm -= 0.06f;
            a.affinity_ref(b.id) = clampv(a.affinity(b.id) + warm, -1.0f, 1.0f);
            b.affinity_ref(a.id) = clampv(b.affinity(a.id) + warm, -1.0f, 1.0f);

            const Bond* bond = a.girl->bond_with(b.id);
            const BondKind kind = bond ? bond->kind : BondKind::None;
            const float fa = a.affinity(b.id), fb = b.affinity(a.id);
            if (kind == BondKind::None && fa >= 0.55f && fb >= 0.55f) {
                const EventId ev = tell(EventType::Bond, 3, a, b.id, 0, strfmt("%s与%s结为挚友", a.name.c_str(), b.name.c_str()));
                set_bond(a, b, BondKind::Friend, BondKind::Friend, ev);
                mark_girl(a.id, 0.0f, 0.2f, ev);
                mark_girl(b.id, 0.0f, 0.2f, ev);
            } else if (kind == BondKind::Friend && (fa < 0.15f || fb < 0.15f)) {
                const EventId ev = tell(EventType::Bond, 3, a, b.id, 0, strfmt("%s与%s的友谊破裂了", a.name.c_str(), b.name.c_str()));
                drop_bond(a, b);
                mark_girl(a.id, 0.25f, 0.0f, ev);
                mark_girl(b.id, 0.25f, 0.0f, ev);
            } else if ((kind == BondKind::None || kind == BondKind::Friend) && ((a_rules && defiant(b)) || (b_rules && defiant(a))) &&
                       std::min(fa, fb) < -0.1f) {
                Character& ruler = a_rules ? a : b;
                Character& rival = a_rules ? b : a;
                const EventId ev = tell(EventType::Bond, 4, rival, ruler.id, 0,
                                        strfmt("%s与统治者%s争夺民心，两人成了对手", rival.name.c_str(), ruler.name.c_str()));
                set_bond(a, b, BondKind::Rival, BondKind::Rival, ev);
            } else if (kind == BondKind::Rival && !defiant(a) && !defiant(b) && fa > 0.1f && fb > 0.1f) {
                tell(EventType::Bond, 2, a, b.id, 0, strfmt("%s与%s放下了竞争", a.name.c_str(), b.name.c_str()));
                drop_bond(a, b);
            }
        }

    // What her people are living through weighs on her; a people doing well lifts her.
    for (Character* c : girls) {
        GirlData& g = *c->girl;
        const Polity* p = ctx_.society->polity(c->polity);
        if (p) {
            const bool rules = p->ruler == c->id;
            const Crisis* famine = p->crisis(CrisisKind::Food);
            if (famine && famine->active) mark_girl(c->id, rules ? 0.1f : 0.05f, 0.0f, famine->event);
            else if (p->wars.empty() && p->stats.food_days > 4.0f && p->stats.ruler_support > 0.6f && p->stats.mood > 0.7f)
                mark_girl(c->id, 0.0f, rules ? 0.05f : 0.03f, 0);
        }
        g.trauma *= kFade;
        g.solace *= kFade;
    }

    // A drive that turns.
    for (Character* c : girls) {
        GirlData& g = *c->girl;
        const Json* d = drive_of(reg, g.drive);
        if (!d) continue;
        const std::string fall = d->str("fall"), rise = d->str("rise");
        if (!fall.empty() && g.trauma >= kTurn && g.trauma > g.solace + 0.5f && rng_.chance(0.5f)) turn_drive(*c, fall, true);
        else if (!rise.empty() && g.solace >= kTurn && g.solace > g.trauma + 0.5f && rng_.chance(0.5f)) turn_drive(*c, rise, false);
    }
}

void Agents::turn_drive(Character& c, const std::string& to, bool darker) {
    GirlData& g = *c.girl;
    const Registry& reg = *ctx_.reg;
    const Json* d = drive_of(reg, to);
    if (!d || to == g.drive) return;
    const std::string from = g.drive;
    if (g.born_drive.empty()) g.born_drive = from;
    g.drive = to;
    g.drive_changed = now_;
    // Her costume takes the colours of the new drive (her hair keeps its own).
    const Json& co = (*d)["costume"];
    c.look.cloth = parse_color(co.str("cloth", "#ffffff"));
    c.look.accent = parse_color(co.str("accent", "#ffcc00"));
    c.body.version++;
    Event e;
    e.type = EventType::DriveChanged;
    e.severity = 5;
    e.pos = c.foot;
    e.actor = c.id;
    e.polity = c.polity;
    for (size_t i = 0; i < g.marks.size() && i < 4; ++i) e.causes[i] = g.marks[g.marks.size() - 1 - i];
    e.text = darker ? strfmt("%s的源动力由「%s」坠入「%s」", c.name.c_str(), drive_name(reg, from).c_str(), drive_name(reg, to).c_str())
                    : strfmt("%s的源动力由「%s」升华为「%s」", c.name.c_str(), drive_name(reg, from).c_str(), drive_name(reg, to).c_str());
    e.data.set("from", from);
    e.data.set("to", to);
    ctx_.chron->emit(std::move(e));
    g.trauma = darker ? 0.3f : std::min(g.trauma, 0.3f);
    g.solace = darker ? std::min(g.solace, 0.3f) : 0.3f;
    g.marks.clear();
    // A darker girl is less patient with her ruler; a brighter one, more.
    if (const Polity* p = ctx_.society->polity(c.polity); p && p->ruler != c.id)
        g.loyalty = clampv(g.loyalty + (darker ? -0.2f : 0.15f), -1.0f, 1.0f);
}

}  // namespace icarus
