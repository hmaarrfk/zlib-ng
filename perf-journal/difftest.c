/* difftest.c -- differential test for the perf journal.
 *
 * For every input file and a matrix of (level, strategy, windowBits, memLevel,
 * I/O pattern) this compresses the file, prints a CRC-32 of the compressed
 * stream plus its length, and then inflates it back through several
 * input/output chunk sizes and verifies the round trip.
 *
 * Built once against the baseline libz-ng.a and once against the modified one,
 * the two outputs can be diffed: identical lines mean byte-identical streams.
 * A step that intentionally changes the encoded output shows up as a diff and
 * is then judged on ratio and round trip only.
 *
 * Usage: difftest [-q] file...      (-q: only print a summary line per file)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "zlib-ng.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 3 << 20) n = 3 << 20;   /* cap at 3 MiB per file to keep runtime sane */
    uint8_t *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { perror("read"); exit(2); }
    fclose(f);
    *len = n;
    return b;
}

static const int levels[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
static const int strategies[] = {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED};
static const int wbits_list[] = {15, -15, 31, 9};
static const int memlevels[] = {8, 1};
/* I/O patterns: {in_chunk, out_chunk, flush_every}; 0 = whole buffer / no periodic flush */
static const struct { size_t in, out; size_t flush_every; int flush; } patterns[] = {
    {0, 0, 0, Z_NO_FLUSH},
    {4096, 1000, 0, Z_NO_FLUSH},
    {777, 0, 777, Z_SYNC_FLUSH},
    {65536, 65536, 65536, Z_FULL_FLUSH},
    {300, 7, 1500, Z_PARTIAL_FLUSH},
};

static int quiet = 0;
static unsigned long failures = 0;

/* Compress src with the given parameters following a chunked I/O pattern. */
static size_t do_deflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap,
                         int level, int strategy, int wbits, int memlevel, int pi) {
    zng_stream s; memset(&s, 0, sizeof(s));
    if (zng_deflateInit2(&s, level, Z_DEFLATED, wbits, memlevel, strategy) != Z_OK) { printf("deflateInit2 failed\n"); exit(3); }
    size_t in_chunk = patterns[pi].in ? patterns[pi].in : len;
    size_t out_chunk = patterns[pi].out ? patterns[pi].out : cap;
    size_t fe = patterns[pi].flush_every;
    size_t pos = 0, outpos = 0, since_flush = 0;
    int rc;
    for (;;) {
        size_t take = len - pos < in_chunk ? len - pos : in_chunk;
        if (fe && take > fe - since_flush) take = fe - since_flush;
        s.next_in = src + pos; s.avail_in = (uint32_t)take; pos += take; since_flush += take;
        int flush = Z_NO_FLUSH;
        if (pos == len) flush = Z_FINISH;
        else if (fe && since_flush == fe) { flush = patterns[pi].flush; since_flush = 0; }
        do {
            size_t room = cap - outpos < out_chunk ? cap - outpos : out_chunk;
            s.next_out = dst + outpos; s.avail_out = (uint32_t)room;
            rc = zng_deflate(&s, flush);
            outpos = s.total_out;
            if (rc == Z_STREAM_ERROR) { printf("deflate stream error\n"); exit(3); }
        } while (s.avail_out == 0 && rc != Z_STREAM_END);
        if (rc == Z_STREAM_END) break;
        if (pos == len && flush == Z_FINISH && rc != Z_OK) { printf("deflate rc=%d\n", rc); exit(3); }
    }
    zng_deflateEnd(&s);
    return outpos;
}

