// Material ledger. Every item unit lives in exactly one Store: a stockpile, a ground
// pile, a character's hands, a construction site, a home pantry or a workshop.
// Creation and destruction of items only happen through add()/remove() with a reason,
// so conservation can be audited at any time.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "icarus/data/registry.h"
#include "icarus/util/binio.h"
#include "icarus/util/rng.h"
#include "icarus/util/types.h"

namespace icarus {

using StoreId = u32;
constexpr StoreId kNoStore = 0;

enum class StoreKind : u8 { Stockpile = 0, Pile, Carried, Site, Home, Workshop };
const char* store_kind_key(StoreKind k);

struct ItemStack {
    ItemId item = kNoItem;
    i32 count = 0;
};

struct Reservation {
    EntityId agent = kNoEntity;
    ItemId item = kNoItem;
    i32 count = 0;
    Tick expires = 0;
};

struct Store {
    StoreId id = 0;
    StoreKind kind = StoreKind::Pile;
    bool alive = false;
    Vec3i pos;          // access point (a standable position next to it)
    u16 polity = 0;
    EntityId owner = kNoEntity;  // character (Carried/Home) or 0
    u32 building = 0;
    float capacity = 1e9f;       // weight units
    float spoil_factor = 1.0f;   // storage quality (granary < 1)
    std::vector<ItemStack> items;
    std::vector<Reservation> reserved;

    i32 count(ItemId it) const {
        for (auto& s : items)
            if (s.item == it) return s.count;
        return 0;
    }
    bool empty() const {
        for (auto& s : items)
            if (s.count > 0) return false;
        return true;
    }
};

struct LedgerLine {
    i64 produced = 0;
    i64 consumed = 0;
};

class Economy {
public:
    explicit Economy(const Registry& reg) : reg_(&reg) {}

    void reset();
    void set_now(Tick t) { now_ = t; }

    StoreId create_store(StoreKind kind, const Vec3i& pos, u16 polity, EntityId owner = kNoEntity,
                         float capacity = 1e9f);
    // Removes a store; remaining items are dropped into a ground pile at its position.
    void destroy_store(StoreId id);
    Store* store(StoreId id) { return (id > 0 && id < stores_.size() && stores_[id].alive) ? &stores_[id] : nullptr; }
    const Store* store(StoreId id) const {
        return (id > 0 && id < stores_.size() && stores_[id].alive) ? &stores_[id] : nullptr;
    }
    const std::vector<Store>& stores() const { return stores_; }

    // Sources / sinks (logged by reason).
    i32 add(StoreId s, ItemId item, i32 n, const std::string& reason);
    i32 remove(StoreId s, ItemId item, i32 n, const std::string& reason);
    // Moves up to n items; returns the number moved.
    i32 transfer(StoreId from, StoreId to, ItemId item, i32 n);
    i32 transfer_all(StoreId from, StoreId to);

    float weight(const Store& s) const;
    float free_capacity(const Store& s) const { return s.capacity - weight(s); }
    i32 available(StoreId s, ItemId item, EntityId for_agent = kNoEntity) const;
    bool reserve(StoreId s, ItemId item, i32 n, EntityId agent, Tick expires);
    void release_agent(EntityId agent);
    void release(StoreId s, EntityId agent);
    void expire_reservations(Tick now);

    // Ground piles: find an existing pile at pos or create one.
    StoreId pile_at(const Vec3i& pos);
    StoreId drop(StoreId from, const Vec3i& pos);  // moves everything to a pile

    // Queries.
    i64 total(ItemId item) const;
    i64 total_in(u16 polity, StoreKind kind, ItemId item) const;
    float food_nutrition_in(StoreId s) const;
    const LedgerLine& ledger(ItemId item) const { return ledger_[item]; }
    const std::map<std::string, i64>& reasons() const { return reasons_; }
    // Food each polity has brought in, by source (0 fields, 1 foraging, 2 hunting,
    // 3 fishing), in nutrition units since the start; zeros for a polity with none.
    std::array<double, 4> food_in(u16 polity) const {
        auto it = food_in_.find(polity);
        return it == food_in_.end() ? std::array<double, 4>{0, 0, 0, 0} : it->second;
    }

    // Daily spoilage of perishable food in every store.
    void spoil(Rng& rng, const std::function<float(u16)>& polity_factor = {});

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    ItemStack* find_stack(Store& s, ItemId item);
    void compact(Store& s);

    const Registry* reg_;
    std::vector<Store> stores_ = std::vector<Store>(1);
    std::vector<StoreId> free_ids_;
    std::vector<LedgerLine> ledger_;
    std::map<std::string, i64> reasons_;
    std::map<u16, std::array<double, 4>> food_in_;
    Tick now_ = 0;
};

}  // namespace icarus
