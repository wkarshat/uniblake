/* Proves the conformance suites can FAIL.
 *
 * A test that always passes proves nothing. This one links a deliberately
 * wrong compression function — the real one with the last round removed — and
 * requires that the checks the other suites rely on reject it.
 *
 * Without this, a mistake that made the oracle comparison vacuous (comparing a
 * value against itself, say) would look exactly like success.
 *
 * Built by `make check-negative`; not part of the library.
 */
#include "internal.h"
#include "uniblake/prefix.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ub_alloc.h"

/* --- the wrong kernel: eleven rounds instead of twelve --- */
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

void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks) {
  for (size_t n = 0; n < nblocks; ++n) {
    S->t[0] += UB_BLOCKBYTES; S->t[1] += (S->t[0] < UB_BLOCKBYTES);
    const uint8_t *block = blocks + n * UB_BLOCKBYTES;
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; ++i) m[i] = ub_load64(block + i * 8);
    for (int i = 0; i < 8;  ++i) v[i] = S->h[i];
    v[ 8] = ub_iv[0]; v[ 9] = ub_iv[1]; v[10] = ub_iv[2]; v[11] = ub_iv[3];
    v[12] = ub_iv[4] ^ S->t[0]; v[13] = ub_iv[5] ^ S->t[1];
    v[14] = ub_iv[6] ^ S->f[0]; v[15] = ub_iv[7] ^ S->f[1];
    for (int r = 0; r < 11; ++r) {          /* <-- one round short */
      G(r,0, v[0],v[4],v[ 8],v[12]);  G(r,1, v[1],v[5],v[ 9],v[13]);
      G(r,2, v[2],v[6],v[10],v[14]);  G(r,3, v[3],v[7],v[11],v[15]);
      G(r,4, v[0],v[5],v[10],v[15]);  G(r,5, v[1],v[6],v[11],v[12]);
      G(r,6, v[2],v[7],v[ 8],v[13]);  G(r,7, v[3],v[4],v[ 9],v[14]);
    }
    for (int i = 0; i < 8; ++i) S->h[i] ^= v[i] ^ v[i + 8];
  }
}
void ub_compress_final(struct ub_state *S, const uint8_t *block) {
  uint64_t s0 = S->t[0], s1 = S->t[1];
  S->t[0] -= UB_BLOCKBYTES; S->t[1] -= (s0 < UB_BLOCKBYTES);
  ub_compress(S, block, 1);
  S->t[0] = s0; S->t[1] = s1;
}

/* --- the checks that must reject it --- */
static int caught, missed;
static void must_differ(int differs, const char *what){
  if (differs) caught++;
  else { missed++; printf("  NOT CAUGHT: %s\n", what); }
}

int main(void){
  if (sodium_init() < 0) return 77;
  uint8_t in[600], a[64], b[64], key[32], pers[16];
  for (size_t i=0;i<sizeof in;i++) in[i]=(uint8_t)(i*31+7);
  for (int i=0;i<32;i++) key[i]=(uint8_t)(i+1);
  for (int i=0;i<16;i++) pers[i]=(uint8_t)(0xA0+i);
  ub_state *S = ub_aligned_alloc(ub_state_align(), ub_state_size());

  /* the RFC 7693 published vector */
  static const uint8_t kat[64]={
    0xBA,0x80,0xA5,0x3F,0x98,0x1C,0x4D,0x0D,0x6A,0x27,0x97,0xB6,0x9F,0x12,0xF6,0xE9,
    0x4C,0x21,0x2F,0x14,0x68,0x5A,0xC4,0xB7,0x4B,0x12,0xBB,0x6F,0xDB,0xFF,0xA2,0xD1,
    0x7D,0x87,0xC5,0x39,0x2A,0xAB,0x79,0x2D,0xC2,0x52,0xD5,0xDE,0x45,0x33,0xCC,0x95,
    0x18,0xD3,0x8A,0xA8,0xDB,0xF1,0x92,0x5A,0xB9,0x23,0x86,0xED,0xD4,0x00,0x99,0x23};
  ub_hash(a,64,"abc",3,NULL,0);
  must_differ(memcmp(a,kat,64)!=0, "RFC 7693 vector");

  /* oracle comparison, one block and many blocks */
  for (size_t n = 0; n <= 400; n += 137) {
    ub_init(S,32); ub_update(S,in,n); ub_final(S,a,32);
    crypto_generichash_blake2b(b,32,in,n,NULL,0);
    must_differ(memcmp(a,b,32)!=0, "oracle, unkeyed");
  }
  /* keyed */
  ub_init_key(S,32,key,32); ub_update(S,in,200); ub_final(S,a,32);
  crypto_generichash_blake2b(b,32,in,200,key,32);
  must_differ(memcmp(a,b,32)!=0, "oracle, keyed");

  /* personalized, via the parameter block */
  ub_param P; ub_param_init(&P,50); memcpy(P.personal,pers,16);
  ub_init_param(S,&P); ub_update(S,in,140); ub_final(S,a,50);
  { crypto_generichash_blake2b_state s;
    crypto_generichash_blake2b_init_salt_personal(&s,NULL,0,50,NULL,pers);
    crypto_generichash_blake2b_update(&s,in,140);
    crypto_generichash_blake2b_final(&s,b,50); }
  must_differ(memcmp(a,b,50)!=0, "oracle, personalized");

  /* the prefix path */
  ub_init_param(S,&P); ub_update(S,in,140);
  ub_hash_n(S,4,0,1,0,0,a,50);
  { crypto_generichash_blake2b_state s;
    crypto_generichash_blake2b_init_salt_personal(&s,NULL,0,50,NULL,pers);
    crypto_generichash_blake2b_update(&s,in,140);
    uint8_t c[4]={0,0,0,0};
    crypto_generichash_blake2b_update(&s,c,4);
    crypto_generichash_blake2b_final(&s,b,50); }
  must_differ(memcmp(a,b,50)!=0, "oracle, prefix path");

  ub_aligned_free(S);
  printf("negative: %d rejections, %d missed -> %s\n",
         caught, missed, missed ? "FAIL" : "PASS");
  return missed != 0;
}
