/* BLAKE2 author-reference compatibility -- the Neves lineage.
 *
 * Covers the `blake2.h` API from github.com/BLAKE2/BLAKE2 (Samuel Neves et
 * al.), which libsodium, CPython and most distributions track. uniblake
 * already follows this API's names and argument order, so this file is
 * aliases rather than translation: include it instead of `blake2.h` and
 * sequential BLAKE2b call sites compile unchanged.
 *
 * WHAT IS NOT COVERED, and why it cannot be shimmed:
 *
 *   blake2bp, blake2sp, blake2s   different algorithms or tree modes; see
 *                                 "No tree hashing" in docs/UniBlake.md.
 *   blake2b_state as a value      the reference state is a struct a caller
 *                                 may assign; ub_state is opaque. Rewrite
 *                                 `b = a;` as `ub_copy(&b, &a)`. This is
 *                                 the one unavoidable source change.
 *   last_node                     tree-hashing flag; ub_state omits it.
 *   BLAKE2B_KEYBYTES etc.         provided below.
 *
 * The parameter block maps field for field, minus `reserved[14]`:
 * ub_init_param serializes the 64-byte block itself, so uniblake has no
 * packing pragma and no reserved bytes to zero. A caller that memsets a
 * blake2b_param and sets fields by name ports directly; one that casts the
 * struct to 64 bytes of memory does not, and should set the fields instead.
 */
#ifndef UNIBLAKE_COMPAT_BLAKE2_H
#define UNIBLAKE_COMPAT_BLAKE2_H

#include "uniblake/uniblake.h"

/* --- constants (blake2b_constant) --- */
#define BLAKE2B_BLOCKBYTES     UB_BLOCKBYTES
#define BLAKE2B_OUTBYTES       UB_OUTBYTES
#define BLAKE2B_KEYBYTES       UB_KEYBYTES
#define BLAKE2B_SALTBYTES      UB_SALTBYTES
#define BLAKE2B_PERSONALBYTES  UB_PERSONALBYTES

/* --- types ---
 *
 * blake2b_state is opaque here; the reference's is not. See the note above
 * on assignment.
 *
 * blake2b_param carries no `reserved` member, so code touching P->reserved
 * fails to compile rather than silently writing nothing. That is deliberate:
 * the reserved bytes are always zero in the block uniblake emits. */
#define blake2b_state  ub_state
#define blake2b_param  ub_param

/* --- streaming ---
 *
 * Signatures and argument order match the reference exactly. blake2b_final
 * takes output capacity in both, so the RFC shim's warning does not apply. */
static inline int blake2b_init(ub_state *S, size_t outlen)
{ return ub_init(S, outlen); }

static inline int blake2b_init_key(ub_state *S, size_t outlen,
                                   const void *key, size_t keylen)
{ return ub_init_key(S, outlen, key, keylen); }

static inline int blake2b_init_param(ub_state *S, const ub_param *P)
{ return ub_init_param(S, P); }

static inline int blake2b_update(ub_state *S, const void *in, size_t inlen)
{ return ub_update(S, in, inlen); }

static inline int blake2b_final(ub_state *S, void *out, size_t outlen)
{ return ub_final(S, out, outlen); }

/* --- one-shot ---
 *
 * The reference puts the key last, as uniblake does. Note the argument order
 * differs from the RFC sample's blake2b(), where the key precedes the
 * message -- compat/ub_rfc.h covers that shape. */
static inline int blake2b(void *out, size_t outlen, const void *in,
                          size_t inlen, const void *key, size_t keylen)
{ return ub_hash(out, outlen, in, inlen, key, keylen); }

#endif
