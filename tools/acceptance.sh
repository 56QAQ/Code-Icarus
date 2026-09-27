#!/usr/bin/env bash
# The v2 acceptance experiments: the three realms (8 seeds) and the wild start (6 seeds),
# 32 game days each on the big island, then a summary table (tools/acceptance.py).
#   tools/acceptance.sh [out_dir] [jobs]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/out/acceptance}"
JOBS="${2:-3}"
CLI="$ROOT/build/tools/cli/icarus_cli"
mkdir -p "$OUT"
runs=()
for s in 1 2 3 4 5 6 7 8; do runs+=("three_realms $s"); done
for s in 1 2 3 4 5 6; do runs+=("wild $s"); done
printf '%s\n' "${runs[@]}" | xargs -P "$JOBS" -L 1 bash -c \
  '"$0" run --data "'"$ROOT"'/game/data" --layout continent --scenario "$1" --seed "$2" --days 32 --every 96 --events > "'"$OUT"'/$1_$2.txt" 2>&1' "$CLI"
python3 "$ROOT/tools/acceptance.py" "$OUT"
