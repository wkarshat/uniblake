/* Streaming core. See include/uniblake/uniblake.h. */
#include "internal.h"

size_t ub_state_size(void)  { return sizeof(struct ub_state); }
size_t ub_state_align(void) { return _Alignof(struct ub_state); }

void ub_param_init(ub_param *P, size_t digest_length) {
  if (!P) return;
  memset(P, 0, sizeof *P);
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
  if (!S || !P) return UB_E_ARG;
  if (P->digest_length == 0 || P->digest_length > UB_OUTBYTES) return UB_E_ARG;
  if (P->key_length > UB_KEYBYTES) return UB_E_ARG;

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
  ub_param P; ub_param_init(&P, outlen);
  return ub_init_param(S, &P);
}

int ub_init_key(ub_state *S, size_t outlen, const void *key, size_t keylen) {
  if (keylen == 0) return ub_init(S, outlen);
  if (!key || keylen > UB_KEYBYTES) return UB_E_ARG;
  ub_param P; ub_param_init(&P, outlen);
  P.key_length = (uint8_t)keylen;
  int rc = ub_init_param(S, &P);
  if (rc != UB_OK) return rc;
  /* The key is absorbed as one zero-padded block (RFC 7693 §2.9). */
  uint8_t blk[UB_BLOCKBYTES];
  memset(blk, 0, sizeof blk);
  memcpy(blk, key, keylen);
  return ub_update(S, blk, UB_BLOCKBYTES);
}

/* Eager: a whole pending block is compressed as soon as it is complete AND
 * more input is known to follow. The final block is always retained, because
 * finalization must mark the last block, so it is never flushed early. */
int ub_update(ub_state *S, const void *in, size_t inlen) {
  if (!S) return UB_E_ARG;
  if (inlen && !in) return UB_E_ARG;
  if (finalized(S)) return UB_E_STATE;
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

int ub_final(ub_state *S, void *out, size_t outcap) {
  if (!S || !out) return UB_E_ARG;
  if (outcap < S->outlen) return UB_E_OUTCAP;
  if (finalized(S)) return UB_E_STATE;

  inc(S, S->buflen);
  S->f[0] = (uint64_t)-1;
  memset(S->buf + S->buflen, 0, UB_BLOCKBYTES - S->buflen);
  ub_compress_final(S, S->buf);

  uint8_t d[UB_OUTBYTES];
  for (int i = 0; i < 8; ++i) ub_store64(d + i * 8, S->h[i]);
  memcpy(out, d, S->outlen);
  return UB_OK;
}

int ub_copy(ub_state *dst, const ub_state *src) {
  if (!dst || !src) return UB_E_ARG;
  *dst = *src;
  return UB_OK;
}

int ub_hash(void *out, size_t outcap, const void *in, size_t inlen,
            const void *key, size_t keylen) {
  struct ub_state S;
  int rc = keylen ? ub_init_key(&S, outcap > UB_OUTBYTES ? UB_OUTBYTES : outcap,
                                key, keylen)
                  : ub_init(&S, outcap > UB_OUTBYTES ? UB_OUTBYTES : outcap);
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
