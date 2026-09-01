/* Streaming core. See include/uniblake/uniblake.h. */
#include "internal.h"
#include <stddef.h>   /* offsetof, in the C99 alignment fallback */

size_t ub_state_size(void)  { return sizeof(struct ub_state); }

/* _Alignof is C11. Under a strict C99 compiler fall back to the offset trick,
 * which is well defined here: the struct's strictest member is uint64_t, so
 * the padding a leading char forces equals the alignment. Both paths report
 * the same value on every target checked; ub_state_align() is computed at
 * runtime precisely so a caller never has to know which was used. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
size_t ub_state_align(void) { return _Alignof(struct ub_state); }
#else
size_t ub_state_align(void) {
  struct probe { char c; struct ub_state s; };
  return offsetof(struct probe, s);
}
#endif

void ub_param_init(ub_param *P, size_t digest_length) {
  /* Returns void, so the handler is the only channel available here.
   *
   * Validate BEFORE the (uint8_t) cast: an oversized digest_length would
   * truncate into a value ub_init_param accepts (288 -> 32), and the caller
   * would then receive a 32-byte digest believing it asked for 288. Leaving
   * the range check to ub_init_param does not help, because by then the
   * out-of-range value is gone. Same shape as the keylen truncation in the
   * libsodium adapter. */
  if (!P) { ub_err(UB_E_ARG, __func__, "NULL parameter block"); return; }
  memset(P, 0, sizeof *P);
  if (digest_length == 0 || digest_length > UB_OUTBYTES)
    return;   /* leaves digest_length 0; ub_init_param owns the rule and the
               * report, so reporting here too would give one mistake two
               * lines. What matters is that the out-of-range value never
               * reaches the cast. */
  P->digest_length = (uint8_t)digest_length;
  P->fanout = 1;                 /* sequential mode, RFC 7693 §2.5 */
  P->depth  = 1;
}

static void inc(struct ub_state *S, uint64_t n) {
  S->t[0] += n;
  S->t[1] += (S->t[0] < n);
}
static int finalized(const struct ub_state *S) { return S->f[0] != 0; }

/* Absorb the parameter block by XOR into the IV (RFC 7693 §2.5). Serialized
 * here rather than stored, so `ub_param` need not be packed. */
int ub_init_param(ub_state *S, const ub_param *P) {
  if (!S || !P) return ub_err(UB_E_ARG, __func__, "NULL state or parameter block");
  if (P->digest_length == 0 || P->digest_length > UB_OUTBYTES)
    return ub_err(UB_E_ARG, __func__, "digest_length must be 1..64");
  if (P->key_length > UB_KEYBYTES)
    return ub_err(UB_E_ARG, __func__, "key_length above 64");

  /* The serialized block is exactly the 64 bytes RFC 7693 s2.5 specifies, and
   * every offset below is written against that. A build where this is false
   * would silently produce wrong digests, so fail at compile time instead.
   * Written as a negative array size rather than a division so it stays
   * warning-clean on compilers that object to a bool operand. */
  (void)sizeof(int[UB_OUTBYTES == 64 ? 1 : -1]);

  uint8_t blk[UB_OUTBYTES];
  memset(blk, 0, sizeof blk);
  blk[0] = P->digest_length; blk[1] = P->key_length;
  blk[2] = P->fanout;        blk[3] = P->depth;
  ub_store64(blk + 4, 0);
  blk[4]=(uint8_t)P->leaf_length; blk[5]=(uint8_t)(P->leaf_length>>8);
  blk[6]=(uint8_t)(P->leaf_length>>16); blk[7]=(uint8_t)(P->leaf_length>>24);
  blk[8]=(uint8_t)P->node_offset; blk[9]=(uint8_t)(P->node_offset>>8);
  blk[10]=(uint8_t)(P->node_offset>>16); blk[11]=(uint8_t)(P->node_offset>>24);
  blk[12]=(uint8_t)P->xof_length; blk[13]=(uint8_t)(P->xof_length>>8);
  blk[14]=(uint8_t)(P->xof_length>>16); blk[15]=(uint8_t)(P->xof_length>>24);
  blk[16] = P->node_depth; blk[17] = P->inner_length;
  memcpy(blk + 32, P->salt,     UB_SALTBYTES);
  memcpy(blk + 48, P->personal, UB_PERSONALBYTES);

  memset(S, 0, sizeof *S);
  for (int i = 0; i < 8; ++i) S->h[i] = ub_iv[i] ^ ub_load64(blk + i * 8);
  S->outlen = P->digest_length;
  return UB_OK;
}

