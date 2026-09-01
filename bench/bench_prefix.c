/* Copyright (c) 2026 UniBlake Developers */
#define _POSIX_C_SOURCE 200112L  /* clock_gettime, posix_memalign */
/* Streaming vs prefix state, on the geometry where they differ.
 * Median ns/digest. Correctness is the tests' job, not this file's. */
#include "uniblake/prefix.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ub_alloc.h"
static double ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static int cmpd(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return(x>y)-(x<y);}
#define N 400000
#define REPS 7
int main(void){
  if(sodium_init()<0) return 77;
  enum{PRE=140,OUT=50};
  uint8_t pre[PRE],out[64]; for(int i=0;i<PRE;i++) pre[i]=(uint8_t)(i*7+1);
  volatile uint8_t sink=0; double t[REPS];
  printf("prefix=%dB digest=%dB N=%d reps=%d (median ns/digest)\n\n",PRE,OUT,N,REPS);

  for(int r=0;r<REPS;r++){
    crypto_generichash_blake2b_state base,s;
    crypto_generichash_blake2b_init(&base,NULL,0,OUT);
    crypto_generichash_blake2b_update(&base,pre,PRE);
    double t0=ns();
    for(uint32_t i=0;i<N;i++){ s=base;
      uint8_t c[4]={(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i>>16),(uint8_t)(i>>24)};
      crypto_generichash_blake2b_update(&s,c,4);
      crypto_generichash_blake2b_final(&s,out,OUT); sink^=out[0]; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double sod=t[REPS/2];

  ub_state *S=ub_aligned_alloc(ub_state_align(),ub_state_size());
  ub_param P; ub_param_init(&P,OUT);
  ub_init_param(S,&P); ub_update(S,pre,PRE);

  /* uniblake streaming, same shape: shows the core's own compression speed */
  ub_state *W=ub_aligned_alloc(ub_state_align(),ub_state_size());
  for(int r=0;r<REPS;r++){ double t0=ns();
    for(uint32_t i=0;i<N;i++){ ub_copy(W,S);
      uint8_t c[4]={(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i>>16),(uint8_t)(i>>24)};
      ub_update(W,c,4); ub_final(W,out,OUT); sink^=out[0]; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double ubs=t[REPS/2];

  for(int r=0;r<REPS;r++){ double t0=ns();
    for(uint32_t i=0;i<N;i++){ ub_hash_n(S,4,i,1,0,0,out,OUT); sink^=out[0]; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double one=t[REPS/2];

  size_t st=64; uint8_t*m=malloc((size_t)N*st);
  for(int r=0;r<REPS;r++){ double t0=ns();
    ub_hash_n(S,4,0,N,0,0,m,st); t[r]=(ns()-t0)/N; sink^=m[0]; }
  qsort(t,REPS,sizeof*t,cmpd); double many=t[REPS/2];

  printf("  libsodium streaming    %7.1f   1.00x\n",sod);
  printf("  uniblake streaming     %7.1f   %.2fx\n",ubs,sod/ubs);
  printf("  ub_hash_n, n=1        %7.1f   %.2fx\n",one,sod/one);
  printf("  ub_hash_n              %7.1f   %.2fx\n",many,sod/many);
  free(m);
  ub_aligned_free(S); ub_aligned_free(W);
  (void)sink; return 0;
}
