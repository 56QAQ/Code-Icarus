// World clock: one tick = one simulation step. 20 ticks per real second at 1x speed.
#pragma once

#include <string>

#include "icarus/util/types.h"

namespace icarus {

constexpr Tick kTicksPerSecond = 20;             // at 1x speed
constexpr Tick kTicksPerDay = 6000;              // 5 real minutes at 1x
constexpr Tick kTicksPerHour = kTicksPerDay / 24;  // 250
constexpr int kDaysPerSeason = 8;
constexpr int kSeasonsPerYear = 4;

inline int day_of(Tick t) { return (int)(t / kTicksPerDay); }
inline float hour_of(Tick t) { return (float)(t % kTicksPerDay) / (float)kTicksPerHour; }
inline bool is_night(Tick t) {
    float h = hour_of(t);
    return h < 5.5f || h >= 21.0f;
}
inline int season_of(Tick t) { return (day_of(t) / kDaysPerSeason) % kSeasonsPerYear; }
inline int year_of(Tick t) { return day_of(t) / (kDaysPerSeason * kSeasonsPerYear); }
const char* season_name_zh(int s);
std::string format_time_zh(Tick t);

}  // namespace icarus
