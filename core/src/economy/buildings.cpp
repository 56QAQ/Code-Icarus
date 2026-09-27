#include "icarus/economy/buildings.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#include <stdexcept>

#include "icarus/sim/physics.h"
#include "icarus/util/log.h"

namespace icarus {

ItemId item_for_material(const Registry& reg, MatId mat) {
    const CoreMats& M = reg.m();
    if (mat == M.door) return reg.find_item("planks");
    if (mat == M.glass) return reg.find_item("sand");
    if (mat == M.path || mat == M.farmland || mat == M.grass) return reg.find_item("dirt");
    const std::string& key = reg.mat(mat).key;
    for (size_t i = 0; i < reg.item_count(); ++i)
        if (reg.item((ItemId)i).place_mat == key) return (ItemId)i;
    return kNoItem;
}

MatId material_for_item_placement(const Registry& reg, ItemId item) {
    const ItemDef& d = reg.item(item);
    if (d.place_mat.empty() || !reg.has_mat(d.place_mat)) return 0;
    return reg.mat_id(d.place_mat);
}

void Buildings::load_defs(const Registry& reg) {
    reg_ = &reg;
    defs_.clear();
    const Json& doc = reg.doc("buildings");
    const Json& legend = doc["legend"];
    for (const Json& b : doc["buildings"].items()) {
        BuildingDef d;
        d.key = b.str("key");
        d.name = b.str("name", d.key);
        d.category = b.str("category");
        d.description = b.str("description");
        d.tech = b.str("tech");
        d.workstation = b.str("workstation");
        d.beds = b.integer("beds", 0);
        d.storage = b.flt("storage", 0);
        d.spoil_factor = b.flt("spoil_factor", 1.0f);
        d.seat = b.boolean("seat", false);
        if (b["inside"].is_array() && b["inside"].size() >= 2)
            d.inside_local = {b["inside"][0].as_int(), 0, b["inside"][1].as_int()};
        if (b["entrance"].is_array() && b["entrance"].size() >= 2)
            d.entrance_local = {b["entrance"][0].as_int(), 0, b["entrance"][1].as_int()};
        const Json& layers = b["layers"];
        d.h = (int)layers.size();
        for (int y = 0; y < d.h; ++y) {
            const Json& rows = layers[(size_t)y];
            d.d = std::max(d.d, (int)rows.size());
            for (int z = 0; z < (int)rows.size(); ++z) {
                const std::string& row = rows[(size_t)z].as_str();
                d.w = std::max(d.w, (int)row.size());
                for (int x = 0; x < (int)row.size(); ++x) {
                    char c = row[(size_t)x];
                    if (c == ' ') continue;
                    MatId m = 0;
                    if (c != '.') {
                        std::string mk = legend.str(std::string(1, c));
                        if (mk.empty()) throw std::runtime_error("buildings.json: unknown legend char in " + d.key);
                        m = reg.mat_id(mk);
                    }
                    d.cells.push_back({{x, y, z}, m});
                    if (m != 0) {
                        ItemId it = item_for_material(reg, m);
                        if (it != kNoItem) d.cost[it]++;
                    }
                    if (y == 0 && (c == 'D' || (c == '.' && z == (int)rows.size() - 1)) && d.door_local.x < 0)
                        d.door_local = {x, 0, z};
                }
            }
        }
        defs_.push_back(std::move(d));
    }
}

void Buildings::reset() {
    list_.assign(1, Building{});
    index_.clear();
}

const BuildingDef* Buildings::def(const std::string& key) const {
    for (auto& d : defs_)
        if (d.key == key) return &d;
    return nullptr;
}

Vec3i Buildings::to_world(const BuildingDef& d, const Vec3i& o, u8 rot, const Vec3i& l) const {
    int x = l.x, z = l.z;
    int rx = x, rz = z;
    switch (rot & 3) {
        case 0: rx = x; rz = z; break;
        case 1: rx = d.d - 1 - z; rz = x; break;
        case 2: rx = d.w - 1 - x; rz = d.d - 1 - z; break;
        case 3: rx = z; rz = d.w - 1 - x; break;
    }
    return {o.x + rx, o.y + l.y, o.z + rz};
}

u32 Buildings::at(const Vec3i& p) const {
    auto it = index_.find(p);
    return it == index_.end() ? 0 : it->second;
}

void Buildings::index_building(const Building& b) {
    for (size_t i = 0; i < b.plan_pos.size(); ++i)
        if (vmat(b.plan_vox[i]) != 0) index_[b.plan_pos[i]] = b.id;
}

static void setup_plan(Building& b, const BuildingDef& d, const std::vector<Vec3i>& world_pos) {
    b.plan_pos = world_pos;
    b.plan_vox.clear();
    b.solid_total = 0;
    for (auto& c : d.cells) {
        b.plan_vox.push_back(make_voxel(c.mat));
        if (c.mat != 0) b.solid_total++;
    }
}

u32 Buildings::place_complete(const std::string& key, const Vec3i& origin, u8 rot, u16 polity, EventId cause) {
    const BuildingDef* d = def(key);
    if (!d) throw std::runtime_error("unknown building " + key);
    u32 id = start_site(key, origin, rot, polity, 0);
    Building& b = list_[id];
    const CoreMats& M = reg_->m();
    // Foundation: make sure every footprint column rests on solid ground.
    for (int z = 0; z < d->d; ++z)
        for (int x = 0; x < d->w; ++x) {
            Vec3i p = to_world(*d, origin, rot, {x, -1, z});
            for (int k = 0; k < 6; ++k) {
                Vec3i q{p.x, p.y - k, p.z};
                const Material& m = w_.material(q);
                if (m.solid && !m.passable) break;
                w_.set(q, make_voxel(k == 0 ? M.path : M.dirt), cause);
            }
            Vec3i top = p;
            const Material& tm = w_.material(top);
            if (tm.solid && vmat(w_.get(top)) != M.path) w_.set(top, make_voxel(M.path), cause);
        }
    for (size_t i = 0; i < b.plan_pos.size(); ++i) w_.set(b.plan_pos[i], b.plan_vox[i], cause);
    // Doorstep: the cube in front of the door must be walkable with head room.
    Vec3i step = b.entrance;
    for (int k = 0; k < 3; ++k) {
        Vec3i q = step + Vec3i{0, k, 0};
        if (at(q) == 0 && w_.material(q).solid) w_.set(q, make_voxel(M.air), cause);
    }
    for (int k = 1; k <= 4; ++k) {
        Vec3i q = step - Vec3i{0, k, 0};
        if (w_.material(q).solid && !w_.material(q).passable) break;
        w_.set(q, make_voxel(k == 1 ? M.path : M.dirt), cause);
    }
    finish(b, cause);
    return id;
}

u32 Buildings::start_site(const std::string& key, const Vec3i& origin, u8 rot, u16 polity, u32 project) {
    const BuildingDef* d = def(key);
    if (!d) throw std::runtime_error("unknown building " + key);
    Building b;
    b.id = (u32)list_.size();
    b.alive = true;
    b.def = key;
    b.name = d->name;
    b.polity = polity;
    b.origin = origin;
    b.rot = rot;
    b.project = project;
    b.beds = d->beds;
    std::vector<Vec3i> wp;
    for (auto& c : d->cells) wp.push_back(to_world(*d, origin, rot, c.local));
    setup_plan(b, *d, wp);
    if (d->door_local.x >= 0) {
        b.entrance = to_world(*d, origin, rot, {d->door_local.x, 0, d->d});
        b.inside = to_world(*d, origin, rot, {d->door_local.x, 0, std::max(0, d->d - 2)});
    } else {
        b.entrance = to_world(*d, origin, rot, {d->w / 2, 0, d->d});
        b.inside = to_world(*d, origin, rot, {d->w / 2, 0, d->d / 2});
    }
    if (d->inside_local.x >= 0) b.inside = to_world(*d, origin, rot, d->inside_local);
    if (d->entrance_local.x >= 0) b.entrance = to_world(*d, origin, rot, d->entrance_local);
    b.site = econ_.create_store(StoreKind::Site, b.entrance, polity);
    econ_.store(b.site)->building = b.id;
    list_.push_back(b);
    index_building(list_.back());
    recompute(list_.back(), 0);
    return b.id;
}

u32 Buildings::place_bridge(const Vec3i& a, const Vec3i& bpos, u16 polity, bool complete, EventId cause, u32 project) {
    const CoreMats& M = reg_->m();
    Building b;
    b.id = (u32)list_.size();
    b.alive = true;
    b.def = "bridge";
    b.name = "桥";
    b.polity = polity;
    b.is_bridge = true;
    b.project = project;
    b.origin = a;
    b.end_a = a;
    b.end_b = bpos;
    float dx = (float)(bpos.x - a.x), dz = (float)(bpos.z - a.z);
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1) len = 1;
    float ux = dx / len, uz = dz / len;   // along
    float px = -uz, pz = ux;              // across
    std::set<Vec3i> deck, rails;
    std::vector<std::pair<Vec3i, float>> deck_order;
    for (float sd = -2.0f; sd <= len + 2.0f; sd += 0.5f) {
        float t = clampv(sd / len, 0.0f, 1.0f);
        float cx = (float)a.x + 0.5f + ux * sd, cz = (float)a.z + 0.5f + uz * sd;
        int y = (int)std::lround((float)(a.y - 1) + (float)(bpos.y - a.y) * t);
        float dist_end = std::min(sd + 2.0f, len + 2.0f - sd);
        for (int k = -2; k <= 2; ++k) {
            Vec3i p{(int)std::floor(cx + px * (float)k), y, (int)std::floor(cz + pz * (float)k)};
            if (deck.insert(p).second) deck_order.push_back({p, dist_end});
            if (k == -2 || k == 2) rails.insert(p + Vec3i{0, 1, 0});
        }
    }
    // Diagonal bridges: make the deck 4-connected so it holds together cube-to-cube.
    {
        std::vector<std::pair<Vec3i, float>> extra;
        for (auto& [p, dist] : deck_order) {
            for (int dx : {-1, 1})
                for (int dz : {-1, 1}) {
                    Vec3i q{p.x + dx, p.y, p.z + dz};
                    if (!deck.count(q)) continue;
                    Vec3i a1{p.x + dx, p.y, p.z}, a2{p.x, p.y, p.z + dz};
                    if (deck.count(a1) || deck.count(a2)) continue;
                    if (deck.insert(a1).second) extra.push_back({a1, dist});
                }
        }
        deck_order.insert(deck_order.end(), extra.begin(), extra.end());
        // Deck cubes at different heights along slopes: connect vertically too.
        std::vector<std::pair<Vec3i, float>> steps;
        for (auto& [p, dist] : deck_order) {
            for (int d = 0; d < 4; ++d) {
                Vec3i n = p + kDir4H[d];
                if (deck.count(n)) continue;
                Vec3i up = n + Vec3i{0, 1, 0}, dn = n + Vec3i{0, -1, 0};
                if (deck.count(up) || deck.count(dn)) {
                    Vec3i fill = deck.count(up) ? p + Vec3i{0, 1, 0} : n;
                    if (deck.insert(fill).second) steps.push_back({fill, dist});
                }
            }
        }
        deck_order.insert(deck_order.end(), steps.begin(), steps.end());
    }
    // Build order: outward from both ends toward the middle.
    std::stable_sort(deck_order.begin(), deck_order.end(),
                     [](const auto& x, const auto& y) { return x.second < y.second; });
    for (auto& [p, dist] : deck_order) {
        b.plan_pos.push_back(p);
        b.plan_vox.push_back(make_voxel(M.planks));
        // Keep head room clear above the walkway.
        (void)dist;
    }
    for (const Vec3i& r : rails) {
        if (deck.count(r)) continue;
        b.plan_pos.push_back(r);
        b.plan_vox.push_back(make_voxel(M.log));
    }
    b.solid_total = (i32)b.plan_pos.size();
    b.entrance = a;
    b.inside = bpos;
    b.site = econ_.create_store(StoreKind::Site, a, polity);
    econ_.store(b.site)->building = b.id;
    list_.push_back(b);
    Building& ref = list_.back();
    index_building(ref);
    if (complete) {
        for (size_t i = 0; i < ref.plan_pos.size(); ++i) w_.set(ref.plan_pos[i], ref.plan_vox[i], cause);
        // Clear head room over the deck.
        for (const Vec3i& p : deck)
            for (int k = 1; k <= 3; ++k) {
                Vec3i q = p + Vec3i{0, k, 0};
                if (rails.count(q)) continue;
                if (w_.material(q).solid) w_.set(q, make_voxel(M.air), cause);
            }
        finish(ref, cause);
    } else {
        recompute(ref, 0);
    }
    return ref.id;
}

