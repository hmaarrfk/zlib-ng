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

## Where it stands

The branch point is 1ab945a8. All numbers are single-thread on an Apple M5
Max with Apple clang 21. The goal set for the branch was 10x without more
cores; that was reached only on input that does not compress, and the table
below says so. The last two steps measure head and the baseline commit back
to back (the machine drifted 4-6% slower over the session, so the earlier
per-step numbers against the first baseline run are slightly pessimistic).

Paired full-tier measurement, head (step 009) vs. baseline rerun (009b).
Each cell is the speedup, then the compression ratio baseline → head (raw
deflate, whole files, `ratio.c`, step 010; corpus rows are total input bytes
over total output bytes, so large files weigh more than in the speed
geomean; a ratio change is shown only when it is at least 0.05%):

| input | L1 | L2 | L3 | L6 | L8 | L9 |
|---|---|---|---|---|---|---|
| random bytes, 1 MiB | 17x · 0.948 → 0.999 (+5.37%) | 6.8x · 1.000 → 1.000 | 7.2x · 1.000 → 1.000 | 4.8x · 1.000 → 1.000 | - · 1.000 → 1.000 | 4.0x · 1.000 → 1.000 |
| realistic_rgb (noisy image), 1 MiB | - · 1.009 → 1.006 (-0.30%) | 5.6x · 1.297 → 1.280 (-1.38%) | 5.8x · 1.298 → 1.280 (-1.39%) | 1.03x · 1.298 → 1.295 (-0.24%) | - · 1.298 → 1.298 | 1.18x · 1.375 → 1.375 |
| JPEG, 123 KiB | 28x · 0.950 → 0.999 (+5.11%) | 8.8x · 1.002 → 1.002 | 8.6x · 1.002 → 1.002 | 4.9x · 1.002 → 1.002 | 4.9x · 1.002 → 1.002 | 1.5x · 1.002 → 1.002 |
| silesia/mozilla, 51 MB | 1.8x · 2.135 → 2.137 (+0.12%) | 2.1x · 2.493 → 2.488 (-0.21%) | 2.0x · 2.574 → 2.568 (-0.22%) | 1.4x · 2.638 → 2.636 (-0.06%) | 1.5x · 2.648 → 2.647 (-0.06%) | 1.2x · 2.691 → 2.691 |
| silesia, 12 files | 1.00x · 2.248 → 2.247 | - · 2.809 → 2.807 (-0.08%) | 1.09x · 2.944 → 2.937 (-0.24%) | 1.00x · 3.068 → 3.068 | 1.10x · 3.102 → 3.101 | 1.05x · 3.128 → 3.128 |
| canterbury, 11 files | 0.99x · 2.442 → 2.442 | - · 3.444 → 3.449 (+0.17%) | 0.98x · 3.642 → 3.643 | 1.01x · 3.767 → 3.767 | 1.14x · 3.863 → 3.862 | 1.01x · 3.846 → 3.846 |
| calgary, 18 files | 0.98x · 2.071 → 2.071 | - · 2.768 → 2.773 (+0.19%) | 1.02x · 2.921 → 2.919 (-0.08%) | 1.02x · 3.026 → 3.026 | 1.07x · 3.066 → 3.066 | 1.04x · 3.063 → 3.063 |
| synthetic text, 1 MiB | 0.97x · 2.486 → 2.486 | 1.00x · 3.338 → 3.343 (+0.16%) | 1.01x · 3.557 → 3.557 | 0.99x · 3.854 → 3.854 | - · 3.962 → 3.960 | 1.02x · 3.907 → 3.907 |

Inflate 0.98-1.02x (untouched), checksums untouched, Huffman-only/RLE
strategies untouched, thread scaling unchanged (the library is single
threaded; `plots/scaling.png`). Ratio: levels 6-9 within 0.06% on every
corpus; levels 2-3 give up to 0.24% on silesia and 1.4% on the noisy RGB
image; level 1 is unchanged on compressible input and no longer expands
incompressible input by 5%. Worst single files are x-ray and geo at levels
2-3 (-1.9% / -1.2%); no other corpus file loses more than 0.25%.

Why not more on compressible input: at the default level two thirds of the
time is the hash-chain walk at one L1 load latency per candidate, the
deflateTune sweep shows chain depth is not a free lever, inlining the match
finder changed nothing, and a smaller hash table is slower. Those
experiments are recorded below with their numbers.

