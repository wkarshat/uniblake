/* Prefix-layer conformance vs libsodium: geometry, personalization, tail
 * encodings, batch, and non-mutation of the shared state. */
#include "uniblake/prefix.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks, fails;
static void ok(int c,const char*w,long i){checks++; if(!c){fails++; if(fails<6) printf("  FAIL %s [%ld]\n",w,i);}}
static void oracle(uint8_t*o,size_t ol,const uint8_t*pre,size_t pl,
                   const uint8_t*t,size_t tl,const uint8_t*pers){
  crypto_generichash_blake2b_state s;
  if(pers) crypto_generichash_blake2b_init_salt_personal(&s,NULL,0,ol,NULL,pers);
  else     crypto_generichash_blake2b_init(&s,NULL,0,ol);
  if(pl) crypto_generichash_blake2b_update(&s,pre,pl);
  if(tl) crypto_generichash_blake2b_update(&s,t,tl);
  crypto_generichash_blake2b_final(&s,o,ol);
}
static ub_state *mk(void){ return aligned_alloc(ub_state_align(),ub_state_size()); }
int main(void){
  if(sodium_init()<0) return 77;
  uint8_t pre[512],a[64],b[64],pers[16];
  for(size_t i=0;i<sizeof pre;i++) pre[i]=(uint8_t)(i*7+1);
  for(int i=0;i<16;i++) pers[i]=(uint8_t)(0xA0+i);
  ub_state *S=mk();

  /* geometry x digest length x personalization, LE32 tails */
  for(size_t pl=0; pl<=260; pl+=4)
    for(size_t ol=1; ol<=64; ol+=21)
      for(int pz=0; pz<2; pz++){
        ub_param P; ub_param_init(&P,ol);
        if(pz) memcpy(P.personal,pers,16);
        ub_init_param(S,&P); ub_update(S,pre,pl);
        size_t pend = pl ? ((pl-1)%128)+1 : 0;   /* full block retained */
        int fits = pend+4 <= 128;
        ok((ub_prefix_check(S,4)==UB_OK)==fits,"geometry",(long)pl);
        if(!fits) continue;
        for(uint32_t x=0;x<40;x++){
          ok(ub_hash_n(S,4,x,1,0,0,a,ol)==UB_OK,"hash_n rc",x);
          uint8_t t[4]={(uint8_t)x,(uint8_t)(x>>8),(uint8_t)(x>>16),(uint8_t)(x>>24)};
          oracle(b,ol,pre,pl,t,4,pz?pers:NULL);
          ok(memcmp(a,b,ol)==0,"hash_n vs oracle",x);
        }
      }
  /* LE64 boundaries */
  { ub_param P; ub_param_init(&P,50); ub_init_param(S,&P); ub_update(S,pre,140);
    uint64_t v[]={0,1,255,256,65535,65536,1ull<<32,~0ull};
    for(size_t i=0;i<8;i++){
      ub_hash_n(S,8,v[i],1,0,0,a,50);
      uint8_t t[8]; for(int j=0;j<8;j++) t[j]=(uint8_t)(v[i]>>(8*j));
      oracle(b,50,pre,140,t,8,NULL);
      ok(memcmp(a,b,50)==0,"le64",(long)i);
    } }
  /* raw tails of every length that fits */
  { ub_param P; ub_param_init(&P,32); ub_init_param(S,&P); ub_update(S,pre,100);
    for(size_t tl=0; tl<=28; tl++){
      ub_hash_tail(S,pre+300,tl,a,32);
      oracle(b,32,pre,100,pre+300,tl,NULL);
      ok(memcmp(a,b,32)==0,"raw tail",(long)tl);
    } }
  /* batch == single, and the shared state is not mutated */
  { ub_param P; ub_param_init(&P,50); ub_init_param(S,&P); ub_update(S,pre,140);
    uint8_t snap[512]; memcpy(snap,S,ub_state_size());
    size_t C=3000, st=64; uint8_t *m=malloc(C*st);
    ok(ub_hash_n(S,4,777,C,0,0,m,st)==UB_OK,"hash_n rc",0);
    ok(memcmp(snap,S,ub_state_size())==0,"state not mutated",0);
    for(size_t i=0;i<C;i++){
      ub_hash_n(S,4,777+i,1,0,0,a,50);
      ok(memcmp(a,m+i*st,50)==0,"batch vs single",(long)i);
    }
    free(m); }
  /* slice output must equal the same bytes of full digests */
  { ub_param P; ub_param_init(&P,48); ub_init_param(S,&P); ub_update(S,pre,140);
    size_t C=500;
    uint8_t *full=malloc(C*64), *sl=malloc(C*24);
    ub_hash_n(S,4,0,C,0,0,full,64);
    for(size_t off=0; off<=24; off+=24){
      ok(ub_hash_n(S,4,0,C,off,24,sl,24)==UB_OK,"slice rc",(long)off);
      for(size_t i=0;i<C;i++)
        ok(memcmp(sl+i*24, full+i*64+off, 24)==0,"slice bytes",(long)i);
    }
    /* slice bounds */
    ok(ub_hash_n(S,4,0,1,40,24,sl,24)==UB_E_ARG,"slice past end",0);
    ok(ub_hash_n(S,4,0,1,0,24,sl,8)==UB_E_OUTCAP,"stride < len",0);
    free(full); free(sl); }

  printf("prefix: checks=%d fails=%d -> %s\n",checks,fails,fails?"FAIL":"PASS");
  return fails!=0;
}
