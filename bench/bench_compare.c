/* Copyright (c) 2026 UniBlake Developers */
#define _POSIX_C_SOURCE 200112L  /* clock_gettime, posix_memalign */
/* Cross-language comparison harness.
 *
 * Deliberately mirrors examples/compare.rs in the uniblake-rs repository:
 * same geometry, same iteration counts, same median-of-REPS statistic, same
 * reported units. Anything measured here should be comparable to the Rust
 * number of the same name; if the two harnesses drift, the table they feed
 * stops meaning anything.
 *
 * The threaded column is built here with pthreads rather than by linking
 * backends/hash_n_threads.c, so that the parallel path measured is the same
 * shape as the Rust one: split the leaf range, copy the read-only prefix
 * state per thread, no coordination.
 */
#include "uniblake/prefix.h"
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ub_alloc.h"

static double ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static int cmpd(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}

enum { PRE = 140, OUT = 50, REPS = 10, THREADS = 2 };
static const size_t LEAF_N[] = { 10000, 100000, 400000 };
static const size_t BULK_N[] = { 1u<<10, 1u<<14, 1u<<20, 1u<<24 };

static volatile uint8_t sink = 0;

struct span { const ub_state *S; size_t lo, hi; uint8_t acc; };

static void *leaf_run(void *arg){
  struct span *w = arg;
  ub_state *W = ub_aligned_alloc(ub_state_align(), ub_state_size());
  uint8_t out[64], local = 0;
  for (size_t i = w->lo; i < w->hi; i++) {
    ub_copy(W, w->S);
    uint8_t c[4] = {(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i>>16),(uint8_t)(i>>24)};
    ub_update(W, c, 4);
    ub_final(W, out, OUT);
    local ^= out[0];
  }
  ub_aligned_free(W);
  w->acc = local;
  return 0;
}