int Buildings::next_buildable(const Building& b, int start) {
    const Registry& reg = *reg_;
    int n = (int)b.plan_pos.size();
    for (int k = 0; k < n; ++k) {
        int i = (start + k) % n;
        const Vec3i& p = b.plan_pos[i];
        Voxel want = b.plan_vox[i];
        Voxel cur = w_.get(p);
        if (vmat(want) == 0) {
            const Material& cm = reg.mat(vmat(cur));
            if (cm.solid && cm.diggable) return i;  // needs clearing
            continue;
        }
        if (vmat(cur) == vmat(want)) continue;
        const Material& cm = reg.mat(vmat(cur));
        if (cm.solid && cm.diggable && vmat(cur) != vmat(want)) return i;  // occupied: clear first
        // Needs support: some solid neighbour.
        bool supported = false;
        for (int d = 0; d < 6 && !supported; ++d) {
            const Material& nm = w_.material(p + kDir6[d]);
            supported = nm.solid && !nm.passable;
        }
        if (supported) return i;
    }
    return -1;
}

bool Buildings::place_cell(Building& b, int idx, EventId cause) {
    if (idx < 0 || idx >= (int)b.plan_pos.size()) return false;
    const Vec3i& p = b.plan_pos[(size_t)idx];
    Voxel want = b.plan_vox[(size_t)idx];
    if (vmat(want) == 0) {
        w_.set(p, make_voxel(0), cause);
        return true;
    }
    ItemId it = item_for_material(*reg_, vmat(want));
    if (it != kNoItem) {
        if (econ_.remove(b.site, it, 1, "construction") < 1) return false;
    }
    w_.set(p, want, cause);
    return true;
}

