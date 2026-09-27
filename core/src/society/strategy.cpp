// Society: strategy between polities. How strong each is in the field; whether a war on
// a neighbour makes sense (a motive, an opportunity, the strength to win, no truce in
// the way, and the first days of building up behind both); grievances that pile up
// along a border; truces after a peace, alliances against a stronger neighbour, and
// vassals that pay tribute to their overlord. Rulers still decide (decision_war.cpp):
// this only tells them what the situation is.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/agents/jobs.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
// A newly founded polity builds up before it thinks of war (days): its first season and
// a little more.
constexpr float kSettleDays = 10.0f;
// Villages closer than this rub along a border (cubes between buildings).
constexpr int kBorderReach = 160;

const Json* ruler_drive(const SimContext& ctx, const Polity* p) {
    const Character* r = p ? ctx.agents->get(p->ruler) : nullptr;
    if (!r || !r->girl) return nullptr;
    for (const Json& d : ctx.reg->doc("drives")["drives"].items())
        if (d.str("key") == r->girl->drive) return &d;
    return nullptr;
}
}  // namespace

float Society::war_appetite(u16 id) const {
    const Json* d = ruler_drive(ctx_, polity(id));
    return d ? d->flt("war_appetite", 0.0f) : 0.0f;
}

float Society::cooperation(u16 id) const {
    const Json* d = ruler_drive(ctx_, polity(id));
    return d ? d->flt("cooperation", 0.5f) : 0.5f;
}

float Society::strength(u16 id) const {
    const Polity* p = polity(id);
    if (!p) return 0.0f;
    // Weapons and armour on hand (worn or in store) arm that many fighters.
    int weapons = 0, armour = 0;
    for (StoreId sid : public_stores(id))
        if (const Store* s = ctx_.econ->store(sid))
            for (const ItemStack& st : s->items) {
                const ItemDef& d = ctx_.reg->item(st.item);
                if (d.has_tag("weapon")) weapons += st.count;
                else if (d.armor > 0.0f) armour += st.count;
            }
    float fighters = 0.0f, girls = 0.0f;
    for (const auto& cp : ctx_.agents->all()) {
        const Character* c = cp.get();
        if (!c || !c->alive || c->departed || c->polity != id) continue;
        if (c->is_girl()) {
            // A magical girl is worth a squad; one made for battle, more.
            bool combat = false;
            for (const Json& d : ctx_.reg->doc("drives")["drives"].items())
                if (d.str("key") == c->girl->drive) combat = d.str("category") == "combat";
            girls += (combat ? 3.0f : 1.6f) + 0.5f * (float)c->girl->level;
            continue;
        }
        if (!c->body.can_hold() || c->body.mobility() < 0.6f || ctx_.agents->is_child(*c) || ctx_.agents->is_elder(*c))
            continue;
        fighters += 1.0f + 0.5f * c->skills[kCombat];
        if (c->weapon != kNoItem) ++weapons;
        if (c->armor != kNoItem) ++armour;
    }
    const float n = std::max(1.0f, fighters);
    const float armed = std::min(1.0f, (float)weapons / n), armoured = std::min(1.0f, (float)armour / n);
    float s = fighters * (1.0f + 0.8f * armed + 0.4f * armoured) + girls;
    // A hungry army fights badly.
    s *= 0.6f + 0.4f * std::min(1.0f, p->stats.food_access);
    return s;
}

