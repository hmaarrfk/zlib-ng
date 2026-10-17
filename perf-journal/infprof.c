/* infprof <level> <out_chunk> <in_chunk> <wrap:raw|zlib|gzip> file...
   Compresses each file at <level>, inflates it (best of 5) with the given
   output/input chunk sizes (0 = whole), prints MB/s and the profile split. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sys/qos.h>
#include "zlib-ng.h"
#include "zbuild.h"
#include "inftrees.h"
#include "inflate.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    if(argc<6){fprintf(stderr,"usage\n");return 2;}
    int level=atoi(argv[1]); size_t ochunk=atol(argv[2]), ichunk=atol(argv[3]);
    int wb = strcmp(argv[4],"raw")==0 ? -15 : strcmp(argv[4],"gzip")==0 ? 31 : 15;
    printf("%-14s %8s %8s %7s | %6s %6s %6s %6s %6s %6s | %6s %6s %5s\n","file","MB","ratio","MB/s","fast","safe","clens","tables","window","other","blocks","fastKB","calls");
    for(int i=5;i<argc;i++){
        FILE*f=fopen(argv[i],"rb"); if(!f){perror(argv[i]);return 2;}
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        uint8_t*d=malloc(n); if(fread(d,1,n,f)!=(size_t)n)return 2; fclose(f);
        size_t cap=zng_compressBound(n)+n/50+1024; uint8_t*c=malloc(cap);
        zng_stream s; memset(&s,0,sizeof s);
        zng_deflateInit2(&s,level,Z_DEFLATED,wb,8,0);
        s.next_in=d;s.avail_in=n;s.next_out=c;s.avail_out=cap;
        if(zng_deflate(&s,Z_FINISH)!=Z_STREAM_END){fprintf(stderr,"deflate err\n");return 3;}
        size_t clen=s.total_out; zng_deflateEnd(&s);
        uint8_t*o=malloc(n+16);
        double best=1e9;
#ifdef ZNG_INFLATE_PROFILE
        struct zng_inflate_prof bestp;
#endif
        for(int rep=0;rep<5;rep++){
#ifdef ZNG_INFLATE_PROFILE
            memset(&zng_inflate_prof,0,sizeof zng_inflate_prof);
#endif
#ifdef ZNG_INFLATE_STATS
            memset(&zng_inflate_stats,0,sizeof zng_inflate_stats);
#endif
            zng_stream z; memset(&z,0,sizeof z);
            zng_inflateInit2(&z,wb);
            double t0=now();
            size_t ipos=0,opos=0; int ret;
            do {
                size_t ia = ichunk? (clen-ipos<ichunk?clen-ipos:ichunk) : clen-ipos;
                size_t oa = ochunk? ((size_t)n+16-opos<ochunk?(size_t)n+16-opos:ochunk) : (size_t)n+16-opos;
                z.next_in=c+ipos; z.avail_in=(unsigned)ia; z.next_out=o+opos; z.avail_out=(unsigned)oa;
                ret=zng_inflate(&z,Z_NO_FLUSH);
                ipos=clen-z.avail_in-0; ipos = (size_t)(z.next_in-c); opos=(size_t)(z.next_out-o);
                if(ret!=Z_OK&&ret!=Z_STREAM_END&&ret!=Z_BUF_ERROR){fprintf(stderr,"inflate err %d\n",ret);return 4;}
                if(ret==Z_BUF_ERROR && z.avail_in==0 && ipos>=clen){fprintf(stderr,"stall\n");return 5;}
            } while(ret!=Z_STREAM_END);
            double dt=now()-t0; zng_inflateEnd(&z);
            if(opos!=(size_t)n||memcmp(o,d,n)){fprintf(stderr,"MISMATCH %s\n",argv[i]);return 6;}
            if(dt<best){best=dt;
#ifdef ZNG_INFLATE_PROFILE
                bestp=zng_inflate_prof;
#endif
            }
        }
        const char*base=strrchr(argv[i],'/'); base=base?base+1:argv[i];
        printf("%-14s %8.2f %8.3f %7.0f",base,n/1e6,(double)n/clen,n/best/1e6);
#ifdef ZNG_INFLATE_PROFILE
        double T=(double)bestp.t_total; if(T<=0)T=1;
        double other=T-bestp.t_fast-bestp.t_safe-bestp.t_codelens-bestp.t_tables-bestp.t_win;
        printf(" | %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%% | %6llu %6llu %5llu",100*bestp.t_fast/T,100*bestp.t_safe/T,100*bestp.t_codelens/T,100*bestp.t_tables/T,100*bestp.t_win/T,100*other/T,
            (unsigned long long)(bestp.n_dyn+bestp.n_fixed+bestp.n_stored),(unsigned long long)(bestp.b_fast>>10),(unsigned long long)bestp.n_calls);
#endif
#ifdef ZNG_INFLATE_STATS
        struct zng_inflate_stats*st=&zng_inflate_stats;
        double tot=(double)(st->n_lit+st->n_len);
        printf(" | lit %.1f%% len %.1f%% avglen %.1f sublen %.2f%% subdist %.2f%% win %.2f%% overlap %.1f%% lit/iter %.2f",
            100*st->n_lit/tot,100*st->n_len/tot,(double)st->sum_len/(st->n_len?st->n_len:1),100.0*st->n_sub_len/tot,100.0*st->n_sub_dist/(st->n_len?st->n_len:1),100.0*st->n_win/(st->n_len?st->n_len:1),100.0*st->n_overlap/(st->n_len?st->n_len:1),(double)st->n_lit/(st->n_iter?st->n_iter:1));
#endif
        printf("\n");
        free(d);free(c);free(o);
    }
    return 0;
}
