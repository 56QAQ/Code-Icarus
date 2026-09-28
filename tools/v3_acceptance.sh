#!/usr/bin/env bash
# Third-version acceptance (场景三): four peoples on a random island, barbarian start,
# many seeds, long runs; the island's civilisation index should trend upward.
#   tools/v3_acceptance.sh [OUT_DIR] [SEEDS] [DAYS]      e.g. tools/v3_acceptance.sh out/v3 1-16 64
# Runs one `icarus_cli trend` per seed (as many at once as there are cores), then sums
# the runs up with tools/v3_acceptance.py.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-out/v3_acceptance}"
SEEDS="${2:-1-16}"
DAYS="${3:-64}"
CLI=build/tools/cli/icarus_cli
[ -x "$CLI" ] || { echo "build first: tools/build.sh" >&2; exit 1; }
mkdir -p "$OUT"
seeds() { if [[ "$1" == *-* ]]; then seq "${1%-*}" "${1#*-}"; else tr ',' '\n' <<<"$1"; fi; }
seeds "$SEEDS" | xargs -P "$(nproc)" -I{} sh -c \
    "$CLI trend --layout random --civs 4 --scenario wild --era wild --seed {} --days $DAYS > $OUT/seed_{}.txt"
python3 tools/v3_acceptance.py "$OUT"
