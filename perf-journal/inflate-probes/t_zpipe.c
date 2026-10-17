/* zpipe-style strict loop: after Z_OK, either the output buffer is full or all input was consumed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib-ng.h"

static int run(const unsigned char *src, size_t n, unsigned chunk, int wbits, int flush) {
    zng_stream c; memset(&c, 0, sizeof c);
    size_t bound = zng_deflateBound(&c, n) + 1000;
    unsigned char *comp = malloc(bound), *out = malloc(n + 1);
    zng_deflateInit2(&c, 6, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY);
    c.next_in = (unsigned char *)src; c.avail_in = (unsigned)n; c.next_out = comp; c.avail_out = (unsigned)bound;
    if (zng_deflate(&c, Z_FINISH) != Z_STREAM_END) return printf("deflate failed\n"), 1;
    size_t clen = c.total_out; zng_deflateEnd(&c);

    zng_stream s; memset(&s, 0, sizeof s);
    zng_inflateInit2(&s, wbits);
    s.next_in = comp; s.avail_in = (unsigned)clen; s.next_out = out; s.avail_out = chunk;
    int ret, early = 0, calls = 0, pending_at_end = 0, nonmono_adler = 0;
    for (;;) {
        ret = zng_inflate(&s, flush);
        calls++;
        if (calls > (int)(n * 4 + 1000)) { printf("  chunk=%u wbits=%d flush=%d: HANG after %d calls: ret=%d avail_in=%u avail_out=%u total_out=%lu/%zu\n", chunk, wbits, flush, calls, ret, s.avail_in, s.avail_out, (unsigned long)s.total_out, n); fflush(stdout); return 1; }
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && !(flush == Z_FINISH && ret == Z_BUF_ERROR)) {
            printf("  chunk=%u wbits=%d flush=%d: ret=%d msg=%s at total_out=%lu/%zu\n", chunk, wbits, flush, ret, s.msg ? s.msg : "", (unsigned long)s.total_out, n);
            return 1;
        }
        if (s.avail_out != 0 && s.avail_in != 0) early++;      /* zpipe would now clobber its input buffer */
        if (s.avail_out == 0) s.avail_out = (unsigned)((n - s.total_out) < chunk ? (n - s.total_out) : chunk);
        if (s.avail_out == 0 && s.avail_in == 0) { printf("  chunk=%u: stuck: Z_OK with avail_in==0 and all output delivered, total_out=%lu/%zu\n", chunk, (unsigned long)s.total_out, n); return 1; }
    }
    if (s.total_out != n) pending_at_end = 1;
    int mism = memcmp(out, src, s.total_out < n ? s.total_out : n) != 0;
    printf("  chunk=%u wbits=%d flush=%d: calls=%d early_returns(avail_out>0&&avail_in>0)=%d total_out=%lu/%zu %s%s\n",
           chunk, wbits, flush, calls, early, (unsigned long)s.total_out, n,
           pending_at_end ? "SHORT_AT_STREAM_END " : "", mism ? "MISMATCH" : "ok");
    zng_inflateEnd(&s); free(comp); free(out);
    return early || pending_at_end || mism;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t n = 300000;
    unsigned char *src = malloc(n);
    /* mildly compressible text-like data */
    unsigned x = 1;
    for (size_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; src[i] = (i % 97 < 60) ? ("the quick brown fox jumps over the lazy dog "[(i + (x >> 24) % 3) % 44]) : (unsigned char)(x >> 16); }
    unsigned chunks[] = { 64, 100, 1000, 4096, 4100, 8192, 16383, 16384 };
    int fail = 0;
    for (unsigned i = 0; i < sizeof chunks / sizeof *chunks; i++) {
        fail |= run(src, n, chunks[i], 15, Z_NO_FLUSH);
        fail |= run(src, n, chunks[i], 15 + 16, Z_NO_FLUSH);
        fail |= run(src, n, chunks[i], -15, Z_NO_FLUSH);
        fail |= run(src, n, chunks[i], 15, Z_FINISH);
    }
    return fail;
}
