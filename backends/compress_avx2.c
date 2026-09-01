/* Copyright (c) 2026 UniBlake Developers */
/* x86-64 AVX2 compression.
 *
 * A 256-bit register holds a full four-word BLAKE2b row, so the whole working
 * vector is four registers and the round macros take one message operand
 * rather than a pair. The twelve rounds are unrolled and the sigma
 * permutation is resolved at compile time.
 *
 * The round and message-load macros are vendored under vendor/libsodium; see
 * its README for source, pin and license. This file is the surrounding kernel:
 * the multi-block loop, the counter handling, and the state interface.
 *
 * Follows the requirements in src/internal.h: advance S->t by 128 before each
 * block, read h/t/f, write only h and t.
 *
 * Requires -mavx2, so it is never part of the default build. Build with
 *   make bench-avx2      (measure)
 *   make check-avx2      (conformance)
 * on a machine whose CPU has AVX2 -- `make probe` reports whether it does.
 */
#include "internal.h"

#if defined(__AVX2__)
#include <immintrin.h>

/* The vendored macros read these names from the enclosing scope. blake2b_IV is
 * ours, aliased to the name the donor expects; ub_iv holds the same constants
 * and is already linked in via src/const.c. */
#define blake2b_IV ub_iv

/* LOADU and STOREU come from the vendored header. */
#define BLAKE2_USE_SSSE3
#define BLAKE2_USE_SSE41
#define BLAKE2_USE_AVX2

#include "vendor/libsodium/blake2b-compress-avx2.h"

void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks) {
  for (size_t k = 0; k < nblocks; ++k) {
    S->t[0] += UB_BLOCKBYTES;
    S->t[1] += (S->t[0] < UB_BLOCKBYTES);

    __m256i a = LOADU(&S->h[0]);
    __m256i b = LOADU(&S->h[4]);
    BLAKE2B_COMPRESS_V1(a, b, blocks + k * UB_BLOCKBYTES,
                        S->t[0], S->t[1], S->f[0], S->f[1]);
    STOREU(&S->h[0], a);
    STOREU(&S->h[4], b);
  }
}

void ub_compress_final(struct ub_state *S, const uint8_t *block) {
  uint64_t s0 = S->t[0], s1 = S->t[1];
  S->t[0] -= UB_BLOCKBYTES; S->t[1] -= (s0 < UB_BLOCKBYTES);
  ub_compress(S, block, 1);
  S->t[0] = s0; S->t[1] = s1;
}
#endif
