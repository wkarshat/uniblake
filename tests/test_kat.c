/* Copyright (c) 2026 UniBlake Developers */
/* Published known-answer vectors, no oracle required.
 *
 * The other conformance suites compare against libsodium, which is the
 * stronger check -- an independent implementation agreeing on 45,000 shapes
 * catches more than a fixed list. But it is unavailable on a bare target, and
 * it cannot detect the two implementations sharing a mistake. These vectors
 * are published answers from the algorithm's authors, so they are independent
 * of any implementation and run anywhere.
 *
 * Coverage: input lengths 0..255 unkeyed and keyed, which spans the empty
 * message, sub-block, the 128-byte block boundary, and multi-block. Digest
 * length is fixed at 64 by the vectors; the digest-length range is covered by
 * tests/test_core.c against the oracle.
 */
#include "uniblake/uniblake.h"
#include "vendor/kat_blake2b.h"
#include "ub_alloc.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ok(int c, const char *what, int i) {
  checks++;
  if (!c) { fails++; if (fails < 6) printf("  FAIL %s [%d]\n", what, i); }
}

int main(void) {
  /* The vectors' input is the sequential byte string 00,01,02,... truncated to
   * the case's length, which is how the reference package defines them. */
  uint8_t in[UB_KAT_COUNT];
  for (int i = 0; i < UB_KAT_COUNT; ++i) in[i] = (uint8_t)i;

  uint8_t out[UB_KAT_OUTLEN];

  for (int i = 0; i < UB_KAT_COUNT; ++i) {
    ok(ub_hash(out, sizeof out, in, (size_t)i, NULL, 0) == UB_OK,
       "unkeyed call", i);
    ok(memcmp(out, ub_kat_unkeyed + (size_t)i * UB_KAT_OUTLEN,
              UB_KAT_OUTLEN) == 0, "unkeyed digest", i);
  }

  for (int i = 0; i < UB_KAT_COUNT; ++i) {
    ok(ub_hash(out, sizeof out, in, (size_t)i,
               ub_kat_key, sizeof ub_kat_key) == UB_OK, "keyed call", i);
    ok(memcmp(out, ub_kat_keyed + (size_t)i * UB_KAT_OUTLEN,
              UB_KAT_OUTLEN) == 0, "keyed digest", i);
  }

  /* Streaming must agree with the one-shot on the same vectors: absorbing in
   * pieces exercises the pending-block path that ub_hash bypasses. Two
   * chunkings, one splitting inside a block and one on the boundary. */
  ub_state *S = ub_aligned_alloc(ub_state_align(), ub_state_size());
  for (int i = 0; i < UB_KAT_COUNT; ++i) {
    for (int split = 1; split <= 2; ++split) {
      size_t at = (split == 1) ? (size_t)i / 2 : ((size_t)i > 128 ? 128 : 0);
      ub_init(S, UB_KAT_OUTLEN);
      ub_update(S, in, at);
      ub_update(S, in + at, (size_t)i - at);
      ub_final(S, out, sizeof out);
      ok(memcmp(out, ub_kat_unkeyed + (size_t)i * UB_KAT_OUTLEN,
                UB_KAT_OUTLEN) == 0, "streamed digest", i);
    }
  }
  ub_aligned_free(S);

  printf("kat: checks=%d fails=%d -> %s\n", checks, fails,
         fails ? "FAIL" : "PASS");
  return fails != 0;
}
