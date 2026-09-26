// Society: trade between polities. A pact is agreed by both rulers; afterwards each
// side's carriers take what it can spare to the other's storehouse and bring back goods
// of equal value. The goods are the stores' own units: nothing is created on the way,
// and a caravan that cannot get there turns back with its load.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

namespace {
// What the polity keeps back before it calls anything spare, by kind of goods.
constexpr float kKeepFoodDays = 5.0f;   // food beyond this many days of eating is spare
constexpr float kWantFoodDays = 4.0f;   // below this, food is wanted
constexpr i32 kMaterialMargin = 20;     // building materials kept beyond what is planned

std::string goods_text(const Registry& reg, const std::vector<std::pair<ItemId, i32>>& list) {
    std::string s;
    for (auto& [it, n] : list) {
        if (!s.empty()) s += "、";
        s += strfmt("%s×%d", reg.item(it).name.c_str(), n);
    }
    return s.empty() ? "无" : s;
}
void add_goods(std::vector<std::pair<ItemId, i32>>& list, ItemId it, i32 n) {
    for (auto& [i, k] : list)
        if (i == it) {
            k += n;
            return;
        }
    list.push_back({it, n});
}
}  // namespace

TradeBook Society::trade_book(u16 id) const {
    TradeBook book;
    const Polity* p = polity(id);
    if (!p) return book;
    const Registry& reg = *ctx_.reg;
    std::vector<i32> stock(reg.item_count(), 0);
    for (StoreId sid : public_stores(id))
        if (const Store* s = ctx_.econ->store(sid))
            for (const ItemStack& st : s->items) stock[st.item] += ctx_.econ->available(sid, st.item);
    int people = 0, unarmed = 0, no_tool = 0;
    for (const auto& cp : ctx_.agents->all()) {
        const Character* c = cp.get();
        if (!c || !c->alive || c->departed || c->polity != id) continue;
        ++people;
        if (c->is_girl()) continue;
        if (c->tool == kNoItem) ++no_tool;
        if (c->weapon == kNoItem) ++unarmed;
    }
    people = std::max(1, people);
    struct Line {
        ItemId item;
        i32 n;
        float rank;
    };
    std::vector<Line> spare, want;

    // Food, in days of eating.
    const float per_day = std::max(1.0f, (float)people * ctx_.agents->tune.food_per_day);
    float nutrition = 0;
    for (size_t i = 0; i < stock.size(); ++i) nutrition += (float)stock[i] * reg.item((ItemId)i).nutrition;
    const float days = nutrition / per_day;
    if (days > kKeepFoodDays + 1.0f) {
        float extra = nutrition - kKeepFoodDays * per_day;
        // Perishable and raw food goes first; prepared meals are kept.
        for (const char* key : {"berries", "grain", "bread", "feast"}) {
            const ItemId it = reg.find_item(key);
            if (it == kNoItem || extra <= 0) continue;
            const float nut = reg.item(it).nutrition;
            const i32 n = std::min(stock[it], (i32)std::floor(extra / nut));
            if (n <= 0) continue;
            spare.push_back({it, n, 2.0f * (float)n * reg.item(it).value});
            extra -= (float)n * nut;
        }
    } else if (days < kWantFoodDays) {
        const float lack = kKeepFoodDays * per_day - nutrition;
        const float urgency = 10.0f + (kWantFoodDays - days);
        for (const char* key : {"grain", "bread", "berries", "feast"}) {
            const ItemId it = reg.find_item(key);
            if (it == kNoItem) continue;
            want.push_back({it, (i32)std::ceil(lack / reg.item(it).nutrition), urgency});
        }
    }

    // Building materials: what construction still needs, plus a margin.
    std::vector<i32> need(reg.item_count(), 0);
    for (const Building& b : ctx_.buildings->all()) {
        if (!b.alive || b.complete || b.polity != id) continue;
        for (auto& [it, n] : ctx_.buildings->remaining_cost(b)) need[it] += n;
    }
    for (size_t i = 1; i < stock.size(); ++i) {
        const ItemDef& d = reg.item((ItemId)i);
        const bool material = d.has_tag("material") || d.has_tag("ore") || d.has_tag("metal") || d.has_tag("fuel") ||
                              d.has_tag("storage") || d.has_tag("magic");
        if (!material || d.nutrition > 0 || d.value < 0.5f) continue;
        const i32 base = (d.key == "wood" || d.key == "stone") ? kMaterialMargin : 0;
        const i32 keep = need[i] + base;
        if (stock[i] < keep) want.push_back({(ItemId)i, keep - stock[i], need[i] > stock[i] ? 3.0f : 1.0f});
        else if (stock[i] > keep + kMaterialMargin)
            spare.push_back({(ItemId)i, stock[i] - keep - kMaterialMargin / 2, (float)(stock[i] - keep) * d.value});
    }

    // Tools, arms and medicine: one for everyone who would use them.
    const bool at_war_now = !p->wars.empty();
    const int guard = std::max(2, people / 3);
    auto kit = [&](const char* tag, int wanted, float urgency) {
        i32 have = 0;
        ItemId most = kNoItem, best = kNoItem;
        for (size_t i = 1; i < stock.size(); ++i) {
            const ItemDef& d = reg.item((ItemId)i);
            if (!d.has_tag(tag)) continue;
            have += stock[i];
            if (stock[i] > 0 && (most == kNoItem || stock[i] > stock[most])) most = (ItemId)i;
            if (best == kNoItem || d.value > reg.item(best).value) best = (ItemId)i;
        }
        if (have < wanted) {
            // Any of the kind will do; the better ones are listed first.
            std::vector<ItemId> kinds;
            for (size_t i = 1; i < stock.size(); ++i)
                if (reg.item((ItemId)i).has_tag(tag)) kinds.push_back((ItemId)i);
            std::sort(kinds.begin(), kinds.end(),
                      [&](ItemId a, ItemId b) { return reg.item(a).value > reg.item(b).value || (reg.item(a).value == reg.item(b).value && a < b); });
            for (ItemId it : kinds) want.push_back({it, wanted - have, urgency});
        } else if (most != kNoItem && have > wanted + 2) {
            spare.push_back({most, std::min(stock[most], have - wanted - 1), (float)(have - wanted) * reg.item(most).value});
        }
        (void)best;
    };
    kit("tool", no_tool, 2.0f);
    kit("weapon", at_war_now ? std::min(unarmed, guard * 2) : guard, at_war_now ? 6.0f : 0.5f);
    kit("armor", at_war_now ? guard : guard / 2, at_war_now ? 4.0f : 0.3f);
    if (p->has_tech("herbalism")) kit("medicine", people / 4 + 2, 1.5f);

    auto order = [](std::vector<Line>& v) {
        std::stable_sort(v.begin(), v.end(), [](const Line& a, const Line& b) { return a.rank > b.rank; });
    };
    order(spare);
    order(want);
    for (auto& l : spare)
        if (l.n > 0) book.spare.push_back({l.item, l.n});
    for (auto& l : want)
        if (l.n > 0) book.want.push_back({l.item, l.n});
    return book;
}

