// The Jev pipeline: strategic decisions of magical girls acting as officials.
//
//   what she can know  ->  program builds feasible options with computed facts
//   ->  a decision provider (local persona model, remote LLM via the host, or a replay
//       log) picks one with a rationale  ->  local executor applies it  ->  outcome is
//       reviewed later and remembered.
//
// The kernel never talks to the network. Requests in "remote" mode wait for the host
// to submit an answer before a deadline (in ticks); otherwise the local model decides.
// Every decision is stored, so a run can be replayed with the same choices.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "icarus/sim/chronicle.h"
#include "icarus/sim/context.h"
#include "icarus/society/polity.h"
#include "icarus/util/json.h"
#include "icarus/util/rng.h"

namespace icarus {

struct Character;

enum Feature : int {
    kFoodSecurity = 0, kWelfare, kOrder, kHarshness, kCooperation, kSelfPower, kGrowth, kFrugality, kMilitary, kRisk,
    kFairness, kSpeed, kFeatureCount
};
const char* feature_key(int f);
const char* feature_name_zh(int f);

struct DecisionOption {
    std::string key;
    std::string title;
    std::string desc;                 // program-computed description with numbers
    float f[kFeatureCount] = {0};     // how the option serves each value (-1..1)
    float bias = 0;                   // state-dependent pull for the local model (e.g. loyalty)
    Json facts;
    bool feasible = true;
    std::string why_not;
    Json action;
};

enum class DecisionStatus : u8 { Pending = 0, AwaitingRemote, Decided, Executed, Cancelled };

// Another magical girl's recommendation on a decision she does not own.
struct Proposal {
    EntityId girl = kNoEntity;
    std::string key;
    std::string reason;
    bool adopted = false;
};

struct Decision {
    u32 id = 0;
    Tick created = 0, deadline = 0, answered = 0;
    EntityId girl = kNoEntity;
    u16 polity = 0;
    std::string kind;       // crisis / governance / stance / petition
    std::string topic;      // human readable topic
    CrisisKind crisis = CrisisKind::None;
    EventId cause = 0;
    EventId request_event = 0;
    EventId decision_event = 0;
    std::string situation;  // what she knows (zh)
    std::vector<DecisionOption> options;
    std::vector<float> local_scores;
    u64 context = 0;        // state signature for staleness checks
    DecisionStatus status = DecisionStatus::Pending;
    int chosen = -1;
    std::string rationale;
    std::string source;     // local / remote / replay / fallback
    std::string note;       // e.g. why a remote answer was rejected
    Tick review_at = 0;
    float baseline = 0;
    std::string outcome;
    u32 project = 0;
    EntityId petitioner = kNoEntity;
    Json petition;          // requested policy change (for petitions)
    std::vector<Proposal> proposals;
};

class Decisions {
public:
    explicit Decisions(SimContext& ctx);
    void reset(u64 seed);
    void step(Tick now);

    // Provider configuration.
    std::string mode = "local";          // local | remote | replay
    int remote_budget_per_day = 24;
    Tick remote_deadline = 900;          // ticks to wait for a remote answer
    int remote_used_today = 0;

    // Host API for remote decisions.
    std::vector<u32> awaiting_remote() const;
    std::string remote_system_prompt(u32 id) const;
    std::string remote_user_prompt(u32 id) const;
    Json remote_schema(u32 id) const;
    bool submit(u32 id, const std::string& option_key, const std::string& rationale, const std::string& source,
                std::string& err);
    void remote_failed(u32 id, const std::string& why);

    // Replay.
    void load_replay(const Json& log);
    Json export_log() const;

    const std::vector<Decision>& all() const { return list_; }
    const Decision* get(u32 id) const { return (id > 0 && id < list_.size()) ? &list_[id] : nullptr; }

    // Admin influence: a whisper nudges a girl's next decision toward an option key.
    void whisper(EntityId girl, const std::string& option_key, EventId cause);

    void save(BinWriter& w) const;
    void load(BinReader& r);
    u64 hash() const;

private:
    void hourly(Tick now);
    void girls_politics(Polity& p);
    void consider(Polity& p);
    u32 open(Decision d);
    void build_crisis_options(Decision& d, Polity& p, const Crisis& c, Character& girl);
    void build_governance_options(Decision& d, Polity& p, Character& girl);
    void build_stance_options(Decision& d, Polity& p, Character& girl);
    void build_petition_options(Decision& d, Polity& p, Character& ruler);
    void build_research_options(Decision& d, Polity& p, Character& ruler);
    // Foreign policy and war (decision_war.cpp).
    void build_diplomacy_options(Decision& d, Polity& p, Polity& other);
    void build_war_options(Decision& d, Polity& p, War& w);
    void build_defense_options(Decision& d, Polity& p, const Crisis& c);
    void build_peace_options(Decision& d, Polity& p, u16 from);
    bool execute_war(Decision& d, const DecisionOption& o, Polity& p, Character& g);
    void consider_foreign(Polity& p, Character& ruler);
    std::string describe_situation(const Polity& p, const Character& girl, const std::string& focus) const;
    u64 context_of(const Decision& d) const;
    std::vector<float> weights(const Character& g) const;
    void decide_local(Decision& d, const std::string& source);
    void finalize(Decision& d, int idx, const std::string& rationale, const std::string& source);
    void execute(Decision& d);
    void review(Decision& d);
    float metric_for(const Decision& d) const;
    void gather_proposals(Decision& d, Polity& p);
    // Residents judge a ruler's decision (and the alternatives other girls proposed)
    // by their own personalities; support shifts accordingly.
    void resident_reaction(const Decision& d);
    int best_local(const Decision& d, const Character& g, std::string* why);
    int decisions_today(EntityId girl) const;

    // Stateless noise so that local, remote and replayed decisions consume no shared
    // random stream (a replay reproduces the original run exactly).
    float jitter(u64 a, u64 b, u64 c) const;

    SimContext& ctx_;
    Rng rng_;
    u64 noise_seed_ = 0;
    std::vector<Decision> list_ = std::vector<Decision>(1);
    struct ReplayEntry {
        std::string choice, rationale;
        Tick answered = 0;
    };
    std::map<u32, ReplayEntry> replay_;
    std::map<EntityId, std::pair<std::string, Tick>> whispers_;
    Tick now_ = 0;
};

}  // namespace icarus
