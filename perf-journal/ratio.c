/* ratio.c -- per-file compressed size at one level, whole file, raw deflate.
 *
 * Usage: ratio <level> file...
 * Prints one line per file: level path input_bytes output_bytes.
 * Build against the library under test, e.g.
 *   cc -O2 -I<build> -I<src> -o ratio perf-journal/ratio.c <build>/libz-ng.a
 * Synthetic inputs (text, random, realistic_rgb) come from test/test_data_p.h
 * generators dumped to files, so they match the benchmark suite's data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "zlib-ng.h"
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: ratio <level> file...\n"); return 2; }
    int level = atoi(argv[1]);
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); if (!f) { perror(argv[i]); return 2; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 2; fclose(f);
        size_t cap = zng_compressBound(n) + n / 50 + 1024; uint8_t *c = malloc(cap);
        zng_stream s; memset(&s, 0, sizeof s);
        zng_deflateInit2(&s, level, Z_DEFLATED, -15, 8, 0);
        s.next_in = d; s.avail_in = n; s.next_out = c; s.avail_out = cap;
        if (zng_deflate(&s, Z_FINISH) != Z_STREAM_END) { fprintf(stderr, "deflate error\n"); return 3; }
        printf("%d %s %ld %lu\n", level, argv[i], n, (unsigned long)s.total_out);
        zng_deflateEnd(&s); free(d); free(c);
    }
    return 0;
}