int main(void){
  if (sodium_init() < 0) return 77;
  uint8_t pre[PRE], out[64];
  for (int i = 0; i < PRE; i++) pre[i] = (uint8_t)(i*7+1);
  double t[REPS];

  printf("# uniblake C comparison harness\n");
  printf("# prefix=%dB digest=%dB reps=%d timed threads=%d (median ns/digest)\n", PRE, OUT, REPS, THREADS);
  printf("# state: uniblake=%zuB libsodium=%zuB\n",
         ub_state_size(), sizeof(crypto_generichash_blake2b_state));

  ub_state *S = ub_aligned_alloc(ub_state_align(), ub_state_size());
  ub_param P; ub_param_init(&P, OUT);
  ub_init_param(S, &P); ub_update(S, pre, PRE);

  /* Absorb the process-startup transient before any timing. On an idle
   * machine the first measured block runs ~2.5x slow (196 ns/digest against a
   * steady-state 78) and decays over the first few milliseconds.
   *
   * It is an artifact of *position in the run*, not of any implementation:
   * reordering this harness so libsodium measures first moves the penalty onto
   * libsodium (388 ns, 2.1x) and leaves ours at 114. Whichever block runs
   * first pays it, so without this spin the first column of the table would be
   * silently penalised. A per-loop warmup is not enough -- the transient spans
   * the process, not the loop. */
  { ub_state *R = ub_aligned_alloc(ub_state_align(), ub_state_size());
    double t0 = ns();
    while (ns() - t0 < 3e8) {
      for (int i = 0; i < 1000; i++) {
        ub_copy(R, S); ub_update(R, pre, 4); ub_final(R, out, OUT); sink ^= out[0];
      }
    }
    ub_aligned_free(R); }

  printf("\n[leaf]\n");
  printf("n,c_ours_ns,c_ours_2t_ns,c_ref_ns\n");
  for (size_t li = 0; li < sizeof LEAF_N/sizeof*LEAF_N; li++) {
    size_t n = LEAF_N[li];

    /* ours, serial. One untimed warmup pass first: at the smallest n the
     * first rep otherwise pays page faults and frequency ramp, which showed
     * up as 196 ns against a steady-state 78. */
    ub_state *W = ub_aligned_alloc(ub_state_align(), ub_state_size());
    for (size_t i = 0; i < n; i++) {
      ub_copy(W, S);
      uint8_t c[4] = {(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i>>16),(uint8_t)(i>>24)};
      ub_update(W, c, 4); ub_final(W, out, OUT); sink ^= out[0];
    }
    for (int r = 0; r < REPS; r++) {
      double t0 = ns();
      for (size_t i = 0; i < n; i++) {
        ub_copy(W, S);
        uint8_t c[4] = {(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i>>16),(uint8_t)(i>>24)};
        ub_update(W, c, 4); ub_final(W, out, OUT); sink ^= out[0];
      }
      t[r] = (ns()-t0)/(double)n;
    }
    ub_aligned_free(W);
    qsort(t, REPS, sizeof*t, cmpd); double ours = t[REPS/2];

    /* ours, THREADS-way split of the same range */
    for (int r = 0; r < REPS; r++) {
      pthread_t th[THREADS]; struct span sp[THREADS];
      size_t chunk = (n + THREADS - 1) / THREADS;
      double t0 = ns();
      for (int k = 0; k < THREADS; k++) {
        size_t lo = (size_t)k*chunk, hi = lo+chunk; if (hi > n) hi = n;
        sp[k] = (struct span){ S, lo, hi, 0 };
        pthread_create(&th[k], 0, leaf_run, &sp[k]);
      }
      for (int k = 0; k < THREADS; k++) { pthread_join(th[k], 0); sink ^= sp[k].acc; }
      t[r] = (ns()-t0)/(double)n;
    }
    qsort(t, REPS, sizeof*t, cmpd); double ours2 = t[REPS/2];

    /* reference: libsodium, same shape */
    for (int r = 0; r < REPS; r++) {
      crypto_generichash_blake2b_state base, s;
      crypto_generichash_blake2b_init(&base, NULL, 0, OUT);
      crypto_generichash_blake2b_update(&base, pre, PRE);
      double t0 = ns();
      for (size_t i = 0; i < n; i++) {
        s = base;
        uint8_t c[4] = {(uint8_t)i,(uint8_t)(i>>8),(uint8_t)(i>>16),(uint8_t)(i>>24)};
        crypto_generichash_blake2b_update(&s, c, 4);
        crypto_generichash_blake2b_final(&s, out, OUT); sink ^= out[0];
      }
      t[r] = (ns()-t0)/(double)n;
    }
    qsort(t, REPS, sizeof*t, cmpd); double refr = t[REPS/2];

    printf("%zu,%.1f,%.1f,%.1f\n", n, ours, ours2, refr);
  }

  printf("\n[bulk]\n");
  printf("bytes,c_ours_mbs,c_ref_mbs\n");
  for (size_t bi = 0; bi < sizeof BULK_N/sizeof*BULK_N; bi++) {
    size_t sz = BULK_N[bi];
    uint8_t *data = malloc(sz); memset(data, 7, sz);
    /* Small messages are far below clock resolution for a single hash --
     * 1 KiB reported an implausibly exact 1024 MB/s for both implementations.
     * Repeat until the timed region is >= ~20 ms, then divide. */
    size_t iters = (20u<<20) / sz; if (iters < 1) iters = 1;
    double a[REPS], b[REPS];
    for (int r = 0; r < REPS; r++) {
      ub_state *B = ub_aligned_alloc(ub_state_align(), ub_state_size());
      double t0 = ns();
      for (size_t it = 0; it < iters; it++) {
        ub_param Q; ub_param_init(&Q, 64); ub_init_param(B, &Q);
        ub_update(B, data, sz); ub_final(B, out, 64); sink ^= out[0];
      }
      double d = ns()-t0;
      a[r] = (double)sz*(double)iters/(d/1e9)/1e6;
      ub_aligned_free(B);

      t0 = ns();
      for (size_t it = 0; it < iters; it++) {
        crypto_generichash_blake2b_state cs;
        crypto_generichash_blake2b_init(&cs, NULL, 0, 64);
        crypto_generichash_blake2b_update(&cs, data, sz);
        crypto_generichash_blake2b_final(&cs, out, 64); sink ^= out[0];
      }
      d = ns()-t0;
      b[r] = (double)sz*(double)iters/(d/1e9)/1e6;
    }
    qsort(a, REPS, sizeof*a, cmpd); qsort(b, REPS, sizeof*b, cmpd);
    printf("%zu,%.0f,%.0f\n", sz, a[REPS/2], b[REPS/2]);
    free(data);
  }

  ub_aligned_free(S);
  (void)sink; return 0;
}
