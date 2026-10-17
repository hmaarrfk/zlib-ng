/* tune.c -- sweep deflateTune() parameters for one level over a set of files.
 *
 * Usage: tune <level> <good> <lazy> <nice> <chain> file...
 *        tune <level> default file...
 * Prints: total input bytes, total output bytes, MB/s (single pass per file,
 * best of 2 passes), so parameter sets can be ranked on a ratio/speed plane.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "zlib-ng.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage\n");return 2;}
    int level=atoi(argv[1]); int argi; int tuned=0; int good=0,lazy=0,nice=0,chain=0;
    if(strcmp(argv[2],"default")==0){argi=3;} else {tuned=1;good=atoi(argv[2]);lazy=atoi(argv[3]);nice=atoi(argv[4]);chain=atoi(argv[5]);argi=6;}
    double tin=0,tout=0,best_total=0;
    for(;argi<argc;argi++){
        FILE*f=fopen(argv[argi],"rb"); if(!f){perror(argv[argi]);return 2;}
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        if(n>8<<20) n=8<<20;
        uint8_t*d=malloc(n); if(fread(d,1,n,f)!=(size_t)n)return 2; fclose(f);
        size_t cap=zng_compressBound(n)+n/50+1024; uint8_t*c=malloc(cap);
        zng_stream s; memset(&s,0,sizeof s);
        zng_deflateInit2(&s,level,Z_DEFLATED,-15,8,0);
        if(tuned) zng_deflateTune(&s,good,lazy,nice,chain);
        double best=1e9; size_t clen=0;
        for(int pass=0;pass<2;pass++){
            zng_deflateReset(&s); s.next_in=d;s.avail_in=n;s.next_out=c;s.avail_out=cap;
            double t0=now(); if(zng_deflate(&s,Z_FINISH)!=Z_STREAM_END){fprintf(stderr,"err\n");return 3;}
            double dt=now()-t0; if(dt<best)best=dt; clen=s.total_out;
        }
        zng_deflateEnd(&s);
        tin+=n; tout+=clen; best_total+=best;
        free(d); free(c);
    }
    printf("level %d %s%d/%d/%d/%d in=%.0f out=%.0f ratio=%.4f MB/s=%.1f\n",level,tuned?"":"default ",good,lazy,nice,chain,tin,tout,tin/tout,tin/best_total/1e6);
    return 0;
}
