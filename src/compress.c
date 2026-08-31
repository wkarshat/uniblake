/* Portable scalar compression function (RFC 7693 §3.2).
 *
 * This is the one function a SIMD, threaded, or offload implementation
 * replaces; internal.h describes how. */
#include "internal.h"

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
#ifdef UB_KERNEL_RUNTIME
void ub_compress_scalar(struct ub_state *S, const uint8_t *blocks, size_t nblocks)
#else
void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks)
#endif
{
  for (size_t n = 0; n < nblocks; ++n) {
    S->t[0] += UB_BLOCKBYTES; S->t[1] += (S->t[0] < UB_BLOCKBYTES);
    const uint8_t *block = blocks + n * UB_BLOCKBYTES;
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; ++i) m[i] = ub_load64(block + i * 8);
    for (int i = 0; i < 8;  ++i) v[i] = S->h[i];
    v[ 8] = ub_iv[0]; v[ 9] = ub_iv[1]; v[10] = ub_iv[2]; v[11] = ub_iv[3];
    v[12] = ub_iv[4] ^ S->t[0];
    v[13] = ub_iv[5] ^ S->t[1];
    v[14] = ub_iv[6] ^ S->f[0];
    v[15] = ub_iv[7] ^ S->f[1];
    for (int r = 0; r < 12; ++r) {
      G(r,0, v[0],v[4],v[ 8],v[12]);
      G(r,1, v[1],v[5],v[ 9],v[13]);
      G(r,2, v[2],v[6],v[10],v[14]);
      G(r,3, v[3],v[7],v[11],v[15]);
      G(r,4, v[0],v[5],v[10],v[15]);
      G(r,5, v[1],v[6],v[11],v[12]);
      G(r,6, v[2],v[7],v[ 8],v[13]);
      G(r,7, v[3],v[4],v[ 9],v[14]);
    }
    for (int i = 0; i < 8; ++i) S->h[i] ^= v[i] ^ v[i + 8];
  }
}

/* One block, counter and flags already set by the caller (finalization). */
void ub_compress_final(struct ub_state *S, const uint8_t *block) {
  uint64_t save0 = S->t[0], save1 = S->t[1];
  S->t[0] -= UB_BLOCKBYTES; S->t[1] -= (save0 < UB_BLOCKBYTES);
  ub_compress(S, block, 1);
  S->t[0] = save0; S->t[1] = save1;
}

#ifdef UB_KERNEL_RUNTIME
/* Runtime selection. Default is the built-in scalar kernel above. */
ub_compress_fn ub_active_compress = ub_compress_scalar;
int ub_kernel_set(ub_compress_fn fn) {
  ub_active_compress = fn ? fn : ub_compress_scalar;
  return UB_OK;
}
#endif
