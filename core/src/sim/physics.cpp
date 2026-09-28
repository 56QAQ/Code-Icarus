#include "icarus/sim/physics.h"

#include <chrono>

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

#include "icarus/util/log.h"

namespace icarus {

namespace {
constexpr float kGravity = 0.018f;       // cubes / tick^2
constexpr float kMaxFall = 1.2f;         // terminal speed, cubes / tick
}  // namespace

Physics::Physics(World& world, Chronicle& chronicle) : w_(world), chron_(chronicle) {}

void Physics::reset(u64 seed) {
    rng_.seed(seed, 0x9A51C5);
    water_.clear();
    puddles_.clear();
    fire_.clear();
    granular_.clear();
    support_.clear();
    springs_.clear();
    spring_state_.clear();
    debris_.clear();
    meteors_.clear();
    damage_.clear();
    removal_causes_.clear();
    next_body_id_ = 1;
    stats_ = {};
}

bool Physics::water_can_enter(Voxel v) const {
    MatId m = vmat(v);
    if (m == 0) return true;
    if (m == w_.reg().m().water) return vlevel(v) < kFluidFull;
    return false;
}

void Physics::wake_neighbors(const Vec3i& p) {
    const Registry& reg = w_.reg();
    for (int i = 0; i < 6; ++i) {
        Vec3i n = p + kDir6[i];
        if (!w_.in_bounds(n)) continue;
        Voxel v = w_.get(n);
        const Material& m = reg.mat(vmat(v));
        if (m.fluid) water_.push(n);
        else if (m.granular) granular_.push(n);
    }
}

void Physics::on_changes(const std::vector<VoxelChange>& changes) {
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    for (const VoxelChange& c : changes) {
        const Material& before = reg.mat(vmat(c.before));
        const Material& after = reg.mat(vmat(c.after));
        if (after.fluid) water_.push(c.p);
        if (after.granular) granular_.push(c.p);
        if (vburning(c.after)) fire_.push(c.p);
        wake_neighbors(c.p);
        bool removed_rigid = before.solid && before.rigid && !(after.solid && after.rigid);
        if (removed_rigid) {
            support_.push(c.p);
            if (c.cause) removal_causes_.push_back({c.p, c.cause});
        }
        if (c.cause)
            for (size_t i = 0; i < springs_.size(); ++i)
                if (springs_[i].dist2(c.p) <= 9) spring_state_[i].touch_cause = c.cause;
        if (vmat(c.before) == M.spring && vmat(c.after) != M.spring) {
            auto it = std::find(springs_.begin(), springs_.end(), c.p);
            if (it != springs_.end()) {
                spring_state_.erase(spring_state_.begin() + (it - springs_.begin()));
                springs_.erase(it);
                Event e;
                e.type = EventType::WaterSourceLost;
                e.severity = 4;
                e.pos = c.p;
                e.causes[0] = c.cause;
                e.text = "泉眼被破坏，水源断绝";
                chron_.emit(std::move(e));
            }
        }
        if (vmat(c.after) == M.spring && vmat(c.before) != M.spring) {
            if (std::find(springs_.begin(), springs_.end(), c.p) == springs_.end()) add_spring(c.p);
        }
    }
}

void Physics::step_evaporation() {
    // Sample random columns of active cells; exposed water surfaces lose a unit. The rate
    // is proportional to exposed surface area, so water above the land's natural water
    // table (floods, puddles) dries up without inflow; lakes keep to their shores.
    if (evaporation_samples <= 0) return;
    const CoreMats& M = w_.reg().m();
    if (now_ % 50 == 0) {
        active_cells_.clear();
        for (int y = 0; y < w_.cells_y(); ++y)
            for (int z = 0; z < w_.cells_z(); ++z)
                for (int x = 0; x < w_.cells_x(); ++x) {
                    const Cell* c = w_.cell({x, y, z});
                    if (c && c->state == CellState::Active && !(c->uniform && vmat(c->uniform_value) == M.air))
                        active_cells_.push_back({x, y, z});
                }
    }
    if (active_cells_.empty()) return;
    // Weather: occasional rain spells (deterministic from the physics RNG).
    if (now_ >= next_weather_) {
        if (next_weather_ != 0 && rng_.chance(0.45f)) {
            rain_until_ = now_ + (Tick)(2500 + rng_.below(4000));
            Event e;
            e.type = EventType::Info;
            e.severity = 1;
            e.text = "下雨了";
            chron_.emit(std::move(e));
        }
        next_weather_ = now_ + 6000 + rng_.below(6000);
    }
    const bool rain = rain_until_ > now_;
    const int samples = rain ? evaporation_samples * 3 : evaporation_samples;
    for (int i = 0; i < samples; ++i) {
        const Vec3i& cc = active_cells_[rng_.below((u32)active_cells_.size())];
        int x = cc.x * kCellSize + (int)rng_.below(kCellSize);
        int z = cc.z * kCellSize + (int)rng_.below(kCellSize);
        for (int y = cc.y * kCellSize + kCellSize - 1; y >= cc.y * kCellSize; --y) {
            Vec3i p{x, y, z};
            Voxel v = w_.get(p);
            MatId m = vmat(v);
            if (m == M.air) continue;
            bool open_sky = vmat(w_.get(p + Vec3i{0, 1, 0})) == M.air;
            if (m == M.water && open_sky && w_.config().layout == WorldLayout::Random) {
                // A random island's river or lake sunk below its level (drained through a
                // breach since mended) fills up again from upstream, a unit at a time.
                const ColumnInfo col = w_.gen().column(x, z);
                if (!col.sea && col.water_top >= 0 && y <= col.water_top && (vlevel(v) < kFluidFull || y < col.water_top)) {
                    if (vlevel(v) < kFluidFull) w_.set(p, make_voxel(M.water, (u8)(vlevel(v) + 1)));
                    else w_.set(p + Vec3i{0, 1, 0}, make_voxel(M.water, 1));
                    stats_.water_units_sea_out++;
                    break;
                }
            }
            if (m == M.water && open_sky) {
                if (rain) {
                    if (vlevel(v) < kFluidFull) {
                        w_.set(p, make_voxel(M.water, (u8)(vlevel(v) + 1)));
                        stats_.water_units_rain++;
                    }
                } else if (y > w_.gen().column(x, z).water_top) {
                    // Lakes are held at their water table by the ground water beneath them;
                    // what stands above it (floods, puddles, a lake dammed higher) dries up.
                    int l = vlevel(v) - 1;
                    w_.set(p, l > 0 ? make_voxel(M.water, (u8)l) : make_voxel(M.air));
                    stats_.water_units_evaporated++;
                }
            } else if (rain && vburning(v) && open_sky) {
                w_.set(p, with_burning(v, false));
            }
            break;
        }
    }
}

void Physics::step(Tick now) {
    now_ = now;
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    auto lap = [&](double& acc) {
        const auto t = clk::now();
        acc += std::chrono::duration<double, std::micro>(t - t0).count();
        t0 = t;
    };
    step_springs(now);
    step_evaporation();
    step_puddles();
    lap(stats_.us_evaporation);
    step_meteors();
    lap(stats_.us_other);
    step_water();
    lap(stats_.us_water);
    step_fire();
    lap(stats_.us_fire);
    step_granular();
    lap(stats_.us_granular);
    step_support();
    lap(stats_.us_support);
    step_debris();
    lap(stats_.us_other);
    stats_.water_active = water_.size();
    stats_.fire_active = fire_.size();
    stats_.granular_active = granular_.size();
    stats_.debris = debris_.size();
    stats_.meteors = meteors_.size();
}

// ---------------------------------------------------------------------------------- springs

void Physics::step_springs(Tick now) {
    if (springs_.empty() || spring_interval <= 0 || now % (Tick)spring_interval != 0) return;
    const CoreMats& M = w_.reg().m();
    const Registry& reg = w_.reg();
    for (size_t i = 0; i < springs_.size(); ++i) {
        const Vec3i s = springs_[i];
        SpringState& ss = spring_state_[i];
        if (w_.mat(s) != M.spring) continue;
        // Emit into the cube above, or any side cube that can take water.
        Vec3i targets[5] = {s + Vec3i{0, 1, 0}, s + kDir4H[0], s + kDir4H[1], s + kDir4H[2], s + kDir4H[3]};
        bool sealed = true;
        for (const Vec3i& t : targets) {
            Voxel v = w_.get(t);
            if (!reg.mat(vmat(v)).solid) sealed = false;
            if (!water_can_enter(v)) continue;
            u8 lvl = vmat(v) == M.water ? vlevel(v) : 0;
            w_.set(t, make_voxel(M.water, (u8)(lvl + 1)));
            stats_.water_units_spring++;
            break;
        }
        // A spring under water is merely full; one sealed in rock is lost until dug out.
        ss.dry = sealed ? ss.dry + 1 : 0;
        if (ss.flowing && ss.dry >= 100) {
            ss.flowing = false;
            Event e;
            e.type = EventType::WaterSourceLost;
            e.severity = 4;
            e.pos = s;
            e.causes[0] = ss.touch_cause;
            e.text = "泉眼被堵塞，水源断绝";
            ss.lost_event = chron_.emit(std::move(e));
        } else if (!ss.flowing && !sealed) {
            ss.flowing = true;
            Event e;
            e.type = EventType::Info;
            e.severity = 3;
            e.pos = s;
            e.causes[0] = ss.touch_cause ? ss.touch_cause : ss.lost_event;
            e.text = "泉水重新涌出";
            chron_.emit(std::move(e));
            ss.lost_event = 0;
        }
    }
}

// ---------------------------------------------------------------------------------- water

void Physics::step_water() {
    const CoreMats& M = w_.reg().m();
    // The sea (random layout): an invisible wall at its rim, and a level that never
    // changes — water rising above it on the sea drains away, water running out of it
    // (into a hole in its floor) is made up at once.
    const WorldGen& gen = w_.gen();
    const int sea_level = gen.sea_level();
    auto walled = [&](const Vec3i& n) { return !w_.in_bounds(n) || (sea_level >= 0 && gen.sea_wall(n.x, n.z)); };
    std::vector<Vec3i> list = water_.take();
    if ((int)list.size() > water_budget) {
        for (size_t i = (size_t)water_budget; i < list.size(); ++i) water_.push(list[i]);
        list.resize((size_t)water_budget);
    }
    for (const Vec3i& p : list) {
        Voxel v = w_.get(p);
        if (vmat(v) != M.water) continue;
        int level = vlevel(v);
        if (level <= 0) {
            w_.set(p, make_voxel(M.air));
            continue;
        }
        bool sea_source = false;
        if (sea_level >= 0 && gen.column(p.x, p.z).sea) {
            if (p.y > sea_level) {
                stats_.water_units_sea_in += level;
                w_.set(p, make_voxel(M.air));
                continue;
            }
            sea_source = true;
        }
        bool moved = false;
        // 1) Fall.
        Vec3i b = p + Vec3i{0, -1, 0};
        if (b.y < 0) {
            stats_.water_units_to_void += level;
            w_.set(p, make_voxel(M.air));
            continue;
        }
        Voxel bv = w_.get(b);
        if (water_can_enter(bv)) {
            int bl = vmat(bv) == M.water ? vlevel(bv) : 0;
            int amt = std::min(level, (int)kFluidFull - bl);
            if (amt > 0) {
                w_.set(b, make_voxel(M.water, (u8)(bl + amt)));
                level -= amt;
                moved = true;
            }
        }
        // 2) Spread sideways toward lower neighbours; prefer edges with a drop below.
        if (level > 0) {
            int start = (int)((now_ + (u64)(p.x * 7 + p.z * 13)) & 3);
            for (int iter = 0; iter < 4 && level > 0; ++iter) {
                int best = -1, best_level = 99;
                bool best_drop = false;
                for (int k = 0; k < 4; ++k) {
                    int d = (start + k) & 3;
                    Vec3i n = p + kDir4H[d];
                    if (walled(n)) continue;  // the world's edge, the sea's wall
                    Voxel nv = w_.get(n);
                    if (!water_can_enter(nv)) continue;
                    int nl = vmat(nv) == M.water ? vlevel(nv) : 0;
                    bool drop = false;
                    if (nl == 0) {
                        Vec3i nb = n + Vec3i{0, -1, 0};
                        drop = nb.y < 0 || water_can_enter(w_.get(nb));
                    }
                    bool better = drop ? (!best_drop || nl < best_level) : (!best_drop && nl < best_level);
                    if (better) {
                        best = d;
                        best_level = nl;
                        best_drop = drop;
                    }
                }
                if (best < 0) break;
                if (!best_drop && best_level >= level) break;
                if (!best_drop && best_level == level - 1) {
                    // A one-unit difference only flows if it continues downhill beyond the
                    // neighbour; this levels long channels without endless jitter.
                    Vec3i n = p + kDir4H[best];
                    bool downhill = false;
                    for (int k = 0; k < 4 && !downhill; ++k) {
                        Vec3i m = n + kDir4H[k];
                        if (m == p || walled(m)) continue;
                        Voxel mv = w_.get(m);
                        if (!water_can_enter(mv)) continue;
                        int ml = vmat(mv) == M.water ? vlevel(mv) : 0;
                        if (ml <= level - 2) downhill = true;
                    }
                    if (!downhill) break;
                }
                Vec3i n = p + kDir4H[best];
                int give = best_drop ? std::max(1, level / 2) : std::max(1, (level - best_level) / 2);
                give = std::min(give, (int)kFluidFull - best_level);
                if (give <= 0) break;
                w_.set(n, make_voxel(M.water, (u8)(best_level + give)));
                level -= give;
                moved = true;
            }
        }
        // 3) Shallow puddles that cannot flow only evaporate: they are checked now
        // and then (step_puddles) until they dry or something around them changes.
        if (!moved && level <= 2) puddles_.push(p);
        if (moved) {
            if (sea_source) {
                stats_.water_units_sea_out += kFluidFull - level;
                level = kFluidFull;
            }
            w_.set(p, level > 0 ? make_voxel(M.water, (u8)level) : make_voxel(M.air));
            water_.push(p);
        }
    }
}

void Physics::step_puddles() {
    if (now_ % kPuddleTicks != 0 || puddles_.empty()) return;
    const CoreMats& M = w_.reg().m();
    const float chance = std::min(1.0f, evaporation * (float)kPuddleTicks);
    for (const Vec3i& p : puddles_.take()) {
        const Voxel v = w_.get(p);
        if (vmat(v) != M.water) continue;
        const int level = vlevel(v);
        if (level > 2) {
            water_.push(p);
            continue;
        }
        if (vmat(w_.get(p + Vec3i{0, 1, 0})) == M.air && rng_.chance(chance)) {
            stats_.water_units_evaporated++;
            w_.set(p, level > 1 ? make_voxel(M.water, (u8)(level - 1)) : make_voxel(M.air));
            water_.push(p);
        } else {
            puddles_.push(p);
        }
    }
}

// ---------------------------------------------------------------------------------- fire

void Physics::ignite(const Vec3i& p, EventId cause) {
    Voxel v = w_.get(p);
    const Material& m = w_.reg().mat(vmat(v));
    if (m.flammability <= 0 || vburning(v)) return;
    w_.set(p, with_burning(v, true), cause);
    fire_.push(p);
}

void Physics::step_fire() {
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    std::vector<Vec3i> list = fire_.take();
    for (const Vec3i& p : list) {
        Voxel v = w_.get(p);
        if (!vburning(v)) continue;
        const Material& m = reg.mat(vmat(v));
        // Water nearby extinguishes the fire (and turns a unit of water to steam).
        bool quenched = false;
        for (int i = 0; i < 6 && !quenched; ++i) {
            Vec3i n = p + kDir6[i];
            Voxel nv = w_.get(n);
            if (vmat(nv) == M.water) {
                quenched = true;
                int l = vlevel(nv) - 1;
                w_.set(n, l > 0 ? make_voxel(M.water, (u8)l) : make_voxel(M.air));
            }
        }
        if (quenched) {
            w_.set(p, with_burning(v, false));
            continue;
        }
        // Rain puts out what burns under the open sky, and wet fuel hardly catches.
        const bool wet = raining() && vmat(w_.get(p + Vec3i{0, 1, 0})) == M.air;
        if (wet && rng_.chance(0.08f)) {
            w_.set(p, with_burning(v, false));
            continue;
        }
        // Spread.
        for (int i = 0; i < 6; ++i) {
            Vec3i n = p + kDir6[i];
            Voxel nv = w_.get(n);
            if (vburning(nv)) continue;
            const Material& nm = reg.mat(vmat(nv));
            if (nm.flammability <= 0) continue;
            float chance = nm.flammability * (i == 2 ? 0.06f : 0.02f) * (wet ? 0.15f : 1.0f);
            if (rng_.chance(chance)) {
                w_.set(n, with_burning(nv, true));
                fire_.push(n);
            }
        }
        // Heat hurts anyone standing nearby.
        if ((now_ + (u64)p.x) % 20 == 0) {
            damage_.push_back({Vec3f((float)p.x + 0.5f, (float)p.y + 0.5f, (float)p.z + 0.5f), 1.6f, 0.02f, 1, 0});
        }
        // Burn progress: expected lifetime = burn_ticks.
        int bt = std::max(8, m.burn_ticks);
        if (rng_.chance(7.0f / (float)bt)) {
            int d = vdamage(v) + 1;
            if (d >= 7) {
                w_.set(p, make_voxel(m.burn_to));
                continue;
            }
            v = with_damage(v, (u8)d);
            w_.set(p, v);
        }
        fire_.push(p);
    }
}

// ---------------------------------------------------------------------------------- granular

void Physics::step_granular() {
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    std::vector<Vec3i> list = granular_.take();
    // Process bottom-up so columns fall together.
    std::stable_sort(list.begin(), list.end(), [](const Vec3i& a, const Vec3i& b) { return a.y < b.y; });
    for (const Vec3i& p : list) {
        Voxel v = w_.get(p);
        const Material& m = reg.mat(vmat(v));
        if (!m.granular) continue;
        Vec3i b = p + Vec3i{0, -1, 0};
        if (b.y < 0) {
            w_.set(p, make_voxel(M.air));
            continue;
        }
        Voxel bv = w_.get(b);
        const Material& bm = reg.mat(vmat(bv));
        if (!bm.solid && !bm.holds_loose) {
            // Fall (displacing water upward).
            w_.set(b, v);
            w_.set(p, bm.fluid ? bv : make_voxel(M.air));
            granular_.push(b);
            continue;
        }
        // Slide diagonally with an angle of repose.
        int start = (int)rng_.below(4);
        for (int k = 0; k < 4; ++k) {
            Vec3i s = p + kDir4H[(start + k) & 3];
            Vec3i sd = s + Vec3i{0, -1, 0};
            if (!w_.in_bounds(s)) continue;
            Voxel sv = w_.get(s), sdv = w_.get(sd);
            const Material& sm = reg.mat(vmat(sv));
            const Material& sdm = reg.mat(vmat(sdv));
            if (!sm.solid && !sm.fluid && vmat(sv) == M.air && !sdm.solid && !sdm.holds_loose) {
                if (rng_.chance(0.6f)) {
                    // Swap so any water in the way is displaced, never destroyed.
                    w_.set(sd, v);
                    w_.set(p, sdm.fluid ? sdv : make_voxel(M.air));
                    granular_.push(sd);
                }
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------- support

EventId Physics::cause_of_removal(const Vec3i& p) const {
    for (auto it = removal_causes_.rbegin(); it != removal_causes_.rend(); ++it)
        if (it->first.chebyshev(p) <= 2) return it->second;
    return 0;
}

void Physics::step_support() {
    if (support_.empty()) {
        removal_causes_.clear();
        return;
    }
    const Registry& reg = w_.reg();
    std::vector<Vec3i> list = support_.take();
    std::unordered_set<Vec3i, Vec3iHash> checked;
    int checks = 0;
    for (const Vec3i& origin : list) {
        for (int i = 0; i < 6; ++i) {
            Vec3i start = origin + kDir6[i];
            if (!w_.in_bounds(start) || checked.count(start)) continue;
            Voxel sv = w_.get(start);
            const Material& sm = reg.mat(vmat(sv));
            if (!(sm.solid && sm.rigid)) continue;
            ++checks;
            // BFS over rigid solids; downward moves explored first. Reaching a levistone
            // anchor or any cube of an unmodified (pristine) cell proves support.
            std::deque<Vec3i> q{start};
            std::unordered_set<Vec3i, Vec3iHash> seen{start};
            std::vector<Vec3i> comp;
            bool supported = false;
            while (!q.empty()) {
                Vec3i c = q.front();
                q.pop_front();
                comp.push_back(c);
                Voxel cv = w_.get(c);
                const Material& cm = reg.mat(vmat(cv));
                if (cm.anchor) { supported = true; break; }
                const Cell* cell = w_.cell(cell_of(c));
                if (cell && cell->pristine) { supported = true; break; }
                if ((int)comp.size() > support_max_nodes) { supported = true; break; }
                // Resting on a heap (sand, rubble, ash) bears weight too: granular matter
                // settles on its own, so whatever lies on it is supported.
                {
                    const Material& below = reg.mat(vmat(w_.get(c + Vec3i{0, -1, 0})));
                    if (below.solid && below.granular) { supported = true; break; }
                }
                for (int d = 0; d < 6; ++d) {
                    Vec3i n = c + kDir6[d];
                    if (!w_.in_bounds(n) || seen.count(n)) continue;
                    Voxel nv = w_.get(n);
                    const Material& nm = reg.mat(vmat(nv));
                    if (!(nm.solid && nm.rigid)) continue;
                    seen.insert(n);
                    if (d == 3) q.push_front(n); else q.push_back(n);
                }
            }
            if (supported) {
                // Everything reached is supported; skip re-checking those starts.
                for (const Vec3i& c : comp) checked.insert(c);
                continue;
            }
            for (const Vec3i& c : comp) checked.insert(c);
            collapse_component(comp, cause_of_removal(origin));
        }
    }
    stats_.support_checks += (size_t)checks;
    removal_causes_.clear();
}

void Physics::collapse_cubes(const std::vector<Vec3i>& cubes, EventId cause, const std::string& text) {
    std::vector<Vec3i> solid;
    for (const Vec3i& c : cubes)
        if (w_.material(c).solid) solid.push_back(c);
    collapse_component(solid, cause, text.c_str());
}

void Physics::collapse_component(const std::vector<Vec3i>& comp, EventId cause, const char* text) {
    if (comp.empty()) return;
    const Registry& reg = w_.reg();
    DebrisBody b;
    b.id = next_body_id_++;
    Vec3i mn = comp[0], mx = comp[0];
    for (const Vec3i& c : comp) {
        mn = {std::min(mn.x, c.x), std::min(mn.y, c.y), std::min(mn.z, c.z)};
        mx = {std::max(mx.x, c.x), std::max(mx.y, c.y), std::max(mx.z, c.z)};
    }
    b.pos = Vec3f(mn);
    b.min_off = {0, 0, 0};
    b.max_off = mx - mn;
    // Attached non-rigid cubes resting on top (crops, bushes, granular) come along.
    std::unordered_set<Vec3i, Vec3iHash> in_comp(comp.begin(), comp.end());
    std::vector<Vec3i> extra;
    for (const Vec3i& c : comp) {
        Vec3i up = c + Vec3i{0, 1, 0};
        if (in_comp.count(up)) continue;
        const Material& um = reg.mat(w_.mat(up));
        if (vmat(w_.get(up)) != 0 && !um.fluid && !(um.solid && um.rigid)) extra.push_back(up);
    }
    for (const Vec3i& c : comp) b.voxels.push_back({c - mn, with_burning(w_.get(c), false)});
    for (const Vec3i& c : extra) {
        b.voxels.push_back({c - mn, w_.get(c)});
        b.max_off = {std::max(b.max_off.x, c.x - mn.x), std::max(b.max_off.y, c.y - mn.y),
                     std::max(b.max_off.z, c.z - mn.z)};
    }
    Event e;
    e.type = EventType::Collapse;
    e.severity = comp.size() > 500 ? 4 : (comp.size() > 50 ? 3 : 2);
    e.pos = comp[0];
    e.causes[0] = cause;
    e.text = text ? strfmt("%s（%zu 个方块）", text, b.voxels.size())
                  : strfmt("失去支撑的结构坍塌（%zu 个方块）", b.voxels.size());
    e.data.set("cubes", (double)b.voxels.size());
    b.cause = chron_.emit(std::move(e));
    for (const DebrisVoxel& dv : b.voxels) w_.set(mn + dv.off, make_voxel(0), b.cause);
    b.vel = {0, 0, 0};
    debris_.push_back(std::move(b));
}

// ---------------------------------------------------------------------------------- debris

void Physics::step_debris() {
    for (size_t i = 0; i < debris_.size();) {
        DebrisBody& b = debris_[i];
        b.vel.y = std::max(-kMaxFall, b.vel.y - kGravity);
        Vec3f np = b.pos + b.vel;
        // Collision test: any cube whose destination is solid (or fluid for heavy impacts).
        bool hit = false;
        int dy = (int)std::floor(np.y) - (int)std::floor(b.pos.y);
        if (dy < 0) {
            int first_hit = -dy + 1;
            for (const DebrisVoxel& dv : b.voxels) {
                Vec3i base{(int)std::floor(b.pos.x) + dv.off.x, (int)std::floor(b.pos.y) + dv.off.y,
                           (int)std::floor(b.pos.z) + dv.off.z};
                for (int s = 1; s < first_hit; ++s) {
                    Vec3i t = base + Vec3i{0, -s, 0};
                    if (t.y < 0) continue;
                    const Material& m = w_.reg().mat(w_.mat(t));
                    if (m.solid) {
                        first_hit = s;
                        break;
                    }
                }
            }
            if (first_hit <= -dy) {
                hit = true;
                np.y = std::floor(b.pos.y) - (float)(first_hit - 1);
            }
        }
        if (hit) {
            b.pos = np;
            float speed = -b.vel.y;
            land_debris(b, speed);
            debris_.erase(debris_.begin() + (long)i);
            continue;
        }
        b.pos = np;
        if (b.pos.y + (float)b.max_off.y < -4.0f) {
            Event e;
            e.type = EventType::DebrisLanded;
            e.severity = 2;
            e.pos = {(int)b.pos.x, 0, (int)b.pos.z};
            e.causes[0] = b.cause;
            e.text = strfmt("%zu 个方块坠入云海深渊", b.voxels.size());
            chron_.emit(std::move(e));
            debris_.erase(debris_.begin() + (long)i);
            continue;
        }
        b.version++;
        // Falling mass crushes whoever is below.
        if ((b.id + (u32)now_) % 4 == 0) {
            Vec3f c = b.pos + Vec3f((float)b.max_off.x * 0.5f, 0.0f, (float)b.max_off.z * 0.5f);
            float r = std::max(1.5f, 0.5f * (float)std::max(b.max_off.x, b.max_off.z));
            damage_.push_back({c, r, 0.05f * std::min(4.0f, -b.vel.y * 4.0f), 2, b.cause});
        }
        ++i;
    }
}

void Physics::land_debris(DebrisBody& b, float impact_speed) {
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    Vec3i base{(int)std::floor(b.pos.x), (int)std::floor(b.pos.y), (int)std::floor(b.pos.z)};
    int placed = 0, shattered = 0;
    // Sort bottom-up for stable stacking.
    std::vector<DebrisVoxel> vs = b.voxels;
    std::stable_sort(vs.begin(), vs.end(), [](const DebrisVoxel& a, const DebrisVoxel& c) { return a.off.y < c.off.y; });
    for (const DebrisVoxel& dv : vs) {
        const Material& m = reg.mat(vmat(dv.v));
        Voxel out = dv.v;
        // Hard impacts shatter brittle parts; stone breaks to rubble.
        if (impact_speed > 0.6f) {
            if (!m.rigid || m.toughness <= 1) {
                ++shattered;
                continue;
            }
            if (vmat(dv.v) == M.stone || vmat(dv.v) == M.stone_brick || vmat(dv.v) == M.brick) {
                if (rng_.chance(0.5f)) out = make_voxel(M.rubble);
            } else if (m.toughness <= 3 && rng_.chance(0.4f)) {
                ++shattered;
                continue;
            }
        }
        Vec3i t = base + dv.off;
        for (int k = 0; k < 24 && w_.in_bounds(t); ++k) {
            Voxel cur = w_.get(t);
            const Material& cm = reg.mat(vmat(cur));
            if (!cm.solid) break;
            t.y += 1;
        }
        if (!w_.in_bounds(t)) continue;
        w_.set(t, out, b.cause);
        ++placed;
    }
    Event e;
    e.type = EventType::DebrisLanded;
    e.severity = 1;
    e.pos = base;
    e.causes[0] = b.cause;
    e.text = strfmt("坠落物落地：%d 个方块留存，%d 个碎裂", placed, shattered);
    chron_.emit(std::move(e));
    Vec3f c = b.pos + Vec3f((float)b.max_off.x * 0.5f, 0.5f, (float)b.max_off.z * 0.5f);
    float r = std::max(1.5f, 0.6f * (float)std::max(b.max_off.x, b.max_off.z));
    damage_.push_back({c, r, 0.25f * std::min(3.0f, impact_speed * 3.0f), 2, b.cause});
}

// ---------------------------------------------------------------------------------- meteors

u32 Physics::spawn_meteor(const Vec3f& target, float radius, EventId cause) {
    Meteor m;
    m.id = next_body_id_++;
    float ang = rng_.uniform(0.0f, 6.2831853f);
    float height = (float)w_.size_y() + 40.0f - target.y;
    float horiz = height * 0.45f;
    m.pos = target + Vec3f(std::cos(ang) * horiz, height, std::sin(ang) * horiz);
    Vec3f dir = (target - m.pos).normalized();
    m.vel = dir * 2.2f;
    m.radius = radius;
    m.cause = cause;
    meteors_.push_back(m);
    return m.id;
}

void Physics::step_meteors() {
    for (size_t i = 0; i < meteors_.size();) {
        Meteor& m = meteors_[i];
        // Sub-step so fast meteors don't tunnel through thin surfaces.
        bool impact = false;
        Vec3f p = m.pos;
        const int sub = 4;
        for (int s = 0; s < sub; ++s) {
            p = p + m.vel * (1.0f / sub);
            Vec3i ip = p.floor_i();
            if (p.y < 0) { impact = true; break; }
            if (!w_.in_bounds(ip)) continue;
            Voxel v = w_.get(ip);
            const Material& mat = w_.reg().mat(vmat(v));
            if (mat.solid || mat.fluid) { impact = true; break; }
        }
        m.pos = p;
        if (impact) {
            if (p.y >= 0) explode(p, m.radius, m.cause, true);
            meteors_.erase(meteors_.begin() + (long)i);
            continue;
        }
        ++i;
    }
}

void Physics::explode(const Vec3f& center, float radius, EventId cause, bool meteor) {
    const Registry& reg = w_.reg();
    const CoreMats& M = reg.m();
    Event e;
    e.type = meteor ? EventType::MeteorImpact : EventType::Collapse;
    e.severity = radius >= 6 ? 5 : 4;
    e.pos = center.floor_i();
    e.causes[0] = cause;
    e.text = meteor ? strfmt("陨石撞击（半径 %.0f）", radius) : strfmt("爆炸（半径 %.0f）", radius);
    e.data.set("radius", radius);
    EventId eid = chron_.emit(std::move(e));

    int r = (int)std::ceil(radius);
    Vec3i c = center.floor_i();
    int removed = 0;
    std::vector<Vec3i> rim;
    for (int dy = -r; dy <= r; ++dy)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                Vec3i p = c + Vec3i{dx, dy, dz};
                if (!w_.in_bounds(p)) continue;
                float d = std::sqrt((float)(dx * dx + dy * dy + dz * dz));
                float jag = radius * (0.85f + 0.3f * rng_.unit());
                Voxel v = w_.get(p);
                MatId mid = vmat(v);
                if (mid == 0) continue;
                const Material& m = reg.mat(mid);
                if (d <= jag) {
                    if (m.anchor && d > radius * 0.5f) continue;  // levistone resists
                    w_.set(p, make_voxel(M.air), eid);
                    ++removed;
                } else if (d <= radius + 1.8f) {
                    rim.push_back(p);
                }
            }
    // Rim effects: scorched and broken material, fires.
    for (const Vec3i& p : rim) {
        Voxel v = w_.get(p);
        const Material& m = reg.mat(vmat(v));
        if (m.flammability > 0 && rng_.chance(0.35f)) {
            w_.set(p, with_burning(v, true), eid);
            fire_.push(p);
        } else if ((vmat(v) == M.grass) && rng_.chance(0.7f)) {
            w_.set(p, make_voxel(M.dirt), eid);
        } else if ((vmat(v) == M.stone || vmat(v) == M.dirt) && rng_.chance(0.25f)) {
            w_.set(p, make_voxel(M.rubble), eid);
        }
    }
    if (meteor) {
        // Meteorite core and a scatter of basalt at the crater floor.
        Vec3i floor = c + Vec3i{0, -(int)(radius * 0.8f), 0};
        int cr = std::max(1, (int)(radius * 0.35f));
        for (int dy = -cr; dy <= cr; ++dy)
            for (int dz = -cr; dz <= cr; ++dz)
                for (int dx = -cr; dx <= cr; ++dx) {
                    if (dx * dx + dy * dy + dz * dz > cr * cr) continue;
                    Vec3i p = floor + Vec3i{dx, dy, dz};
                    if (!w_.in_bounds(p)) continue;
                    w_.set(p, make_voxel(dy <= 0 && rng_.chance(0.6f) ? M.meteorite : M.basalt), eid);
                }
    }
    damage_.push_back({center, radius * 2.2f, 0.6f, 0, eid});
}

// ---------------------------------------------------------------------------------- persistence

namespace {
void save_queue(BinWriter& w, const PosQueue& q) {
    w.varu(q.size());
    for (const Vec3i& p : q.items()) w.vec3i(p);
}
void load_queue(BinReader& r, PosQueue& q) {
    q.clear();
    u64 n = r.varu();
    for (u64 i = 0; i < n; ++i) q.push(r.vec3i());
}
}  // namespace

void Physics::save(BinWriter& w) const {
    size_t s = w.begin_section("PHYS");
    w.u64v(rng_.state());
    w.u64v(rng_.inc());
    save_queue(w, water_);
    save_queue(w, fire_);
    save_queue(w, granular_);
    save_queue(w, support_);
    w.varu(springs_.size());
    for (const Vec3i& p : springs_) w.vec3i(p);
    w.u32v(next_body_id_);
    w.varu(debris_.size());
    for (const DebrisBody& b : debris_) {
        w.u32v(b.id);
        w.vec3f(b.pos);
        w.vec3f(b.vel);
        w.vec3i(b.max_off);
        w.u32v(b.cause);
        w.varu(b.voxels.size());
        for (const DebrisVoxel& dv : b.voxels) {
            w.vec3i(dv.off);
            w.u16v(dv.v);
        }
    }
    w.varu(meteors_.size());
    for (const Meteor& m : meteors_) {
        w.u32v(m.id);
        w.vec3f(m.pos);
        w.vec3f(m.vel);
        w.f32(m.radius);
        w.u32v(m.cause);
    }
    w.varu(removal_causes_.size());
    for (auto& rc : removal_causes_) {
        w.vec3i(rc.first);
        w.u32v(rc.second);
    }
    w.i64v(stats_.water_units_to_void);
    w.i64v(stats_.water_units_spring);
    w.i64v(stats_.water_units_evaporated);
    w.varu(active_cells_.size());
    for (const Vec3i& c : active_cells_) w.vec3i(c);
    w.u64v(rain_until_);
    w.u64v(next_weather_);
    w.i64v(stats_.water_units_rain);
    for (const SpringState& ss : spring_state_) {
        w.boolean(ss.flowing);
        w.u32v(ss.dry);
        w.u32v(ss.touch_cause);
        w.u32v(ss.lost_event);
    }
    save_queue(w, puddles_);
    w.end_section(s);
}

void Physics::load(BinReader& outer) {
    BinReader r = outer.section("PHYS");
    u64 st = r.u64v(), inc = r.u64v();
    rng_.set_raw(st, inc);
    load_queue(r, water_);
    load_queue(r, fire_);
    load_queue(r, granular_);
    load_queue(r, support_);
    springs_.clear();
    u64 ns = r.varu();
    for (u64 i = 0; i < ns; ++i) springs_.push_back(r.vec3i());
    next_body_id_ = r.u32v();
    debris_.clear();
    u64 nd = r.varu();
    for (u64 i = 0; i < nd; ++i) {
        DebrisBody b;
        b.id = r.u32v();
        b.pos = r.vec3f();
        b.vel = r.vec3f();
        b.max_off = r.vec3i();
        b.cause = r.u32v();
        u64 nv = r.varu();
        for (u64 k = 0; k < nv; ++k) {
            DebrisVoxel dv;
            dv.off = r.vec3i();
            dv.v = r.u16v();
            b.voxels.push_back(dv);
        }
        debris_.push_back(std::move(b));
    }
    meteors_.clear();
    u64 nm = r.varu();
    for (u64 i = 0; i < nm; ++i) {
        Meteor m;
        m.id = r.u32v();
        m.pos = r.vec3f();
        m.vel = r.vec3f();
        m.radius = r.f32();
        m.cause = r.u32v();
        meteors_.push_back(m);
    }
    removal_causes_.clear();
    u64 nrc = r.varu();
    for (u64 i = 0; i < nrc; ++i) {
        Vec3i p = r.vec3i();
        u32 c = r.u32v();
        removal_causes_.push_back({p, c});
    }
    stats_ = {};
    stats_.water_units_to_void = r.i64v();
    stats_.water_units_spring = r.i64v();
    stats_.water_units_evaporated = r.i64v();
    active_cells_.clear();
    u64 nac = r.varu();
    for (u64 i = 0; i < nac; ++i) active_cells_.push_back(r.vec3i());
    rain_until_ = r.u64v();
    next_weather_ = r.u64v();
    stats_.water_units_rain = r.i64v();
    spring_state_.assign(springs_.size(), SpringState{});
    for (SpringState& ss : spring_state_) {
        ss.flowing = r.boolean();
        ss.dry = r.u32v();
        ss.touch_cause = r.u32v();
        ss.lost_event = r.u32v();
    }
    puddles_.clear();
    if (!r.at_end()) load_queue(r, puddles_);
    damage_.clear();
}

u64 Physics::hash() const {
    u64 h = hash_combine(rng_.state(), rng_.inc());
    for (const Vec3i& p : water_.items()) h = hash_combine(h, (u64)Vec3iHash{}(p));
    for (const Vec3i& p : puddles_.items()) h = hash_combine(h, (u64)Vec3iHash{}(p) ^ 0x9D);
    for (const Vec3i& p : fire_.items()) h = hash_combine(h, (u64)Vec3iHash{}(p));
    h = hash_combine(h, debris_.size());
    for (const DebrisBody& b : debris_) {
        h = fnv1a64(&b.pos, sizeof b.pos, h);
    }
    return h;
}

}  // namespace icarus
