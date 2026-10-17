/* Z_FINISH one-shot with exact avail_out after a preset dictionary (zlib wrapper, Z_NEED_DICT path, and raw). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib-ng.h"

static int run(int wbits, unsigned dictlen, unsigned n, int flush) {
    unsigned char *dict = malloc(dictlen), *src = malloc(n), *comp = malloc(2 * n + 1000), *out = malloc(n);
    unsigned x = 7;
    for (unsigned i = 0; i < dictlen; i++) { x = x * 1103515245u + 12345u; dict[i] = (unsigned char)(x >> 16); }
    for (unsigned i = 0; i < n; i++) src[i] = dict[(i * 7) % dictlen];          /* matches into the dictionary */
    zng_stream c; memset(&c, 0, sizeof c);
    zng_deflateInit2(&c, 6, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY);
    zng_deflateSetDictionary(&c, dict, dictlen);
    c.next_in = src; c.avail_in = n; c.next_out = comp; c.avail_out = 2 * n + 1000;
    zng_deflate(&c, Z_FINISH); size_t clen = c.total_out; zng_deflateEnd(&c);

    zng_stream s; memset(&s, 0, sizeof s);
    zng_inflateInit2(&s, wbits);
    if (wbits < 0) zng_inflateSetDictionary(&s, dict, dictlen);
    s.next_in = comp; s.avail_in = (unsigned)clen; s.next_out = out; s.avail_out = n;
    int ret = zng_inflate(&s, flush);
    if (ret == Z_NEED_DICT) {
        zng_inflateSetDictionary(&s, dict, dictlen);
        ret = zng_inflate(&s, flush);
    }
    printf("  wbits=%d dict=%u out=%u flush=%d: ret=%d avail_out=%u avail_in=%u total_out=%lu %s\n", wbits, dictlen, n, flush, ret, s.avail_out, s.avail_in,
           (unsigned long)s.total_out, (ret == Z_STREAM_END && memcmp(out, src, n) == 0) ? "ok" : "NOT_STREAM_END_OR_MISMATCH");
    zng_inflateEnd(&s);
    return ret != Z_STREAM_END;
}
int main(void) {
    int fail = 0;
    fail |= run(15, 32000, 10000, Z_FINISH);
    fail |= run(15, 32000, 10000, Z_NO_FLUSH);
    fail |= run(-15, 32000, 10000, Z_FINISH);
    fail |= run(-15, 32000, 10000, Z_SYNC_FLUSH);
    fail |= run(15, 1000, 10000, Z_FINISH);
    return fail;
}
