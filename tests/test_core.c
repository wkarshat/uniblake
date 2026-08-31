/* Core conformance vs libsodium: lengths, digest sizes, keys, salt/personal,
 * chunked updates, and the RFC 7693 "abc" KAT. */
#include "uniblake/uniblake.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Opaque state: allocate as a real consumer does. */
#define UBS(name) ub_state *name = alloca_state()
static void *pool[8]; static int pooln;
static ub_state *alloca_state(void){
  if(pooln<8 && !pool[pooln]) pool[pooln]=aligned_alloc(ub_state_align(),ub_state_size());
  return (ub_state*)pool[pooln++ % 8];
}
static void pool_reset(void){ pooln=0; }
static int checks, fails;
static void ok(int c, const char *w, long i){ checks++; if(!c){fails++; if(fails<6) printf("  FAIL %s [%ld]\n",w,i);} }
int main(void){
  if (sodium_init() < 0) return 77;
  uint8_t in[1024], a[64], b[64], key[64], salt[16], pers[16];
  for (size_t i=0;i<sizeof in;i++) in[i]=(uint8_t)(i*31+7);
  for (int i=0;i<64;i++) key[i]=(uint8_t)(i+1);
  for (int i=0;i<16;i++){ salt[i]=(uint8_t)(0x50+i); pers[i]=(uint8_t)(0xA0+i); }

  /* RFC 7693 Appendix A: BLAKE2b-512("abc") */
  static const uint8_t kat[64]={
    0xBA,0x80,0xA5,0x3F,0x98,0x1C,0x4D,0x0D,0x6A,0x27,0x97,0xB6,0x9F,0x12,0xF6,0xE9,
    0x4C,0x21,0x2F,0x14,0x68,0x5A,0xC4,0xB7,0x4B,0x12,0xBB,0x6F,0xDB,0xFF,0xA2,0xD1,
    0x7D,0x87,0xC5,0x39,0x2A,0xAB,0x79,0x2D,0xC2,0x52,0xD5,0xDE,0x45,0x33,0xCC,0x95,
    0x18,0xD3,0x8A,0xA8,0xDB,0xF1,0x92,0x5A,0xB9,0x23,0x86,0xED,0xD4,0x00,0x99,0x23};
  ok(ub_hash(a,64,"abc",3,NULL,0)==UB_OK,"kat call",0);
  ok(memcmp(a,kat,64)==0,"RFC 7693 abc KAT",0);

  /* lengths x digest sizes, unkeyed */
  for (size_t n=0;n<=600;n+=7)
    for (size_t ol=1; ol<=64; ol+=21){
      pool_reset(); UBS(Sp); ub_state *S = Sp; ub_init(S,ol); ub_update(S,in,n); ub_final(S,a,ol);
      crypto_generichash_blake2b(b,ol,in,n,NULL,0);
      ok(memcmp(a,b,ol)==0,"unkeyed",(long)n);
    }
  /* keyed */
  for (size_t kl=1; kl<=64; kl+=9)
    for (size_t n=0;n<=300;n+=11){
      pool_reset(); UBS(Sp); ub_state *S = Sp; ub_init_key(S,32,key,kl); ub_update(S,in,n); ub_final(S,a,32);
      crypto_generichash_blake2b(b,32,in,n,key,kl);
      ok(memcmp(a,b,32)==0,"keyed",(long)kl);
    }
  /* salt + personalization */
  for (size_t n=0;n<=300;n+=13){
    ub_param P; ub_param_init(&P,50);
    memcpy(P.salt,salt,16); memcpy(P.personal,pers,16);
    pool_reset(); UBS(Sp); ub_state *S = Sp; ub_init_param(S,&P); ub_update(S,in,n); ub_final(S,a,50);
    crypto_generichash_blake2b_state s;
    crypto_generichash_blake2b_init_salt_personal(&s,NULL,0,50,salt,pers);
    crypto_generichash_blake2b_update(&s,in,n);
    crypto_generichash_blake2b_final(&s,b,50);
    ok(memcmp(a,b,50)==0,"salt+personal",(long)n);
  }
  /* chunked updates must equal one-shot (streaming seam) */
  for (size_t step=1; step<=200; step+=7){
    pool_reset(); UBS(Sp); ub_state *S = Sp; ub_init(S,64);
    for (size_t o=0;o<600;o+=step){ size_t c=(600-o<step)?600-o:step; ub_update(S,in+o,c); }
    ub_final(S,a,64);
    crypto_generichash_blake2b(b,64,in,600,NULL,0);
    ok(memcmp(a,b,64)==0,"chunked",(long)step);
  }
  /* copy must be a value: continue from either side */
  { pool_reset(); UBS(Sp); UBS(Tp); ub_state *S=Sp,*T=Tp; ub_init(S,32); ub_update(S,in,200); ub_copy(T,S);
    ub_update(S,in+200,100); ub_update(T,in+200,100);
    ub_final(S,a,32); ub_final(T,b,32);
    ok(memcmp(a,b,32)==0,"copy independence",0); }
  /* error surface */
  { pool_reset(); UBS(Sp); ub_state *S = Sp; ub_init(S,32);
    ok(ub_final(S,a,10)==UB_E_OUTCAP,"outcap",0);
    ok(ub_final(S,a,32)==UB_OK,"final ok",0);
    ok(ub_final(S,a,32)==UB_E_STATE,"double final",0);
    ok(ub_update(S,in,1)==UB_E_STATE,"update after final",0);
    ok(ub_init(S,0)==UB_E_ARG,"outlen 0",0);
    ok(ub_init(S,65)==UB_E_ARG,"outlen 65",0); }
  printf("core: checks=%d fails=%d -> %s\n",checks,fails,fails?"FAIL":"PASS");
  return fails!=0;
}