## Steps

### 001 register-resident loop state (kept)

The deflate_fast / deflate_medium / deflate_slow loops kept strstart,
lookahead, sym_next and the hash-table pointers in the state struct. The symbol
buffer is written with byte-typed (memcpy) stores, which the compiler must
assume alias every field of the struct, so all of those were reloaded from
memory after every symbol and the loop carried a store-to-load dependency
through memory. They now live in locals and are written back only before
calls that read the state. Output is byte-identical (difftest: identical).

Result (quick tier, geomean vs. baseline): deflate L1-3 1.11x, L6-9 1.05x,
strategies 1.04x, inflate unchanged. Largest gains on literal-heavy input:
random/realistic_rgb at level 2 1.26-1.32x, random at level 9 1.34x.
Level 6 on text barely moves because two thirds of its time is inside
longest_match, which this step does not touch. Noise floor observed between
untouched inflate benchmarks: about +-8%.

### Finding: deflateTune() parameter sweep at levels 6-9 (no code change)

Before changing the match finder, `tune.c` swept good/lazy/nice/chain over
silesia + canterbury (95 MB). Chain depth is not where the time goes: at level
6, chain 32 instead of 128 costs 0.3% ratio for 1.18x; chain 16 costs 0.7% for
1.29x. Levels 7-9 behave the same (chain 256 at level 9: 1.09x for 0.03%).
Nothing here gives a multiple, so the level table was left alone. The sweep
output is reproduced in `sweep.sh`.

### 002 literal-run acceleration (kept, changes encoded output)

Incompressible or barely compressible input (random bytes, already compressed
payloads, noisy images) paid a hash insert, a chain-head lookup and a
longest_match call per byte, most of it cache misses, and then emitted every
byte as a literal anyway. The fast, medium and slow strategies now count
consecutive positions searched without a match; after `skip_start` of them
they advance over extra positions without searching, the step growing by one
every `1 << skip_shift` misses up to 32, and the count resets on any match.
Skipped literals are tallied lazily so that the next match found can grow
backwards over them (the same trick LZ4 uses), and per level the skipped
positions are either not hashed at all (levels 2-4), hashed every other one
(5-6) or all hashed (7-9), which is what keeps the ratio at the higher levels.
Levels 3-4 also tally searched literals lazily and grow matches over them,
which recovers ratio on text at the cost of nothing. The per-level table is
`skip_table` in deflate.c.

Verification: full test suite, an AddressSanitizer build over the corpus at
every level (an earlier draft had a symbol-buffer underflow that ASan + the
difftest round trip caught), and `difftest` round trips of 11 files x 475
parameter/IO combinations with 0 failures.

Speed (tune.c, best of 2, MB/s) and ratio on silesia + canterbury, 95 MB:

| level | baseline ratio | new ratio | baseline MB/s | new MB/s |
|---|---|---|---|---|
| 2 | 2.3082 | 2.3011 (-0.31%) | 282 | 347 (1.23x) |
| 3 | 2.3891 | 2.3798 (-0.39%) | 237 | 281 (1.19x) |
| 4 | 2.4273 | 2.4179 (-0.39%) | 206 | 236 (1.15x) |
| 5 | 2.4555 | 2.4545 (-0.04%) | 203 | 208 (1.03x) |
| 6 | 2.4651 | 2.4641 (-0.04%) | 172 | 176 (1.03x) |
| 7 | 2.4830 | 2.4823 (-0.03%) | 125 | 138 (1.10x) |
| 8 | 2.4862 | 2.4857 (-0.02%) | 110 | 120 (1.09x) |
| 9 | 2.5127 | 2.5127 (+0.00%) | 82 | 87 (1.06x) |

Per file, ratio change and speedup vs. baseline (levels 2 / 6 / 9):
random bytes 0% / 0% / 0% at 10.4x / 5.8x / 4.1x; fireworks.jpg 0% at 8.8x /
4.9x / 1.5x; mozilla -0.94% / -0.23% / +0.02% at 2.2x / 1.4x / 1.2x;
x-ray -1.9% / -0.04% / 0% at 1.7x / 0.96x / 1.09x; geo -1.1% / -0.04% / 0% at
1.5x / 1.0x / 1.0x; dickens +0.25% / 0% / 0% at 1.04x / 0.99x / 1.03x.
The losses on x-ray and geo at levels 2-4 are the price of not hashing the
skipped positions; hashing them (as levels 7-9 do) halves the gain on random
data. The default level trades nothing measurable on regular data and is
3-4% slower only on x-ray, which engages the acceleration without benefiting.