namespace {
// What the buyer would take in payment from the seller's spare goods: something it
// wants, or else anything of worth it does not itself have in plenty.
ItemId payment(const Registry& reg, const TradeBook& buyer, const TradeBook& seller, ItemId not_this) {
    for (auto& [it, n] : buyer.want)
        if (it != not_this && seller.spare_of(it) > 0) return it;
    ItemId best = kNoItem;
    float bv = 0;
    for (auto& [it, n] : seller.spare) {
        if (it == not_this || buyer.spare_of(it) > 0 || reg.item(it).value < 0.5f) continue;
        const float v = (float)n * reg.item(it).value;
        if (v > bv) {
            bv = v;
            best = it;
        }
    }
    return best;
}
}  // namespace

ItemId Society::trade_export(u16 from, u16 to, i32* amount) const {
    const Registry& reg = *ctx_.reg;
    const TradeBook a = trade_book(from), b = trade_book(to);
    // Something they need that we can spare (and they have something to pay with).
    for (auto& [it, n] : b.want) {
        const i32 s = a.spare_of(it);
        if (s <= 0 || payment(reg, a, b, it) == kNoItem) continue;
        if (amount) *amount = std::min(s, n);
        return it;
    }
    // Something we need that they can spare, paid with goods they would take.
    for (auto& [want, wn] : a.want) {
        const i32 offer = b.spare_of(want);
        if (offer <= 0) continue;
        const float worth = (float)std::min(offer, wn) * reg.item(want).value;
        ItemId pay = payment(reg, b, a, want);
        if (pay == kNoItem) continue;
        if (amount)
            *amount = std::min(a.spare_of(pay), std::max(1, (i32)std::ceil(worth / std::max(0.1f, reg.item(pay).value))));
        return pay;
    }
    return kNoItem;
}

bool Society::trade_prospect(u16 a, u16 b) const {
    return trade_export(a, b) != kNoItem || trade_export(b, a) != kNoItem;
}

