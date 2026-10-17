/* loop.c -- run deflate (or inflate with level -1) on a file repeatedly for
 * profiling with an external sampler. Usage: loop <file> <level> [seconds] [strategy] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "zlib-ng.h"
#ifdef __APPLE__
#  include <pthread.h>
#  include <sys/qos.h>
/* Ask for a performance core; otherwise macOS may run the loop on an efficiency core */
#  define PIN_PERFORMANCE_CORE() pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)
#else
#  define PIN_PERFORMANCE_CORE() do {} while (0)
#endif
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
    PIN_PERFORMANCE_CORE();
    if(argc<3){fprintf(stderr,"usage\n");return 2;}
    FILE*f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 2;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*d=malloc(n); if(fread(d,1,n,f)!=(size_t)n)return 2; fclose(f);
    int level=atoi(argv[2]); double secs=argc>3?atof(argv[3]):3.0; int strat=argc>4?atoi(argv[4]):0;
    size_t cap=zng_compressBound(n)+n/50+1024; uint8_t*c=malloc(cap); uint8_t*o=malloc(n);
    zng_stream s; memset(&s,0,sizeof s);
    size_t clen=0;
    if(level<0){ zng_deflateInit2(&s,6,Z_DEFLATED,-15,8,0); s.next_in=d;s.avail_in=n;s.next_out=c;s.avail_out=cap; zng_deflate(&s,Z_FINISH); clen=s.total_out; zng_deflateEnd(&s); memset(&s,0,sizeof s); zng_inflateInit2(&s,-15);}
    else zng_deflateInit2(&s,level,Z_DEFLATED,-15,8,strat);
    double t0=now(); long it=0; double bytes=0;
    while(now()-t0<secs){
        if(level<0){ zng_inflateReset(&s); s.next_in=c;s.avail_in=clen;s.next_out=o;s.avail_out=n; if(zng_inflate(&s,Z_FINISH)!=Z_STREAM_END){fprintf(stderr,"inflate err\n");return 3;} bytes+=n; }
        else { zng_deflateReset(&s); s.next_in=d;s.avail_in=n;s.next_out=c;s.avail_out=cap; if(zng_deflate(&s,Z_FINISH)!=Z_STREAM_END){fprintf(stderr,"deflate err\n");return 3;} bytes+=n; clen=s.total_out; }
        it++;
    }
    double dt=now()-t0;
    printf("%s level %d: %ld iters, %.1f MB/s, ratio %.3f\n",argv[1],level,it,bytes/dt/1e6,(double)n/clen);
    return 0;
}
