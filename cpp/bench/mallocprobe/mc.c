#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static atomic_ullong g_count = 0;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t,size_t);
static void *(*real_realloc)(void*,size_t);
static char early[65536]; static size_t early_off = 0; static int inited = 0;
static void dump(int sig){ (void)sig; char b[64];
  int n = snprintf(b,sizeof b,"MALLOC_TOTAL=%llu\n",(unsigned long long)atomic_load(&g_count));
  write(2,b,n); }
__attribute__((constructor)) static void init(void){
  real_malloc=dlsym(RTLD_NEXT,"malloc");
  real_calloc=dlsym(RTLD_NEXT,"calloc");
  real_realloc=dlsym(RTLD_NEXT,"realloc");
  signal(SIGUSR1,dump); inited=1; }
void *malloc(size_t s){ if(!real_malloc){ void*p=early+early_off; early_off+=(s+15)&~15u; return p; }
  atomic_fetch_add(&g_count,1); return real_malloc(s); }
void *realloc(void*p,size_t s){ if(!real_realloc) return NULL; atomic_fetch_add(&g_count,1); return real_realloc(p,s); }
void *calloc(size_t n,size_t s){ if(!real_calloc){ void*p=early+early_off; early_off+=(n*s+15)&~15u; memset(p,0,n*s); return p; }
  atomic_fetch_add(&g_count,1); return real_calloc(n,s); }
