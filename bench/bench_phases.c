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
#include <math.h>
#include "ub_alloc.h"

static double ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}

/* Dispersion of the sorted sample, alongside the median.
 *
 * The median alone says nothing about how well it is known. Digits chosen from
 * the magnitude of a number claim a precision the measurement may not have;
 * the sample is the only honest source.
 *
 * Two statistics, because one is not enough for this data. Fifty repetitions
 * of the leaf shape on an idle machine give a right-skewed sample: the upper
 * half of the interquartile range is about 1.8x the lower. That is the
 * expected shape for a timing -- a hard floor at the true cost, and a tail
 * from interference that has no matching effect below.
 *
 *   iqr  the middle half, unmoved by a single outlier, which a startup
 *        transient or a scheduler hiccup produces routinely.
 *   mad  median absolute deviation, a different estimator rather than a
 *        second quantile difference. Scaled by 1.4826 it is comparable to a
 *        standard deviation on normal data, so the gap between the two says
 *        how much the tail is inflating an ordinary sd: 0.119 against 0.126
 *        on the sample above.
 *
 * Both are reported. A symmetric summary of an asymmetric sample is a claim
 * the data does not support. */
static double iqr(const double *t, int n){
  return t[(3*n)/4] - t[n/4];
}

static int cmpd_(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}

static double mad(const double *t, int n){
  double d[64], med = t[n/2];
  if (n > 64) n = 64;
  for (int i = 0; i < n; i++) d[i] = fabs(t[i] - med);
  qsort(d, n, sizeof *d, cmpd_);
  return d[n/2];
}
static int cmpd(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}

/* REPS comes from the standard menu (5, 10, 100) rather than being chosen per
 * harness: two runs at 7 and 9 reps are not comparable, and nothing is gained
 * by the difference.
 *
 * REPS counts TIMED repetitions only. The warmup below runs before any clock
 * starts and is not one of them, so a reported rep count is always the number
 * of measurements the median was taken over. */
enum { PRE = 140, OUT = 50, REPS = 10 };
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

  /* ub_init_personal is the shape a caller uses; it must cost the same as the
   * parameter-block spelling it replaces. */
  ub_state *S = ub_aligned_alloc(ub_state_align(), ub_state_size());
  ub_init_personal(S, OUT, NULL); ub_update(S, pre, PRE);
  ub_state *W = ub_aligned_alloc(ub_state_align(), ub_state_size());

  /* Steady clock before timing: see bench_compare.c. */
  { double t0 = ns();
    while (ns() - t0 < 3e8)
      for (int i = 0; i < 1000; i++) {
        ub_copy(W,S); tail(c,(unsigned)i); ub_update(W,c,4);
        ub_final(W,out,OUT); sink ^= out[0];
      } }

  printf("prefix=%dB digest=%dB N=%u reps=%d timed (median ns/digest)\n\n", PRE, OUT, N, REPS);
  printf("incremental build-up -- each row adds one stage:\n");

  for (int r=0;r<REPS;r++){ double t0=ns();
    for (unsigned i=0;i<N;i++){ ub_copy(W,S); sink ^= *(const volatile uint8_t*)W; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double a=t[REPS/2], ia=iqr(t,REPS), ma=mad(t,REPS);

  for (int r=0;r<REPS;r++){ double t0=ns();
    for (unsigned i=0;i<N;i++){ ub_copy(W,S); tail(c,i); ub_update(W,c,4);
                                sink ^= *(const volatile uint8_t*)W; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double b=t[REPS/2], ib=iqr(t,REPS), mb=mad(t,REPS);

  for (int r=0;r<REPS;r++){ double t0=ns();
    for (unsigned i=0;i<N;i++){ ub_copy(W,S); tail(c,i); ub_update(W,c,4);
                                ub_final(W,out,OUT); sink ^= out[0]; }
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd); double d=t[REPS/2], id=iqr(t,REPS), md=mad(t,REPS);

  printf("  state copy                  %9.4f  iqr %.4f  mad %.4f\n", a, ia, ma);
  printf("  + update(4B)                %9.4f  iqr %.4f  mad %.4f\n", b, ib, mb);
  printf("  + ub_final (full leaf)      %9.4f  iqr %.4f  mad %.4f\n", d, id, md);

  uint8_t blk[UB_BLOCKBYTES]; memset(blk,3,sizeof blk);
  ub_state *Z = ub_aligned_alloc(ub_state_align(), ub_state_size());
  for (int r=0;r<REPS;r++){ ub_copy(Z,S); double t0=ns();
    for (unsigned i=0;i<N;i++) ub_compress(Z,blk,1);
    t[r]=(ns()-t0)/N; }
  qsort(t,REPS,sizeof*t,cmpd);
  printf("\nnot a component of the above (see file header):\n");
  printf("  ub_compress, tight loop     %9.4f  iqr %.4f  mad %.4f\n", t[REPS/2], iqr(t,REPS), mad(t,REPS));

  ub_aligned_free(S); ub_aligned_free(W); ub_aligned_free(Z);
  (void)sink; return 0;
}