EventId Society::open_trade(u16 a, u16 b, EntityId by, EventId cause) {
    Polity* pa = polity(a);
    Polity* pb = polity(b);
    if (!pa || !pb || pa->pact_with(b) || at_war(a, b)) return 0;
    Event e;
    e.type = EventType::Trade;
    e.severity = 4;
    e.actor = by;
    e.polity = a;
    e.causes[0] = cause;
    e.text = strfmt("「%s」与「%s」缔结通商之约：互通有无，以物易物", title(a).c_str(), title(b).c_str());
    e.data.set("other", (int)b);
    const EventId ev = ctx_.chron->emit(std::move(e));
    for (auto [p, o] : {std::pair<Polity*, u16>{pa, b}, std::pair<Polity*, u16>{pb, a}}) {
        TradePact t;
        t.partner = o;
        t.since = ctx_.now;
        t.event = ev;
        p->pacts.push_back(t);
        p->attitude_ref(o) = std::min(1.0f, p->attitude_to(o) + 0.1f);
    }
    return ev;
}

void Society::end_trade(u16 a, u16 b, const std::string& why, EventId cause) {
    Polity* pa = polity(a);
    Polity* pb = polity(b);
    if (!pa || !pa->pact_with(b)) return;
    auto drop = [](Polity& p, u16 other) {
        p.pacts.erase(std::remove_if(p.pacts.begin(), p.pacts.end(), [&](const TradePact& t) { return t.partner == other; }),
                      p.pacts.end());
    };
    drop(*pa, b);
    if (pb) drop(*pb, a);
    Event e;
    e.type = EventType::Trade;
    e.severity = 4;
    e.polity = a;
    e.causes[0] = cause;
    e.text = strfmt("「%s」与「%s」的通商中断：%s", title(a).c_str(), pb ? title(b).c_str() : "?", why.c_str());
    e.data.set("other", (int)b);
    ctx_.chron->emit(std::move(e));
}

TradeDeal Society::exchange(u16 from, u16 to, StoreId inv, StoreId at, ItemId load, i32 count, float carry) {
    TradeDeal deal;
    Polity* pa = polity(from);
    Polity* pb = polity(to);
    const Store* bag = ctx_.econ->store(inv);
    const Store* store = ctx_.econ->store(at);
    if (!pa || !pb || !bag || !store || load == kNoItem) return deal;
    TradePact* ta = pa->pact_with(to);
    TradePact* tb = pb->pact_with(from);
    if (!ta || !tb || at_war(from, to)) return deal;
    const Registry& reg = *ctx_.reg;
    const TradeBook a = trade_book(from), b = trade_book(to);
    // Only the load is for sale (never the carrier's own tools), and only if they do
    // not have plenty of it themselves.
    const ItemId out = load;
    const i32 have = std::min(count, bag->count(load));
    if (have <= 0 || b.spare_of(out) > 0) return deal;
    const ItemId in = payment(reg, a, b, out);
    if (in == kNoItem) return deal;
    const float vo = std::max(0.05f, reg.item(out).value), vi = std::max(0.05f, reg.item(in).value);
    // What the payment can be: their spare goods at this store, what the carrier can
    // hold once the load is handed over, and no more than the load is worth.
    const i32 offer = std::min(b.spare_of(in), ctx_.econ->available(at, in));
    const i32 room = (i32)std::floor(std::max(0.0f, carry - (ctx_.econ->weight(*bag) - (float)have * reg.item(out).weight)) /
                                     std::max(0.05f, reg.item(in).weight));
    i32 in_n = std::min({offer, room, (i32)std::floor((float)have * vo / vi + 0.5f)});
    if (in_n <= 0) return deal;
    // Hand over as much of the load as the payment is worth.
    const i32 out_n = std::min(have, std::max(1, (i32)std::lround((float)in_n * vi / vo)));
    const i32 given = ctx_.econ->transfer(inv, at, out, out_n);
    if (given <= 0) return deal;
    in_n = std::min(in_n, std::max(1, (i32)std::lround((float)given * vo / vi)));
    const i32 got = ctx_.econ->transfer(at, inv, in, in_n);
    deal.out = out;
    deal.in = in;
    deal.out_n = given;
    deal.in_n = got;
    deal.value = (float)given * vo;
    ta->trips++;
    ta->sent += (float)given * vo;
    ta->received += (float)got * vi;
    tb->sent += (float)got * vi;
    tb->received += (float)given * vo;
    add_goods(ta->out_today, out, given);
    if (got > 0) add_goods(ta->in_today, in, got);
    // Dealing breeds familiarity.
    for (auto [p, o] : {std::pair<Polity*, u16>{pa, to}, std::pair<Polity*, u16>{pb, from}})
        if (float& att = p->attitude_ref(o); att < 0.8f) att = std::min(0.8f, att + 0.02f);
    if (ta->blocked) {
        Event e;
        e.type = EventType::Trade;
        e.severity = 3;
        e.polity = from;
        e.causes[0] = ta->blocked;
        e.text = strfmt("「%s」通往「%s」的商路重新畅通", pa->name.c_str(), pb->name.c_str());
        ctx_.chron->emit(std::move(e));
        ta->blocked = 0;
    }
    ta->blocked_until = 0;
    return deal;
}