std::map<ItemId, int> Buildings::remaining_cost(const Building& b) const {
    std::map<ItemId, int> need;
    for (size_t i = 0; i < b.plan_pos.size(); ++i) {
        MatId want = vmat(b.plan_vox[i]);
        if (want == 0) continue;
        if (vmat(w_.peek(b.plan_pos[i])) == want) continue;
        ItemId it = item_for_material(*reg_, want);
        if (it != kNoItem) need[it]++;
    }
    const Store* s = econ_.store(b.site);
    if (s)
        for (auto& st : s->items) {
            auto itn = need.find(st.item);
            if (itn != need.end()) itn->second = std::max(0, itn->second - st.count);
        }
    for (auto it = need.begin(); it != need.end();) it = it->second <= 0 ? need.erase(it) : std::next(it);
    return need;
}

bool Buildings::site_done(const Building& b) {
    for (size_t i = 0; i < b.plan_pos.size(); ++i) {
        MatId want = vmat(b.plan_vox[i]);
        MatId cur = w_.mat(b.plan_pos[i]);
        if (want == 0) {
            if (w_.reg().mat(cur).solid) return false;
        } else if (cur != want) {
            return false;
        }
    }
    return true;
}

void Buildings::finish(Building& b, EventId cause) {
    b.complete = true;
    b.completed_tick = w_.now();
    const BuildingDef* d = def(b.def);
    // Leftover site materials stay as a ground pile for haulers.
    if (b.site) {
        econ_.destroy_store(b.site);
        b.site = kNoStore;
    }
    if (d && d->storage > 0 && !econ_.store(b.store)) {
        // A seat with a workstation (the band's campfire) is also where everything is kept.
        const bool stockpile = d->workstation.empty() || d->seat;
        b.store = econ_.create_store(stockpile ? StoreKind::Stockpile : StoreKind::Workshop, b.inside, b.polity,
                                     kNoEntity, d->storage);
        Store* s = econ_.store(b.store);
        s->building = b.id;
        s->spoil_factor = d->spoil_factor;
    }
    b.functional = true;
    recompute(b, cause);
    Event e;
    e.type = EventType::Construction;
    e.severity = 2;
    e.pos = b.origin;
    e.polity = b.polity;
    e.causes[0] = cause;
    e.text = b.name + "落成";
    e.data.set("building", (double)b.id);
    b.last_event = chron_.emit(std::move(e));
}

