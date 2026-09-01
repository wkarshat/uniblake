/* Copyright (c) 2026 UniBlake Developers */
/* AArch64 NEON compression, 2 lanes of 64-bit within one message.
 *
 * Rows of the working vector are held as pairs in 128-bit registers, so each
 * G-function step is two 64-bit operations at once. Diagonalisation between
 * the column and diagonal halves of a round is register rotation.
 *
 * Follows the requirements in src/internal.h: advance S->t by 128 before each
 * block, read h/t/f, write only h and t.
 *
 * NEON is base ISA on aarch64, so no runtime probe is needed. Whether it is
 * FASTER is a separate question -- see backends/README.md.
 *
 * Formulations measured on an Apple M4 Pro, leaf shape, ns/digest:
 *
 *   141  original: scalar m[16] array, vector literals {m[a],m[b]}
 *   136  this one: message left in memory, pairs built with dup + lane load
 *   164  this one, but sigma resolved at compile time (12 literal ROUNDs)
 *   163  the above, twelve rounds unrolled with literal sigma indices
 *   163  same, pairs via vcombine_u64(vcreate_u64(..), ..)
 *   163  same, pairs via dup + lane load
 *   180  rot24/rot16 as two per-half vext instead of one tbl
 *
 * Two results worth keeping. Unrolling costs ~27 ns here regardless of how the
 * message is gathered -- the opposite of the scalar kernel, where it gained
 * 11%. And tbl beats a pair of ext for the byte rotations despite its higher
 * nominal latency, because ext needs the halves split and recombined.
 *
 * Literal sigma indices are not separable from unrolling here: each round's
 * MP() operands differ, so twelve bodies get emitted (157 -> 1056
 * instructions, 0 -> 24 spill stores) and the 9 sigma byte-loads they remove
 * do not pay for that. This is the opposite of src/compress.c and the reason
 * the AVX2 donor's sigma-free LOAD_MSG shape does not port to aarch64.
 *
 * The gather is the remaining cost: only 3 of 96 sigma pairs are adjacent in
 * memory, so a pair-load fast path buys nothing.
 */
#include "internal.h"

#if defined(__aarch64__)
#include <arm_neon.h>

static inline uint64x2_t rot32(uint64x2_t v){
  return vreinterpretq_u64_u32(vrev64q_u32(vreinterpretq_u32_u64(v)));
}
static inline uint64x2_t rot24(uint64x2_t v){
  const uint8x16_t idx = {3,4,5,6,7,0,1,2, 11,12,13,14,15,8,9,10};
  return vreinterpretq_u64_u8(vqtbl1q_u8(vreinterpretq_u8_u64(v), idx));
}
static inline uint64x2_t rot16(uint64x2_t v){
  const uint8x16_t idx = {2,3,4,5,6,7,0,1, 10,11,12,13,14,15,8,9};
  return vreinterpretq_u64_u8(vqtbl1q_u8(vreinterpretq_u8_u64(v), idx));
}
static inline uint64x2_t rot63(uint64x2_t v){
  return vsriq_n_u64(vshlq_n_u64(v, 1), v, 63);
}

#define G1(a,b,c,d,m0)  do{ a=vaddq_u64(vaddq_u64(a,b),m0); \
                            d=rot32(veorq_u64(d,a));        \
                            c=vaddq_u64(c,d);               \
                            b=rot24(veorq_u64(b,c)); }while(0)
#define G2(a,b,c,d,m1)  do{ a=vaddq_u64(vaddq_u64(a,b),m1); \
                            d=rot16(veorq_u64(d,a));        \
                            c=vaddq_u64(c,d);               \
                            b=rot63(veorq_u64(b,c)); }while(0)

/* rotate the b/c/d row pairs to move between column and diagonal steps */
#define DIAG(b0,b1,c0,c1,d0,d1) do{                     \
    uint64x2_t t_ = vextq_u64(b0,b1,1);                 \
    b1 = vextq_u64(b1,b0,1); b0 = t_;                   \
    t_ = c0; c0 = c1; c1 = t_;                          \
    t_ = vextq_u64(d1,d0,1);                            \
    d1 = vextq_u64(d0,d1,1); d0 = t_; }while(0)