Quick-tier geomeans vs. baseline: deflate L1-3 1.51x, L6-9 1.20x, strategies
1.22x, inflate/checksums unchanged. The strategy family moves because
Z_FILTERED and Z_FIXED go through the same loops.

### 003 level 1: literal-run skip and stored-block mode (kept, changes encoded output)

Level 1 (deflate_quick) codes everything with the static Huffman tree, so on
incompressible input it ran at 135-240 MB/s and *expanded* the data by 5%
(ratio 0.948 on random bytes and JPEG). It now has the same literal-run skip as
the other levels (skipped positions are still hashed, head only, which is
cheap here), and after 512 consecutive misses it probes the next stretch
(one hash lookup every 16 bytes, up to 8 KiB); when fewer than one probe in
64 finds a match the stretch is copied as a stored block and the static block
is reopened afterwards. To make that possible in one-shot Z_FINISH calls, the
final block is no longer opened eagerly: a Z_FINISH call opens an ordinary
block unless the remaining input is too small for stored mode to matter, and
the stream then ends with an empty final static block (10 bits). An earlier
draft appended a stored block while a partial flush had left data in the
pending buffer, which corrupts the stream in small-output-buffer streaming;
the difftest round trips caught it.

Level 1, per file (ratio, MB/s), baseline -> new: random 0.948 @ 134 ->
0.999 @ 2351 (17.6x); fireworks.jpg 0.950 @ 135 -> 0.999 @ 3847 (28x);
mozilla 1.5435 @ 353 -> 1.5535 @ 640 (1.8x, and smaller); x-ray 1.0758 @ 262
-> 1.0667 @ 285 (-0.85% ratio); geo -0.4%; sao/osdb/ooffice/webster/dickens
unchanged ratio at 0.96-0.98x speed (the per-literal miss counter costs about
one cycle in a loop of ten). Corpus total at level 1: 446 -> 475 MB/s, ratio
1.8593 -> 1.8590.

Measurement note: from this step on the drivers (tune.c, loop.c, scaling.c)
request a performance core via QoS; before that, runs landing on an
efficiency core produced 10-20% swings between identical binaries.

The quick-tier geomeans do not show this step because the suite only runs
levels 2-9 on the non-text data types; the "deflate L1-3" family is
unchanged at 1.50x.

### 004 stored-block mode for levels 2-9 (FAILED, rolled back in 005)

Attempt to extend the level 1 stored-block idea to the hashing strategies:
once the literal-run skip had seen 512 misses, flush the current block and
emit the next stretch as stored blocks. Five variants were tried for the
"is this stretch incompressible" decision:

1. A read-only hash probe every 16 bytes (as level 1 does). Wrong on data
   whose matches are short-distance: the probe cannot see them because the
   skipped positions were never hashed, so x-ray at level 2 was stored in
   its entirety (ratio 1.34 -> 1.00).
2. Trust the block coder: store only after the last coded block came out as
   a stored block, re-verify every 64-512 KiB. Small evidence blocks
   (1-4K symbols) are biased towards "stored" because the dynamic tree
   overhead is a larger share of a small block, so samba, whose middle
   megabyte compresses only 1.14x, lost 1.3-4%; mozilla lost 2-4%.
3. Same, but hold the skip off while the evidence block is coded so that it
   is fully hashed. Did not help: the dictionary was still missing the
   stored stretches, so the coder found no matches and said "stored".
4. Hash every 4th position of stored stretches. Recovered some, not enough.
5. Only full-size (16K symbol) blocks as evidence. Same losses, and the speed
   advantage over the plain skip shrank to little (random 1.1-1.9 GB/s vs.
   1.5-2.5 with the skip alone at levels 2-6).