Society::Assessment Society::assess(u16 us, u16 them) const {
    Assessment a;
    const Polity* p = polity(us);
    const Polity* o = polity(them);
    if (!p || !o) return a;
    a.ours = strength(us);
    a.theirs = strength(them);
    // Their allies would come to their aid.
    for (const Diplo& d : o->diplo)
        if (d.allied && d.other != us && polity(d.other)) a.theirs += 0.7f * strength(d.other);
    a.ratio = a.ours / std::max(1.0f, a.theirs);
    const Diplo* dp = p->diplo_of(them);
    const float grievance = dp ? dp->grievance : 0.0f;
    const float att = p->attitude_to(them);
    std::vector<std::string> why;
    // Motive.
    a.motive = std::max(0.0f, -att) * 0.6f + std::min(1.0f, grievance) * 0.6f;
    if (grievance > 0.3f) why.push_back("积怨已深");
    if (p->stats.food_days < 2.0f && o->stats.food_days > 4.0f) {
        a.motive += 0.4f;
        why.push_back("我们缺粮而对方仓廪充实");
    }
    if (o->stats.population > p->stats.population * 1.3f && a.ratio < 1.2f) {
        a.motive += 0.15f;
        why.push_back("对方日益坐大");
    }
    // The ruler's own nature: some drives look for a fight, some for any way to avoid one.
    const float appetite = war_appetite(us);
    a.motive = std::max(0.0f, a.motive + appetite);
    if (appetite >= 0.3f) why.push_back("统治者好战");
    const Character* ruler = ctx_.agents->get(p->ruler);
    const std::string drive = ruler && ruler->girl ? ruler->girl->drive : std::string();
    if (drive == "envy" && (a.theirs > a.ours * 1.1f || o->stats.population > p->stats.population)) {
        a.motive += 0.2f;
        why.push_back("嫉妒对方的强盛");
    }
    if (drive == "gluttony" && o->stats.food_stock > std::max(40.0f, p->stats.food_stock * 1.3f)) {
        a.motive += 0.25f;
        why.push_back("觊觎对方的粮仓");
    }
    // Opportunity: their trouble.
    if (const Crisis* c = o->crisis(CrisisKind::Food); c && c->active) {
        a.opportunity += 0.3f;
        why.push_back("对方正闹饥荒");
    }
    if (o->stats.ruler_support < -0.3f) {
        a.opportunity += 0.2f;
        why.push_back("对方民心涣散");
    }
    for (const War& w : o->wars)
        if (w.enemy != us) {
            a.opportunity += 0.3f;
            why.push_back("对方正与别国交战");
            break;
        }
    // What we have to fight with.
    for (StoreId sid : public_stores(us))
        if (const Store* s = ctx_.econ->store(sid))
            for (const ItemStack& st : s->items)
                if (ctx_.reg->item(st.item).has_tag("weapon")) a.armed += st.count;
    // Not yet: the first days of building up, or a truce.
    const Tick settle = (Tick)(kSettleDays * (float)kTicksPerDay);
    a.settled = ctx_.now > p->founded + settle && ctx_.now > o->founded + settle;
    a.truce = dp && dp->truce_until > ctx_.now;
    if (a.ratio >= 1.5f) why.push_back(strfmt("我方兵力约为对方的 %.1f 倍", a.ratio));
    else if (a.ratio < 0.8f) why.push_back(strfmt("对方兵力约为我方的 %.1f 倍", 1.0f / std::max(0.05f, a.ratio)));
    for (size_t i = 0; i < why.size(); ++i) a.why += (i ? "，" : "") + why[i];
    return a;
}

void Society::add_grievance(u16 who, u16 against, float amount) {
    Polity* p = polity(who);
    if (!p || !polity(against) || who == against) return;
    Diplo& d = p->diplo_ref(against);
    d.grievance = clampv(d.grievance + amount, 0.0f, 2.0f);
    d.last_incident = ctx_.now;
}

void Society::set_truce(u16 a, u16 b, float days) {
    const Tick until = ctx_.now + (Tick)(days * (float)kTicksPerDay);
    if (Polity* pa = polity(a)) pa->diplo_ref(b).truce_until = std::max(pa->diplo_ref(b).truce_until, until);
    if (Polity* pb = polity(b)) pb->diplo_ref(a).truce_until = std::max(pb->diplo_ref(a).truce_until, until);
}

void Society::make_alliance(u16 a, u16 b, EventId cause) {
    Polity* pa = polity(a);
    Polity* pb = polity(b);
    if (!pa || !pb || pa->allied_with(b)) return;
    for (auto [x, y] : {std::pair<Polity*, u16>{pa, b}, {pb, a}}) {
        Diplo& d = x->diplo_ref(y);
        d.allied = true;
        d.allied_since = ctx_.now;
        d.grievance *= 0.5f;
        x->attitude_ref(y) = std::max(x->attitude_to(y), 0.3f);
    }
    Event e;
    e.type = EventType::Peace;
    e.severity = 4;
    e.polity = a;
    e.causes[0] = cause;
    e.text = strfmt("「%s」与「%s」结成同盟", title(a).c_str(), title(b).c_str());
    ctx_.chron->emit(std::move(e));
}

