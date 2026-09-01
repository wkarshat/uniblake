/* compat/ub_blake2.h must reproduce the BLAKE2 author reference byte for
 * byte. The vendored reference is the oracle, renamed ref_* so the shim's
 * unprefixed names and the reference can coexist here (see compat/ref). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ub_blake2.h"
#include "ref_blake2.h"

static int fails = 0, checks = 0;
static void cmp(const char *what, const void *a, const void *b, size_t n) {
  checks++;
  if (memcmp(a, b, n)) { fails++; printf("FAIL %s\n", what); }
}

int main(void) {
  unsigned char msg[600], key[64], got[64], want[64];
  for (size_t i = 0; i < sizeof msg; i++) msg[i] = (unsigned char)(i * 7 + 1);
  for (size_t i = 0; i < sizeof key; i++) key[i] = (unsigned char)(i + 3);

  ub_state *S = aligned_alloc(ub_state_align(), ub_state_size());
  ref_blake2b_state R;

  /* 1. streaming, unkeyed, across lengths and digest sizes */
  for (size_t outlen = 1; outlen <= 64; outlen++) {
    for (size_t n = 0; n <= 300; n += 17) {
      blake2b_init(S, outlen);
      blake2b_update(S, msg, n);
      blake2b_final(S, got, outlen);
      ref_blake2b_init(&R, outlen);
      ref_blake2b_update(&R, msg, n);
      ref_blake2b_final(&R, want, outlen);
      cmp("stream", got, want, outlen);
    }
  }

  /* 2. keyed */
  for (size_t keylen = 1; keylen <= 64; keylen++) {
    blake2b_init_key(S, 32, key, keylen);
    blake2b_update(S, msg, 250);
    blake2b_final(S, got, 32);
    ref_blake2b_init_key(&R, 32, key, keylen);
    ref_blake2b_update(&R, msg, 250);
    ref_blake2b_final(&R, want, 32);
    cmp("keyed", got, want, 32);
  }

  /* 3. parameter block: salt + personalization, the whole point of this API */
  {
    blake2b_param P;            /* == ub_param */
    ref_blake2b_param RP;
    ub_param_init(&P, 48);
    memset(&RP, 0, sizeof RP);
    RP.digest_length = 48; RP.fanout = 1; RP.depth = 1;
    for (int i = 0; i < 16; i++) {
      P.salt[i]     = RP.salt[i]     = (unsigned char)(0xA0 + i);
      P.personal[i] = RP.personal[i] = (unsigned char)(0x50 + i);
    }
    blake2b_init_param(S, &P);
    blake2b_update(S, msg, 400);
    blake2b_final(S, got, 48);
    ref_blake2b_init_param(&R, &RP);
    ref_blake2b_update(&R, msg, 400);
    ref_blake2b_final(&R, want, 48);
    cmp("param salt/personal", got, want, 48);
  }

  /* 4. one-shot, key last */
  blake2b(got, 64, msg, 333, key, 32);
  ref_blake2b(want, 64, msg, 333, key, 32);
  cmp("one-shot", got, want, 64);

  free(S);
  printf("blake2-alias: checks=%d fails=%d -> %s\n",
         checks, fails, fails ? "FAIL" : "PASS");
  return fails != 0;
}