ItemId Society::aid_food(u16 from, u16 to, i32* amount) const {
    const Registry& reg = *ctx_.reg;
    const TradeBook a = trade_book(from), b = trade_book(to);
    for (auto& [it, n] : a.spare) {
        if (reg.item(it).nutrition <= 0) continue;
        const i32 lack = b.want_of(it);
        if (lack <= 0) continue;
        if (amount) *amount = std::min({n, lack, 120});
        return it;
    }
    return kNoItem;
}

void Society::aid_delivered(u16 from, u16 to, ItemId item, i32 n, EntityId carrier, EventId cause) {
    Polity* pa = polity(from);
    Polity* pb = polity(to);
    if (!pa || !pb || n <= 0) return;
    if (float& att = pb->attitude_ref(from); att < 0.8f) att = std::min(0.8f, att + 0.06f);
    Event e;
    e.type = EventType::Trade;
    e.severity = 3;
    e.actor = carrier;
    e.polity = from;
    e.causes[0] = cause;
    e.text = strfmt("「%s」的援粮送抵「%s」：%s×%d", pa->name.c_str(), pb->name.c_str(), ctx_.reg->item(item).name.c_str(), n);
    e.data.set("other", (int)to);
    ctx_.chron->emit(std::move(e));
}

void Society::trade_road_blocked(u16 from, u16 to) {
    Polity* pa = polity(from);
    Polity* pb = polity(to);
    if (!pa || !pb) return;
    TradePact* t = pa->pact_with(to);
    if (!t) return;
    t->blocked_until = ctx_.now + kTicksPerHour * 12;
    if (t->blocked) return;
    // Why: the latest destruction on the island, if there was any lately.
    EventId why = 0;
    const auto& ev = ctx_.chron->events();
    for (auto it = ev.rbegin(); it != ev.rend() && !why; ++it) {
        if (ctx_.now - it->tick > kTicksPerDay * 2) break;
        if (it->type == EventType::StructureDestroyed || it->type == EventType::Collapse ||
            it->type == EventType::MeteorImpact || it->type == EventType::PathBlocked)
            why = it->id;
    }
    Event e;
    e.type = EventType::Trade;
    e.severity = 3;
    e.polity = from;
    e.causes[0] = why ? why : t->event;
    e.causes[1] = t->event;
    e.text = strfmt("「%s」通往「%s」的商路受阻：商队到不了对方的仓库，只好折返", pa->name.c_str(), pb->name.c_str());
    t->blocked = ctx_.chron->emit(std::move(e));
}

void Society::tidy_pacts(Polity& p) {
    // Partners that no longer exist (annexed, dissolved) leave nothing to trade with.
    p.pacts.erase(std::remove_if(p.pacts.begin(), p.pacts.end(), [&](const TradePact& t) { return !polity(t.partner); }),
                  p.pacts.end());
}

void Society::trade_daily() {
    const Registry& reg = *ctx_.reg;
    for (Polity& p : polities_) {
        if (!p.alive) continue;
        for (TradePact& t : p.pacts) {
            if (t.out_today.empty() && t.in_today.empty()) continue;
            const Polity* o = polity(t.partner);
            Event e;
            e.type = EventType::Trade;
            e.severity = 2;
            e.polity = p.id;
            e.causes[0] = t.event;
            e.text = strfmt("「%s」的商队往来「%s」：送去 %s，换回 %s", p.name.c_str(), o ? o->name.c_str() : "?",
                            goods_text(reg, t.out_today).c_str(), goods_text(reg, t.in_today).c_str());
            e.data.set("other", (int)t.partner);
            ctx_.chron->emit(std::move(e));
            t.out_today.clear();
            t.in_today.clear();
        }
    }
}

}  // namespace icarus
