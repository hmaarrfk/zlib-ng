#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib-ng.h"

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    unsigned char *b = malloc(*n + 1); fread(b, 1, *n, f); fclose(f); return b;
}

static unsigned instep;
static int run(const unsigned char *in, size_t inlen, const unsigned char *exp, size_t explen, unsigned chunk) {
    zng_stream s; memset(&s, 0, sizeof s);
    unsigned char *out = calloc(1, explen + 1024);
    if (zng_inflateInit2(&s, -15) != Z_OK) return 1;
    s.next_in = (unsigned char *)in; size_t fed = instep ? (instep < inlen ? instep : inlen) : inlen; s.avail_in = (unsigned)fed;
    s.next_out = out; s.avail_out = chunk;
    int ret;
    for (;;) {
        ret = zng_inflate(&s, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) { printf("  chunk=%u: ret=%d msg=%s total_out=%lu\n", chunk, ret, s.msg ? s.msg : "", (unsigned long)s.total_out); return 1; }
        if (s.avail_out == 0) s.avail_out = chunk;
        if (instep && s.avail_in == 0) { size_t m = inlen - fed; if (m == 0) { printf("  chunk=%u: input exhausted, ret=%d\n", chunk, ret); return 1; } if (m > instep) m = instep; s.avail_in = (unsigned)m; fed += m; }
    }
    zng_inflateEnd(&s);
    size_t bad = (size_t)-1;
    for (size_t i = 0; i < explen; i++) if (out[i] != exp[i]) { bad = i; break; }
    printf("  chunk=%u: total_out=%lu (expected %zu) first mismatch=%zd\n", chunk, (unsigned long)s.total_out, explen, (ssize_t)bad);
    free(out);
    return bad != (size_t)-1 || s.total_out != explen;
}

int main(int argc, char **argv) {
    size_t inlen, explen;
    unsigned char *in = slurp(argv[1], &inlen), *exp = slurp(argv[2], &explen);
    instep = argc > 3 ? (unsigned)atoi(argv[3]) : 0;
    printf("input step=%u\n", instep);
    unsigned chunks[] = { 65536, 16384, 8192, 4096, 1000, 64 };
    int fail = 0;
    for (unsigned i = 0; i < sizeof chunks / sizeof *chunks; i++) fail |= run(in, inlen, exp, explen, chunks[i]);
    return fail;
}
