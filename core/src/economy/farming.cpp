#include "icarus/economy/farming.h"

#include <cmath>

#include "icarus/sim/clock.h"
#include "icarus/util/log.h"

namespace icarus {

void Farming::reset() {
    farms_.assign(1, Farm{});
    cursor_ = 0;
    withered_today_ = 0;
    last_wither_report_ = 0;
}

u32 Farming::create(u16 polity, const std::string& name, const std::vector<Vec3i>& grounds) {
    Farm f;
    f.id = (u32)farms_.size();
    f.alive = true;
    f.polity = polity;
    f.name = name;
    Vec3f c{0, 0, 0};
    for (const Vec3i& g : grounds) {
        Plot p;
        p.ground = g;
        f.plots.push_back(p);
        c += Vec3f(g);
    }
    if (!grounds.empty()) c = c * (1.0f / (float)grounds.size());
    f.center = c.floor_i();
    farms_.push_back(std::move(f));
    return farms_.back().id;
}

PlotState Farming::state(const Plot& p) {
    const CoreMats& M = w_.reg().m();
    MatId g = w_.mat(p.ground);
    if (g != M.farmland) return PlotState::NeedsTill;
    Voxel above = w_.get(p.ground + Vec3i{0, 1, 0});
    if (vmat(above) != M.crop) return PlotState::Empty;
    return vlevel(above) >= 7 ? PlotState::Mature : PlotState::Growing;
}

bool Farming::till(Plot& p, EventId cause) {
    const CoreMats& M = w_.reg().m();
    const Material& g = w_.material(p.ground);
    if (!g.solid || !g.fertile) {
        // Rebuild the soil if it was destroyed (fill with dirt later via construction).
        return false;
    }
    Vec3i up = p.ground + Vec3i{0, 1, 0};
    const Material& um = w_.material(up);
    if (um.solid) return false;
    w_.set(p.ground, make_voxel(M.farmland), cause);
    p.growth = 0;
    return true;
}

bool Farming::sow(Plot& p, EventId cause) {
    const CoreMats& M = w_.reg().m();
    if (w_.mat(p.ground) != M.farmland) return false;
    Vec3i up = p.ground + Vec3i{0, 1, 0};
    if (w_.mat(up) != M.air) return false;
    w_.set(up, make_voxel(M.crop, 0), cause);
    p.growth = 0;
    p.dry_since = 0;
    return true;
}

int Farming::harvest(Plot& p, EventId cause) {
    const CoreMats& M = w_.reg().m();
    Vec3i up = p.ground + Vec3i{0, 1, 0};
    Voxel v = w_.get(up);
    if (vmat(v) != M.crop) return 0;
    int stage = vlevel(v);
    w_.set(up, make_voxel(M.air), cause);
    p.growth = 0;
    if (stage >= 7) return yield;
    return stage >= 5 ? 1 : 0;
}

bool Farming::check_irrigation(const Plot& p) {
    const CoreMats& M = w_.reg().m();
    const int R = 4;
    for (int dy = 0; dy >= -1; --dy)
        for (int dz = -R; dz <= R; ++dz)
            for (int dx = -R; dx <= R; ++dx) {
                Vec3i q{p.ground.x + dx, p.ground.y + dy, p.ground.z + dz};
                Voxel v = w_.get(q);
                if (vmat(v) == M.water && vlevel(v) >= 2) return true;
            }
    return false;
}

void Farming::step(Tick now, Rng& rng) {
    (void)rng;
    // Update every plot once per 20 ticks, a slice per tick.
    constexpr Tick kPeriod = 20;
    const CoreMats& M = w_.reg().m();
    const float per_update = (float)kPeriod / ((float)kTicksPerDay * growth_days);
    for (auto& f : farms_) {
        if (!f.alive) continue;
        for (size_t i = 0; i < f.plots.size(); ++i) {
            if ((i + f.id) % kPeriod != now % kPeriod) continue;
            Plot& p = f.plots[i];
            Vec3i up = p.ground + Vec3i{0, 1, 0};
            Voxel v = w_.get(up);
            if (vmat(v) != M.crop) continue;
            int stage = vlevel(v);
            if (stage >= 7) continue;
            // Irrigation is re-checked hourly.
            if ((now / kPeriod) % (kTicksPerHour / kPeriod) == (i % (kTicksPerHour / kPeriod))) {
                bool was = p.irrigated;
                p.irrigated = check_irrigation(p);
                if (!p.irrigated && (was || p.dry_since == 0)) p.dry_since = now;
                if (p.irrigated) p.dry_since = 0;
            }
            float rate = p.irrigated ? 1.0f : dry_factor;
            p.growth = std::min(1.0f, p.growth + per_update * rate);
            if (!p.irrigated && p.dry_since > 0 && now - p.dry_since > (Tick)(wither_days * (float)kTicksPerDay)) {
                w_.set(up, make_voxel(M.air));
                p.growth = 0;
                p.dry_since = now;
                withered_today_++;
                continue;
            }
            int want = std::min(7, (int)std::floor(p.growth * 7.0f + 1e-4f));
            if (want > stage) w_.set(up, make_voxel(M.crop, (u8)want));
        }
    }
    if (withered_today_ > 0 && now - last_wither_report_ >= kTicksPerHour * 3) {
        Event e;
        e.type = EventType::CropFailure;
        e.severity = withered_today_ >= 10 ? 3 : 2;
        e.text = strfmt("缺乏灌溉，%d 株作物枯死", withered_today_);
        e.data.set("count", withered_today_);
        chron_.emit(std::move(e));
        withered_today_ = 0;
        last_wither_report_ = now;
    }
}

FarmStats Farming::stats(u32 farm_id) {
    FarmStats s;
    Farm* f = get(farm_id);
    if (!f) return s;
    for (auto& p : f->plots) {
        s.plots++;
        switch (state(p)) {
            case PlotState::NeedsTill: s.needs_till++; break;
            case PlotState::Empty: s.empty++; break;
            case PlotState::Growing: s.growing++; break;
            case PlotState::Mature: s.mature++; break;
        }
        if (p.irrigated) s.irrigated++;
    }
    return s;
}

FarmStats Farming::stats_polity(u16 polity) {
    FarmStats t;
    for (auto& f : farms_) {
        if (!f.alive || f.polity != polity) continue;
        FarmStats s = stats(f.id);
        t.plots += s.plots;
        t.needs_till += s.needs_till;
        t.empty += s.empty;
        t.growing += s.growing;
        t.mature += s.mature;
        t.irrigated += s.irrigated;
    }
    return t;
}

void Farming::save(BinWriter& w) const {
    size_t s = w.begin_section("FARM");
    w.varu(farms_.size());
    for (size_t i = 1; i < farms_.size(); ++i) {
        const Farm& f = farms_[i];
        w.boolean(f.alive);
        if (!f.alive) continue;
        w.u16v(f.polity);
        w.str(f.name);
        w.vec3i(f.center);
        w.varu(f.plots.size());
        for (const Plot& p : f.plots) {
            w.vec3i(p.ground);
            w.f32(p.growth);
            w.boolean(p.irrigated);
            w.u64v(p.dry_since);
        }
    }
    w.vari(withered_today_);
    w.u64v(last_wither_report_);
    w.end_section(s);
}

void Farming::load(BinReader& outer) {
    BinReader r = outer.section("FARM");
    u64 n = r.varu();
    farms_.assign((size_t)n, Farm{});
    for (size_t i = 1; i < (size_t)n; ++i) {
        Farm& f = farms_[i];
        f.id = (u32)i;
        f.alive = r.boolean();
        if (!f.alive) continue;
        f.polity = r.u16v();
        f.name = r.str();
        f.center = r.vec3i();
        u64 np = r.varu();
        for (u64 k = 0; k < np; ++k) {
            Plot p;
            p.ground = r.vec3i();
            p.growth = r.f32();
            p.irrigated = r.boolean();
            p.dry_since = r.u64v();
            f.plots.push_back(p);
        }
    }
    withered_today_ = (int)r.vari();
    last_wither_report_ = r.u64v();
}

u64 Farming::hash() const {
    u64 h = 13;
    for (auto& f : farms_)
        for (auto& p : f.plots) h = hash_combine(h, (u64)(p.growth * 1000.0f) + (p.irrigated ? 7 : 0));
    return h;
}

}  // namespace icarus
