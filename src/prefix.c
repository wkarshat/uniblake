/* Repeated-prefix hashing. See include/uniblake/prefix.h. */
#include "internal.h"
#include "uniblake/prefix.h"

int ub_prefix_check(const ub_state *S, size_t tailmax) {
  if (!S) return ub_err(UB_E_ARG, __func__, "NULL state");
  /* The point of this call is to predict what the hashing calls will do, so
   * it has to reject everything they reject -- including a finalized state. */
  if (S->f[0] != 0) return ub_err(UB_E_STATE, __func__, "state already finalized");
  if (tailmax > UB_BLOCKBYTES)
    return ub_err(UB_E_GEOMETRY, __func__, "tailmax exceeds one block");
  /* update() is eager, so at most one block is pending. A digest costs one
   * compression exactly when the tail still fits that block. */
  /* Subtract rather than add: buflen + tailmax wraps for a huge tailmax and
   * would pass. buflen is never above BLOCKBYTES, so this cannot underflow. */
  if (tailmax > (size_t)(UB_BLOCKBYTES - S->buflen))
    return ub_err(UB_E_GEOMETRY, __func__,
                  "prefix leaves no room for the tail in one block");
  return UB_OK;
}

/* Hot path: copy the state, append the tail into the pending block, finalize.
 * One compression.
 *
 * Silent by design: ub_hash_n calls this once per digest, so reporting here
 * would emit one line per digest, violating the one-report-per-call rule
 * stated in uniblake.h. Both callers validate the same conditions up front,
 * where a batch reports once. These checks are retained for the case a caller
 * reaches finish() by another path, and return their code bare. */
static int finish(const struct ub_state *S, const uint8_t *tail, size_t taillen,
                  void *out, size_t outcap) {
  if (outcap < S->outlen) return UB_E_OUTCAP;
  /* Subtract, do not add: buflen + taillen wraps for a huge taillen and the
   * memcpy below would then run off the state. */
  if (taillen > (size_t)(UB_BLOCKBYTES - S->buflen)) return UB_E_GEOMETRY;
  if (S->f[0] != 0) return UB_E_STATE;
  struct ub_state t = *S;
  memcpy(t.buf + t.buflen, tail, taillen);
  t.buflen += taillen;
  return ub_final(&t, out, outcap);
}

int ub_hash_tail(const ub_state *S, const void *tail, size_t taillen,
                 void *out, size_t outcap) {
  if (!S || !out) return ub_err(UB_E_ARG, __func__, "NULL state or output");
  if (taillen && !tail)
    return ub_err(UB_E_ARG, __func__, "NULL tail with nonzero taillen");
  /* One digest per call, so the conditions finish() rechecks are reported
   * here -- see the note on finish() above. */
  if (S->f[0] != 0) return ub_err(UB_E_STATE, __func__, "state already finalized");
  if (outcap < S->outlen)
    return ub_err(UB_E_OUTCAP, __func__, "output buffer smaller than the digest");
  if (taillen > (size_t)(UB_BLOCKBYTES - S->buflen))
    return ub_err(UB_E_GEOMETRY, __func__,
                  "prefix leaves no room for the tail in one block");
  return finish(S, (const uint8_t *)tail, taillen, out, outcap);
}

/* Counters are serialized little-endian, matching BLAKE2b's own convention
 * for every multi-byte field in the parameter block. */
static void enc_tail(uint64_t v, size_t w, uint8_t b[8]) {
  for (size_t i = 0; i < w; ++i) b[i] = (uint8_t)(v >> (8 * i));
}

/* A concurrent replacement defines ub_hash_n itself and calls this per span;
 * building with -DUB_HASH_N_SERIAL renames the one here out of the way. */
#ifdef UB_HASH_N_SERIAL
# define ub_hash_n ub_hash_n_serial
#endif

int ub_hash_n(const ub_state *S, size_t tailwidth, uint64_t first, size_t n,
              size_t off, size_t len, void *out, size_t stride) {
  if (!S || !out) return ub_err(UB_E_ARG, __func__, "NULL state or output");
  if (tailwidth != 4 && tailwidth != 8)
    return ub_err(UB_E_ARG, __func__, "tailwidth must be 4 or 8");
  /* Bound `off` BEFORE deriving len from it: `S->outlen - off` is size_t, so
   * off > outlen underflows to a huge length, and the `off + len` that would
   * catch it wraps back to exactly outlen and passes. Both checks below are
   * written as subtractions on the known-good side for that reason. */
  if (off > S->outlen)
    return ub_err(UB_E_ARG, __func__, "slice outside the digest");
  if (len == 0) len = S->outlen - off;          /* whole digest from off */
  if (len > S->outlen - off)
    return ub_err(UB_E_ARG, __func__, "slice outside the digest");
  if (stride < len)
    return ub_err(UB_E_OUTCAP, __func__, "stride below bytes written");
  if (S->f[0] != 0)
    return ub_err(UB_E_STATE, __func__, "state already finalized");
  /* Geometry is a property of the state, so validate once, not per digest. */
  if (tailwidth > (size_t)(UB_BLOCKBYTES - S->buflen))
    return ub_err(UB_E_GEOMETRY, __func__,
                  "prefix leaves no room for the tail in one block");

  /* `o` walks the output rather than indexing out + i*stride; both compile to
   * the same code, and the walking form keeps the stride arithmetic in one
   * place. */
  uint8_t *o = (uint8_t *)out, b[8], d[UB_OUTBYTES];
  for (size_t i = 0; i < n; ++i) {
    enc_tail(first + i, tailwidth, b);
    int rc = finish(S, b, tailwidth, d, sizeof d);
    if (rc != UB_OK) return rc;
    memcpy(o, d + off, len);
    o += stride;
  }
  return UB_OK;
}