int ub_init(ub_state *S, size_t outlen) {
  /* Range is reported by ub_init_param, which owns the 1..64 rule; reporting
   * again here would give a caller two lines for one mistake. ub_param_init
   * stops an oversized outlen BEFORE the (uint8_t) cast, so a value like 288
   * arrives as 0 and is rejected rather than silently becoming 32. */
  ub_param P; ub_param_init(&P, outlen);
  return ub_init_param(S, &P);
}

int ub_init_key(ub_state *S, size_t outlen, const void *key, size_t keylen) {
  if (keylen == 0) return ub_init(S, outlen);
  if (!key) return ub_err(UB_E_ARG, __func__, "NULL key with nonzero keylen");
  if (keylen > UB_KEYBYTES) return ub_err(UB_E_ARG, __func__, "keylen above 64");
  ub_param P; ub_param_init(&P, outlen);
  P.key_length = (uint8_t)keylen;
  int rc = ub_init_param(S, &P);
  if (rc != UB_OK) return rc;
#if UB_WIPE
  S->keyed = 1;   /* ub_init_param zeroed the state, so set this after it */
#endif
  /* The key is absorbed as one zero-padded block (RFC 7693 §2.9). */
  uint8_t blk[UB_BLOCKBYTES];
  memset(blk, 0, sizeof blk);
  memcpy(blk, key, keylen);
  int krc = ub_update(S, blk, UB_BLOCKBYTES);
  ub_wipe(blk, sizeof blk);   /* the caller's key, on our stack */
  return krc;
}

/* Eager: a whole pending block is compressed as soon as it is complete AND
 * more input is known to follow. The final block is always retained, because
 * finalization must mark the last block, so it is never flushed early. */
int ub_update(ub_state *S, const void *in, size_t inlen) {
  if (!S) return ub_err(UB_E_ARG, __func__, "NULL state");
  if (inlen && !in) return ub_err(UB_E_ARG, __func__, "NULL input with nonzero inlen");
  if (finalized(S)) return ub_err(UB_E_STATE, __func__, "state already finalized");
  const uint8_t *p = (const uint8_t *)in;

  if (inlen == 0) return UB_OK;
  size_t fill = UB_BLOCKBYTES - S->buflen;
  if (inlen > fill) {
    memcpy(S->buf + S->buflen, p, fill);
    S->buflen = 0;
    ub_compress(S, S->buf, 1);
    p += fill; inlen -= fill;
    /* Hand every whole block to the kernel in ONE call. */
    if (inlen > UB_BLOCKBYTES) {
      size_t nb = (inlen - 1) / UB_BLOCKBYTES;   /* keep the last block pending */
      ub_compress(S, p, nb);
      p += nb * UB_BLOCKBYTES; inlen -= nb * UB_BLOCKBYTES;
    }
  }
  memcpy(S->buf + S->buflen, p, inlen);
  S->buflen += inlen;
  return UB_OK;
}

