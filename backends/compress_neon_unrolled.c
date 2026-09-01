/* AArch64 NEON compression, unrolled, using the reference package's round and
 * message-load macros.
 *
 * The twelve rounds are unrolled and each message vector is assembled by
 * LOAD_MSG_r_n from words already in registers, so the sigma permutation is
 * resolved at compile time and no lane inserts remain. compress_neon.c takes
 * the other approach, indexing ub_sigma at runtime; it measures faster on this
 * core because the unrolled form spills. See backends/README.md.
 *
 * Donor macros are vendored under backends/vendor; see its README for source,
 * pin and license. Only the surrounding kernel -- the multi-block loop, the
 * counter handling, and the state interface -- is this project's.
 *
 * Follows the requirements in src/internal.h: advance S->t by 128 before each
 * block, read h/t/f, write only h and t.
 */
#include "internal.h"

#if defined(__aarch64__)
#include <arm_neon.h>

#include "vendor/blake2b-round.h"
#include "vendor/blake2b-load-neon.h"

void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks) {
  const uint64x2_t iv0 = vld1q_u64(&ub_iv[0]);
  const uint64x2_t iv1 = vld1q_u64(&ub_iv[2]);
  const uint64x2_t iv2 = vld1q_u64(&ub_iv[4]);
  const uint64x2_t iv3 = vld1q_u64(&ub_iv[6]);

  for (size_t k = 0; k < nblocks; ++k) {
    S->t[0] += UB_BLOCKBYTES;
    S->t[1] += (S->t[0] < UB_BLOCKBYTES);
    const uint8_t *block = blocks + k * UB_BLOCKBYTES;

    /* The donor's LOAD_MSG macros read m0..m7 by name. Loading as bytes and
     * reinterpreting is a plain little-endian load on this target and needs no
     * alignment: vld1q_u8 permits any address. */
    const uint64x2_t m0 = vreinterpretq_u64_u8(vld1q_u8(block +   0));
    const uint64x2_t m1 = vreinterpretq_u64_u8(vld1q_u8(block +  16));
    const uint64x2_t m2 = vreinterpretq_u64_u8(vld1q_u8(block +  32));
    const uint64x2_t m3 = vreinterpretq_u64_u8(vld1q_u8(block +  48));
    const uint64x2_t m4 = vreinterpretq_u64_u8(vld1q_u8(block +  64));
    const uint64x2_t m5 = vreinterpretq_u64_u8(vld1q_u8(block +  80));
    const uint64x2_t m6 = vreinterpretq_u64_u8(vld1q_u8(block +  96));
    const uint64x2_t m7 = vreinterpretq_u64_u8(vld1q_u8(block + 112));

    uint64x2_t row1l = vld1q_u64(&S->h[0]), row1h = vld1q_u64(&S->h[2]);
    uint64x2_t row2l = vld1q_u64(&S->h[4]), row2h = vld1q_u64(&S->h[6]);
    uint64x2_t row3l = iv0, row3h = iv1;

    const uint64_t tf[4] = { S->t[0], S->t[1], S->f[0], S->f[1] };
    uint64x2_t row4l = veorq_u64(iv2, vld1q_u64(&tf[0]));
    uint64x2_t row4h = veorq_u64(iv3, vld1q_u64(&tf[2]));

    /* The donor macros write these by name: t0/t1 in DIAGONALIZE and
     * UNDIAGONALIZE, b0/b1 in ROUND's LOAD_MSG steps. */
    uint64x2_t t0, t1, b0, b1;

    ROUND(0);  ROUND(1);  ROUND(2);  ROUND(3);
    ROUND(4);  ROUND(5);  ROUND(6);  ROUND(7);
    ROUND(8);  ROUND(9);  ROUND(10); ROUND(11);

    vst1q_u64(&S->h[0], veorq_u64(vld1q_u64(&S->h[0]), veorq_u64(row1l, row3l)));
    vst1q_u64(&S->h[2], veorq_u64(vld1q_u64(&S->h[2]), veorq_u64(row1h, row3h)));
    vst1q_u64(&S->h[4], veorq_u64(vld1q_u64(&S->h[4]), veorq_u64(row2l, row4l)));
    vst1q_u64(&S->h[6], veorq_u64(vld1q_u64(&S->h[6]), veorq_u64(row2h, row4h)));
  }
}

void ub_compress_final(struct ub_state *S, const uint8_t *block) {
  uint64_t s0 = S->t[0], s1 = S->t[1];
  S->t[0] -= UB_BLOCKBYTES; S->t[1] -= (s0 < UB_BLOCKBYTES);
  ub_compress(S, block, 1);
  S->t[0] = s0; S->t[1] = s1;
}
#endif
