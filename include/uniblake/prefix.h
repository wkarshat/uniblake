/* uniblake — hashing many messages that begin with the same bytes.
 *
 * For H(prefix || tail_0), H(prefix || tail_1), ... Absorb the prefix into a
 * ub_state once with ub_update, then use the functions here: each produces a
 * digest without re-absorbing the prefix and without modifying the state.
 *
 * Two of them, differing only in where the trailing bytes come from:
 *
 *   ub_hash_tail   one digest; you supply the trailing bytes
 *   ub_hash_n      n digests; the trailing bytes are consecutive counters
 *
 * plus ub_prefix_check, which reports whether your sizes allow the one-
 * compression fast path before you rely on it.
 *
 * For trailing bytes too long for that fast path, or arriving in pieces,
 * ub_copy the state and continue with ordinary ub_update / ub_final calls;
 * the prefix is still absorbed only once.
 */
#ifndef UNIBLAKE_PREFIX_H
#define UNIBLAKE_PREFIX_H

#include "uniblake/uniblake.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Can a digest with a tail of up to `tailmax` bytes be produced from S with a
 * single block compression? Returns UB_OK, or UB_E_GEOMETRY if the prefix
 * leaves too little room in the pending block. The hashing calls below make
 * the same check and refuse rather than quietly costing twice as much. */
int ub_prefix_check(const ub_state *S, size_t tailmax);

/* H(S || tail) -> out. S is NOT modified: it may be shared read-only across
 * threads and reused for any number of digests. */
int ub_hash_tail(const ub_state *S, const void *tail, size_t taillen,
                 void *out, size_t outcap);

/* n digests over consecutive counters, written at `stride` spacing.
 *
 * Counter i is serialized little-endian into `tailwidth` bytes (4 or 8) and
 * appended to the prefix. Digest i is written to (char *)out + i * stride.
 *
 * `off` and `len` select which part of each digest is written: len = 0 means
 * the whole digest from `off`, otherwise bytes [off, off+len). A caller
 * consuming a digest as fixed-width fields — a 48-byte digest read as two
 * 24-byte values, say — asks for one field and gets a packed array of just
 * those, with no second pass over the output.
 *
 * `stride` spaces successive outputs and is independent of `len`: set it to
 * the caller's row width to scatter into an existing layout, or to `len` for
 * a packed result. It must be at least `len`.
 *
 * Arguments are checked once per call, before any hashing — about 5 ns, so
 * roughly 5% of a single-digest call and under 0.4% from n = 16 upward.
 *
 * This is where parallelism attaches: a range of independent digests plus
 * where each one goes is what a thread pool, a SIMD kernel, or an accelerator
 * needs. On a scalar build this measures at parity with a caller loop — the
 * saving is the shared prefix, not the batching. It is one call because a
 * replacement splitting the
 * range cannot be handed a precomputed tail array without defeating the
 * point. */
int ub_hash_n(const ub_state *S, size_t tailwidth, uint64_t first, size_t n,
              size_t off, size_t len, void *out, size_t stride);

#ifdef __cplusplus
}
#endif
#endif /* UNIBLAKE_PREFIX_H */
