#!/usr/bin/env bash
# Renders a real frame of the game (Forward+ via lavapipe under Xvfb) and saves a PNG.
#   tools/screenshot.sh out/shot.png [extra args passed to the game after --]
# Example: tools/screenshot.sh out/a.png --cam 520,165,560,35,45,120 --ticks 200
# The window size is 1600x900 unless RES is set (e.g. RES=3840x2160 for a 4K frame).
set -euo pipefail
RES="${RES:-1600x900}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(realpath -m "$1")"; shift
mkdir -p "$(dirname "$OUT")"
cd "$ROOT"
[ -d game/.godot ] || timeout 300 godot --headless --path game --import >/dev/null 2>&1 || true
timeout 300 xvfb-run -a -s "-screen 0 ${RES}x24" \
    godot --path game --rendering-driver vulkan --resolution "$RES" -- --shot "$OUT" "$@" 2>&1 \
    | grep -vE "^\s*$" | grep -vE "Vulkan|vulkan|llvmpipe|lavapipe|RenderingDevice|Godot Engine|OpenGL|^WARNING: .*DPI" || true
[ -f "$OUT" ] && echo "ok: $OUT"