bool Buildings::reopen(u32 id, u32 project) {
    Building* b = get(id);
    if (!b || !b->complete) return false;
    b->complete = false;
    b->project = project;
    if (!econ_.store(b->site)) {
        b->site = econ_.create_store(StoreKind::Site, b->entrance, b->polity);
        econ_.store(b->site)->building = b->id;
    }
    return true;
}

bool Buildings::find_site(const std::string& key, const Vec3i& near, int radius, Vec3i& origin, u8& rot) {
    const BuildingDef* d = def(key);
    if (!d) return false;
    const Registry& reg = *reg_;
    // Spiral search for a footprint whose ground is flat (+-1), dry, unbuilt and clear of trees.
    for (int r = 4; r <= radius; r += 2) {
        for (int i = 0; i < 16; ++i) {
            float a = (float)i / 16.0f * 6.2831853f + (float)r * 0.37f;
            int cx = near.x + (int)std::lround(std::cos(a) * (float)r);
            int cz = near.z + (int)std::lround(std::sin(a) * (float)r);
            int x0 = cx - d->w / 2, z0 = cz - d->d / 2;
            int gy = w_.surface_y(cx, cz);
            bool ok = gy > 0;
            for (int z = z0 - 1; z <= z0 + d->d && ok; ++z)
                for (int x = x0 - 1; x <= x0 + d->w && ok; ++x) {
                    int y = w_.surface_y(x, z);
                    if (std::abs(y - gy) > 1) ok = false;
                    MatId top = w_.mat({x, y, z});
                    const Material& tm = reg.mat(top);
                    if (tm.fluid || top == reg.m().farmland || tm.trunk || tm.foliage || top == reg.m().planks)
                        ok = false;
                    // Dry ground: a shallow lake's sandy bed is no place for a floor.
                    for (int k = 1; k <= 2 && ok; ++k)
                        if (reg.mat(w_.mat({x, y + k, z})).fluid) ok = false;
                    for (int k = 0; k <= 4 && ok; ++k)
                        if (at({x, y + k, z})) ok = false;
                }
            if (!ok) continue;
            origin = {x0, gy + 1, z0};
            // Face the requested point.
            float dx = (float)(near.x - cx), dz = (float)(near.z - cz);
            if (std::fabs(dx) > std::fabs(dz)) rot = dx > 0 ? 3 : 1;
            else rot = dz > 0 ? 0 : 2;
            if (rot & 1) {
                origin.x = cx - d->d / 2;
                origin.z = cz - d->w / 2;
            }
            return true;
        }
    }
    return false;
}

