/* scaling.c -- thread-scaling benchmark for the perf journal.
 *
 * Runs N independent deflate (levels 1, 6, 9) or inflate streams on N threads,
 * each over its own copy of the same input, and reports aggregate throughput.
 * zlib-ng is single-threaded, so this shows how the library's per-core
 * throughput holds up when several cores are busy (cache/memory pressure),
 * not any intra-stream parallelism.
 *
 * Usage: scaling <input-file>   (prints JSON array to stdout)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "zlib-ng.h"
#ifdef __APPLE__
#  include <pthread.h>
#  include <sys/qos.h>
/* Ask for a performance core; otherwise macOS may run the loop on an efficiency core */
#  define PIN_PERFORMANCE_CORE() pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)
#else
#  define PIN_PERFORMANCE_CORE() do {} while (0)
#endif

#define RUN_SECONDS 0.6

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct job {
    const uint8_t *in; size_t in_len;      /* input for this stream */
    uint8_t *out; size_t out_cap;
    int level;                              /* -1 = inflate */
    double bytes;                           /* bytes processed (input bytes) */
    double seconds;
};

static void *worker(void *arg) {
    struct job *j = arg;
    PIN_PERFORMANCE_CORE();
    zng_stream strm; memset(&strm, 0, sizeof(strm));
    if (j->level >= 0)
        zng_deflateInit2(&strm, j->level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    else
        zng_inflateInit2(&strm, -15);
    double t0 = now(), t1 = t0;
    long iters = 0;
    while (t1 - t0 < RUN_SECONDS) {
        if (j->level >= 0) zng_deflateReset(&strm); else zng_inflateReset(&strm);
        strm.next_in = (const uint8_t *)j->in; strm.avail_in = (uint32_t)j->in_len;
        strm.next_out = j->out; strm.avail_out = (uint32_t)j->out_cap;
        int rc = j->level >= 0 ? zng_deflate(&strm, Z_FINISH) : zng_inflate(&strm, Z_FINISH);
        if (rc != Z_STREAM_END) { fprintf(stderr, "stream error %d\n", rc); exit(1); }
        iters++;
        t1 = now();
    }
    j->bytes = (double)iters * (j->level >= 0 ? (double)j->in_len : (double)strm.total_out);
    j->seconds = t1 - t0;
    if (j->level >= 0) zng_deflateEnd(&strm); else zng_inflateEnd(&strm);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s input\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 4 << 20) n = 4 << 20;     /* 4 MiB is enough to exceed L2 */
    uint8_t *data = malloc(n);
    if (fread(data, 1, n, f) != (size_t)n) { perror("read"); return 2; }
    fclose(f);

    /* Pre-compress once for the inflate case */
    size_t cbound = zng_compressBound(n);
    uint8_t *comp = malloc(cbound);
    zng_stream cs; memset(&cs, 0, sizeof(cs));
    zng_deflateInit2(&cs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    cs.next_in = data; cs.avail_in = (uint32_t)n; cs.next_out = comp; cs.avail_out = (uint32_t)cbound;
    zng_deflate(&cs, Z_FINISH); size_t clen = cs.total_out; zng_deflateEnd(&cs);

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int threads_list[4] = {1, 2, 4, (int)ncpu};
    int levels[4] = {1, 6, 9, -1};
    printf("[\n");
    int first = 1;
    for (int li = 0; li < 4; li++) {
        for (int ti = 0; ti < 4; ti++) {
            int nt = threads_list[ti];
            if (ti == 3 && nt <= 4) continue;
            pthread_t *th = calloc(nt, sizeof(*th));
            struct job *jobs = calloc(nt, sizeof(*jobs));
            for (int i = 0; i < nt; i++) {
                jobs[i].level = levels[li];
                if (levels[li] >= 0) {
                    jobs[i].in = malloc(n); memcpy((void *)jobs[i].in, data, n); jobs[i].in_len = n;
                    jobs[i].out_cap = cbound; jobs[i].out = malloc(cbound);
                } else {
                    jobs[i].in = malloc(clen); memcpy((void *)jobs[i].in, comp, clen); jobs[i].in_len = clen;
                    jobs[i].out_cap = n; jobs[i].out = malloc(n);
                }
            }
            for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &jobs[i]);
            double agg = 0;
            for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); agg += jobs[i].bytes / jobs[i].seconds; }
            printf("%s {\"op\": \"%s\", \"level\": %d, \"threads\": %d, \"aggregate_MBps\": %.1f, \"per_thread_MBps\": %.1f}",
                   first ? "" : ",\n", levels[li] >= 0 ? "deflate" : "inflate", levels[li], nt, agg / 1e6, agg / 1e6 / nt);
            first = 0;
            for (int i = 0; i < nt; i++) { free((void *)jobs[i].in); free(jobs[i].out); }
            free(th); free(jobs);
        }
    }
    printf("\n]\n");
    free(data); free(comp);
    return 0;
}
