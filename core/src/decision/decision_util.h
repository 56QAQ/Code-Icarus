// Internal helpers shared by the decision option builders.
#pragma once

#include <initializer_list>
#include <string>

#include "icarus/decision/decisions.h"

namespace icarus {
namespace decision_util {

struct F {
    int f;
    float v;
};

inline DecisionOption make(const std::string& key, const std::string& title, const std::string& desc,
                    std::initializer_list<F> feats, Json action) {
    DecisionOption o;
    o.key = key;
    o.title = title;
    o.desc = desc;
    for (const F& x : feats) o.f[x.f] = x.v;
    o.action = std::move(action);
    return o;
}

inline Json act(const std::string& what) {
    Json j = Json::object();
    j.set("do", what);
    return j;
}

}  // namespace decision_util
}  // namespace icarus
