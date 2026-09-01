/* Copyright (c) 2026 UniBlake Developers */
#define _POSIX_C_SOURCE 200112L  /* clock_gettime, posix_memalign */
/* Where the time in one leaf digest goes.
 *
 * Reports an incremental build-up -- copy, then copy+update, then the full
 * leaf -- so each stage's cost is a difference between two measured totals
 * rather than a separate timing that has to be trusted to compose.
 *
 * It also times the compression alone. That number is deliberately printed
 * apart from the build-up: a compression in a tight loop gets better branch
 * prediction and cache behaviour than the same call inside ub_final, and
 * measures HIGHER than the increment ub_final adds. The two do not subtract.
 * Treat it as an upper bound on the kernel, not as a component.
 */
#include "uniblake/prefix.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ub_alloc.h"

static double ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static int cmpd(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}

enum { PRE = 140, OUT = 50, REPS = 9 };
#ifndef UB_BENCH_N
#define UB_BENCH_N 400000
#endif
static const unsigned N = UB_BENCH_N;

static volatile uint8_t sink = 0;
static void tail(uint8_t *c, unsigned i){
  c[0]=(uint8_t)i; c[1]=(uint8_t)(i>>8); c[2]=(uint8_t)(i>>16); c[3]=(uint8_t)(i>>24);
}

int main(void){
  uint8_t pre[PRE], out[64], c[4];
  for (int i = 0; i < PRE; i++) pre[i] = (uint8_t)(i*7+1);
  double t[REPS];

  ub_state *S = ub_aligned_alloc(ub_state_align(), ub_state_size());
  ub_param P; ub_param_init(&P, OUT);
  ub_init_param(S, &P); ub_update(S, pre, PRE);
  ub_state *W = ub_aligned_alloc(ub_state_align(), ub_state_size());

  /* Steady clock before timing: see bench_compare.c. */
  { double t0 = ns();
    while (ns() - t0 < 3e8)
      for (int i = 0; i < 1000; i++) {
        ub_copy(W,S); tail(c,(unsigned)i); ub_update(W,c,4);
        ub_final(W,out,OUT); sink ^= out[0];
      } }

  printf("prefix=%dB digest=%dB N=%u reps=%d (median ns/digest)\n\n", PRE, OUT, N, REPS);
  printf("incremental build-up -- each row adds one stage:\n");

  for (int r=0;r<REPS;r++){ double t0=ns();
    for (unsigned i=0;i<N;i++){ ub_copy(W,S); sink ^= *(const volatile uint8_t*)W; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double a=t[REPS/2];

  for (int r=0;r<REPS;r++){ double t0=ns();
    for (unsigned i=0;i<N;i++){ ub_copy(W,S); tail(c,i); ub_update(W,c,4);
                                sink ^= *(const volatile uint8_t*)W; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double b=t[REPS/2];

  for (int r=0;r<REPS;r++){ double t0=ns();
    for (unsigned i=0;i<N;i++){ ub_copy(W,S); tail(c,i); ub_update(W,c,4);
                                ub_final(W,out,OUT); sink ^= out[0]; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double d=t[REPS/2];

  printf("  state copy                  %7.2f\n", a);
  printf("  + update(4B)                %7.2f   (+%.2f)\n", b, b-a);
  printf("  + ub_final (full leaf)      %7.2f   (+%.2f)\n", d, d-b);

  uint8_t blk[UB_BLOCKBYTES]; memset(blk,3,sizeof blk);
  ub_state *Z = ub_aligned_alloc(ub_state_align(), ub_state_size());
  for (int r=0;r<REPS;r++){ ub_copy(Z,S); double t0=ns();
    for (unsigned i=0;i<N;i++) ub_compress(Z,blk,1);
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd);
  printf("\nnot a component of the above (see file header):\n");
  printf("  ub_compress, tight loop     %7.2f\n", t[REPS/2]);

  ub_aligned_free(S); ub_aligned_free(W); ub_aligned_free(Z);
  (void)sink; return 0;
}