Root cause: the block coder already converts incompressible blocks to stored
blocks after the fact, with the dynamic tree still winning on data whose
byte entropy is below ~7.95 bits (samba's binary middle is 7.79 bits and
compresses 1.14x). Deciding before coding cannot see that, and keeping the
dictionary consistent through stored stretches costs about as much as coding
the literals. The stretch also made difftest crawl for 40 CPU minutes on
memLevel 1 streams (512 byte pending buffer: every literal flushed a one
symbol block).

Measured anyway (quick tier, clean run): deflate L1-3 1.46x, L6-9 1.30x,
strategies 1.12x, "inflate" 0.91x. The inflate change is an artifact: the
inflate benchmark decompresses what this build's level 9 produced, which now
contains stored blocks. The ratio losses above are from tune.c, not from the
benchmark suite, which has no ratio gate. Rolled back in the next step; level
1 keeps its stored mode, which has no dynamic-tree alternative and was
measured as a gain on every file.

### 005 roll back stored mode for levels 2-9, keep stride hashing (kept)

Removes zng_deflate_try_stored and the evidence tracking from step 004 and
returns levels 2-9 to the literal-run skip of step 002. What survives from
the experiment: skipped positions are hashed with a stride (every 2nd at
levels 2-6, all at 7-9) via zng_deflate_insert_stride, and the streaming
round-trip checker rt.c. Verified: test suite, ASan at levels 1-9, streaming
round trips of mozilla at four chunk sizes, difftest 11 x 475 (0 failures).

Ratio is back where step 002 had it or better (mozilla level 6 -0.25%,
samba -0.06%, x-ray level 2 -1.0% instead of -1.9% thanks to the stride
hashing). The price of hashing every 2nd skipped position at levels 2-4 is
visible on incompressible data: random bytes at level 2 run at 450 MB/s
instead of 1.5-2 GB/s. The next step revisits that trade.

### 006 levels 2-4: do not hash skipped positions (kept)

Reverses the one part of step 005 that cost the most: at levels 2-4 the
skipped positions are not hashed again (as in step 002); levels 5-6 keep
every 2nd, 7-9 keep all. Random bytes at level 2: 1.3-1.6 GB/s (10x) instead
of 450 MB/s, JPEG 1.4 GB/s; x-ray and geo at levels 2-4 return to -1.0% /
-1.9% ratio, mozilla to -0.5 / -0.9%. Quick-tier geomeans: deflate L1-3
1.48x, L6-9 1.22x, strategies 1.21x.

Noise note: the first run of a freshly built binary on random data came out
at 720 MB/s; five repeats gave 1.30-1.57 GB/s. Single numbers on
incompressible data carry about +-10%.

### 007 full-tier checkpoint (measurement only)

Full tier (adds silesia, canterbury and calgary) of the state after step
006, the state the pull request was opened from. Geomean speedup vs. the
baseline per corpus and level:

| corpus | L1 | L3 | L6 | L8 | L9 | inflate |
|---|---|---|---|---|---|---|
| silesia | 1.00 | 1.09 | 1.00 | 1.08 | 1.06 | 1.00 |
| canterbury | 0.98 | 0.99 | 0.99 | 1.04 | 1.03 | 1.00 |
| calgary | 0.98 | 1.02 | 1.01 | 1.07 | 1.02 | 1.00 |

This is the honest number for ordinary compressible files: a few percent,
with level 1 on small text files 2% slower (the per-literal miss counter in a
ten-cycle loop). The large multiples are confined to input that does not
compress: random and realistic_rgb 1 MiB at levels 2-3 are 5.6-7.6x in this
run, and the per-file tables of steps 002/003 have the 10-28x cases.

Thread scaling (independent streams, silesia/dickens): unchanged within
noise at every thread count, as expected for a single-threaded library.
The chunkmemset microbenchmarks read 0.73-0.83x in this run although that
code is untouched; they are nanosecond-scale and move with code alignment.

### Finding: a 15-bit hash table (HASH_SIZE=32768) is slower everywhere

Tried in case the 128 KiB head table missing L1 was the bottleneck at the
higher levels. Ratio within 0.1%, speed 0-10% worse at levels 2-6 and
10-37% worse at level 9 (kennedy.xls 74 -> 47 MB/s): the extra collisions
lengthen every chain. Not committed.

### Finding: inlining longest_match into the strategies on ARM64 gains nothing

