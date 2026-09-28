// Agents: medicine and carts. Knowing herbalism is not healing: herbs have to be
// gathered, carried to the wounded (or the wounded to them), and used up; a 药庐 makes
// the treatment better. Knowing the wheel is not hauling more: someone has to build
// the carts in a workshop, and a hauler has to take one along.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"
#include "icarus/util/log.h"

namespace icarus {

bool Agents::nearest_walkable(const Vec3i& p, int radius, Vec3i& out) const {
    int best = -1;
    for (int dy = -8; dy <= 8; ++dy)
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx) {
                const Vec3i q = p + Vec3i{dx, dy, dz};
                if (region_map_.find(q) == region_map_.end()) continue;
                const int d = dx * dx + dz * dz + 4 * dy * dy;
                if (best < 0 || d < best || (d == best && q < out)) {
                    best = d;
                    out = q;
                }
            }
    return best >= 0;
}

float Agents::carry_capacity(const Character& c) const {
    float cap = tune.carry_capacity;
    if (c.cart != kNoItem) cap += ctx_.reg->item(c.cart).carry;
    return cap;
}

float Agents::treatment_need(const Character& c) const {
    if (!c.alive || c.is_girl() || c.treated_until > now_) return 0.0f;
    const Polity* p = ctx_.society->polity(c.polity);
    if (!p || !p->has_tech("herbalism")) return 0.0f;
    const int total = std::max(1, c.body.total_voxels());
    const float missing = 1.0f - (float)c.body.total_alive() / (float)total;
    const float need = std::min(1.0f, c.body.bleeding * 2.0f) + missing * 2.0f;
    return need > 0.12f ? need : 0.0f;
}

bool Agents::task_treat(Character& c) {
    Task& t = c.task;
    const ItemId herbs = ctx_.reg->find_item("herbs");
    // Step 0: fetch a bundle of herbs (unless already carried).
    if (t.step == 0) {
        const Store* inv = ctx_.econ->store(c.inv);
        if (inv && inv->count(herbs) > 0) {
            t.step = 2;
        } else {
            StoreId best = kNoStore;
            float bd = 1e30f;
            for (StoreId sid : ctx_.society->public_stores(c.polity)) {
                if (ctx_.econ->available(sid, herbs, c.id) <= 0) continue;
                float d = (float)ctx_.econ->store(sid)->pos.dist2(c.foot);
                if (d < bd) {
                    bd = d;
                    best = sid;
                }
            }
            if (!best) {
                end_task(c, false);
                return false;
            }
            ctx_.econ->reserve(best, herbs, 1, c.id, now_ + kTicksPerHour);
            t.store = best;
            t.step = 1;
        }
    }
    if (t.step == 1) {
        const Store* s = ctx_.econ->store(t.store);
        if (!s) {
            end_task(c, false);
            return false;
        }
        Move m = move_to(c, s->pos, true);
        if (m == Move::Failed) {
            blacklist(c, s->pos, kTicksPerHour);
            end_task(c, false);
            return false;
        }
        if (m != Move::Arrived) {
            say(c, "去取草药");
            return true;
        }
        const i32 got = ctx_.econ->transfer(t.store, c.inv, herbs, 1);
        ctx_.econ->release(t.store, c.id);
        if (got <= 0) {
            end_task(c, false);
            return false;
        }
        t.step = 2;
    }
    // Step 2: to the 药庐 if there is one, else dress the wounds where they are.
    if (t.step == 2) {
        const Building* best = nullptr;
        float bd = 1e30f;
        for (const Building& b : ctx_.buildings->all()) {
            if (!b.alive || !b.functional || b.polity != c.polity || b.def != "herbalist") continue;
            float d = (float)b.entrance.dist2(c.foot);
            if (d < bd) {
                bd = d;
                best = &b;
            }
        }
        t.other = best ? best->id : 0;
        t.target = best ? best->entrance : c.foot;
        // The cot in the 药庐 when it is free (else by the herb rack).
        if (best) {
            const BuildingSlot* cot = free_slot(*best, SlotKind::Bed, c);
            if (!cot) cot = free_slot(*best, SlotKind::Work, c);
            if (cot) {
                t.target = cot->pos;
                t.count = 1;
            }
        }
        t.step = 3;
    }
    if (t.step == 3) {
        const Building* hb = t.count == 1 ? ctx_.buildings->get(t.other) : nullptr;
        const BuildingSlot* cot = hb ? hb->slot_at(t.target) : nullptr;
        if (cot) {
            const Move m = use_slot(c, *cot);
            if (m == Move::Moving) {
                say(c, "前往药庐");
                return true;
            }
            if (m == Move::Failed) {
                t.count = 0;
                t.target = hb->entrance;
                return true;
            }
            if (cot->kind == SlotKind::Bed) c.sleeping = true;  // lying on the cot
        } else if (c.foot.dist2(t.target) > 2 * 2) {
            Move m = move_to(c, t.target, true);
            if (m == Move::Failed) {
                t.target = c.foot;  // cannot get there: treat on the spot
                t.other = 0;
            } else {
                say(c, "前往药庐");
                return true;
            }
        }
        t.until = now_ + (t.other ? 40 : 60);
        t.step = 4;
    }
    if (t.step == 4) {
        c.moving = false;
        say(c, t.other ? (c.sleeping ? "躺在药庐的病床上疗伤" : "在药庐疗伤") : "包扎伤口");
        if (now_ < t.until) return true;
        if (ctx_.econ->remove(c.inv, herbs, 1, "treatment") <= 0) {
            end_task(c, false);
            return false;
        }
        c.body.bleeding = 0.0f;
        c.treated_until = now_ + kTicksPerDay;
        c.treated_well = t.other != 0;
        c.remember(now_, MemoryKind::Healed, kNoEntity, 0.15f, 0);
        end_task(c, true);
    }
    return true;
}

}  // namespace icarus
