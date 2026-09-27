#include "icarus/economy/economy.h"

#include <algorithm>
#include <cmath>

namespace icarus {

const char* store_kind_key(StoreKind k) {
    switch (k) {
        case StoreKind::Stockpile: return "stockpile";
        case StoreKind::Pile: return "pile";
        case StoreKind::Carried: return "carried";
        case StoreKind::Site: return "site";
        case StoreKind::Home: return "home";
        case StoreKind::Workshop: return "workshop";
    }
    return "?";
}

void Economy::reset() {
    stores_.assign(1, Store{});
    free_ids_.clear();
    ledger_.assign(reg_->item_count(), LedgerLine{});
    reasons_.clear();
}

StoreId Economy::create_store(StoreKind kind, const Vec3i& pos, u16 polity, EntityId owner, float capacity) {
    StoreId id;
    if (!free_ids_.empty()) {
        // Reuse the lowest free id for determinism.
        auto it = std::min_element(free_ids_.begin(), free_ids_.end());
        id = *it;
        free_ids_.erase(it);
    } else {
        id = (StoreId)stores_.size();
        stores_.emplace_back();
    }
    Store& s = stores_[id];
    s = Store{};
    s.id = id;
    s.kind = kind;
    s.alive = true;
    s.pos = pos;
    s.polity = polity;
    s.owner = owner;
    s.capacity = capacity;
    return id;
}

void Economy::destroy_store(StoreId id) {
    Store* s = store(id);
    if (!s) return;
    if (!s->empty() && s->kind != StoreKind::Pile) {
        Vec3i p = s->pos;
        StoreId pile = pile_at(p);
        transfer_all(id, pile);
    }
    s = store(id);
    if (!s) return;
    if (!s->empty()) {
        // A pile being destroyed with contents: move them to a fresh pile (never lose items).
        StoreId np = create_store(StoreKind::Pile, stores_[id].pos, 0);
        transfer_all(id, np);
    }
    stores_[id].alive = false;
    stores_[id].items.clear();
    stores_[id].reserved.clear();
    free_ids_.push_back(id);
}

ItemStack* Economy::find_stack(Store& s, ItemId item) {
    for (auto& st : s.items)
        if (st.item == item) return &st;
    return nullptr;
}

void Economy::compact(Store& s) {
    s.items.erase(std::remove_if(s.items.begin(), s.items.end(), [](const ItemStack& x) { return x.count <= 0; }),
                  s.items.end());
}

float Economy::weight(const Store& s) const {
    float w = 0;
    for (auto& st : s.items) w += reg_->item(st.item).weight * (float)st.count;
    return w;
}

i32 Economy::add(StoreId sid, ItemId item, i32 n, const std::string& reason) {
    Store* s = store(sid);
    if (!s || n <= 0 || item == kNoItem) return 0;
    ItemStack* st = find_stack(*s, item);
    if (!st) {
        s->items.push_back({item, 0});
        st = &s->items.back();
        std::sort(s->items.begin(), s->items.end(), [](const ItemStack& a, const ItemStack& b) { return a.item < b.item; });
        st = find_stack(*s, item);
    }
    st->count += n;
    ledger_[item].produced += n;
    reasons_["+" + reason + ":" + reg_->item(item).key] += n;
    return n;
}

i32 Economy::remove(StoreId sid, ItemId item, i32 n, const std::string& reason) {
    Store* s = store(sid);
    if (!s || n <= 0) return 0;
    ItemStack* st = find_stack(*s, item);
    if (!st) return 0;
    i32 k = std::min(n, st->count);
    st->count -= k;
    ledger_[item].consumed += k;
    reasons_["-" + reason + ":" + reg_->item(item).key] += k;
    compact(*s);
    return k;
}

i32 Economy::transfer(StoreId from, StoreId to, ItemId item, i32 n) {
    Store* a = store(from);
    Store* b = store(to);
    if (!a || !b || from == to || n <= 0) return 0;
    ItemStack* st = find_stack(*a, item);
    if (!st) return 0;
    float unit = std::max(0.001f, reg_->item(item).weight);
    // (Room counted in doubles: an unlimited store holds more light items than an i32.)
    const double fit = std::floor((double)free_capacity(*b) / (double)unit + 1e-4);
    const i32 room = (i32)std::clamp(fit, -1.0, 2.0e9);
    i32 k = std::min({n, st->count, room});
    if (k <= 0) return 0;
    st->count -= k;
    compact(*a);
    ItemStack* dst = find_stack(*b, item);
    if (!dst) {
        b->items.push_back({item, 0});
        std::sort(b->items.begin(), b->items.end(), [](const ItemStack& x, const ItemStack& y) { return x.item < y.item; });
        dst = find_stack(*b, item);
    }
    dst->count += k;
    return k;
}

i32 Economy::transfer_all(StoreId from, StoreId to) {
    Store* a = store(from);
    if (!a) return 0;
    i32 moved = 0;
    std::vector<ItemStack> copy = a->items;
    for (auto& st : copy) moved += transfer(from, to, st.item, st.count);
    return moved;
}

i32 Economy::available(StoreId sid, ItemId item, EntityId for_agent) const {
    const Store* s = store(sid);
    if (!s) return 0;
    i32 c = s->count(item);
    for (auto& r : s->reserved)
        if (r.item == item && r.agent != for_agent) c -= r.count;
    return std::max(0, c);
}

bool Economy::reserve(StoreId sid, ItemId item, i32 n, EntityId agent, Tick expires) {
    Store* s = store(sid);
    if (!s || available(sid, item, agent) < n) return false;
    for (auto& r : s->reserved) {
        if (r.agent == agent && r.item == item) {
            r.count = n;
            r.expires = expires;
            return true;
        }
    }
    s->reserved.push_back({agent, item, n, expires});
    return true;
}

void Economy::release_agent(EntityId agent) {
    for (auto& s : stores_) {
        if (!s.alive || s.reserved.empty()) continue;
        s.reserved.erase(std::remove_if(s.reserved.begin(), s.reserved.end(),
                                        [&](const Reservation& r) { return r.agent == agent; }),
                         s.reserved.end());
    }
}

void Economy::release(StoreId sid, EntityId agent) {
    Store* s = store(sid);
    if (!s) return;
    s->reserved.erase(std::remove_if(s->reserved.begin(), s->reserved.end(),
                                     [&](const Reservation& r) { return r.agent == agent; }),
                      s->reserved.end());
}

void Economy::expire_reservations(Tick now) {
    for (auto& s : stores_) {
        if (!s.alive || s.reserved.empty()) continue;
        s.reserved.erase(std::remove_if(s.reserved.begin(), s.reserved.end(),
                                        [&](const Reservation& r) { return r.expires <= now; }),
                         s.reserved.end());
    }
}

StoreId Economy::pile_at(const Vec3i& pos) {
    for (auto& s : stores_)
        if (s.alive && s.kind == StoreKind::Pile && s.pos == pos) return s.id;
    return create_store(StoreKind::Pile, pos, 0);
}

StoreId Economy::drop(StoreId from, const Vec3i& pos) {
    StoreId p = pile_at(pos);
    transfer_all(from, p);
    return p;
}

i64 Economy::total(ItemId item) const {
    i64 t = 0;
    for (auto& s : stores_)
        if (s.alive) t += s.count(item);
    return t;
}

i64 Economy::total_in(u16 polity, StoreKind kind, ItemId item) const {
    i64 t = 0;
    for (auto& s : stores_)
        if (s.alive && s.polity == polity && s.kind == kind) t += s.count(item);
    return t;
}

float Economy::food_nutrition_in(StoreId sid) const {
    const Store* s = store(sid);
    if (!s) return 0;
    float n = 0;
    for (auto& st : s->items) n += reg_->item(st.item).nutrition * (float)st.count;
    return n;
}

void Economy::spoil(Rng& rng, const std::function<float(u16)>& polity_factor) {
    for (auto& s : stores_) {
        if (!s.alive) continue;
        const float pf = (polity_factor && s.polity) ? polity_factor(s.polity) : 1.0f;
        std::vector<ItemStack> copy = s.items;
        for (auto& st : copy) {
            const ItemDef& d = reg_->item(st.item);
            if (d.spoil_per_day <= 0 || st.count <= 0) continue;
            float expected = (float)st.count * d.spoil_per_day * s.spoil_factor * pf;
            i32 k = (i32)std::floor(expected);
            if (rng.chance(expected - (float)k)) ++k;
            if (k > 0) remove(s.id, st.item, k, "spoiled");
        }
    }
}

void Economy::save(BinWriter& w) const {
    size_t sec = w.begin_section("ECON");
    w.varu(stores_.size());
    for (size_t i = 1; i < stores_.size(); ++i) {
        const Store& s = stores_[i];
        w.boolean(s.alive);
        if (!s.alive) continue;
        w.u8v((u8)s.kind);
        w.vec3i(s.pos);
        w.u16v(s.polity);
        w.u32v(s.owner);
        w.u32v(s.building);
        w.f32(s.capacity);
        w.f32(s.spoil_factor);
        w.varu(s.items.size());
        for (auto& st : s.items) {
            w.u16v(st.item);
            w.vari(st.count);
        }
        w.varu(s.reserved.size());
        for (auto& r : s.reserved) {
            w.u32v(r.agent);
            w.u16v(r.item);
            w.vari(r.count);
            w.u64v(r.expires);
        }
    }
    w.varu(free_ids_.size());
    for (StoreId f : free_ids_) w.u32v(f);
    w.varu(ledger_.size());
    for (auto& l : ledger_) {
        w.i64v(l.produced);
        w.i64v(l.consumed);
    }
    w.varu(reasons_.size());
    for (auto& [k, v] : reasons_) {
        w.str(k);
        w.i64v(v);
    }
    w.end_section(sec);
}

void Economy::load(BinReader& outer) {
    BinReader r = outer.section("ECON");
    u64 n = r.varu();
    stores_.assign((size_t)n, Store{});
    for (size_t i = 1; i < (size_t)n; ++i) {
        Store& s = stores_[i];
        s.id = (StoreId)i;
        s.alive = r.boolean();
        if (!s.alive) continue;
        s.kind = (StoreKind)r.u8v();
        s.pos = r.vec3i();
        s.polity = r.u16v();
        s.owner = r.u32v();
        s.building = r.u32v();
        s.capacity = r.f32();
        s.spoil_factor = r.f32();
        u64 ni = r.varu();
        for (u64 k = 0; k < ni; ++k) {
            ItemStack st;
            st.item = r.u16v();
            st.count = (i32)r.vari();
            s.items.push_back(st);
        }
        u64 nr = r.varu();
        for (u64 k = 0; k < nr; ++k) {
            Reservation rs;
            rs.agent = r.u32v();
            rs.item = r.u16v();
            rs.count = (i32)r.vari();
            rs.expires = r.u64v();
            s.reserved.push_back(rs);
        }
    }
    free_ids_.clear();
    u64 nf = r.varu();
    for (u64 k = 0; k < nf; ++k) free_ids_.push_back(r.u32v());
    u64 nl = r.varu();
    ledger_.assign(std::max<size_t>((size_t)nl, reg_->item_count()), LedgerLine{});
    for (u64 k = 0; k < nl; ++k) {
        ledger_[k].produced = r.i64v();
        ledger_[k].consumed = r.i64v();
    }
    reasons_.clear();
    u64 nrs = r.varu();
    for (u64 k = 0; k < nrs; ++k) {
        std::string key = r.str();
        reasons_[key] = r.i64v();
    }
}

u64 Economy::hash() const {
    u64 h = 7;
    for (auto& s : stores_) {
        if (!s.alive) continue;
        h = hash_combine(h, s.id);
        for (auto& st : s.items) h = hash_combine(h, ((u64)st.item << 32) ^ (u64)(u32)st.count);
    }
    return h;
}

}  // namespace icarus
