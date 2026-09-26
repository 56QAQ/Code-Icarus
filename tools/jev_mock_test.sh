#!/usr/bin/env bash
# Runs the Jev remote client end-to-end against the local mock API server.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
python3 tools/jev_mock_server.py 8765 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
sleep 0.5
ICARUS_JEV_URL="http://127.0.0.1:8765/v1/messages" HOME="$(mktemp -d)" \
    timeout 180 godot --headless --path game --script res://tests/jev_mock_test.gd 2>&1 | grep -E "JEV_MOCK|decision|ERROR" || true
