// Agents: using the furniture of buildings. A bed is climbed into from the floor beside
// it and lain in with the head on the pillow; a mat is stepped onto; a stool or bench is
// sat on facing the desk or table; a workbench, hearth or herb rack is worked at from
// the floor before it. The slots come from the blueprints (Buildings::derive); who uses
// which is read off what people are doing (their task's target), so nothing more is saved.
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"

namespace icarus {

bool Agents::slot_busy(const Vec3i& p, const Character& c) const {
    for (const auto& op : chars_) {
        if (!op || !op->alive || op->departed || op.get() == &c) continue;
        if (op->foot == p && !op->moving) return true;
        if (op->task.type != TaskType::None && op->task.target == p) return true;
    }
    return false;
}

const BuildingSlot* Agents::free_slot(const Building& b, SlotKind k, const Character& c, int rank,
                                      const char* what) const {
    std::vector<const BuildingSlot*> of;
    for (const BuildingSlot& s : b.slots)
        if (s.kind == k && (!what || ctx_.world->material(s.pos).furniture == what)) of.push_back(&s);
    if (of.empty()) return nullptr;
    const size_t n = of.size(), r = (size_t)std::max(0, rank) % n;
    for (size_t k2 = 0; k2 < n; ++k2) {
        const BuildingSlot* s = of[(r + k2) % n];
        // Still there (not burnt or smashed) and free.
        if (ctx_.world->material(s->pos).furniture.empty() && ctx_.world->material(s->face).furniture.empty()) continue;
        if (!slot_busy(s->pos, c)) return s;
    }
    return nullptr;
}

Agents::Move Agents::use_slot(Character& c, const BuildingSlot& s) {
    auto face = [&]() {
        if (s.kind == SlotKind::Bed) {
            // Lying on her side along the bed, the head at its head end (the figure lies
            // with its head toward the local -x).
            const float hx = (float)(s.face.x - s.pos.x), hz = (float)(s.face.z - s.pos.z);
            const float ax = hx != 0.0f || hz != 0.0f ? hx : -(float)s.axis.x, az = hx != 0.0f || hz != 0.0f ? hz : -(float)s.axis.z;
            c.yaw = std::atan2(az, -ax);
        } else {
            c.yaw = std::atan2((float)s.face.x + 0.5f - c.pos.x, (float)s.face.z + 0.5f - c.pos.z);
        }
    };
    if (c.foot == s.pos) {
        c.moving = false;
        face();
        return Move::Arrived;
    }
    if (c.foot != s.access) {
        const Move m = move_to(c, s.access, false);
        if (m != Move::Arrived) return m;
        if (s.access == s.pos) {
            face();
            return Move::Arrived;
        }
    }
    // Beside the bed: over onto it.
    const Vec3f to((float)s.pos.x + 0.5f, (float)s.pos.y, (float)s.pos.z + 0.5f);
    const Vec3f d = to - c.pos;
    const float dist = d.length(), step = 0.12f;
    if (dist > step) {
        c.moving = true;  // (not pushed about by keep_apart on the way)
        c.pos += d * (step / dist);
        return Move::Moving;
    }
    c.moving = false;
    c.pos = to;
    c.foot = s.pos;
    c.path.clear();
    if (Store* st = ctx_.econ->store(c.inv)) st->pos = c.foot;
    face();
    return Move::Arrived;
}

void Agents::leave_furniture(Character& c) {
    const Material& m = ctx_.world->material(c.foot);
    Nav& nav = *ctx_.nav;
    // (A mat or a stool with room overhead: she just gets up. Out of a bed, or from a mat
    // under a low roof, back to where she got in.)
    if (m.furniture.empty() || (!m.solid && nav.standable(c.foot))) return;
    // Back onto the floor she climbed in from, else any open floor beside the bed.
    if (const Building* b = ctx_.buildings->get(ctx_.buildings->at(c.foot)))
        if (const BuildingSlot* s = b->slot_at(c.foot); s && nav.standable(s->access)) {
            place_at(c, s->access);
            return;
        }
    for (const Vec3i& d : {Vec3i{1, 0, 0}, Vec3i{-1, 0, 0}, Vec3i{0, 0, 1}, Vec3i{0, 0, -1}})
        if (nav.standable(c.foot + d)) {
            place_at(c, c.foot + d);
            return;
        }
    Vec3i np;
    if (nav.find_standable_near(c.foot + Vec3i{0, 1, 0}, np, 3)) place_at(c, np);
}

}  // namespace icarus
