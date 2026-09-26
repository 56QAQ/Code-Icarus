// Job board: work offered by fields, construction sites, projects and piles. Jobs are
// claimed (reserved) by one character at a time with an expiry, so two residents never
// work the same cube or carry the same sack, and a vanished worker frees the job.
#pragma once

#include <vector>

#include "icarus/economy/economy.h"
#include "icarus/sim/chronicle.h"
#include "icarus/util/binio.h"

namespace icarus {

enum class JobType : u8 {
    None = 0, Till, Sow, Harvest, Chop, Mine, Dig, HaulPile, HaulToSite, Build, Cook, Craft, Research, Forage, Guard,
    Trade,  // a caravan: goods to a partner polity's store and payment back (to = its store, project = the partner)
    Count
};
const char* job_name_zh(JobType t);
int job_skill(JobType t);
const char* job_category(JobType t);  // food / build / gather / research / military / haul

struct Job {
    u32 id = 0;
    JobType type = JobType::None;
    bool alive = false;
    u16 polity = 0;
    Vec3i pos;               // where the work happens (cube to work on)
    StoreId from = kNoStore; // source store for hauling
    StoreId to = kNoStore;   // destination store
    ItemId item = kNoItem;
    i32 count = 0;
    u32 building = 0;
    u32 farm = 0;
    u32 plot = 0;
    u32 project = 0;
    float priority = 1.0f;
    EntityId claimed_by = kNoEntity;
    Tick claim_expiry = 0;
    // Nobody could get there: after a few failed routes the job rests for a while
    // (for everyone), instead of each resident rediscovering it is out of reach.
    u8 path_fails = 0;
    Tick suspended_until = 0;
    Tick created = 0;
    EventId cause = 0;
};

class JobBoard {
public:
    void reset();
    u32 add(Job j);
    Job* get(u32 id) { return (id > 0 && id < jobs_.size() && jobs_[id].alive) ? &jobs_[id] : nullptr; }
    const std::vector<Job>& all() const { return jobs_; }
    bool claim(u32 id, EntityId who, Tick until);
    void release(u32 id, EntityId who);
    void complete(u32 id);
    void cancel(u32 id) { complete(id); }
    void expire(Tick now);
    // Existing job of a type at a position (dedupe for generators).
    u32 find(JobType t, const Vec3i& pos) const;
    size_t open_count() const;

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    std::vector<Job> jobs_ = std::vector<Job>(1);
    std::vector<u32> free_;
};

}  // namespace icarus