#define UNDIAG(b0,b1,c0,c1,d0,d1) do{                   \
    uint64x2_t t_ = vextq_u64(b1,b0,1);                 \
    b1 = vextq_u64(b0,b1,1); b0 = t_;                   \
    t_ = c0; c0 = c1; c1 = t_;                          \
    t_ = vextq_u64(d0,d1,1);                            \
    d1 = vextq_u64(d1,d0,1); d0 = t_; }while(0)

/* `last` is the finalization mask, an argument rather than state: the state
 * carries no f[] words. Mirrors src/compress.c. */
static void neon_block(struct ub_state *S, const uint8_t *B, uint64_t last){
  {

    const uint64_t *M = (const uint64_t *)(const void *)B;
#define MP(x,y) vld1q_lane_u64(&M[y], vld1q_dup_u64(&M[x]), 1)

    uint64x2_t a0 = vld1q_u64(&S->h[0]), a1 = vld1q_u64(&S->h[2]);
    uint64x2_t b0 = vld1q_u64(&S->h[4]), b1 = vld1q_u64(&S->h[6]);
    uint64x2_t c0 = vld1q_u64(&ub_iv[0]), c1 = vld1q_u64(&ub_iv[2]);
    uint64_t tf[4] = { ub_iv[4]^S->t[0], ub_iv[5]^S->t[1],
                       ub_iv[6]^last,    ub_iv[7] };
    uint64x2_t d0 = vld1q_u64(&tf[0]), d1 = vld1q_u64(&tf[2]);

    /* Rows: a=(v0,v1) a1=(v2,v3), b=(v4,v5) b1=(v6,v7),
     *       c=(v8,v9) c1=(v10,v11), d=(v12,v13) d1=(v14,v15).
     * Column step  G(0..3) works on these directly.
     * Diagonal step G(4..7) needs b rotated left one lane, c swapped, d
     * rotated right one lane -- classic BLAKE2 SIMD diagonalisation. */
    for (int r = 0; r < 12; ++r) {
      const uint8_t *g = ub_sigma[r];
      /* column step: G(r,i, v[i], v[4+i], v[8+i], v[12+i]) for i=0..3 */
      uint64x2_t m0 = MP(g[0],g[2]), m1 = MP(g[4],g[6]);
      uint64x2_t m2 = MP(g[1],g[3]), m3 = MP(g[5],g[7]);
      G1(a0,b0,c0,d0,m0); G1(a1,b1,c1,d1,m1);
      G2(a0,b0,c0,d0,m2); G2(a1,b1,c1,d1,m3);
      DIAG(b0,b1,c0,c1,d0,d1);
      uint64x2_t m4 = MP(g[8],g[10]), m5 = MP(g[12],g[14]);
      uint64x2_t m6 = MP(g[9],g[11]), m7 = MP(g[13],g[15]);
      G1(a0,b0,c0,d0,m4); G1(a1,b1,c1,d1,m5);
      G2(a0,b0,c0,d0,m6); G2(a1,b1,c1,d1,m7);
      UNDIAG(b0,b1,c0,c1,d0,d1);
    }
#undef MP
    uint64x2_t h0 = vld1q_u64(&S->h[0]), h1 = vld1q_u64(&S->h[2]);
    uint64x2_t h2 = vld1q_u64(&S->h[4]), h3 = vld1q_u64(&S->h[6]);
    vst1q_u64(&S->h[0], veorq_u64(h0, veorq_u64(a0,c0)));
    vst1q_u64(&S->h[2], veorq_u64(h1, veorq_u64(a1,c1)));
    vst1q_u64(&S->h[4], veorq_u64(h2, veorq_u64(b0,d0)));
    vst1q_u64(&S->h[6], veorq_u64(h3, veorq_u64(b1,d1)));
  }
}

void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks){
  for (size_t k = 0; k < nblocks; ++k) {
    S->t[0] += UB_BLOCKBYTES;
    S->t[1] += (S->t[0] < UB_BLOCKBYTES);
    neon_block(S, blocks + k * UB_BLOCKBYTES, 0);
  }
}
void ub_compress_final(struct ub_state *S, const uint8_t *block){
  neon_block(S, block, (uint64_t)-1);
}
#endif
