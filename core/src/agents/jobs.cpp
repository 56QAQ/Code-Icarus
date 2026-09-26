#include "icarus/agents/jobs.h"

#include <algorithm>

#include "icarus/agents/character.h"
#include "icarus/util/rng.h"

namespace icarus {

const char* job_name_zh(JobType t) {
    switch (t) {
        case JobType::Till: return "翻耕";
        case JobType::Sow: return "播种";
        case JobType::Harvest: return "收割";
        case JobType::Chop: return "伐木";
        case JobType::Mine: return "采石";
        case JobType::Dig: return "挖掘";
        case JobType::HaulPile: return "搬运入库";
        case JobType::HaulToSite: return "运送建材";
        case JobType::Build: return "建造";
        case JobType::Cook: return "烹饪";
        case JobType::Craft: return "制作";
        case JobType::Research: return "研究";
        case JobType::Forage: return "采集";
        case JobType::Guard: return "守卫";
        default: return "工作";
    }
}

int job_skill(JobType t) {
    switch (t) {
        case JobType::Till:
        case JobType::Sow:
        case JobType::Harvest: return kFarming;
        case JobType::Chop:
        case JobType::Mine:
        case JobType::Dig: return kMining;
        case JobType::HaulPile:
        case JobType::HaulToSite: return kHauling;
        case JobType::Build: return kBuilding;
        case JobType::Cook: return kCooking;
        case JobType::Craft: return kCrafting;
        case JobType::Research: return kResearch;
        case JobType::Forage: return kFarming;
        case JobType::Guard: return kCombat;
        default: return kHauling;
    }
}

const char* job_category(JobType t) {
    switch (t) {
        case JobType::Till:
        case JobType::Sow:
        case JobType::Harvest:
        case JobType::Cook:
        case JobType::Forage: return "food";
        case JobType::Build:
        case JobType::HaulToSite:
        case JobType::Craft:
        case JobType::Dig: return "build";
        case JobType::Chop:
        case JobType::Mine:
        case JobType::HaulPile: return "gather";
        case JobType::Research: return "research";
        case JobType::Guard: return "military";
        default: return "gather";
    }
}

void JobBoard::reset() {
    jobs_.assign(1, Job{});
    free_.clear();
}

u32 JobBoard::add(Job j) {
    u32 id;
    if (!free_.empty()) {
        auto it = std::min_element(free_.begin(), free_.end());
        id = *it;
        free_.erase(it);
    } else {
        id = (u32)jobs_.size();
        jobs_.emplace_back();
    }
    j.id = id;
    j.alive = true;
    jobs_[id] = j;
    return id;
}

bool JobBoard::claim(u32 id, EntityId who, Tick until) {
    Job* j = get(id);
    if (!j) return false;
    if (j->claimed_by != kNoEntity && j->claimed_by != who) return false;
    j->claimed_by = who;
    j->claim_expiry = until;
    return true;
}

void JobBoard::release(u32 id, EntityId who) {
    Job* j = get(id);
    if (j && j->claimed_by == who) {
        j->claimed_by = kNoEntity;
        j->claim_expiry = 0;
    }
}

void JobBoard::complete(u32 id) {
    Job* j = get(id);
    if (!j) return;
    j->alive = false;
    free_.push_back(id);
}

void JobBoard::expire(Tick now) {
    for (auto& j : jobs_)
        if (j.alive && j.claimed_by != kNoEntity && j.claim_expiry <= now) {
            j.claimed_by = kNoEntity;
            j.claim_expiry = 0;
        }
}

u32 JobBoard::find(JobType t, const Vec3i& pos) const {
    for (const auto& j : jobs_)
        if (j.alive && j.type == t && j.pos == pos) return j.id;
    return 0;
}

size_t JobBoard::open_count() const {
    size_t n = 0;
    for (const auto& j : jobs_)
        if (j.alive && j.claimed_by == kNoEntity) ++n;
    return n;
}

void JobBoard::save(BinWriter& w) const {
    size_t s = w.begin_section("JOBS");
    w.varu(jobs_.size());
    for (size_t i = 1; i < jobs_.size(); ++i) {
        const Job& j = jobs_[i];
        w.boolean(j.alive);
        if (!j.alive) continue;
        w.u8v((u8)j.type);
        w.u16v(j.polity);
        w.vec3i(j.pos);
        w.u32v(j.from);
        w.u32v(j.to);
        w.u16v(j.item);
        w.vari(j.count);
        w.u32v(j.building);
        w.u32v(j.farm);
        w.u32v(j.plot);
        w.u32v(j.project);
        w.f32(j.priority);
        w.u32v(j.claimed_by);
        w.u64v(j.claim_expiry);
        w.u64v(j.created);
        w.u32v(j.cause);
    }
    w.varu(free_.size());
    for (u32 f : free_) w.u32v(f);
    // Unreachable jobs (added later; older saves have none).
    std::vector<const Job*> resting;
    for (size_t i = 1; i < jobs_.size(); ++i)
        if (jobs_[i].alive && (jobs_[i].path_fails || jobs_[i].suspended_until)) resting.push_back(&jobs_[i]);
    w.varu(resting.size());
    for (const Job* j : resting) {
        w.u32v(j->id);
        w.u8v(j->path_fails);
        w.u64v(j->suspended_until);
    }
    w.end_section(s);
}

void JobBoard::load(BinReader& outer) {
    BinReader r = outer.section("JOBS");
    u64 n = r.varu();
    jobs_.assign((size_t)n, Job{});
    for (size_t i = 1; i < (size_t)n; ++i) {
        Job& j = jobs_[i];
        j.id = (u32)i;
        j.alive = r.boolean();
        if (!j.alive) continue;
        j.type = (JobType)r.u8v();
        j.polity = r.u16v();
        j.pos = r.vec3i();
        j.from = r.u32v();
        j.to = r.u32v();
        j.item = r.u16v();
        j.count = (i32)r.vari();
        j.building = r.u32v();
        j.farm = r.u32v();
        j.plot = r.u32v();
        j.project = r.u32v();
        j.priority = r.f32();
        j.claimed_by = r.u32v();
        j.claim_expiry = r.u64v();
        j.created = r.u64v();
        j.cause = r.u32v();
    }
    free_.clear();
    u64 nf = r.varu();
    for (u64 k = 0; k < nf; ++k) free_.push_back(r.u32v());
    if (!r.at_end()) {
        u64 nr = r.varu();
        for (u64 k = 0; k < nr; ++k) {
            u32 id = r.u32v();
            u8 fails = r.u8v();
            Tick until = r.u64v();
            if (Job* j = get(id)) {
                j->path_fails = fails;
                j->suspended_until = until;
            }
        }
    }
}

u64 JobBoard::hash() const {
    u64 h = 17;
    for (auto& j : jobs_)
        if (j.alive) h = hash_combine(h, ((u64)j.id << 8) ^ (u64)j.type ^ ((u64)j.claimed_by << 32));
    return h;
}

}  // namespace icarus