void Buildings::demolish(u32 id) {
    Building* b = get(id);
    if (!b) return;
    for (size_t i = 0; i < b->plan_pos.size(); ++i) {
        auto it = index_.find(b->plan_pos[i]);
        if (it != index_.end() && it->second == id) index_.erase(it);
    }
    if (b->store) econ_.destroy_store(b->store);
    if (b->site) econ_.destroy_store(b->site);
    b->alive = false;
}

void Buildings::on_changes(const std::vector<VoxelChange>& changes) {
    std::vector<std::pair<u32, EventId>> touched;
    for (const VoxelChange& c : changes) {
        auto it = index_.find(c.p);
        if (it == index_.end()) continue;
        bool dup = false;
        for (auto& t : touched)
            if (t.first == it->second) {
                if (!t.second) t.second = c.cause;
                dup = true;
            }
        if (!dup) touched.push_back({it->second, c.cause});
    }
    for (auto& [id, cause] : touched) {
        Building* b = get(id);
        if (b) recompute(*b, cause);
    }
}

void Buildings::recompute(Building& b, EventId cause) {
    i32 intact = 0;
    for (size_t i = 0; i < b.plan_pos.size(); ++i) {
        MatId want = vmat(b.plan_vox[i]);
        if (want == 0) continue;
        if (vmat(w_.peek(b.plan_pos[i])) == want) intact++;
    }
    b.solid_intact = intact;
    float old = b.integrity;
    b.integrity = b.solid_total > 0 ? (float)intact / (float)b.solid_total : 0.0f;
    if (!b.complete) return;
    if (b.is_bridge) {
        bridge_check(b, cause);
        return;
    }
    if (b.functional && b.integrity < 0.6f) {
        b.functional = false;
        Event e;
        e.type = EventType::StructureDestroyed;
        e.severity = 3;
        e.pos = b.origin;
        e.polity = b.polity;
        e.causes[0] = cause;
        e.text = strfmt("%s被毁（完好度 %.0f%%）", b.name.c_str(), b.integrity * 100.0f);
        e.data.set("building", (double)b.id);
        b.last_event = chron_.emit(std::move(e));
    } else if (b.functional && old >= 0.95f && b.integrity < 0.95f) {
        Event e;
        e.type = EventType::StructureDamaged;
        e.severity = 2;
        e.pos = b.origin;
        e.polity = b.polity;
        e.causes[0] = cause;
        e.text = strfmt("%s受损（完好度 %.0f%%）", b.name.c_str(), b.integrity * 100.0f);
        e.data.set("building", (double)b.id);
        b.last_event = chron_.emit(std::move(e));
    } else if (!b.functional && b.integrity >= 0.9f) {
        b.functional = true;
        Event e;
        e.type = EventType::Construction;
        e.severity = 2;
        e.pos = b.origin;
        e.polity = b.polity;
        e.causes[0] = cause;
        e.text = b.name + "修复完成";
        b.last_event = chron_.emit(std::move(e));
    }
}