void Society::end_alliance(u16 a, u16 b, const std::string& why, EventId cause) {
    Polity* pa = polity(a);
    Polity* pb = polity(b);
    if (!pa || !pa->allied_with(b)) return;
    pa->diplo_ref(b).allied = false;
    if (pb) pb->diplo_ref(a).allied = false;
    Event e;
    e.type = EventType::Info;
    e.severity = 4;
    e.polity = a;
    e.causes[0] = cause;
    e.text = strfmt("「%s」与「%s」的同盟破裂：%s", title(a).c_str(), title(b).c_str(), why.c_str());
    ctx_.chron->emit(std::move(e));
}

i32 Society::send_tribute(u16 from, u16 to, i32 food, const std::string& why, EventId cause) {
    Polity* pa = polity(from);
    Polity* pb = polity(to);
    if (!pa || !pb || food <= 0) return 0;
    // The destination: their storehouse nearest to our seat.
    const Building* seat = ctx_.buildings->get(pa->seat);
    StoreId dst = kNoStore;
    float bd = 1e30f;
    for (StoreId sid : public_stores(to))
        if (const Store* s = ctx_.econ->store(sid); s && s->kind == StoreKind::Stockpile) {
            const float d = seat ? (float)s->pos.dist2(seat->entrance) : 0.0f;
            if (d < bd) {
                bd = d;
                dst = sid;
            }
        }
    if (!dst) return 0;
    // Grain first, then whatever else feeds people; loads a carrier can manage.
    std::vector<std::pair<float, std::pair<StoreId, ItemId>>> stock;
    for (StoreId sid : public_stores(from))
        if (const Store* s = ctx_.econ->store(sid))
            for (const ItemStack& st : s->items) {
                const ItemDef& d = ctx_.reg->item(st.item);
                if (d.nutrition <= 0.0f) continue;
                stock.push_back({-(d.nutrition * 10.0f - d.spoil_per_day), {sid, st.item}});
            }
    std::sort(stock.begin(), stock.end());
    Event e;
    e.type = EventType::Trade;
    e.severity = 3;
    e.polity = from;
    e.causes[0] = cause;
    e.text = strfmt("「%s」向「%s」%s：粮食×%d", pa->name.c_str(), pb->name.c_str(), why.c_str(), food);
    e.data.set("other", (int)to);
    const EventId ev = ctx_.chron->emit(std::move(e));
    i32 left = food, sent = 0;
    for (const auto& [rank, where] : stock) {
        (void)rank;
        const auto [sid, item] = where;
        const Store* src = ctx_.econ->store(sid);
        i32 here = src ? std::min(left, ctx_.econ->available(sid, item)) : 0;
        const i32 load = std::max(1, (i32)std::floor(ctx_.agents->tune.carry_capacity /
                                                    std::max(0.05f, ctx_.reg->item(item).weight)));
        while (here > 0) {
            Job j;
            j.type = JobType::Trade;
            j.polity = from;
            j.pos = src->pos;
            j.from = sid;
            j.to = dst;
            j.item = item;
            j.count = std::min(here, load);
            j.project = to;
            j.plot = 2;  // tribute: nothing comes back
            j.priority = 1.15f;
            j.created = ctx_.now;
            j.cause = ev;
            ctx_.jobs->add(j);
            here -= j.count;
            left -= j.count;
            sent += j.count;
        }
        if (left <= 0) break;
    }
    return sent;
}

void Society::make_vassal(u16 vassal, u16 overlord, EventId cause) {
    Polity* v = polity(vassal);
    Polity* o = polity(overlord);
    if (!v || !o || vassal == overlord) return;
    v->overlord = overlord;
    v->tribute_next = ctx_.now + kTicksPerDay * 2;
    // A vassal's quarrels are its overlord's: no war between them.
    set_truce(vassal, overlord, 30.0f);
    Event e;
    e.type = EventType::Coup;
    e.severity = 5;
    e.polity = vassal;
    e.causes[0] = cause;
    e.text = strfmt("「%s」向「%s」称臣，成为其附庸，定期纳贡", title(vassal).c_str(), title(overlord).c_str());
    ctx_.chron->emit(std::move(e));
}

int Society::border_distance(u16 a, u16 b) const {
    i64 best = (i64)1 << 40;
    for (const Building& x : ctx_.buildings->all()) {
        if (!x.alive || x.polity != a) continue;
        for (const Building& y : ctx_.buildings->all())
            if (y.alive && y.polity == b) best = std::min(best, x.entrance.dist2(y.entrance));
    }
    return (int)std::sqrt((double)best);
}

