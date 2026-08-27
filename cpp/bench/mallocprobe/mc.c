#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
/* Size-class histogram of malloc/calloc/realloc, dumped on SIGUSR1.
   Attributes per-request allocations to size classes without reading code:
   ~32B = shared_ptr control block (make_shared/Value), ~64B = std::string buffer,
   ~48B = map/set node, large = parse/response buffer. */
#define NB 9
static atomic_ullong g_hist[NB];
static atomic_ullong g_total;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t,size_t);
static void *(*real_realloc)(void*,size_t);
static char early[65536]; static size_t early_off=0;
static int bucket(size_t s){
  if(s<=16)return 0; if(s<=32)return 1; if(s<=48)return 2; if(s<=64)return 3;
  if(s<=128)return 4; if(s<=256)return 5; if(s<=1024)return 6; if(s<=4096)return 7; return 8; }
static void count(size_t s){ atomic_fetch_add(&g_total,1); atomic_fetch_add(&g_hist[bucket(s)],1); }
static void dump(int sig){ (void)sig; char b[256];
  int n=snprintf(b,sizeof b,"HIST %llu | 16:%llu 32:%llu 48:%llu 64:%llu 128:%llu 256:%llu 1K:%llu 4K:%llu big:%llu\n",
    (unsigned long long)atomic_load(&g_total),
    (unsigned long long)atomic_load(&g_hist[0]),(unsigned long long)atomic_load(&g_hist[1]),
    (unsigned long long)atomic_load(&g_hist[2]),(unsigned long long)atomic_load(&g_hist[3]),
    (unsigned long long)atomic_load(&g_hist[4]),(unsigned long long)atomic_load(&g_hist[5]),
    (unsigned long long)atomic_load(&g_hist[6]),(unsigned long long)atomic_load(&g_hist[7]),
    (unsigned long long)atomic_load(&g_hist[8]));
  write(2,b,n); }
__attribute__((constructor)) static void init(void){
  real_malloc=dlsym(RTLD_NEXT,"malloc"); real_calloc=dlsym(RTLD_NEXT,"calloc");
  real_realloc=dlsym(RTLD_NEXT,"realloc"); signal(SIGUSR1,dump); }
void *malloc(size_t s){ if(!real_malloc){void*p=early+early_off;early_off+=(s+15)&~15u;return p;}
  count(s); return real_malloc(s); }
void *realloc(void*p,size_t s){ if(!real_realloc)return NULL; count(s); return real_realloc(p,s); }
void *calloc(size_t n,size_t s){ if(!real_calloc){void*p=early+early_off;early_off+=(n*s+15)&~15u;memset(p,0,n*s);return p;}
  count(n*s); return real_calloc(n,s); }
