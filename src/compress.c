/* Copyright (c) 2026 UniBlake Developers */
/* Portable scalar compression function (RFC 7693 §3.2).
 *
 * This is the one function a SIMD, threaded, or offload implementation
 * replaces; internal.h describes how. */
#include "internal.h"

#define GK(x,y,a,b,c,d)                                   \
  do {                                                     \
    a = a + b + m[x];                                      \
    d = ub_rotr64(d ^ a, 32);                              \
    c = c + d;                                             \
    b = ub_rotr64(b ^ c, 24);                              \
    a = a + b + m[y];                                      \
    d = ub_rotr64(d ^ a, 16);                              \
    c = c + d;                                             \
    b = ub_rotr64(b ^ c, 63);                              \
  } while (0)

#define G(r,i,a,b,c,d)                                     \
  do {                                                     \
    a = a + b + m[ub_sigma[r][2*i+0]];                     \
    d = ub_rotr64(d ^ a, 32);                              \
    c = c + d;                                             \
    b = ub_rotr64(b ^ c, 24);                              \
    a = a + b + m[ub_sigma[r][2*i+1]];                     \
    d = ub_rotr64(d ^ a, 16);                              \
    c = c + d;                                             \
    b = ub_rotr64(b ^ c, 63);                              \
  } while (0)

/* Compresses `nblocks` consecutive full blocks, advancing S->t by
 * UB_BLOCKBYTES BEFORE each one. The kernel owns the counter for every block
 * it is given -- that is what lets a batched or offloaded kernel run a whole
 * range without returning to the core between blocks.
 *
 * Finalization is the exception: ub_final sets t and f itself and passes a
 * single already-accounted block, so it calls the raw body via nblocks = 0
 * semantics -- see ub_compress_final. */
/* One block, counter and flags already set by the caller. Both entry points
 * below build on this, so the round code exists once. */
