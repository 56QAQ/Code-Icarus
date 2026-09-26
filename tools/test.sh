#!/usr/bin/env bash
# Runs all automated checks: kernel unit/integration tests, the Godot headless smoke
# test and the Jev remote-client test against a local mock of the Messages API.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
./build/tests/icarus_tests "$@"
if command -v godot >/dev/null 2>&1 && [ -f game/bin/libicarus.linux.template_debug.x86_64.so ]; then
    [ -d game/.godot ] || timeout 300 godot --headless --path game --import >/dev/null 2>&1 || true
    timeout 120 godot --headless --path game --script res://tests/smoke.gd 2>&1 | grep -E "SMOKE|polity|kernel|ERROR"
    tools/jev_mock_test.sh | tee /dev/stderr | grep -q "JEV_MOCK PASS"
fi
