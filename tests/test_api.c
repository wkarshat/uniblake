/* Interface behaviour: return codes, call ordering, and the error handler.
 *
 * Digest correctness is tests/test_core.c and tests/test_prefix.c; this suite
 * checks what happens when a caller gets it wrong, and that a diagnosis is
 * delivered once rather than per digest. Needs no oracle. */
#include "uniblake/prefix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void ok(int c, const char *w){ checks++; if(!c){ fails++; printf("  FAIL %s\n", w);} }

static int n_reports; static ub_status last_code; static char last_fn[64];
static void handler(ub_status c, const char *fn, const char *detail, void *cookie){
  (void)detail; (void)cookie;
  n_reports++; last_code=c; snprintf(last_fn,sizeof last_fn,"%s",fn);
}
static ub_state *mk(void){ return aligned_alloc(ub_state_align(), ub_state_size()); }

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

  /* --- without a handler the code is still returned --- */
  ub_set_error_handler(NULL,NULL);
  fprintf(stderr,"(one uniblake: line expected below)\n");
  ok(ub_hash_n(S,4,0,1,0,0,big,64)==UB_E_GEOMETRY, "default path returns the code");

  printf("api: checks=%d fails=%d -> %s\n",checks,fails,fails?"FAIL":"PASS");
  free(big);
  return fails!=0;
}