/* Inflate comp back with a chunk pattern and compare to src. */
static int do_inflate(const uint8_t *comp, size_t clen, const uint8_t *src, size_t len, int wbits,
                      size_t in_chunk, size_t out_chunk, uint8_t *tmp) {
    zng_stream s; memset(&s, 0, sizeof(s));
    if (zng_inflateInit2(&s, wbits > 0 && wbits < 16 ? 15 : wbits == 31 ? 47 : wbits) != Z_OK) return -1;
    size_t pos = 0, outpos = 0;
    int rc;
    if (!in_chunk) in_chunk = clen + 1;
    if (!out_chunk) out_chunk = len + 1;
    for (;;) {
        size_t take = clen - pos < in_chunk ? clen - pos : in_chunk;
        s.next_in = comp + pos; s.avail_in = (uint32_t)take; pos += take;
        do {
            size_t room = len + 1 - outpos < out_chunk ? len + 1 - outpos : out_chunk;
            s.next_out = tmp + outpos; s.avail_out = (uint32_t)room;
            rc = zng_inflate(&s, Z_NO_FLUSH);
            outpos = s.total_out;
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) { zng_inflateEnd(&s); return rc; }
        } while (s.avail_out == 0 && rc != Z_STREAM_END);
        if (rc == Z_STREAM_END) break;
        if (pos >= clen && s.avail_in == 0 && rc == Z_BUF_ERROR) { zng_inflateEnd(&s); return -2; }
    }
    zng_inflateEnd(&s);
    if (outpos != len || memcmp(tmp, src, len) != 0) return -3;
    return 0;
}

int main(int argc, char **argv) {
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-q") == 0) { quiet = 1; argi++; }
    for (; argi < argc; argi++) {
        size_t len;
        uint8_t *src = read_file(argv[argi], &len);
        size_t cap = zng_deflateBound(NULL, len) + len / 100 + 4096 + (len / 300 + 1) * 16;
        uint8_t *dst = malloc(cap), *tmp = malloc(len + 1);
        uint32_t filehash = 0; unsigned long total_out = 0; unsigned streams = 0;
        const char *base = strrchr(argv[argi], '/'); base = base ? base + 1 : argv[argi];
        for (unsigned li = 0; li < sizeof(levels)/sizeof(*levels); li++)
        for (unsigned si = 0; si < sizeof(strategies)/sizeof(*strategies); si++)
        for (unsigned wi = 0; wi < sizeof(wbits_list)/sizeof(*wbits_list); wi++)
        for (unsigned mi = 0; mi < sizeof(memlevels)/sizeof(*memlevels); mi++)
        for (unsigned pi = 0; pi < sizeof(patterns)/sizeof(*patterns); pi++) {
            /* Trim the matrix: non-default strategies and memLevel 1 only on a subset */
            if (si > 0 && (wi > 1 || mi > 0 || (pi & 1))) continue;
            if (mi > 0 && (wi > 1 || pi > 2)) continue;
            if (wi == 3 && (levels[li] & 1)) continue;
            size_t clen = do_deflate(src, len, dst, cap, levels[li], strategies[si], wbits_list[wi], memlevels[mi], pi);
            uint32_t h = zng_crc32(0, dst, (uint32_t)clen);
            if (!quiet)
                printf("%s L%d S%d W%d M%d P%u %08x %zu\n", base, levels[li], strategies[si], wbits_list[wi], memlevels[mi], pi, h, clen);
            filehash = zng_crc32(filehash, (const uint8_t *)&h, 4);
            total_out += clen; streams++;
            /* round trip through a few inflate chunkings */
            static const size_t ic[] = {0, 1, 13, 4096}, oc[] = {0, 1, 100, 65536};
            for (int k = 0; k < 4; k++) {
                int r = do_inflate(dst, clen, src, len, wbits_list[wi], ic[k], oc[k], tmp);
                if (r != 0) { printf("ROUNDTRIP FAIL %s L%d S%d W%d M%d P%u chunking %d: %d\n", base, levels[li], strategies[si], wbits_list[wi], memlevels[mi], pi, k, r); failures++; }
            }
        }
        printf("SUMMARY %s streams=%u total_out=%lu hash=%08x\n", base, streams, total_out, filehash);
        free(src); free(dst); free(tmp);
    }
    if (failures) { printf("FAILURES: %lu\n", failures); return 1; }
    return 0;
}
