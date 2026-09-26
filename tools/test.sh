#!/usr/bin/env bash
# Runs all automated checks: kernel unit/integration tests + Godot headless smoke test.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
./build/tests/icarus_tests "$@"
if command -v godot >/dev/null 2>&1 && [ -f game/bin/libicarus.linux.template_debug.x86_64.so ]; then
    [ -d game/.godot ] || timeout 300 godot --headless --path game --import >/dev/null 2>&1 || true
    timeout 120 godot --headless --path game --script res://tests/smoke.gd
fi