#if UB_WIPE
/* Out of line so ub_final's body stays free of the volatile indirect call,
 * which inhibits optimization of whatever contains it.
 *
 * On cost: bench/bench_prefix.c shows this branch at ~8 ns/digest, but that
 * is an artifact of its loop, where ub_final would otherwise inline and fold
 * away. Measured against a full init/update/final cycle the unkeyed path
 * pays 1.7 ns (172.8 -> 174.5), and the keyed path 17.7 ns for the wiping it
 * actually does. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void wipe_secrets(struct ub_state *S, uint8_t *d) {
  ub_wipe(d, UB_OUTBYTES);
  ub_wipe(S->h, sizeof S->h);
  ub_wipe(S->buf, sizeof S->buf);
}
#endif

int ub_final(ub_state *S, void *out, size_t outcap) {
  if (!S || !out) return ub_err(UB_E_ARG, __func__, "NULL state or output");
  if (outcap < S->outlen)
    return ub_err(UB_E_OUTCAP, __func__, "output buffer smaller than the digest");
  if (finalized(S)) return ub_err(UB_E_STATE, __func__, "state already finalized");

  inc(S, S->buflen);
  S->f[0] = (uint64_t)-1;
  memset(S->buf + S->buflen, 0, UB_BLOCKBYTES - S->buflen);
  ub_compress_final(S, S->buf);

  uint8_t d[UB_OUTBYTES];
  for (int i = 0; i < 8; ++i) ub_store64(d + i * 8, S->h[i]);
  memcpy(out, d, S->outlen);

  /* Wiping is spent only where there is a secret to protect: a keyed state,
   * whose chaining value and pending block are derived from the caller's key.
   * Unkeyed hashing of public data never wipes.
   *
   * Only the secret-bearing fields are cleared. t, f, buflen and outlen stay,
   * so the finalized() guard above still rejects a second ub_final -- zeroing
   * the whole struct would clear f[0] and silently re-enable it. `d` holds all
   * 64 bytes even when outlen is shorter, so it is cleared too. */
#if UB_WIPE
  if (S->keyed) wipe_secrets(S, d);
#endif
  return UB_OK;
}

int ub_copy(ub_state *dst, const ub_state *src) {
  if (!dst || !src) return ub_err(UB_E_ARG, __func__, "NULL source or destination");
  *dst = *src;
  return UB_OK;
}

int ub_hash(void *out, size_t outcap, const void *in, size_t inlen,
            const void *key, size_t keylen) {
  /* Validate here rather than letting the inner calls do it. They would
   * report under THEIR names -- a caller who wrote ub_hash(out, 0, ...) was
   * told "ub_init_param: digest_length must be 1..64", naming a function it
   * never called and a parameter block it cannot see, and a NULL `out` was
   * reported as "ub_final: NULL state" when the state is internal and fine.
   * The codes were right; the diagnosis pointed at the wrong argument. */
  if (!out) return ub_err(UB_E_ARG, __func__, "NULL output");
  if (inlen && !in) return ub_err(UB_E_ARG, __func__, "NULL input with nonzero inlen");
  if (outcap == 0) return ub_err(UB_E_ARG, __func__, "outcap 0 names a 0-byte digest");
  if (keylen) {
    if (!key) return ub_err(UB_E_ARG, __func__, "NULL key with nonzero keylen");
    if (keylen > UB_KEYBYTES) return ub_err(UB_E_ARG, __func__, "keylen above 64");
  }

  struct ub_state S;
  /* outcap above 64 is clamped, not refused: `out` is a buffer capacity, and a
   * caller passing a larger buffer gets the longest digest that fits. Tested
   * in tests/test_api.c. */
  size_t dl = outcap > UB_OUTBYTES ? UB_OUTBYTES : outcap;
  int rc = keylen ? ub_init_key(&S, dl, key, keylen) : ub_init(&S, dl);
  if (rc != UB_OK) return rc;
  rc = ub_update(&S, in, inlen);
  if (rc != UB_OK) return rc;
  return ub_final(&S, out, outcap);
}

/* --- error reporting --- */

#include <stdio.h>

static ub_error_fn g_err_fn;
static void       *g_err_cookie;

void ub_set_error_handler(ub_error_fn fn, void *cookie) {
  g_err_fn = fn; g_err_cookie = cookie;
}

/* Internal: report and pass the code straight through, so call sites read
 * `return ub_err(UB_E_GEOMETRY, __func__, "...")`. */
int ub_err(ub_status code, const char *fn, const char *detail) {
  if (g_err_fn) g_err_fn(code, fn, detail, g_err_cookie);
  else fprintf(stderr, "uniblake: %s: %s (%d)\n", fn, detail, (int)code);
  return (int)code;
}
