/* libsodium source compatibility for uniblake.
 *
 * Lets a consumer keep libsodium call sites unchanged. Not part of the core:
 * nothing in ../include or ../src refers to this header, and a consumer that
 * ports its call sites should not include it.
 *
 * The one thing a shim cannot hide: libsodium's state is a fixed-size array,
 * so `b = a;` copies it. `ub_state` is opaque, so that becomes ub_copy(&b,&a).
 * Search for assignments between two state variables to find them.
 */
#ifndef UNIBLAKE_COMPAT_SODIUM_H
#define UNIBLAKE_COMPAT_SODIUM_H

#include "uniblake/uniblake.h"
#include <string.h>

#define crypto_generichash_blake2b_state          ub_state
#define crypto_generichash_blake2b_PERSONALBYTES  UB_PERSONALBYTES
#define crypto_generichash_blake2b_SALTBYTES      UB_SALTBYTES
#define crypto_generichash_blake2b_BYTES_MAX      UB_OUTBYTES
#define crypto_generichash_blake2b_KEYBYTES_MAX   UB_KEYBYTES

/* Argument order differs: libsodium puts the key first, the reference (and
 * uniblake) put outlen first. */
static inline int
crypto_generichash_blake2b_init(ub_state *S, const unsigned char *key,
                                size_t keylen, size_t outlen)
{ return keylen ? ub_init_key(S, outlen, key, keylen) : ub_init(S, outlen); }

/* Routes keyed calls through ub_init_key, which absorbs the key as one
 * zero-padded 128-byte block (RFC 7693 §2.9). Setting P.key_length and
 * feeding the raw key would produce a wrong digest. */
static inline int
crypto_generichash_blake2b_init_salt_personal(ub_state *S,
    const unsigned char *key, size_t keylen, size_t outlen,
    const unsigned char *salt, const unsigned char *personal)
{
  if (!salt && !personal)
    return crypto_generichash_blake2b_init(S, key, keylen, outlen);
  /* Before the (uint8_t) cast below: an oversized keylen would truncate to a
   * value ub_init_param accepts (288 -> 32), and the memcpy into the 128-byte
   * block would then run off the stack. libsodium rejects this too.
   *
   * Returns the code bare: ub_err is internal to src/, and this is a
   * header-only shim, so the handler is not reachable from here. Everything
   * this file forwards to reports normally -- only this one check is silent. */
  if (keylen > UB_KEYBYTES) return UB_E_ARG;
  ub_param P;
  ub_param_init(&P, outlen);
  if (salt)     memcpy(P.salt, salt, UB_SALTBYTES);
  if (personal) memcpy(P.personal, personal, UB_PERSONALBYTES);
  if (keylen) {
    P.key_length = (uint8_t)keylen;
    int rc = ub_init_param(S, &P);
    if (rc != UB_OK) return rc;
    unsigned char blk[UB_BLOCKBYTES];
    memset(blk, 0, sizeof blk);
    memcpy(blk, key, keylen);
    return ub_update(S, blk, UB_BLOCKBYTES);
  }
  return ub_init_param(S, &P);
}

static inline int
crypto_generichash_blake2b_update(ub_state *S, const unsigned char *in,
                                  unsigned long long inlen)
{ return ub_update(S, in, (size_t)inlen); }

static inline int
crypto_generichash_blake2b_final(ub_state *S, unsigned char *out, size_t outlen)
{ return ub_final(S, out, outlen); }

static inline int
crypto_generichash_blake2b(unsigned char *out, size_t outlen,
                           const unsigned char *in, unsigned long long inlen,
                           const unsigned char *key, size_t keylen)
{ return ub_hash(out, outlen, in, (size_t)inlen, key, keylen); }

#endif