void Buildings::bridge_check(Building& b, EventId cause) {
    const Registry& reg = *reg_;
    const CoreMats& M = reg.m();
    // Deck cells present in the world.
    std::vector<Vec3i> deck;
    std::set<Vec3i> present;
    for (size_t i = 0; i < b.plan_pos.size(); ++i) {
        if (vmat(b.plan_vox[i]) != M.planks) continue;
        deck.push_back(b.plan_pos[i]);
        if (w_.mat(b.plan_pos[i]) == M.planks) present.insert(b.plan_pos[i]);
    }
    // Anchors: deck cubes resting on non-bridge solid ground.
    auto anchored = [&](const Vec3i& p) {
        Vec3i below = p + Vec3i{0, -1, 0};
        if (at(below) == b.id) return false;
        const Material& m = w_.material(below);
        return m.solid && !m.passable;
    };
    auto bfs = [&](const Vec3i& end, std::map<Vec3i, int>& dist) {
        std::deque<Vec3i> q;
        for (const Vec3i& p : present) {
            if (anchored(p) && p.chebyshev(end) <= 6) {
                dist[p] = 0;
                q.push_back(p);
            }
        }
        while (!q.empty()) {
            Vec3i c = q.front();
            q.pop_front();
            for (int d = 0; d < 6; ++d) {
                Vec3i n = c + kDir6[d];
                if (!present.count(n) || dist.count(n)) continue;
                dist[n] = dist[c] + 1;
                q.push_back(n);
            }
        }
    };
    std::map<Vec3i, int> da, db;
    bfs(Vec3i{b.end_a.x, b.end_a.y - 1, b.end_a.z}, da);
    bfs(Vec3i{b.end_b.x, b.end_b.y - 1, b.end_b.z}, db);
    std::vector<Vec3i> falling;
    for (const Vec3i& p : present) {
        bool both = da.count(p) && db.count(p);
        int dmin = 1 << 20;
        if (da.count(p)) dmin = std::min(dmin, da[p]);
        if (db.count(p)) dmin = std::min(dmin, db[p]);
        if (!both && dmin > 4) falling.push_back(p);
    }
    bool connected = false;
    for (const Vec3i& p : present)
        if (da.count(p) && db.count(p)) {
            connected = true;
            break;
        }
    if (!falling.empty() && physics_) {
        // Rails above falling deck cubes come down with them.
        std::vector<Vec3i> extra;
        for (const Vec3i& p : falling) {
            Vec3i up = p + Vec3i{0, 1, 0};
            if (at(up) == b.id && w_.mat(up) == M.log) extra.push_back(up);
        }
        falling.insert(falling.end(), extra.begin(), extra.end());
        physics_->collapse_cubes(falling, cause, "失去支撑的桥面坠落");
    }
    if (b.functional && !connected) {
        b.functional = false;
        Event e;
        e.type = EventType::StructureDestroyed;
        e.severity = 4;
        e.pos = b.origin;
        e.polity = b.polity;
        e.causes[0] = cause;
        e.text = "桥梁断裂，两岸交通中断";
        e.data.set("building", (double)b.id);
        e.data.set("bridge", true);
        b.last_event = chron_.emit(std::move(e));
    } else if (!b.functional && connected && b.integrity >= 0.85f) {
        b.functional = true;
        Event e;
        e.type = EventType::Construction;
        e.severity = 3;
        e.pos = b.origin;
        e.polity = b.polity;
        e.causes[0] = cause;
        e.text = "桥梁重新贯通";
        e.data.set("building", (double)b.id);
        e.data.set("bridge", true);
        b.last_event = chron_.emit(std::move(e));
    }
}