Counters compiled into match_tpl.h show the default level calls
longest_match about once per two input bytes (dickens: 3.9M calls, 21M
counted chain candidates and 1.0M compare256 calls for 8.4 MB) and that
kennedy.xls at level 8 walks 90 candidates per input byte, which is why that
file is 14x slower at level 8 than at level 6. Since NEON is part of the
ARM64 base architecture, the strategies were made to instantiate the NEON
match finder as a static function and inline it, bypassing the function
table, its indirect call and the ten callee-saved registers. Output stayed
identical and so did speed: dickens, mozilla, webster and kennedy.xls at
levels 2, 3, 6, 7 and 9 moved by -1.5% to +2.5%, all within noise. The
per-call overhead is already hidden by out-of-order execution; the cost is
the chain walk itself, one L1 load latency per candidate. Reverted.

### 008 level 8: chain limit 1024 -> 512 (kept, changes encoded output)

The counters from the previous finding showed level 8 walking 90 chain
candidates per input byte on kennedy.xls (14x slower than level 6 for 5%
better ratio). The deflateTune sweep had already shown chain 512 at level 8
costs 0.01% on the corpus for 5-6% speed. Applied to the level table:
corpus 106.7 -> 123.2 MB/s (1.15x) at ratio 2.4862 -> 2.4855 (-0.03%);
kennedy.xls 27 -> 44 MB/s, mozilla 106 -> 160 MB/s (ratio -0.09%), ptt5
145 -> 181 MB/s (-0.19%), dickens/webster +4-5% at identical ratio.

### Finding: two-literal inflate table entries gain nothing with a 10-bit root

The literal path of inflate_fast is bound by the dependency chain
hold -> table index -> table load -> shift, about six cycles per literal
(650 MB/s on pure-literal input). An entry format was added where a root
table slot whose code is a literal and whose remaining root bits hold a
complete second literal code decodes both at once (op = 128, both codes'
bits, both literals in val), with inflate(), inflateBack() and inflate_fast()
all updated. It passed the test suite, ASan and the chunked round trips, and
changed nothing: 0.98-1.02x on every inflate benchmark. Counting showed why:
with the 10-bit root table not a single root entry pairs on dickens or
x-ray at level 9, since literal codes there are 6-9 bits long. Pairs would
need a 12-bit root (4096 entries, new ENOUGH constants and 4x the table
build cost per dynamic block). Reverted.

### 009 final full-tier measurement and 009b paired baseline rerun

Step 009 is the head of the branch measured with the full tier. Measured
alone against the first baseline run it looked 3-5% slower on everything,
including untouched inflate and checksum code; the baseline binary itself,
rerun at that point, measured 0.94-0.97 of its own earlier numbers (load
average 3-8 from other processes, twelve days of uptime). So the baseline
commit was built from a worktree and measured again right after head
(009b), and `plots/paired-009-full-tier-final.png` plus the table at the top
compare the two. bench.sh gained `PERF_SOURCE_DIR` and `PERF_SKIP_TESTS` for
that, and plot.py pairs any `*-baseline-rerun` result with the step before it.

### 010 compression ratio next to every speedup (measurement only)

The benchmark suite has no ratio gate, so `ratio.c` (new, linked against the
baseline and head static libraries) compresses every whole file of the
summary table at levels 1/2/3/6/8/9 and records input and output bytes in
`results/010-ratio-baseline.txt` and `results/010-ratio-head.txt`. The
synthetic inputs are the `test/test_data_p.h` generators (text, random,
realistic_rgb) dumped to files, so they are the benchmark's data. The
combined table at the top is built from these two files. Findings: the only
ratio changes above 0.3% are levels 2-3 on the noisy RGB image (-1.4%),
x-ray (-1.9%) and geo (-1.2%), all input where the literal-run skip fires on
data that still has sparse short matches; level 1 on incompressible input
gains 5% because it now emits stored blocks.

## Decompression

The second mission on this branch is inflate. Unlike deflate, the output is
fixed byte for byte, so there is no "different valid stream" lever; the only
levers are cycles per symbol in the decoder and the overhead around it.

### 011 inflate profile (measurement only)

