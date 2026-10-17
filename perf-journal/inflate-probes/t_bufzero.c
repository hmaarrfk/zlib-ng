/* After a small-buffer call leaves output pending, a call with avail_out == 0 must not return Z_OK (no progress possible). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib-ng.h"
int main(void) {
    size_t n = 200000; unsigned char *src = malloc(n); unsigned x = 1;
    for (size_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; src[i] = (i % 97 < 60) ? ("the quick brown fox jumps over the lazy dog "[(i + (x >> 24) % 3) % 44]) : (unsigned char)(x >> 16); }
    zng_stream c; memset(&c, 0, sizeof c); size_t bound = zng_deflateBound(&c, n) + 1000;
    unsigned char *comp = malloc(bound), *out = malloc(n);
    zng_deflateInit(&c, 6); c.next_in = src; c.avail_in = (unsigned)n; c.next_out = comp; c.avail_out = (unsigned)bound;
    zng_deflate(&c, Z_FINISH); size_t clen = c.total_out; zng_deflateEnd(&c);
    zng_stream s; memset(&s, 0, sizeof s); zng_inflateInit(&s);
    s.next_in = comp; s.avail_in = (unsigned)clen; s.next_out = out; s.avail_out = 64;
    int ret, zero_ok = 0, zero_buf = 0, zero_other = 0, calls = 0;
    for (;;) {
        ret = zng_inflate(&s, Z_NO_FLUSH); calls++;
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) { printf("ret=%d at call %d\n", ret, calls); return 1; }
        /* zpipe-style caller that forgot to refill: avail_out == 0, avail_in > 0 */
        if (s.avail_out == 0 && s.avail_in != 0) {
            unsigned long ti = s.total_in, to = s.total_out;
            int r2 = zng_inflate(&s, Z_NO_FLUSH);
            if (r2 == Z_OK) zero_ok++; else if (r2 == Z_BUF_ERROR) zero_buf++; else zero_other++;
            if (ti != s.total_in || to != s.total_out) printf("progress with avail_out==0 ?! (%lu->%lu, %lu->%lu)\n", ti, (unsigned long)s.total_in, to, (unsigned long)s.total_out);
            s.avail_out = (unsigned)(n - s.total_out < 64 ? n - s.total_out : 64);
        }
        if (s.avail_out == 0) s.avail_out = (unsigned)(n - s.total_out < 64 ? n - s.total_out : 64);
        if (calls > 100000) { printf("hang\n"); return 1; }
    }
    printf("calls=%d; avail_out==0 probes: Z_OK=%d Z_BUF_ERROR=%d other=%d; data %s\n", calls, zero_ok, zero_buf, zero_other, memcmp(out, src, n) ? "MISMATCH" : "ok");
    return zero_ok != 0;
}