void Buildings::save(BinWriter& w) const {
    size_t s = w.begin_section("BLDG");
    w.varu(list_.size());
    for (size_t i = 1; i < list_.size(); ++i) {
        const Building& b = list_[i];
        w.boolean(b.alive);
        if (!b.alive) continue;
        w.str(b.def);
        w.str(b.name);
        w.u16v(b.polity);
        w.vec3i(b.origin);
        w.u8v(b.rot);
        w.boolean(b.complete);
        w.boolean(b.functional);
        w.boolean(b.is_bridge);
        w.varu(b.plan_pos.size());
        for (size_t k = 0; k < b.plan_pos.size(); ++k) {
            w.vec3i(b.plan_pos[k]);
            w.u16v(b.plan_vox[k]);
        }
        w.vari(b.solid_total);
        w.vari(b.solid_intact);
        w.f32(b.integrity);
        w.u32v(b.store);
        w.u32v(b.site);
        w.varu(b.residents.size());
        for (EntityId r : b.residents) w.u32v(r);
        w.vec3i(b.entrance);
        w.vec3i(b.inside);
        w.u32v(b.project);
        w.u64v(b.completed_tick);
        w.u32v(b.last_event);
        w.vari(b.beds);
        w.vec3i(b.end_a);
        w.vec3i(b.end_b);
    }
    w.end_section(s);
}

void Buildings::load(BinReader& outer) {
    BinReader r = outer.section("BLDG");
    u64 n = r.varu();
    list_.assign((size_t)n, Building{});
    index_.clear();
    for (size_t i = 1; i < (size_t)n; ++i) {
        Building& b = list_[i];
        b.id = (u32)i;
        b.alive = r.boolean();
        if (!b.alive) continue;
        b.def = r.str();
        b.name = r.str();
        b.polity = r.u16v();
        b.origin = r.vec3i();
        b.rot = r.u8v();
        b.complete = r.boolean();
        b.functional = r.boolean();
        b.is_bridge = r.boolean();
        u64 np = r.varu();
        for (u64 k = 0; k < np; ++k) {
            b.plan_pos.push_back(r.vec3i());
            b.plan_vox.push_back(r.u16v());
        }
        b.solid_total = (i32)r.vari();
        b.solid_intact = (i32)r.vari();
        b.integrity = r.f32();
        b.store = r.u32v();
        b.site = r.u32v();
        u64 nr = r.varu();
        for (u64 k = 0; k < nr; ++k) b.residents.push_back(r.u32v());
        b.entrance = r.vec3i();
        b.inside = r.vec3i();
        b.project = r.u32v();
        b.completed_tick = r.u64v();
        b.last_event = r.u32v();
        b.beds = (int)r.vari();
        b.end_a = r.vec3i();
        b.end_b = r.vec3i();
        index_building(b);
    }
}

u64 Buildings::hash() const {
    u64 h = 11;
    for (auto& b : list_) {
        if (!b.alive) continue;
        h = hash_combine(h, b.id);
        h = hash_combine(h, (u64)b.solid_intact);
        h = hash_combine(h, b.functional ? 1 : 2);
    }
    return h;
}

}  // namespace icarus
