/* Concurrent ub_hash_n: split the digest range across POSIX threads.
 *
 * This replaces the BATCH entry point, not the compression function. Each
 * digest is independent and the prefix state is read-only, so a range splits
 * with no coordination: every thread copies the state itself and writes into
 * its own slice of the output.
 *
 * Link this instead of the ub_hash_n in src/prefix.c. Requires -lpthread.
 * UB_THREADS sets the thread count; below UB_THREAD_MIN digests the range is
 * run inline, since thread creation costs more than the work.
 */
#include "internal.h"
#include "uniblake/prefix.h"
#include <pthread.h>
#include <stdlib.h>

#ifndef UB_THREADS
#define UB_THREADS 4
#endif
#ifndef UB_THREAD_MIN
#define UB_THREAD_MIN 512
#endif

int ub_hash_n_serial(const ub_state *S, size_t tailwidth, uint64_t first,
                     size_t n, size_t off, size_t len,
                     void *out, size_t stride);

struct span {
  const ub_state *S; size_t tailwidth; uint64_t first; size_t n;
  size_t off, len; uint8_t *out; size_t stride; int rc;
};

static void *run(void *arg){
  struct span *w = arg;
  w->rc = ub_hash_n_serial(w->S, w->tailwidth, w->first, w->n,
                           w->off, w->len, w->out, w->stride);
  return 0;
}

int ub_hash_n(const ub_state *S, size_t tailwidth, uint64_t first, size_t n,
              size_t off, size_t len, void *out, size_t stride) {
  if (n < UB_THREAD_MIN)
    return ub_hash_n_serial(S, tailwidth, first, n, off, len, out, stride);

  /* Validate once here so a bad call fails before any thread starts. */
  int rc = ub_hash_n_serial(S, tailwidth, first, 0, off, len, out, stride);
  if (rc != UB_OK) return rc;

  pthread_t th[UB_THREADS];
  struct span w[UB_THREADS];
  size_t per = n / UB_THREADS, extra = n % UB_THREADS, at = 0;
  int live[UB_THREADS];

  /* Assign every span BEFORE creating any thread. A failed pthread_create
   * must not leave later entries unassigned: they would be read below with
   * garbage n, out and rc -- running spans over a wild pointer and letting a
   * batch that never produced its digests still return UB_OK. */
  for (int i = 0; i < UB_THREADS; ++i) {
    size_t take = per + (i < (int)extra ? 1 : 0);
    w[i] = (struct span){ S, tailwidth, first + at, take, off, len,
                          (uint8_t *)out + at * stride, stride, UB_OK };
    at += take;
    live[i] = 0;
  }

  /* The caller's own thread runs the last span rather than idling, so it is
   * never handed to pthread_create. */
  for (int i = 0; i < UB_THREADS - 1; ++i) {
    if (w[i].n == 0) continue;
    if (pthread_create(&th[i], 0, run, &w[i]) == 0) live[i] = 1;
    /* On failure the span stays unrun and is picked up inline below. */
  }
  run(&w[UB_THREADS - 1]);

  for (int i = 0; i < UB_THREADS - 1; ++i)
    if (live[i]) pthread_join(th[i], 0);

  /* Any span whose thread never started still has work to do. */
  for (int i = 0; i < UB_THREADS - 1; ++i)
    if (!live[i] && w[i].n) run(&w[i]);

  for (int i = 0; i < UB_THREADS; ++i)
    if (w[i].rc != UB_OK) return w[i].rc;
  return UB_OK;
}
