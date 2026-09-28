// Agents: equipment. Every kind of work has its tool — an axe to fell, a pick to
// quarry, a hoe to till, a hammer to build, a sickle to reap, a knife to cut and sew.
// With the right tool in hand the work goes at its normal pace (faster with copper and
// iron); with bare hands felling and quarrying crawl. Workers fetch the right tool from a
// store on the way when one is there, hand back the one they held, and wear tools out.
// Clothes keep out cold, rain and the night.
#include <algorithm>
#include <cmath>

#include "icarus/agents/agents.h"
#include "icarus/economy/buildings.h"
#include "icarus/sim/clock.h"
#include "icarus/society/society.h"

namespace icarus {

namespace {
// Bare-handed work relative to a stone tool of the kind.
float bare_hands(const std::string& kind) {
    if (kind == "axe") return 0.35f;
    if (kind == "pick") return 0.3f;
    if (kind == "hoe") return 0.55f;
    if (kind == "hammer") return 0.6f;
    if (kind == "sickle") return 0.7f;
    if (kind == "knife") return 0.6f;
    return 1.0f;
}
constexpr int kToolDetour = 60;  // cubes a worker walks out of the way for a tool
}  // namespace

const char* Agents::occupation_tool(const std::string& occupation) {
    if (occupation == "food") return "hoe";
    if (occupation == "build") return "hammer";
    if (occupation == "gather") return "axe";
    return "";
}

std::string Agents::tool_kind_for(const Job& j) const {
    const Registry& reg = *ctx_.reg;
    switch (j.type) {
        case JobType::Chop: return "axe";
        case JobType::Mine: return "pick";
        case JobType::Dig: return reg.mat(vmat(ctx_.world->peek(j.pos))).hardness >= 50 ? "pick" : "hoe";
        case JobType::Till: return "hoe";
        case JobType::Harvest: return "sickle";
        case JobType::Forage: {
            const Material& m = reg.mat(vmat(ctx_.world->peek(j.pos)));
            if (m.forage_item == kNoItem) return "";
            const std::string& k = reg.item(m.forage_item).key;
            return (k == "grain" || k == "fiber") ? "sickle" : "";
        }
        case JobType::Build: return "hammer";
        case JobType::Hunt: return "knife";  // the butchering; the chase is the weapon's
        case JobType::Craft: {
            const Json& recipes = reg.doc("recipes")["recipes"];
            if (j.plot >= recipes.size()) return "hammer";
            const std::string t = recipes[(size_t)j.plot].str("tool");
            return t.empty() ? "hammer" : t;
        }
        default: return "";
    }
}

float Agents::tool_factor(const Character& c, const std::string& kind) const {
    if (kind.empty()) return 1.0f;
    if (c.tool != kNoItem) {
        const ItemDef& d = ctx_.reg->item(c.tool);
        if (d.tool_kind == kind) return std::max(0.2f, d.power);
        if (d.tool_kind == "kit") return d.power * 0.8f;
    }
    return bare_hands(kind);
}

StoreId Agents::tool_store_for(Character& c, const std::string& kind, const Vec3i& work) {
    if (kind.empty() || c.is_girl() || !c.body.can_hold()) return kNoStore;
    const Registry& reg = *ctx_.reg;
    if (c.tool != kNoItem) {
        const ItemDef& d = reg.item(c.tool);
        if (d.tool_kind == kind || d.tool_kind == "kit") return kNoStore;
    }
    const float direct = std::sqrt((float)c.foot.dist2(work));
    StoreId best = kNoStore;
    ItemId best_item = kNoItem;
    float best_cost = (float)kToolDetour;
    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
        const Store* st = ctx_.econ->store(sid);
        if (!st) continue;
        const float detour =
            std::sqrt((float)c.foot.dist2(st->pos)) + std::sqrt((float)st->pos.dist2(work)) - direct;
        if (detour > best_cost) continue;
        ItemId pick = kNoItem;
        float pw = 0.0f;
        for (const ItemStack& is : st->items) {
            const ItemDef& d = reg.item(is.item);
            if (d.tool_kind != kind || ctx_.econ->available(sid, is.item, c.id) <= 0) continue;
            if (d.power > pw) {
                pw = d.power;
                pick = is.item;
            }
        }
        if (pick == kNoItem) continue;
        best = sid;
        best_item = pick;
        best_cost = detour;
    }
    if (best != kNoStore) ctx_.econ->reserve(best, best_item, 1, c.id, now_ + kTicksPerHour);
    return best;
}

bool Agents::swap_tool(Character& c, StoreId sid, const std::string& kind) {
    const Registry& reg = *ctx_.reg;
    const Store* st = ctx_.econ->store(sid);
    if (!st) return false;
    ctx_.econ->release(sid, c.id);
    ItemId pick = kNoItem;
    float pw = 0.0f;
    for (const ItemStack& is : st->items) {
        const ItemDef& d = reg.item(is.item);
        if (d.tool_kind != kind || ctx_.econ->available(sid, is.item, c.id) <= 0) continue;
        if (d.power > pw) {
            pw = d.power;
            pick = is.item;
        }
    }
    if (pick == kNoItem) return false;
    if (ctx_.econ->transfer(sid, c.inv, pick, 1) != 1) return false;
    if (c.tool != kNoItem && c.tool != pick) ctx_.econ->transfer(c.inv, sid, c.tool, 1);
    c.tool = pick;
    c.tool_wear = 0;
    return true;
}

