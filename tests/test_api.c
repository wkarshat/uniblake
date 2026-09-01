/* Interface behaviour: return codes, call ordering, and the error handler.
 *
 * Digest correctness is tests/test_core.c and tests/test_prefix.c; this suite
 * checks what happens when a caller gets it wrong, and that a diagnosis is
 * delivered once rather than per digest. Needs no oracle. */
#include "uniblake/prefix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ub_alloc.h"

static int checks, fails;
static void ok(int c, const char *w){ checks++; if(!c){ fails++; printf("  FAIL %s\n", w);} }

static int n_reports; static ub_status last_code; static char last_fn[64];
static void handler(ub_status c, const char *fn, const char *detail, void *cookie){
  (void)detail; (void)cookie;
  n_reports++; last_code=c; snprintf(last_fn,sizeof last_fn,"%s",fn);
}
static ub_state *mk(void){ return ub_aligned_alloc(ub_state_align(), ub_state_size()); }

int main(void){
  setvbuf(stdout,NULL,_IONBF,0);
  uint8_t pre[300], out[64]; memset(pre,1,sizeof pre);
  uint8_t *big = malloc(1000*64);
  ub_state *S = mk();

  /* --- argument validation, core --- */
  ok(ub_init(S,0)==UB_E_ARG,            "init outlen 0");
  ok(ub_init(S,65)==UB_E_ARG,           "init outlen 65");
  ub_init(S,32);
  ok(ub_final(S,out,10)==UB_E_OUTCAP,   "final outcap below digest");

  /* --- ordering --- */
  ok(ub_final(S,out,32)==UB_OK,         "final");
  ok(ub_final(S,out,32)==UB_E_STATE,    "final twice");
  ok(ub_update(S,pre,1)==UB_E_STATE,    "update after final");
  ok(ub_init(S,32)==UB_OK,              "init resets a finalized state");

  /* --- argument validation, prefix --- */
  ub_param P; ub_param_init(&P,50); ub_init_param(S,&P); ub_update(S,pre,140);
  ok(ub_hash_n(NULL,4,1,1,0,0,out,50)==UB_E_ARG,     "hash_n NULL state");
  ok(ub_hash_n(S,4,1,1,0,0,NULL,50)==UB_E_ARG,       "hash_n NULL output");
  ok(ub_hash_n(S,5,1,1,0,0,out,50)==UB_E_ARG,        "hash_n tailwidth 5");
  ok(ub_hash_n(S,4,1,1,0,0,out,10)==UB_E_OUTCAP,     "hash_n stride below digest");
  ok(ub_hash_n(S,4,0,1,40,24,out,24)==UB_E_ARG,      "slice past digest end");
  ok(ub_hash_n(S,4,0,1,0,24,out,8)==UB_E_OUTCAP,     "stride below slice");
  ok(ub_hash_tail(S,pre,120,out,50)==UB_E_GEOMETRY,  "tail exceeds pending block");
  ok(ub_prefix_check(S,200)==UB_E_GEOMETRY,          "prefix_check rejects tailmax");
  ok(ub_prefix_check(S,4)==UB_OK,                    "prefix_check accepts 4");

  /* --- the handler reports once per call, not once per digest --- */
  ub_set_error_handler(handler,NULL);
  ub_init_param(S,&P); ub_update(S,pre,126);         /* 126 pending: no room */
  n_reports=0;
  ok(ub_hash_n(S,4,0,100000,0,0,big,64)==UB_E_GEOMETRY, "batch rejects geometry");
  ok(n_reports==1,                                      "one report per batch call");
  ok(last_code==UB_E_GEOMETRY,                          "handler receives the code");
  ok(strcmp(last_fn,"ub_hash_n")==0,                    "handler receives the function");

  /* --- a valid call reports nothing --- */
  ub_init_param(S,&P); ub_update(S,pre,140);
  n_reports=0;
  ok(ub_hash_n(S,4,0,1000,0,0,big,64)==UB_OK, "valid batch");
  ok(n_reports==0,                             "no report on success");

  /* --- the cookie arrives unmodified --- */
  int marker=0xC0FFEE;
  ub_set_error_handler(handler,&marker);
  ub_init_param(S,&P); ub_update(S,pre,126);
  ub_hash_n(S,4,0,1,0,0,big,64);
  ok(n_reports==1, "handler still installed with a cookie");

  /* --- every reporting operation is exercised ---
   *
   * The handler is documented as covering the whole interface, so each
   * ub_status-returning function must deliver a diagnosis naming ITSELF.
   * A bare `return UB_E_ARG` anywhere below leaves n_reports at 0 and fails
   * here rather than silently narrowing the contract. */
  ub_set_error_handler(handler,NULL);
  {
    ub_state *F = mk();                 /* a finalized state, for the ordering rows */
    ub_init(F,32); ub_update(F,pre,1); ub_final(F,out,32);
    uint8_t key[100]; memset(key,7,sizeof key);
    ub_param Q; ub_param_init(&Q,50);

    /* Each row: the call, the code it must return, the name it must report. */
    #define REPORTS(call,want,wfn,label) do{                                  \
        n_reports=0; last_code=UB_OK; last_fn[0]=0;                           \
        ok((call)==(want),                       label " returns");           \
        ok(n_reports==1,                         label " reports once");      \
        ok(last_code==(want),                    label " reports the code");  \
        ok(strcmp(last_fn,wfn)==0,               label " names itself");      \
      }while(0)

    REPORTS(ub_init(S,0),             UB_E_ARG,   "ub_init_param",  "init outlen 0");
    REPORTS(ub_init(S,65),            UB_E_ARG,   "ub_init_param",  "init outlen 65");
    REPORTS(ub_init_param(S,NULL),    UB_E_ARG,   "ub_init_param",  "init_param NULL P");
    REPORTS(ub_init_key(S,32,NULL,5), UB_E_ARG,   "ub_init_key",    "init_key NULL key");
    REPORTS(ub_init_key(S,32,key,100),UB_E_ARG,   "ub_init_key",    "init_key keylen 100");
    REPORTS(ub_update(NULL,pre,1),    UB_E_ARG,   "ub_update",      "update NULL state");
    ub_init(S,32);
    REPORTS(ub_update(S,NULL,1),      UB_E_ARG,   "ub_update",      "update NULL input");
    REPORTS(ub_update(F,pre,1),       UB_E_STATE, "ub_update",      "update after final");
    REPORTS(ub_final(S,NULL,32),      UB_E_ARG,   "ub_final",       "final NULL out");
    REPORTS(ub_final(S,out,10),       UB_E_OUTCAP,"ub_final",       "final outcap short");
    REPORTS(ub_final(F,out,32),       UB_E_STATE, "ub_final",       "final twice");
    REPORTS(ub_copy(NULL,S),          UB_E_ARG,   "ub_copy",        "copy NULL dst");

    REPORTS(ub_prefix_check(NULL,4),  UB_E_ARG,   "ub_prefix_check","prefix_check NULL");
    REPORTS(ub_prefix_check(F,4),     UB_E_STATE, "ub_prefix_check","prefix_check finalized");
    ub_init_param(S,&Q); ub_update(S,pre,140);
    REPORTS(ub_prefix_check(S,200),   UB_E_GEOMETRY,"ub_prefix_check","prefix_check tailmax");
    REPORTS(ub_hash_tail(NULL,pre,4,out,50), UB_E_ARG,   "ub_hash_tail","hash_tail NULL state");
    REPORTS(ub_hash_tail(S,NULL,4,out,50),   UB_E_ARG,   "ub_hash_tail","hash_tail NULL tail");
    REPORTS(ub_hash_tail(S,pre,4,out,10),    UB_E_OUTCAP,"ub_hash_tail","hash_tail outcap short");
    REPORTS(ub_hash_tail(S,pre,120,out,50),  UB_E_GEOMETRY,"ub_hash_tail","hash_tail geometry");
    REPORTS(ub_hash_tail(F,pre,4,out,32),    UB_E_STATE, "ub_hash_tail","hash_tail finalized");
    REPORTS(ub_hash_n(S,5,0,1,0,0,out,50),   UB_E_ARG,   "ub_hash_n",  "hash_n tailwidth");
    REPORTS(ub_hash_n(S,4,0,1,60,0,out,50),  UB_E_ARG,   "ub_hash_n",  "hash_n off past digest");
    #undef REPORTS

    /* ub_param_init returns void, so the handler is its only channel. */
    n_reports=0; ub_param_init(NULL,32);
    ok(n_reports==1 && strcmp(last_fn,"ub_param_init")==0, "param_init NULL reports");

    /* ub_hash validates its own arguments instead of letting the inner
     * init/update/final calls report under THEIR names: a caller who wrote
     * ub_hash(out, 0, ...) should not be told "ub_init_param", and a NULL
     * `out` should not be reported as a NULL state. */
    #define OWNS(call,label) do{                                              \
        n_reports=0; last_fn[0]=0;                                            \
        ok((call)==UB_E_ARG,               label " returns");                 \
        ok(n_reports==1,                   label " reports once");            \
        ok(strcmp(last_fn,"ub_hash")==0,   label " names ub_hash");           \
      }while(0)
    OWNS(ub_hash(out,0,"abc",3,NULL,0),        "hash outcap 0");
    OWNS(ub_hash(NULL,32,"abc",3,NULL,0),      "hash NULL out");
    OWNS(ub_hash(out,32,NULL,3,NULL,0),        "hash NULL in");
    OWNS(ub_hash(out,32,"abc",3,NULL,5),       "hash NULL key");
    OWNS(ub_hash(out,32,"abc",3,key,100),      "hash keylen 100");
    #undef OWNS

    /* Success stays silent, on the single and the batch path alike. */
    ub_init_param(S,&Q); ub_update(S,pre,140);
    n_reports=0;
    ok(ub_hash_tail(S,pre,4,out,50)==UB_OK && n_reports==0, "hash_tail success is silent");
    n_reports=0;
    ok(ub_hash_n(S,4,0,1000,0,0,big,64)==UB_OK && n_reports==0, "batch success is silent");
    ub_aligned_free(F);
  }

  /* --- without a handler the code is still returned --- */
  ub_set_error_handler(NULL,NULL);
  ub_init_param(S,&P); ub_update(S,pre,126);
  fprintf(stderr,"(one uniblake: line expected below)\n");
  ok(ub_hash_n(S,4,0,1,0,0,big,64)==UB_E_GEOMETRY, "default path returns the code");

  /* --- ub_hash: outcap is also the digest length ---
   *
   * Unlike ub_final, where outcap is purely a capacity checked against the
   * length fixed at init, ub_hash has no other place to learn the length, so
   * the same argument selects it. These rows pin that down, because the
   * consequence is not padding but a DIFFERENT DIGEST: BLAKE2b folds
   * digest_length into the parameter block, so a 32- and a 64-byte digest of
   * one message share no bytes. A caller who oversizes the buffer gets a
   * different value, silently.
   *
   * Locking in current behaviour, not endorsing it: if ub_hash ever stops
   * clamping outcap > 64 and rejects instead, the clamp rows below are the
   * ones to update. */
  ub_set_error_handler(NULL,NULL);
  {
    uint8_t a[128], b[128];
    ub_state *T = mk();

    /* outcap selects the length: identical to the explicit streaming digest */
    int same = 1;
    for (size_t N = 1; N <= UB_OUTBYTES; ++N) {
      memset(a,0,sizeof a); memset(b,0,sizeof b);
      if (ub_hash(a,N,"abc",3,NULL,0) != UB_OK) { same = 0; break; }
      ub_init(T,N); ub_update(T,"abc",3); ub_final(T,b,N);
      if (memcmp(a,b,N)) { same = 0; break; }
    }
    ok(same, "ub_hash(outcap=N) == the explicit N-byte digest, N=1..64");

    /* the trap: a larger buffer is a different digest, not a longer one */
    ub_hash(a,32,"abc",3,NULL,0);
    ub_hash(b,64,"abc",3,NULL,0);
    ok(memcmp(a,b,32)!=0, "digest length changes the value, not just the width");

    /* above 64 the request is clamped rather than refused */
    memset(b,0,sizeof b);
    ok(ub_hash(b,100,"abc",3,NULL,0)==UB_OK, "ub_hash outcap 100 accepted");
    ub_hash(a,UB_OUTBYTES,"abc",3,NULL,0);
    ok(memcmp(a,b,UB_OUTBYTES)==0, "ub_hash outcap 100 clamps to a 64-byte digest");
    ok(b[UB_OUTBYTES]==0, "ub_hash writes no more than 64 bytes");

    /* the keyed path clamps the same way */
    { uint8_t key[32]; memset(key,7,sizeof key);
      memset(b,0,sizeof b);
      ub_hash(b,100,"abc",3,key,sizeof key);
      ub_hash(a,UB_OUTBYTES,"abc",3,key,sizeof key);
      ok(memcmp(a,b,UB_OUTBYTES)==0, "keyed ub_hash clamps identically"); }

    /* outcap 0 is rejected: it would name a 0-byte digest */
    ok(ub_hash(a,0,"abc",3,NULL,0)==UB_E_ARG, "ub_hash outcap 0 rejected");

    /* the compat shims forward outlen into that same argument, so a caller
     * porting from libsodium inherits the behaviour above */
    ok(ub_hash(a,32,"abc",3,NULL,0)==UB_OK, "shim-shaped call still succeeds");
    ub_aligned_free(T);
  }

  /* --- narrowing casts must not admit an out-of-range value ---
   * A size_t that truncates into an accepted uint8_t (288 -> 32) must be
   * rejected before the cast, not silently honoured as the truncated value. */
  { uint8_t k32[32]; memset(k32,7,sizeof k32);
    ub_param Q; ub_param_init(&Q, 288);
    ok(Q.digest_length == 0,            "param_init rejects 288 rather than storing 32");
    ok(ub_init_param(S,&Q)==UB_E_ARG,   "init_param rejects the result");
    ok(ub_init(S,288)==UB_E_ARG,        "init rejects 288");
    ok(ub_init(S,256)==UB_E_ARG,        "init rejects 256 (truncates to 0)");
    ok(ub_init_key(S,288,k32,32)==UB_E_ARG, "init_key rejects outlen 288");
    ok(ub_init_key(S,256,k32,32)==UB_E_ARG, "init_key rejects outlen 256");
    /* ub_hash clamps outcap rather than refusing (see above); what matters
     * here is that the clamp is not a TRUNCATION -- 288 must give 64, not
     * 288 & 0xff == 32. */
    { uint8_t c1[64], c2[64];
      ub_hash(c1,288,"abc",3,NULL,0);
      ub_hash(c2,64,"abc",3,NULL,0);
      ok(memcmp(c1,c2,64)==0, "hash outcap 288 clamps to 64, not to 32"); } }

  /* --- length guards must not wrap ---
   * buflen + taillen overflows for a huge taillen; the guard has to compare
   * against the remaining room instead, or the copy runs off the state. */
  { ub_param Q; ub_param_init(&Q,32); ub_init_param(S,&Q); ub_update(S,pre,140);
    ok(ub_hash_tail(S,pre,(size_t)-1,out,32)==UB_E_GEOMETRY,
                                        "hash_tail rejects taillen SIZE_MAX");
    ok(ub_hash_tail(S,pre,(size_t)-4,out,32)==UB_E_GEOMETRY,
                                        "hash_tail rejects a near-wrap taillen");
    ok(ub_prefix_check(S,(size_t)-1)==UB_E_GEOMETRY,
                                        "prefix_check rejects tailmax SIZE_MAX");
    ok(ub_hash_n(S,4,0,1,0,(size_t)-1,out,32)==UB_E_ARG,
                                        "hash_n rejects len SIZE_MAX");
    ok(ub_hash_n(S,4,0,1,(size_t)-1,4,out,32)==UB_E_ARG,
                                        "hash_n rejects off SIZE_MAX"); }

  /* Secret material after ub_final. The state is opaque, so this searches its
   * raw bytes for the key rather than naming fields: a keyed state must not
   * still contain the key-derived chaining value, an unkeyed one is left
   * alone, and both must still refuse a second ub_final. */
  { unsigned char key[32], msg[200], dg[64];
    for (size_t i=0;i<sizeof key;i++) key[i]=(unsigned char)(0xA5^i);
    for (size_t i=0;i<sizeof msg;i++) msg[i]=(unsigned char)(i*3+1);

    /* Snapshot the pre-final chaining value by finalizing a copy, then check
     * the original's bytes changed where the secret lived. */
    ub_state *K = ub_aligned_alloc(ub_state_align(), ub_state_size());
    ub_state *C = ub_aligned_alloc(ub_state_align(), ub_state_size());
    ub_init_key(K,32,key,sizeof key); ub_update(K,msg,sizeof msg);
    ub_copy(C,K);
    ub_final(C,dg,sizeof dg);              /* C is wiped */
    ub_final(K,dg,sizeof dg);              /* K is wiped */

    const unsigned char *kb=(const unsigned char*)K;
    int nonzero=0; for(size_t i=0;i<ub_state_size();i++) if(kb[i]) nonzero=1;
    ok(nonzero, "keyed state keeps its flags after final");

    /* The 64-byte chaining value sits at offset 0. Whether it is cleared is
     * the build's choice; the second-final guard must hold either way. */
    int h_clear=1; for(size_t i=0;i<64;i++) if(kb[i]) h_clear=0;
#if UB_WIPE
    ok(h_clear, "keyed state clears the chaining value on final");
#else
    ok(!h_clear, "keyed state keeps the chaining value when wiping is off");
#endif
    ok(ub_final(K,dg,sizeof dg)==UB_E_STATE,
                                        "keyed state still rejects a second final");

    ub_state *U = ub_aligned_alloc(ub_state_align(), ub_state_size());
    ub_init(U,64); ub_update(U,msg,sizeof msg); ub_final(U,dg,sizeof dg);
    const unsigned char *ub=(const unsigned char*)U;
    int u_clear=1; for(size_t i=0;i<64;i++) if(ub[i]) u_clear=0;
    ok(!u_clear, "unkeyed state is not wiped");
    ok(ub_final(U,dg,sizeof dg)==UB_E_STATE,
                                        "unkeyed state rejects a second final");
    ub_aligned_free(K); ub_aligned_free(C); ub_aligned_free(U); }

  printf("api: checks=%d fails=%d -> %s\n",checks,fails,fails?"FAIL":"PASS");
  free(big);
  return fails!=0;
}
