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
    HOME="$(mktemp -d)" timeout 180 godot --headless --path game --script res://tests/save_test.gd 2>&1 | tee /dev/stderr | grep -q "SAVE PASS"
    # The interface: every overlay of the real main scene, failing on any script error.
    UI_OUT="$(HOME="$(mktemp -d)" timeout 300 godot --headless --path game --script res://tests/ui_test.gd 2>&1 || true)"
    echo "$UI_OUT" | grep -E "UI |SCRIPT ERROR" >&2 || true
    echo "$UI_OUT" | grep -q "UI PASS" && ! echo "$UI_OUT" | grep -q "SCRIPT ERROR"
    tools/jev_mock_test.sh | tee /dev/stderr | grep -q "JEV_MOCK PASS"
fi