StoreId Agents::hunting_weapon_store(Character& c, const Vec3i& work) {
    if (c.weapon != kNoItem || c.is_girl() || !c.body.can_hold()) return kNoStore;
    const Registry& reg = *ctx_.reg;
    const float direct = std::sqrt((float)c.foot.dist2(work));
    StoreId best = kNoStore;
    ItemId best_item = kNoItem;
    float best_cost = (float)kToolDetour;
    for (StoreId sid : ctx_.society->public_stores(c.polity)) {
        const Store* st = ctx_.econ->store(sid);
        if (!st) continue;
        const float detour =
            std::sqrt((float)c.foot.dist2(st->pos)) + std::sqrt((float)st->pos.dist2(work)) - direct;
        if (detour > best_cost) continue;
        for (const ItemStack& is : st->items) {
            if (!reg.item(is.item).has_tag("hunting") || ctx_.econ->available(sid, is.item, c.id) <= 0) continue;
            best = sid;
            best_item = is.item;
            best_cost = detour;
            break;
        }
    }
    if (best != kNoStore) ctx_.econ->reserve(best, best_item, 1, c.id, now_ + kTicksPerHour);
    return best;
}

bool Agents::take_hunting_weapon(Character& c, StoreId sid) {
    const Registry& reg = *ctx_.reg;
    const Store* st = ctx_.econ->store(sid);
    if (!st || c.weapon != kNoItem) return false;
    ctx_.econ->release(sid, c.id);
    // The one that hits hardest (a bow reaches farther, a spear strikes harder).
    ItemId pick = kNoItem;
    float pw = -1.0f;
    for (const ItemStack& is : st->items) {
        const ItemDef& d = reg.item(is.item);
        if (!d.has_tag("hunting") || ctx_.econ->available(sid, is.item, c.id) <= 0) continue;
        const float v = d.power * (d.range > 3.0f ? 1.3f : 1.0f);
        if (v > pw) {
            pw = v;
            pick = is.item;
        }
    }
    if (pick == kNoItem || ctx_.econ->transfer(sid, c.inv, pick, 1) != 1) return false;
    c.weapon = pick;
    return true;
}

void Agents::wear_tool(Character& c) {
    if (c.tool == kNoItem) return;
    const ItemDef& d = ctx_.reg->item(c.tool);
    if (d.durability <= 0) return;
    if (++c.tool_wear < d.durability) return;
    ctx_.econ->remove(c.inv, c.tool, 1, "worn_out");
    say(c, d.name + "用坏了");
    c.tool = kNoItem;
    c.tool_wear = 0;
}

i32 Agents::cargo_kept(const Character& c, ItemId item) const {
    i32 n = 0;
    for (const Job& j : ctx_.jobs->all())
        if (j.alive && j.type == JobType::Trade && j.claimed_by == c.id && j.from == c.inv && j.item == item) n += j.count;
    return n;
}

void Agents::drop_cargo(Character& c) {
    const Store* s = ctx_.econ->store(c.inv);
    if (!s) return;
    const std::vector<ItemStack> items = s->items;
    StoreId pile = kNoStore;
    for (const ItemStack& st : items) {
        const i32 keep = (st.item == c.tool) + (st.item == c.weapon) + (st.item == c.armor) + (st.item == c.cart) +
                         (st.item == c.clothes) + cargo_kept(c, st.item);
        if (st.count <= keep) continue;
        if (!pile) pile = ctx_.econ->pile_at(c.foot);
        ctx_.econ->transfer(c.inv, pile, st.item, st.count - keep);
    }
}

bool Agents::at_home(const Character& c) const {
    const Building* h = ctx_.buildings->get(c.home);
    if (!h || !h->functional) return false;
    // A house furnished with beds: only within its walls.
    if (!h->slots.empty()) return h->contains(c.foot);
    if (c.foot.chebyshev(h->inside) <= 3) return true;
    // A large house (a hall, a longhouse): anywhere within its walls.
    if (std::abs(c.foot.y - h->inside.y) > 1) return false;
    int x0 = 1 << 30, x1 = -(1 << 30), z0 = 1 << 30, z1 = -(1 << 30);
    for (const Vec3i& p : h->plan_pos) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        z0 = std::min(z0, p.z);
        z1 = std::max(z1, p.z);
    }
    return c.foot.x > x0 && c.foot.x < x1 && c.foot.z > z0 && c.foot.z < z1;
}

bool Agents::near_campfire(const Vec3i& p) const {
    for (const Building& b : ctx_.buildings->all())
        if (b.alive && b.functional && b.def == "campfire" && b.inside.chebyshev(p) <= 8) return true;
    return false;
}

float Agents::exposure(const Character& c) const {
    const World& w = *ctx_.world;
    // Climate of the place (the classic island is mild).
    const ColumnInfo col = w.gen().column(c.foot.x, c.foot.z);
    const float temp = (float)col.temp / 255.0f;
    float cold = saturate((0.45f - temp) * 2.5f);
    if (is_night(now_)) cold += 0.2f;
    if (ctx_.physics && ctx_.physics->raining()) cold += 0.15f;
    // Under a roof it hardly matters; by a campfire it is much warmer.
    if (at_home(c)) cold *= 0.2f;
    else if (near_campfire(c.foot)) cold = std::max(0.0f, cold - 0.3f);
    const float warmth = c.clothes != kNoItem ? ctx_.reg->item(c.clothes).warmth : 0.0f;
    return saturate(cold - warmth);
}

}  // namespace icarus