static void compress_block(struct ub_state *S, const uint8_t *block, uint64_t last) {
  {
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; ++i) m[i] = ub_load64(block + i * 8);
    for (int i = 0; i < 8;  ++i) v[i] = S->h[i];
    v[ 8] = ub_iv[0]; v[ 9] = ub_iv[1]; v[10] = ub_iv[2]; v[11] = ub_iv[3];
    v[12] = ub_iv[4] ^ S->t[0];
    v[13] = ub_iv[5] ^ S->t[1];
    v[14] = ub_iv[6] ^ last;
    v[15] = ub_iv[7];   /* f[1] is the tree-hashing last-node flag; this
                         * library hashes sequentially, so it is always 0. */
      GK( 0, 1, v[0],v[4],v[ 8],v[12]);
      GK( 2, 3, v[1],v[5],v[ 9],v[13]);
      GK( 4, 5, v[2],v[6],v[10],v[14]);
      GK( 6, 7, v[3],v[7],v[11],v[15]);
      GK( 8, 9, v[0],v[5],v[10],v[15]);
      GK(10,11, v[1],v[6],v[11],v[12]);
      GK(12,13, v[2],v[7],v[ 8],v[13]);
      GK(14,15, v[3],v[4],v[ 9],v[14]);
      GK(14,10, v[0],v[4],v[ 8],v[12]);
      GK( 4, 8, v[1],v[5],v[ 9],v[13]);
      GK( 9,15, v[2],v[6],v[10],v[14]);
      GK(13, 6, v[3],v[7],v[11],v[15]);
      GK( 1,12, v[0],v[5],v[10],v[15]);
      GK( 0, 2, v[1],v[6],v[11],v[12]);
      GK(11, 7, v[2],v[7],v[ 8],v[13]);
      GK( 5, 3, v[3],v[4],v[ 9],v[14]);
      GK(11, 8, v[0],v[4],v[ 8],v[12]);
      GK(12, 0, v[1],v[5],v[ 9],v[13]);
      GK( 5, 2, v[2],v[6],v[10],v[14]);
      GK(15,13, v[3],v[7],v[11],v[15]);
      GK(10,14, v[0],v[5],v[10],v[15]);
      GK( 3, 6, v[1],v[6],v[11],v[12]);
      GK( 7, 1, v[2],v[7],v[ 8],v[13]);
      GK( 9, 4, v[3],v[4],v[ 9],v[14]);
      GK( 7, 9, v[0],v[4],v[ 8],v[12]);
      GK( 3, 1, v[1],v[5],v[ 9],v[13]);
      GK(13,12, v[2],v[6],v[10],v[14]);
      GK(11,14, v[3],v[7],v[11],v[15]);
      GK( 2, 6, v[0],v[5],v[10],v[15]);
      GK( 5,10, v[1],v[6],v[11],v[12]);
      GK( 4, 0, v[2],v[7],v[ 8],v[13]);
      GK(15, 8, v[3],v[4],v[ 9],v[14]);
      GK( 9, 0, v[0],v[4],v[ 8],v[12]);
      GK( 5, 7, v[1],v[5],v[ 9],v[13]);
      GK( 2, 4, v[2],v[6],v[10],v[14]);
      GK(10,15, v[3],v[7],v[11],v[15]);
      GK(14, 1, v[0],v[5],v[10],v[15]);
      GK(11,12, v[1],v[6],v[11],v[12]);
      GK( 6, 8, v[2],v[7],v[ 8],v[13]);
      GK( 3,13, v[3],v[4],v[ 9],v[14]);
      GK( 2,12, v[0],v[4],v[ 8],v[12]);
      GK( 6,10, v[1],v[5],v[ 9],v[13]);
      GK( 0,11, v[2],v[6],v[10],v[14]);
      GK( 8, 3, v[3],v[7],v[11],v[15]);
      GK( 4,13, v[0],v[5],v[10],v[15]);
      GK( 7, 5, v[1],v[6],v[11],v[12]);
      GK(15,14, v[2],v[7],v[ 8],v[13]);
      GK( 1, 9, v[3],v[4],v[ 9],v[14]);
      GK(12, 5, v[0],v[4],v[ 8],v[12]);
      GK( 1,15, v[1],v[5],v[ 9],v[13]);
      GK(14,13, v[2],v[6],v[10],v[14]);
      GK( 4,10, v[3],v[7],v[11],v[15]);
      GK( 0, 7, v[0],v[5],v[10],v[15]);
      GK( 6, 3, v[1],v[6],v[11],v[12]);
      GK( 9, 2, v[2],v[7],v[ 8],v[13]);
      GK( 8,11, v[3],v[4],v[ 9],v[14]);
      GK(13,11, v[0],v[4],v[ 8],v[12]);
      GK( 7,14, v[1],v[5],v[ 9],v[13]);
      GK(12, 1, v[2],v[6],v[10],v[14]);
      GK( 3, 9, v[3],v[7],v[11],v[15]);
      GK( 5, 0, v[0],v[5],v[10],v[15]);
      GK(15, 4, v[1],v[6],v[11],v[12]);
      GK( 8, 6, v[2],v[7],v[ 8],v[13]);
      GK( 2,10, v[3],v[4],v[ 9],v[14]);
      GK( 6,15, v[0],v[4],v[ 8],v[12]);
      GK(14, 9, v[1],v[5],v[ 9],v[13]);
      GK(11, 3, v[2],v[6],v[10],v[14]);
      GK( 0, 8, v[3],v[7],v[11],v[15]);
      GK(12, 2, v[0],v[5],v[10],v[15]);
      GK(13, 7, v[1],v[6],v[11],v[12]);
      GK( 1, 4, v[2],v[7],v[ 8],v[13]);
      GK(10, 5, v[3],v[4],v[ 9],v[14]);
      GK(10, 2, v[0],v[4],v[ 8],v[12]);
      GK( 8, 4, v[1],v[5],v[ 9],v[13]);
      GK( 7, 6, v[2],v[6],v[10],v[14]);
      GK( 1, 5, v[3],v[7],v[11],v[15]);
      GK(15,11, v[0],v[5],v[10],v[15]);
      GK( 9,14, v[1],v[6],v[11],v[12]);
      GK( 3,12, v[2],v[7],v[ 8],v[13]);
      GK(13, 0, v[3],v[4],v[ 9],v[14]);
      GK( 0, 1, v[0],v[4],v[ 8],v[12]);
      GK( 2, 3, v[1],v[5],v[ 9],v[13]);
      GK( 4, 5, v[2],v[6],v[10],v[14]);
      GK( 6, 7, v[3],v[7],v[11],v[15]);
      GK( 8, 9, v[0],v[5],v[10],v[15]);
      GK(10,11, v[1],v[6],v[11],v[12]);
      GK(12,13, v[2],v[7],v[ 8],v[13]);
      GK(14,15, v[3],v[4],v[ 9],v[14]);
      GK(14,10, v[0],v[4],v[ 8],v[12]);
      GK( 4, 8, v[1],v[5],v[ 9],v[13]);
      GK( 9,15, v[2],v[6],v[10],v[14]);
      GK(13, 6, v[3],v[7],v[11],v[15]);
      GK( 1,12, v[0],v[5],v[10],v[15]);
      GK( 0, 2, v[1],v[6],v[11],v[12]);
      GK(11, 7, v[2],v[7],v[ 8],v[13]);
      GK( 5, 3, v[3],v[4],v[ 9],v[14]);
    for (int i = 0; i < 8; ++i) S->h[i] ^= v[i] ^ v[i + 8];
  }
}

#ifdef UB_KERNEL_RUNTIME
void ub_compress_scalar(struct ub_state *S, const uint8_t *blocks, size_t nblocks)
#else
void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks)
#endif
{
  for (size_t n = 0; n < nblocks; ++n) {
    S->t[0] += UB_BLOCKBYTES; S->t[1] += (S->t[0] < UB_BLOCKBYTES);
    compress_block(S, blocks + n * UB_BLOCKBYTES, 0);
  }
}

/* One block, counter and flags already set by the caller (finalization). */
/* Finalization: the counter and flags are the caller's, so this compresses
 * directly instead of decrementing t, calling ub_compress, and restoring it. */
void ub_compress_final(struct ub_state *S, const uint8_t *block) {
  compress_block(S, block, (uint64_t)-1);
}

#ifdef UB_KERNEL_RUNTIME
/* Runtime selection. Default is the built-in scalar kernel above. */
ub_compress_fn ub_active_compress = ub_compress_scalar;
int ub_kernel_set(ub_compress_fn fn) {
  ub_active_compress = fn ? fn : ub_compress_scalar;
  return UB_OK;
}
#endif
