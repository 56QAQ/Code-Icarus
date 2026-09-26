#include "icarus/sim/clock.h"

#include "icarus/util/log.h"

namespace icarus {

const char* season_name_zh(int s) {
    static const char* n[] = {"春", "夏", "秋", "冬"};
    return n[((s % 4) + 4) % 4];
}

std::string format_time_zh(Tick t) {
    int y = year_of(t) + 1;
    int d = day_of(t) % (kDaysPerSeason * kSeasonsPerYear) % kDaysPerSeason + 1;
    float h = hour_of(t);
    int hh = (int)h;
    int mm = (int)((h - (float)hh) * 60.0f);
    return strfmt("第%d年 %s·第%d日 %02d:%02d", y, season_name_zh(season_of(t)), d, hh, mm);
}

}  // namespace icarus
