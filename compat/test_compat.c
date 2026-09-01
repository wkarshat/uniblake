/* The adapters must reproduce libsodium byte-for-byte.
 * Real libsodium is the oracle; the shim is renamed to avoid collision. */
#include <sodium.h>
#include "uniblake/uniblake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ub_alloc.h"

/* what compat_sodium.h defines, inlined here under ub_* names so both the
 * real libsodium and the shim are callable in one TU */
static int shim_init_sp(ub_state *S,const unsigned char*k,size_t kl,size_t ol,
                        const unsigned char*salt,const unsigned char*pers){
  ub_param P; ub_param_init(&P,ol);
  if(salt) memcpy(P.salt,salt,UB_SALTBYTES);
  if(pers) memcpy(P.personal,pers,UB_PERSONALBYTES);
  if(kl){ P.key_length=(uint8_t)kl; int rc=ub_init_param(S,&P); if(rc) return rc;
    unsigned char b[UB_BLOCKBYTES]; memset(b,0,sizeof b); memcpy(b,k,kl);
    return ub_update(S,b,UB_BLOCKBYTES); }
  return ub_init_param(S,&P);
}
static int checks,fails;
static void ok(int c,const char*w,long i){checks++;if(!c){fails++;if(fails<6)printf("  FAIL %s [%ld]\n",w,i);}}
int main(void){
  if(sodium_init()<0) return 77;
  unsigned char in[400],a[64],b[64],key[64],salt[16],pers[16];
  for(size_t i=0;i<sizeof in;i++) in[i]=(unsigned char)(i*11+3);
  for(int i=0;i<64;i++) key[i]=(unsigned char)(i*3+1);
  for(int i=0;i<16;i++){salt[i]=(unsigned char)(0x30+i);pers[i]=(unsigned char)(0x70+i);}
  ub_state *S=ub_aligned_alloc(ub_state_align(),ub_state_size());

  /* personalization only: no key, no salt */
  unsigned char zp[16]={0}; memcpy(zp,"testtag1",8);
  uint32_t leN=192,leK=7; memcpy(zp+8,&leN,4); memcpy(zp+12,&leK,4);
  for(size_t n=0;n<=300;n+=7){
    shim_init_sp(S,NULL,0,48,NULL,zp); ub_update(S,in,n); ub_final(S,a,48);
    crypto_generichash_blake2b_state s;
    crypto_generichash_blake2b_init_salt_personal(&s,NULL,0,48,NULL,zp);
    crypto_generichash_blake2b_update(&s,in,n);
    crypto_generichash_blake2b_final(&s,b,48);
    ok(memcmp(a,b,48)==0,"ZERO_PoW shape",(long)n);
  }
  /* keyed + salt + personal: the padding path the old shim got wrong */
  for(size_t kl=1;kl<=64;kl+=7){
    shim_init_sp(S,key,kl,50,salt,pers); ub_update(S,in,200); ub_final(S,a,50);
    crypto_generichash_blake2b_state s;
    crypto_generichash_blake2b_init_salt_personal(&s,key,kl,50,salt,pers);
    crypto_generichash_blake2b_update(&s,in,200);
    crypto_generichash_blake2b_final(&s,b,50);
    ok(memcmp(a,b,50)==0,"keyed+salt+personal",(long)kl);
  }
  /* Out-of-range outlen must give libsodium's answer, not uniblake's:
   * libsodium rejects, ub_hash clamps to 64. */
  { unsigned char big[200];
    ok(crypto_generichash_blake2b(big,100,in,3,NULL,0)==-1,
       "one-shot rejects outlen 100 as libsodium does",0);
    ok(crypto_generichash_blake2b(big,0,in,3,NULL,0)==-1,
       "one-shot rejects outlen 0 as libsodium does",0);
    ok(crypto_generichash_blake2b(big,64,in,3,NULL,0)==0,
       "one-shot accepts outlen 64",0); }

  ub_aligned_free(S);
  printf("compat: checks=%d fails=%d -> %s\n",checks,fails,fails?"FAIL":"PASS");
  return fails!=0;
}