`patches/inflate-profile.patch` adds timers to `inflate()` (fast loop, safe
fast loop, code-length decoding, table building, window update, rest) and
symbol counters to the fast loop, both under ifdefs; `infprof.c` compresses a
file at a level, inflates it with chosen input/output chunk sizes (best of
5) and prints the split. Full output: `results/011-inflate-profile.txt`.
Level 9 raw streams unless noted.

Whole-buffer inflate, where the time goes:

| input | MB/s | fast loop | code lengths + tables | other |
|---|---|---|---|---|
| dickens | 1374 | 97.9% | 2.0% | 0.1% |
| mozilla (838 blocks) | 1121 | 96.1% | 3.8% | 0.1% |
| sao (256 blocks, 81% literals) | 711 | 94.8% | 5.2% | 0.1% |
| nci | 3965 | 98.4% | 1.5% | 0.1% |
| synthetic text | 1913 | 98.2% | 1.5% | 0.4% |

The zlib wrapper (adler32 at the end) costs 1-5%, gzip (crc32) 1-4%. Level 1
streams decode at about the same speed as level 9 streams (fewer, shorter
matches, more literals); level 1 random bytes decode at 6-18 GB/s because
the stream is stored blocks, i.e. memcpy.

Symbol mix (level 9): literals are 26% (dickens) to 81% (sao) of symbols,
average match length 3.5 (x-ray) to 34 (nci), second-level table lookups
0.05-4% of symbols, overlapping copies (dist < len) under 1.5% except on
JPEG. At 1.1-1.4 GB/s with 3.8-5 bytes per symbol the loop runs at roughly
11-16 cycles per symbol; a pure-literal stream (the `literals` benchmark
input) runs at about 7 cycles per literal, which is close to the dependent
chain table-load -> shift -> next index. The gap on mixed data is the
literal-vs-match branch, which is data dependent.

Reference point: libdeflate (commit 92e6a0d, built here, same compressed
bytes, same process, `results/011-libdeflate-reference.txt`) is 0.96x to
1.18x of zlib-ng's whole-buffer inflate on these files. So the main loop is
within 20% of the fastest known scalar decoder and that is the ceiling for
work on it.

Streaming with small output buffers is a different picture. Output chunk
size vs. throughput, level 9 raw, mozilla / dickens / text:

| output buffer | mozilla | dickens | text | where the time goes (mozilla) |
|---|---|---|---|---|
| whole | 1121 | 1374 | 1913 | fast 96% |
| 64 KiB | 1059 | 1173 | 1683 | fast 95%, window copy 1%, rest 1% |
| 4 KiB | 846 | 820 | 1219 | fast 86%, safe 8%, window 1%, rest 3% |
| 512 B | 651 | 583 | 814 | fast 34%, safe 50%, window 3%, rest 10% |
| 64 B | 365 | 306 | 393 | safe 50%, window 12%, rest 36% |

With a 4 KiB input chunking (output whole) mozilla drops to 899 MB/s: every
call ends with the last 14 input bytes decoded by the byte-at-a-time state
machine and a 32 KiB window copy.

Conclusions that set the plan: (1) the whole-buffer loop has at most 10-20%
in it; (2) small output buffers (libpng rows, HTTP streaming, pipes) lose
1.3x at 4 KiB and 3x at 64 B to the safe loop, the per-call window copy
and per-call fixed overhead, and that is where a multi-x win is possible;
(3) table building is 2-5% on files with many blocks.

### 012 in-window decoding for small output buffers (kept, output unchanged)

When the caller's buffer is small, `inflate()` now decodes into the sliding
window at the write index and copies the caller's share out, instead of
running the end-of-buffer safe loop and then copying every byte into the
window on return. This step keeps the decode bounded by the caller's
`avail_out`; only the symbol that crosses the limit (at most 257 bytes)
waits in the window for the next call, which is the amount of in-flight
output the MATCH state already allowed. Input is never consumed ahead of
that, so a call still returns only when input or output is exhausted.

The first draft read ahead by a whole window and was 2-4x faster at 64-byte
buffers, and it broke gzread: `gz_decomp` (and libpng) never call inflate
with `avail_in == 0`; they treat "input exhausted without Z_STREAM_END" as
a truncated file, and read-ahead consumed the input while output was still
pending. That draft was discarded before an adversarial review of the
bounded design found four more problems, all reproduced with probes
(`inflate-probes/`):

