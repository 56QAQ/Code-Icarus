#include "icarus/world/world.h"

#include <cstring>

#include "icarus/util/log.h"

namespace icarus {

World::World(const Registry& reg) : reg_(&reg) {}

void World::init(const WorldConfig& cfg) {
    gen_.init(cfg, *reg_);
    cells_x_ = cfg.cells_x;
    cells_y_ = cfg.cells_y;
    cells_z_ = cfg.cells_z;
    cells_.clear();
    cells_.resize((size_t)cells_x_ * cells_y_ * cells_z_);
    for (int y = 0; y < cells_y_; ++y)
        for (int z = 0; z < cells_z_; ++z)
            for (int x = 0; x < cells_x_; ++x) {
                Cell& c = cells_[cell_index({x, y, z})];
                c.coord = {x, y, z};
                c.biome = gen_.cell_biome(c.coord);
            }
    changes_.clear();
    for (auto& e : peek_cache_) e = PeekCacheEntry{};
    dirty_flag_.assign(cells_.size(), 0);
    dirty_list_.clear();
    now_ = 0;
}

void World::generate(Cell& c) const {
    std::vector<Voxel> buf(kCellVol);
    gen_.generate_cell(c.coord, buf.data());
    bool same = true;
    for (int i = 1; i < kCellVol && same; ++i) same = buf[i] == buf[0];
    c.pristine = true;
    if (same) {
        c.uniform = true;
        c.uniform_value = buf[0];
        c.vox.clear();
    } else {
        c.uniform = false;
        c.vox = std::move(buf);
    }
    c.packed.clear();
}

void World::unpack_into(const Cell& c, Voxel* out) const {
    if (c.state == CellState::Ungenerated) {
        gen_.generate_cell(c.coord, out);
        return;
    }
    if (c.uniform) {
        for (int i = 0; i < kCellVol; ++i) out[i] = c.uniform_value;
    } else if (!c.vox.empty()) {
        std::memcpy(out, c.vox.data(), sizeof(Voxel) * kCellVol);
    } else {
        if (!rle_decode_u16(c.packed, out, kCellVol)) {
            log_error("corrupt packed cell " + c.coord.str());
            for (int i = 0; i < kCellVol; ++i) out[i] = 0;
        }
    }
}

void World::activate(Cell& c) {
    if (c.state == CellState::Active) return;
    if (c.state == CellState::Ungenerated) {
        generate(c);
    } else if (!c.uniform && c.vox.empty()) {
        c.vox.resize(kCellVol);
        if (!rle_decode_u16(c.packed, c.vox.data(), kCellVol)) log_error("corrupt packed cell " + c.coord.str());
        c.packed.clear();
        c.packed.shrink_to_fit();
    }
    c.state = CellState::Active;
    c.activated_tick = now_;
    Tick last = c.last_sim_tick;
    if (on_wake && last < now_) on_wake(c, last, now_);
    c.last_sim_tick = now_;
}

void World::compress(Cell& c) {
    if (c.state != CellState::Active) return;
    if (!c.uniform) {
        bool same = true;
        for (int i = 1; i < kCellVol && same; ++i) same = c.vox[i] == c.vox[0];
        if (same) {
            c.uniform = true;
            c.uniform_value = c.vox[0];
            c.vox.clear();
            c.vox.shrink_to_fit();
        } else {
            c.packed = rle_encode_u16(c.vox.data(), kCellVol);
            c.vox.clear();
            c.vox.shrink_to_fit();
        }
    }
    c.state = CellState::Dormant;
    c.last_sim_tick = now_;
}

Cell& World::ensure_active(const Vec3i& cc) {
    Cell& c = cells_[cell_index(cc)];
    if (c.state != CellState::Active) activate(c);
    c.last_touch_tick = now_;
    return c;
}

Voxel World::get(const Vec3i& p) {
    if (!in_bounds(p)) return 0;  // void is air
    Cell& c = cells_[cell_index(cell_of(p))];
    if (c.state != CellState::Active) activate(c);
    c.last_touch_tick = now_;
    if (c.uniform) return c.uniform_value;
    return c.vox[local_index(p.x & kCellMask, p.y & kCellMask, p.z & kCellMask)];
}

void World::mark_dirty(Cell& c) {
    size_t idx = cell_index(c.coord);
    if (!dirty_flag_[idx]) {
        dirty_flag_[idx] = 1;
        dirty_list_.push_back(c.coord);
    }
}

void World::set_silent(const Vec3i& p, Voxel v) {
    if (!in_bounds(p)) return;
    Vec3i cc = cell_of(p);
    Cell& c = cells_[cell_index(cc)];
    if (c.state != CellState::Active) activate(c);
    c.last_touch_tick = now_;
    if (c.uniform) {
        if (c.uniform_value == v) return;
        c.vox.assign(kCellVol, c.uniform_value);
        c.uniform = false;
    }
    Voxel& slot = c.vox[local_index(p.x & kCellMask, p.y & kCellMask, p.z & kCellMask)];
    if (slot == v) return;
    slot = v;
    c.pristine = false;
    c.version++;
    mark_dirty(c);
    // Neighbouring cells need re-meshing when a boundary cube changes.
    const int lx = p.x & kCellMask, ly = p.y & kCellMask, lz = p.z & kCellMask;
    auto nb = [&](int dx, int dy, int dz) {
        Vec3i n{cc.x + dx, cc.y + dy, cc.z + dz};
        if (!cell_in_bounds(n)) return;
        Cell& nc = cells_[cell_index(n)];
        nc.version++;
        mark_dirty(nc);
    };
    if (lx == 0) nb(-1, 0, 0);
    if (lx == kCellMask) nb(1, 0, 0);
    if (ly == 0) nb(0, -1, 0);
    if (ly == kCellMask) nb(0, 1, 0);
    if (lz == 0) nb(0, 0, -1);
    if (lz == kCellMask) nb(0, 0, 1);
}

void World::set(const Vec3i& p, Voxel v, u32 cause) {
    if (!in_bounds(p)) return;
    Voxel before = get(p);
    if (before == v) return;
    set_silent(p, v);
    changes_.push_back({p, before, v, cause});
}

Voxel World::peek(const Vec3i& p) const {
    if (!in_bounds(p)) return 0;
    const Cell& c = cells_[cell_index(cell_of(p))];
    if (c.state == CellState::Ungenerated) {
        // Generate into the cell as dormant pristine data; this does not alter any
        // simulation-relevant field (last_sim_tick / last_touch_tick / state semantics).
        Cell& mc = const_cast<Cell&>(c);
        generate(mc);
        if (!mc.uniform) {
            mc.packed = rle_encode_u16(mc.vox.data(), kCellVol);
            mc.vox.clear();
            mc.vox.shrink_to_fit();
        }
        mc.state = CellState::Dormant;
    }
    if (c.uniform) return c.uniform_value;
    const int li = local_index(p.x & kCellMask, p.y & kCellMask, p.z & kCellMask);
    if (!c.vox.empty()) return c.vox[li];
    // Dormant compressed cell: decode through a small cache.
    const size_t ci = cell_index(c.coord);
    for (auto& e : peek_cache_)
        if (e.cell == ci && e.version == c.version && !e.data.empty()) return e.data[li];
    auto& e = peek_cache_[peek_cache_next_];
    peek_cache_next_ = (peek_cache_next_ + 1) % peek_cache_.size();
    e.cell = ci;
    e.version = c.version;
    e.data.resize(kCellVol);
    unpack_into(c, e.data.data());
    return e.data[li];
}

void World::peek_cell(const Vec3i& cc, Voxel* out) const {
    if (!cell_in_bounds(cc)) {
        for (int i = 0; i < kCellVol; ++i) out[i] = 0;
        return;
    }
    const Cell& c = cells_[cell_index(cc)];
    if (c.state == CellState::Ungenerated) peek(Vec3i{cc.x * kCellSize, cc.y * kCellSize, cc.z * kCellSize});
    unpack_into(c, out);
}

void World::touch(const Vec3i& p) {
    if (!in_bounds(p)) return;
    Cell& c = cells_[cell_index(cell_of(p))];
    if (c.state != CellState::Active) activate(c);
    c.last_touch_tick = now_;
}

void World::hold_cell(const Vec3i& cc, bool hold) {
    Cell* c = cell(cc);
    if (!c) return;
    if (hold) {
        if (c->state != CellState::Active) activate(*c);
        c->hold++;
    } else if (c->hold > 0) {
        c->hold--;
    }
}

u32 World::cell_version(const Vec3i& cc) const {
    const Cell* c = cell(cc);
    return c ? c->version : 0;
}

Biome World::cell_biome(const Vec3i& cc) const {
    const Cell* c = cell(cc);
    return c ? c->biome : Biome::Sky;
}

int World::update_lifecycle(Tick now, Tick idle_ticks) {
    now_ = now;
    int demoted = 0;
    for (Cell& c : cells_) {
        if (c.state != CellState::Active || c.hold > 0) continue;
        if (now - c.last_touch_tick < idle_ticks) continue;
        compress(c);
        ++demoted;
    }
    return demoted;
}

std::vector<Vec3i> World::take_dirty_cells() {
    std::vector<Vec3i> out;
    out.swap(dirty_list_);
    for (const Vec3i& cc : out) dirty_flag_[cell_index(cc)] = 0;
    return out;
}

int World::surface_y(int x, int z) {
    for (int y = size_y() - 1; y >= 0; --y) {
        const Material& m = reg_->mat(vmat(get({x, y, z})));
        if (m.solid) return y;
    }
    return -1;
}

int World::surface_y_peek(int x, int z) const {
    for (int y = size_y() - 1; y >= 0; --y) {
        const Material& m = reg_->mat(vmat(peek({x, y, z})));
        if (m.solid) return y;
    }
    return -1;
}

WorldStats World::stats() const {
    WorldStats s;
    for (const Cell& c : cells_) {
        switch (c.state) {
            case CellState::Ungenerated: s.ungenerated++; break;
            case CellState::Dormant: s.dormant++; break;
            case CellState::Active: s.active++; break;
        }
        if (!c.uniform) s.non_uniform++;
        s.bytes += c.vox.size() * sizeof(Voxel) + c.packed.size();
    }
    return s;
}

std::vector<std::pair<Vec3i, std::vector<PendingEvent>>> World::all_pending() const {
    std::vector<std::pair<Vec3i, std::vector<PendingEvent>>> out;
    for (const Cell& c : cells_)
        if (!c.pending.empty()) out.push_back({c.coord, c.pending});
    return out;
}

// ---------------------------------------------------------------------------------
// Persistence. Only simulation-relevant information is stored: pristine cells are
// regenerated from the seed, so a save holds the world's *changes* plus metadata.

namespace {
bool meta_is_default(const Cell& c) {
    return c.last_sim_tick == 0 && c.last_touch_tick == 0 && c.owner == 0 && c.pending.empty() && c.hold == 0 &&
           c.fertility == 0.5f && c.moisture == 0.5f && c.pollution == 0.0f && c.version == 0;
}
}  // namespace

void World::save(BinWriter& w) const {
    size_t sec = w.begin_section("WRLD");
    const WorldConfig& cfg = gen_.config();
    w.u64v(cfg.seed);
    w.vari(cfg.cells_x);
    w.vari(cfg.cells_y);
    w.vari(cfg.cells_z);
    w.f32(cfg.island_radius);
    w.vari(cfg.base_height);
    w.vari(cfg.islet_count);
    w.u64v(now_);

    std::vector<const Cell*> stored;
    for (const Cell& c : cells_) {
        bool sim_active = c.state == CellState::Active;
        if (!c.pristine || sim_active || !meta_is_default(c)) stored.push_back(&c);
    }
    w.varu(stored.size());
    std::vector<Voxel> tmp(kCellVol);
    for (const Cell* c : stored) {
        w.vec3i(c->coord);
        // State: 2 = active, 1 = dormant with data, 0 = no stored data (regenerate).
        u8 st = c->state == CellState::Active ? 2 : (c->pristine ? 0 : 1);
        w.u8v(st);
        w.boolean(c->pristine);
        w.u64v(c->last_sim_tick);
        w.u64v(c->last_touch_tick);
        w.u64v(c->activated_tick);
        w.u32v(c->version);
        w.u16v(c->owner);
        w.f32(c->fertility);
        w.f32(c->moisture);
        w.f32(c->pollution);
        w.u32v(c->hold);
        w.varu(c->pending.size());
        for (const auto& e : c->pending) {
            w.u16v(e.type);
            w.u64v(e.due);
            w.vec3i(e.pos);
            w.i32v(e.a);
            w.i32v(e.b);
            w.u32v(e.cause);
        }
        if (!c->pristine) {
            if (c->uniform) {
                w.u8v(1);
                w.u16v(c->uniform_value);
            } else if (!c->vox.empty()) {
                w.u8v(2);
                w.bytes(rle_encode_u16(c->vox.data(), kCellVol));
            } else {
                w.u8v(2);
                w.bytes(c->packed);
            }
        }
    }
    w.end_section(sec);
}

void World::load(BinReader& outer) {
    BinReader r = outer.section("WRLD");
    WorldConfig cfg;
    cfg.seed = r.u64v();
    cfg.cells_x = (int)r.vari();
    cfg.cells_y = (int)r.vari();
    cfg.cells_z = (int)r.vari();
    cfg.island_radius = r.f32();
    cfg.base_height = (int)r.vari();
    cfg.islet_count = (int)r.vari();
    Tick now = r.u64v();
    init(cfg);
    now_ = now;
    u64 n = r.varu();
    for (u64 i = 0; i < n; ++i) {
        Vec3i cc = r.vec3i();
        if (!cell_in_bounds(cc)) throw BinError("cell out of bounds in save");
        Cell& c = cells_[cell_index(cc)];
        u8 st = r.u8v();
        c.pristine = r.boolean();
        c.last_sim_tick = r.u64v();
        c.last_touch_tick = r.u64v();
        c.activated_tick = r.u64v();
        c.version = r.u32v();
        c.owner = r.u16v();
        c.fertility = r.f32();
        c.moisture = r.f32();
        c.pollution = r.f32();
        c.hold = r.u32v();
        u64 np = r.varu();
        c.pending.clear();
        for (u64 k = 0; k < np; ++k) {
            PendingEvent e;
            e.type = r.u16v();
            e.due = r.u64v();
            e.pos = r.vec3i();
            e.a = r.i32v();
            e.b = r.i32v();
            e.cause = r.u32v();
            c.pending.push_back(e);
        }
        if (!c.pristine) {
            u8 kind = r.u8v();
            if (kind == 1) {
                c.uniform = true;
                c.uniform_value = r.u16v();
            } else {
                c.uniform = false;
                c.packed = r.bytes();
            }
            c.state = CellState::Dormant;
        } else {
            c.state = CellState::Ungenerated;
        }
        if (st == 2) {
            // Restore active state exactly, without running wake hooks.
            if (c.state == CellState::Ungenerated) {
                generate(c);
            } else if (!c.uniform) {
                c.vox.resize(kCellVol);
                if (!rle_decode_u16(c.packed, c.vox.data(), kCellVol)) throw BinError("corrupt cell data");
                c.packed.clear();
            }
            c.state = CellState::Active;
        }
        mark_dirty(c);
    }
}

u64 World::state_hash() const {
    u64 h = 1469598103934665603ull;
    std::vector<Voxel> tmp(kCellVol);
    for (const Cell& c : cells_) {
        bool active = c.state == CellState::Active;
        if (c.pristine && !active && meta_is_default(c)) continue;
        h = fnv1a64(&c.coord, sizeof(c.coord), h);
        u8 a = active ? 1 : 0;
        h = fnv1a64(&a, 1, h);
        h = fnv1a64(&c.last_sim_tick, sizeof(Tick), h);
        h = fnv1a64(&c.last_touch_tick, sizeof(Tick), h);
        h = fnv1a64(&c.owner, sizeof(c.owner), h);
        if (!c.pristine) {
            unpack_into(c, tmp.data());
            h = fnv1a64(tmp.data(), sizeof(Voxel) * kCellVol, h);
        }
    }
    return h;
}

}  // namespace icarus
