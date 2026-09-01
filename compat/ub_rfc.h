/* Copyright (c) 2026 UniBlake Developers */
/* RFC 7693 Appendix-A sample-code compatibility.
 *
 * WARNING: the RFC's blake2b_final(ctx, out) takes no output capacity -- the
 * digest length is whatever was given to init. This shim must therefore trust
 * that the caller's buffer is at least that large; it cannot check. That is a
 * property of the RFC API, not of uniblake. Prefer the core API.
 */
#ifndef UNIBLAKE_COMPAT_RFC_H
#define UNIBLAKE_COMPAT_RFC_H

#include "uniblake/uniblake.h"

#define blake2b_ctx ub_state

static inline int blake2b_init(ub_state *S, size_t outlen,
                               const void *key, size_t keylen)
{ return keylen ? ub_init_key(S, outlen, key, keylen) : ub_init(S, outlen); }

static inline void blake2b_update(ub_state *S, const void *in, size_t inlen)
{ (void)ub_update(S, in, inlen); }

/* UB_OUTBYTES is the only capacity available; see the warning above. */
static inline void blake2b_final(ub_state *S, void *out)
{ (void)ub_final(S, out, UB_OUTBYTES); }

static inline int blake2b(void *out, size_t outlen, const void *key,
                          size_t keylen, const void *in, size_t inlen)
{ return ub_hash(out, outlen, in, inlen, key, keylen); }

#endif
