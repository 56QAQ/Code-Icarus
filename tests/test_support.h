#pragma once

#include <string>

#include "icarus/data/registry.h"

#ifndef ICARUS_DATA_DIR
#define ICARUS_DATA_DIR "game/data"
#endif

inline const icarus::Registry& test_registry() {
    static icarus::Registry reg = [] {
        icarus::Registry r;
        r.load_from_dir(ICARUS_DATA_DIR);
        return r;
    }();
    return reg;
}
