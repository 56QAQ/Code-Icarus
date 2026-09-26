#!/usr/bin/env bash
# Builds everything: kernel + tests + CLI (CMake) and the GDExtension (SCons).
#   tools/build.sh            debug extension + RelWithDebInfo kernel
#   tools/build.sh release    also builds the release extension
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build
(cd gdext && scons platform=linux target=template_debug -j"$(nproc)")
if [ "${1:-}" = "release" ]; then
    (cd gdext && scons platform=linux target=template_release -j"$(nproc)")
fi
