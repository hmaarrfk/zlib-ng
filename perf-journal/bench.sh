#!/usr/bin/env bash
# perf-journal/bench.sh -- build the current tree and record one measurement.
#
# Usage: perf-journal/bench.sh <step-name> [quick|full]
#
# Machine speed drifts over a long session (the baseline binary itself was
# measured 4-6% slower after a few hours), so a step named
# <step>-baseline-rerun, produced from a worktree of the baseline commit via
# PERF_SOURCE_DIR, pairs with the step before it in the plots.
#
# Writes perf-journal/results/<step-name>.json containing the Google Benchmark
# JSON output for the chosen tier plus the thread-scaling results from
# perf-journal/scaling.c, and regenerates the plots.
#
# Every number in the journal comes from this script, on this machine, with the
# same compiler, so steps are comparable with each other but not with other
# machines.
set -euo pipefail

STEP="${1:?step name}"
TIER="${2:-quick}"
JOURNAL="$(cd "$(dirname "$0")" && pwd)"
# The tree to build and measure; defaults to the tree this script lives in.
# PERF_SOURCE_DIR=<worktree of another commit> measures that commit instead,
# writing into this journal (used for paired baseline reruns).
ROOT="${PERF_SOURCE_DIR:-$(cd "$JOURNAL/.." && pwd)}"
BUILD="${PERF_BUILD_DIR:-$JOURNAL/build}"
CC_BIN="${CC:-/usr/bin/cc}"
CXX_BIN="${CXX:-/usr/bin/c++}"
REPS="${PERF_REPS:-3}"
MIN_TIME="${PERF_MIN_TIME:-0.15s}"

mkdir -p "$JOURNAL/results" "$JOURNAL/plots"

# 1. Build (Release, static, with gtest + benchmarks)
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_C_COMPILER="$CC_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=ON -DWITH_BENCHMARKS=ON -DWITH_GTEST=ON \
    -DZLIB_COMPAT=OFF >/dev/null
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

# 2. Correctness gate: the whole gtest + ctest suite must pass before we measure
#    (PERF_SKIP_TESTS=1 skips it, for reruns of an already verified commit)
if [ -z "${PERF_SKIP_TESTS:-}" ]; then
    (cd "$BUILD" && ctest -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" -E benchmark --output-on-failure >"$BUILD/ctest.log" 2>&1) \
        || { echo "TESTS FAILED, see $BUILD/ctest.log"; exit 1; }
fi

# 3. Benchmark filter per tier
QUICK_FILTER='^(deflate_bench/(level|nocrc)/(text|logfile|mixed|realistic_rgb|random|dna|short_match)/1048576/'
QUICK_FILTER+='|deflate_bench/(huffman|rle|fixed|filtered)/(text|realistic_rgb|logfile)/1048576/'
QUICK_FILTER+='|deflate_bench/level/text/(1024|16384|131072)/'
QUICK_FILTER+='|deflate_bench/sync_flush/text/'
QUICK_FILTER+='|inflate_bench/nocrc/.*/(16384|1048576)$'
QUICK_FILTER+='|inflate_chunked/'
QUICK_FILTER+='|compress_bench/|uncompress_bench/'
QUICK_FILTER+='|crc32/armv8_pmull_eor3/|adler32/neon_dotprod/'
QUICK_FILTER+='|longest_match|insert_batch_bench|compare256/|slide_hash/|chunkmemset/)'
FULL_FILTER="$QUICK_FILTER"
FULL_FILTER="${FULL_FILTER%)}|deflate_corpora/(silesia|canterbury|calgary)/|inflate_corpora/(silesia|canterbury|calgary)/)"

if [ "$TIER" = full ]; then FILTER="$FULL_FILTER"; else FILTER="$QUICK_FILTER"; fi

GB_JSON="$BUILD/gbench-$STEP.json"
"$BUILD/test/benchmarks/benchmark_zlib" \
    --benchmark_data_types=all \
    --benchmark_filter="$FILTER" \
    --benchmark_min_time="$MIN_TIME" \
    --benchmark_repetitions="$REPS" \
    --benchmark_report_aggregates_only=true \
    --benchmark_out_format=json --benchmark_out="$GB_JSON" \
    --benchmark_no_aslr >/dev/null

# 4. Thread scaling: same work on 1, 2, 4 and all cores
"$CC_BIN" -O2 -std=c11 -I"$ROOT" -I"$BUILD" -o "$BUILD/scaling" "$JOURNAL/scaling.c" "$BUILD/libz-ng.a" -lpthread
SCALING_JSON="$BUILD/scaling-$STEP.json"
"$BUILD/scaling" "$ROOT/test/data/corpora/silesia/dickens" > "$SCALING_JSON"

# 5. Merge into one result file with provenance
python3 - "$STEP" "$TIER" "$GB_JSON" "$SCALING_JSON" "$JOURNAL/results/$STEP.json" <<'PY'
import json, sys, os, subprocess, platform, datetime
step, tier, gb, sc, out = sys.argv[1:]
gbj = json.load(open(gb))
scj = json.load(open(sc))
sha = subprocess.run(["git", "-C", os.environ.get("PERF_SOURCE_DIR", "."), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
res = {
    "step": step,
    "tier": tier,
    "commit": sha,
    "date": datetime.datetime.now().isoformat(timespec="seconds"),
    "machine": platform.platform() + " " + platform.machine(),
    "context": gbj.get("context", {}),
    "benchmarks": [b for b in gbj["benchmarks"] if b.get("aggregate_name") == "median"],
    "scaling": scj,
}
json.dump(res, open(out, "w"), indent=1)
print(f"wrote {out}: {len(res['benchmarks'])} medians, scaling {len(scj)} rows")
PY

# 6. Regenerate plots
python3 "$JOURNAL/plot.py"
