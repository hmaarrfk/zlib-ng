/* rt.c -- streaming round-trip check: rt <file> <level> <in_chunk> <out_chunk> [wbits] [flush_every] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "zlib-ng.h"
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 2; fclose(f);
    int level = atoi(argv[2]); size_t ic = atoi(argv[3]), oc = atoi(argv[4]);
    int wbits = argc > 5 ? atoi(argv[5]) : -15; size_t fe = argc > 6 ? atoi(argv[6]) : 0;
    size_t cap = zng_compressBound(n) + n / 50 + 65536; uint8_t *c = malloc(cap);
    zng_stream s; memset(&s, 0, sizeof s);
    zng_deflateInit2(&s, level, Z_DEFLATED, wbits, 8, 0);
    size_t pos = 0, outpos = 0, since = 0; int rc;
    for (;;) {
        size_t take = n - pos < ic ? n - pos : ic;
        if (fe && take > fe - since) take = fe - since;
        s.next_in = d + pos; s.avail_in = (uint32_t)take; pos += take; since += take;
        int flush = pos == n ? Z_FINISH : (fe && since == fe ? (since = 0, Z_SYNC_FLUSH) : Z_NO_FLUSH);
        do {
            size_t room = cap - outpos < oc ? cap - outpos : oc;
            s.next_out = c + outpos; s.avail_out = (uint32_t)room;
            rc = zng_deflate(&s, flush); outpos = s.total_out;
            if (rc == Z_STREAM_ERROR) { printf("deflate error\n"); return 3; }
        } while (s.avail_out == 0 && rc != Z_STREAM_END);
        if (rc == Z_STREAM_END) break;
    }
    zng_deflateEnd(&s);
    uint8_t *o = malloc(n + 1); memset(&s, 0, sizeof s);
    zng_inflateInit2(&s, wbits > 0 && wbits < 16 ? 15 : wbits == 31 ? 47 : wbits);
    s.next_in = c; s.avail_in = (uint32_t)outpos; s.next_out = o; s.avail_out = (uint32_t)n + 1;
    rc = zng_inflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) { printf("inflate rc=%d at out %lu msg=%s\n", rc, (unsigned long)s.total_out, s.msg ? s.msg : ""); return 4; }
    if (s.total_out != (size_t)n || memcmp(o, d, n)) { size_t i = 0; while (i < n && o[i] == d[i]) i++; printf("MISMATCH at %zu (out %lu)\n", i, (unsigned long)s.total_out); return 5; }
    printf("ok: %ld -> %zu (ratio %.4f)\n", n, outpos, (double)n / outpos);
    return 0;
}