1. **Blocker.** The chunked copies write up to 258 bytes past the token
   they copy. In the caller's buffer that is harmless; in a circular window
   those slots hold the oldest history, and a later match with a distance
   within 258 of 32768 read the garbage. zlib-ng's own deflate caps
   distances at 32506, so no round-trip test can see this; libdeflate,
   zopfli and 7-zip emit distances up to 32768. Fixed by making the circular
   window 512 bytes larger than the 32K distance range: the clobbered slots
   are at distances no valid stream can reference. `test_inflate_window.cc`
   hand-builds such streams and fails if the gap is removed.
2. Z_OK returned when no progress was possible (zlib promises Z_BUF_ERROR).
3. `inflateMark` reported pending bytes as a stored block in progress.
4. `inflateSetDictionary` on a raw stream with pending output advanced the
   window past the pending bytes.

Also from that review: the checksum is now computed while copying out
(fused adler32_copy / crc32_copy), so `strm->adler` covers exactly
`total_out` bytes as documented. `results/012-chunk-timing.txt`, level 9,
head vs. baseline, same driver (`infprof.c`), MB/s:

| output buffer | dickens | mozilla | xml | text | kennedy.xls |
|---|---|---|---|---|---|
| 64 B | 1.44x | 1.14x | 1.12x | 1.27x | 0.98x |
| 512 B | 1.42x | 1.19x | 1.26x | 1.37x | 1.05x |
| 2 KiB | 1.10x | 1.05x | 1.08x | 1.11x | - |
| 4 KiB | 1.03x | 1.01x | 1.02x | 1.04x | 0.99x |
| 16 KiB (in-window) | 0.96x | 0.97x | 0.95x | 0.94x | 1.00x |
| 64 KiB, whole (direct) | 1.00x | 1.00x | 1.00x | 1.01x | 1.00x |

Bounded decoding helps only below 4 KiB, because the per-call fixed cost
and the copy are still paid per call; the threshold was set to 4 KiB
(`results/012-threshold.txt`).

### 013 read-ahead into the window (kept, output unchanged)

The fast loop now runs to the window's end instead of stopping at the
caller's buffer size; the surplus waits in the window and is delivered by
the following calls, so a consumer reading 64-byte or 4 KiB pieces gets
the full-buffer fast loop and pays the per-call cost on a copy only. What
makes this compatible with gzread and libpng: the fast loop holds one input
byte back while it runs ahead, so pending output never coincides with
`avail_in == 0`, and pending output always comes with `avail_out == 0`.
The byte-at-a-time path never runs ahead, so Z_FINISH one-shot calls with
an exact buffer, Z_BLOCK and Z_TREES are unchanged. The visible semantic
change is that `total_in` and the window lead `total_out` by up to a window
of output while bytes are pending; `test_inflate_window.cc` models the
gzread pattern (never call inflate with `avail_in == 0`) at 1 to 4095 byte
buffers. The threshold moved to 24 KiB after `results/013-threshold.txt`
showed in-window read-ahead ahead of direct decoding up to 24 KiB and
behind it from 32 KiB.

`results/013-chunk-timing.txt`, level 9, head vs. baseline, MB/s:

| output buffer | dickens | mozilla | xml | x-ray | kennedy.xls | text | noisy rgb |
|---|---|---|---|---|---|---|---|
| 64 B | 2.09x | 1.61x | 1.96x | 1.28x | 1.53x | 2.15x | 1.22x |
| 512 B | 1.54x | 1.36x | 1.51x | 1.12x | 1.18x | 1.59x | 1.10x |
| 2 KiB | 1.18x | 1.17x | 1.24x | 1.06x | 1.06x | 1.23x | 1.04x |
| 4 KiB | 1.14x | 1.13x | 1.18x | - | - | 1.26x | - |
| 8 KiB | 1.15x | 1.10x | 1.14x | - | - | 1.17x | - |
| 16 KiB | 1.13x | 1.04x | 1.11x | 1.06x | - | 1.16x | - |
| 64 KiB, whole (direct) | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |

gzip-wrapped streams gain the same (the checksum is fused into the copy).
The remaining gap to whole-buffer speed at 64 bytes (dickens 879 vs 1376
MB/s) is the per-call entry and exit of `inflate()` plus a 64-byte copy.
