# Performance journal

Every step in this branch is recorded here with a measurement commit. The goal
set for the branch was a large single-core speedup of zlib-ng with the public
API/ABI and the DEFLATE format unchanged. Failures and rollbacks are recorded
the same way as successes.

How a step is measured:

* `bench.sh <step> [quick|full]` rebuilds the tree in Release mode, runs the
  full test suite as a gate, runs the Google Benchmark subset for the tier,
  runs `scaling.c` (independent streams on 1, 2, 4 and all cores) and writes
  `results/<step>.json`, then regenerates every plot in `plots/`.
* `difftest.c` compresses a corpus through a matrix of levels, strategies,
  window sizes, memory levels and chunked I/O patterns, prints a CRC of each
  stream and round-trips it through several inflate chunkings. Built against
  the baseline and the candidate library, its output shows whether a step
  changed the encoded bytes.
* Numbers are medians of 3 repetitions with ASLR disabled, on one machine
  (Apple M5 Max, Apple clang 21). Only ratios between steps are meaningful.

Plots:

* `plots/timeline.png`: geometric-mean speedup vs. the baseline, per family, per step.
* `plots/latest-vs-baseline.png`: per-benchmark speedup of the newest step.
* `plots/step-<name>.png`: per-benchmark speedup of that step vs. baseline and vs. the previous step.
* `plots/scaling.png`: aggregate throughput of N independent streams on N threads.

## Steps