bool Society::outpost_site(u16 id, Vec3i& center, Vec3i& water) const {
    const Polity* p = polity(id);
    const Building* seat = p ? ctx_.buildings->get(p->seat) : nullptr;
    if (!seat) return false;
    const IslandFeatures& f = ctx_.world->gen().features();
    std::vector<Vec3i> waters = f.waters;
    for (const Site& s : f.sites) waters.push_back(s.water);
    i64 best = -1;
    for (const Vec3i& wv : waters) {
        if (wv.y <= 0) continue;
        const i64 d = wv.dist2(seat->entrance);
        if (d < 45 * 45 || d > 140 * 140) continue;
        bool clear = true;
        for (const Building& b : ctx_.buildings->all()) {
            if (!b.alive) continue;
            const i64 db = b.entrance.dist2(wv);
            if ((b.polity == id && db < 45 * 45) || (b.polity != id && db < 80 * 80)) clear = false;
        }
        if (!clear) continue;
        if (best < 0 || d < best) {
            best = d;
            water = wv;
        }
    }
    if (best < 0) return false;
    // The village stands on dry ground a little way from the water, toward home.
    const float dx = (float)(seat->entrance.x - water.x), dz = (float)(seat->entrance.z - water.z);
    const float l = std::max(1.0f, std::sqrt(dx * dx + dz * dz));
    const int cx = water.x + (int)std::lround(dx / l * 14.0f), cz = water.z + (int)std::lround(dz / l * 14.0f);
    const ColumnInfo col = ctx_.world->gen().column(cx, cz);
    if (!col.land) return false;
    center = Vec3i{cx, (int)col.top + 1, cz};
    return true;
}

void Society::update_diplomacy(Polity& p) {
    const Building* seat = ctx_.buildings->get(p.seat);
    for (Polity& o : polities_) {
        if (!o.alive || o.id == p.id) continue;
        Diplo& d = p.diplo_ref(o.id);
        // Trespass: their people foraging, hunting or felling near our buildings; and
        // villages that have grown close to each other rub along a border.
        if (seat && !p.allied_with(o.id) && p.overlord != o.id && o.overlord != p.id) {
            int trespass = 0;
            for (const auto& cp : ctx_.agents->all()) {
                const Character* c = cp.get();
                if (!c || !c->alive || c->departed || c->polity != o.id || c->is_girl() || c->drafted) continue;
                bool near = c->foot.dist2(seat->entrance) < 70 * 70;
                for (const Vec3i& op : p.outposts)
                    if (!near && c->foot.dist2(op) < 45 * 45) near = true;
                if (near) ++trespass;
            }
            if (trespass > 0) d.grievance = std::min(2.0f, d.grievance + 0.03f * (float)std::min(trespass, 5));
            const int border = border_distance(p.id, o.id);
            if (border < kBorderReach)
                d.grievance = std::min(2.0f, d.grievance + 0.05f * (1.0f - (float)border / (float)kBorderReach) + 0.01f);
        }
        // Old wrongs fade, slowly; a truce or an alliance soothes.
        d.grievance *= d.allied || d.truce_until > ctx_.now ? 0.9f : 0.97f;
        float& att = p.attitude_ref(o.id);
        att = clampv(att - 0.03f * d.grievance, -1.0f, 1.0f);
        // Hostility cools without cause, but not below what the ruler's nature keeps up:
        // a warlike ruler goes on distrusting strangers.
        const float rest = -0.6f * std::max(0.0f, war_appetite(p.id));
        if (d.grievance < 0.1f && !p.war_with(o.id) && att < rest) att += (rest - att) * 0.03f;
        else if (d.grievance < 0.1f && !p.war_with(o.id) && att > 0.0f) att += (0.0f - att) * 0.01f;
        // An alliance does not survive open enmity.
        if (d.allied && (att < -0.2f || p.war_with(o.id))) end_alliance(p.id, o.id, "彼此失和", 0);
    }
    // Vassals pay their overlord every few days; a strong vassal may throw off the yoke
    // (decided by its ruler, see decision_war.cpp).
    if (p.overlord) {
        Polity* o = polity(p.overlord);
        if (!o) {
            p.overlord = 0;
        } else if (ctx_.now >= p.tribute_next) {
            p.tribute_next = ctx_.now + kTicksPerDay * 4;
            const i32 due = (i32)std::floor(0.15f * public_food(p.id) / 0.3f);  // about 15% of the larder
            if (due >= 3) send_tribute(p.id, o->id, due, "按期纳贡", 0);
        }
    }
}

}  // namespace icarus
