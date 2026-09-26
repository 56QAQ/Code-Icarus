// Society: polities, their statistics, crises, projects and political dynamics.
#pragma once

#include <string>
#include <vector>

#include "icarus/economy/economy.h"
#include "icarus/sim/context.h"
#include "icarus/society/polity.h"

namespace icarus {

class Society {
public:
    explicit Society(SimContext& ctx);

    void reset(u64 seed);
    void step(Tick now);

    u16 create_polity(const std::string& name, u32 color, u16 parent = 0);
    Polity* polity(u16 id) { return (id > 0 && id < polities_.size() && polities_[id].alive) ? &polities_[id] : nullptr; }
    const Polity* polity(u16 id) const {
        return (id > 0 && id < polities_.size() && polities_[id].alive) ? &polities_[id] : nullptr;
    }
    const std::vector<Polity>& polities() const { return polities_; }
    std::string title(u16 id) const;  // "美食的文明，拉米娅"
    void set_ruler(u16 id, EntityId girl, const std::string& how, EventId cause);

    // Public stores of a polity (stockpiles and workshops).
    std::vector<StoreId> public_stores(u16 polity) const;
    float public_food(u16 polity) const;  // nutrition units

    u32 add_project(Project p);
    Project* project(u32 id) { return (id > 0 && id < projects_.size() && projects_[id].alive) ? &projects_[id] : nullptr; }
    const std::vector<Project>& projects() const { return projects_; }
    void finish_project(u32 id, bool success, EventId cause);

    void compute_stats(Polity& p);
    // Technology.
    const Json* tech(const std::string& key) const;
    bool tech_available(const Polity& p, const std::string& key) const;  // requirements met, not known
    std::vector<std::string> available_techs(const Polity& p) const;
    float tech_effect(u16 polity, const std::string& effect) const;
    int era(const Polity& p) const;
    // Adds research points to the current target; discovers it when complete.
    void add_research(u16 polity, float points, EntityId by);
    void discover(Polity& p, const std::string& key, EntityId by, EventId cause);
    // War and peace.
    EventId declare_war(u16 attacker, u16 defender, const std::string& aim, EntityId by, EventId cause);
    EventId make_peace(u16 a, u16 b, const std::string& how, EventId cause);
    bool at_war(u16 a, u16 b) const;
    // Drafts up to n residents (strongest, most combative first); returns how many serve.
    int draft(u16 polity, int n, EventId cause);
    void discharge(u16 polity);
    int soldiers(u16 polity) const;
    // The winner absorbs the loser: people, fields, buildings and stores change hands.
    void annex(u16 winner, u16 loser, EventId cause, const std::string& how = "");
    void refresh_passives(Polity& p);
    // Convenience for systems that only know a polity id.
    float passive(u16 polity, const std::string& effect) const {
        const Polity* p = this->polity(polity);
        return p ? p->passive(effect) : 0.0f;
    }

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

    Rng& rng() { return rng_; }

private:
    void hourly(Tick now);
    void daily(Tick now);
    void update_crises(Polity& p);
    void update_support(Polity& p);
    void update_projects();
    void update_wars(Polity& p);

    SimContext& ctx_;
    Rng rng_;
    std::vector<Polity> polities_ = std::vector<Polity>(1);
    std::vector<Project> projects_ = std::vector<Project>(1);
};

}  // namespace icarus
