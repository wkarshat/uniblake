/* uniblake — BLAKE2b (RFC 7693).
 *
 * Self-contained: this directory depends on no other implementation.
 *
 * Layering. Each header stands alone; later ones add to earlier.
 *   uniblake/uniblake.h   core: parameter block, streaming, one-shot
 *   uniblake/prefix.h     repeated-prefix hashing
 *
 * A caller wanting only RFC 7693 includes this header and links src/core.c.
 *
 * A `ub_state` holds one hashing computation in progress: everything absorbed
 * so far, and nothing else. It is a value — `ub_copy` duplicates it and both
 * copies continue independently.
 */
#ifndef UNIBLAKE_H
#define UNIBLAKE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BLAKE2b sizes, all in bytes (RFC 7693 §2).
 *
 * UB_BLOCKBYTES is the compression block: the message is consumed 128 bytes
 * at a time, and every cost in this library is a count of block compressions.
 * It also sets the limit on the fast path in prefix.h — a digest costs one
 * compression only while the bytes still pending plus the new bytes fit
 * inside one block. */
/* Whether ub_final clears a keyed state's secret material before returning.
 *
 * On by default: a caller hashing with a key should not have to know to ask.
 * Build the library AND its callers with -DUB_WIPE=0 to compile it
 * out entirely -- no branch, no flag in the state -- which suits a consumer
 * hashing only public data, where the cost buys nothing.
 *
 * Digests are identical either way. This changes what is left in memory after
 * ub_final, not what is computed. It affects the layout of the opaque state,
 * so a library and a caller built with different settings must not be mixed;
 * ub_state_size() reports the size actually compiled. */
#ifndef UB_WIPE
#define UB_WIPE 1
#endif

#define UB_BLOCKBYTES     128   /* compression block */
#define UB_OUTBYTES        64   /* largest digest */
#define UB_KEYBYTES        64   /* largest key */
#define UB_SALTBYTES       16   /* salt field width */
#define UB_PERSONALBYTES   16   /* personalization field width */

/* Distinct codes so a caller can tell misuse from geometry from capacity.
 * 0 is success, so existing `!= 0` checks against libsodium keep working. */
typedef enum {
  UB_OK         =  0,
  UB_E_ARG      = -1,   /* NULL or out-of-range argument */
  UB_E_OUTCAP   = -2,   /* output buffer smaller than the digest length */
  UB_E_GEOMETRY = -3,   /* prefix + tail cannot share one final block */
  UB_E_STATE    = -4    /* operation invalid for this state */
} ub_status;

/* The BLAKE2b parameter block (RFC 7693 §2.5), in the specification's field
 * order and naming. It describes WHAT to compute — digest length, key length,
 * and optional salt and personalization — as opposed to the message itself.
 *
 * It is consumed entirely by ub_init_param, which folds it into the starting
 * state. Nothing reads it afterwards, so it may be discarded once init
 * returns.
 *
 * The tree-hashing fields (fanout, depth, leaf_length, node_offset,
 * xof_length, node_depth, inner_length) are written into the block as given.
 * This library hashes sequentially and does not walk trees, but it does not
 * silently drop the fields either. */
typedef struct {
  uint8_t  digest_length;
  uint8_t  key_length;
  uint8_t  fanout;
  uint8_t  depth;
  uint32_t leaf_length;
  uint32_t node_offset;
  uint32_t xof_length;
  uint8_t  node_depth;
  uint8_t  inner_length;
  uint8_t  salt[UB_SALTBYTES];
  uint8_t  personal[UB_PERSONALBYTES];
} ub_param;

/* Sequential-mode defaults (fanout = depth = 1) for `digest_length` bytes. */
void ub_param_init(ub_param *P, size_t digest_length);

/* Opaque, caller-allocated. The library never allocates and holds no pointer
 * into caller storage; automatic, static, or heap all work. Not internally
 * locked — one state per thread, shared via ub_copy. */
typedef struct ub_state ub_state;
size_t ub_state_size(void);
size_t ub_state_align(void);

/* --- streaming (RFC 7693 §3.3) --- */
int ub_init      (ub_state *S, size_t outlen);
int ub_init_key  (ub_state *S, size_t outlen, const void *key, size_t keylen);
int ub_init_param(ub_state *S, const ub_param *P);
int ub_update    (ub_state *S, const void *in, size_t inlen);
int ub_final     (ub_state *S, void *out, size_t outcap);
int ub_copy      (ub_state *dst, const ub_state *src);

/* --- error reporting ---
 *
 * The return code stays the primary channel. This handler carries the
 * diagnosis a code cannot: which function, and why. It is advisory — a
 * function that reports still returns its ub_status — so the library works
 * in builds that install nothing.
 *
 * Default: one line to stderr. The library never aborts or exits; choosing
 * termination policy is the host's business, not a hash library's.
 *
 * Every function that returns a ub_status reports through it, so a handler
 * covers the whole interface rather than one corner of it. `ub_param_init`
 * returns void and reports a NULL argument this way too.
 *
 * Batch calls validate their arguments once, before hashing, so a handler
 * sees one report per call rather than one per digest.
 * The handler may be called from any thread; install it before first use.
 *
 * `cookie` is stored alongside the handler and passed back unmodified on
 * every call. The library never dereferences or interprets it -- it is opaque
 * by design, not merely by convention. It exists so a handler can reach its
 * context without a global: a logger instance, a test's result struct, a C++
 * object behind a trampoline. Pass NULL if the handler needs nothing. */
typedef void (*ub_error_fn)(ub_status code, const char *fn,
                            const char *detail, void *cookie);
void ub_set_error_handler(ub_error_fn fn, void *cookie);

/* --- one-shot ---
 *
 * ub_init/ub_init_key, ub_update, ub_final in a single call, for a message
 * already in one buffer. */
int ub_hash(void *out, size_t outcap, const void *in, size_t inlen,
            const void *key, size_t keylen);

#ifdef __cplusplus
}
#endif
#endif /* UNIBLAKE_H */
