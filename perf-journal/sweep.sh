#!/usr/bin/env bash
# Sweep deflateTune() parameters per level over silesia + canterbury with perf-journal/tune.c.
# Usage: perf-journal/sweep.sh <path-to-tune-binary> [grid|defaults]
set -euo pipefail
TUNE="$1"; MODE="${2:-grid}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILES=("$ROOT"/test/data/corpora/silesia/* "$ROOT"/test/data/corpora/canterbury/*)
for l in 1 2 3 4 5 6 7 8 9; do "$TUNE" "$l" default "${FILES[@]}"; done
[ "$MODE" = defaults ] && exit 0
echo "--- level 6 grid (good/lazy/nice/chain)"
for lazy in 8 16 32; do for nice in 32 64 128; do for chain in 16 32 48 64 128; do
    "$TUNE" 6 8 "$lazy" "$nice" "$chain" "${FILES[@]}"; done; done; done
echo "--- level 9 grid"
for chain in 256 512 1024 2048 4096; do "$TUNE" 9 32 258 258 "$chain" "${FILES[@]}"; done
echo "--- level 8 grid"
for chain in 128 256 512 1024; do "$TUNE" 8 32 128 258 "$chain" "${FILES[@]}"; done
echo "--- level 7 grid"
for chain in 64 128 256; do "$TUNE" 7 8 32 128 "$chain" "${FILES[@]}"; done
