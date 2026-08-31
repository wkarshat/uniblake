/* Repeated-prefix hashing. See include/uniblake/prefix.h. */
#include "internal.h"
#include "uniblake/prefix.h"

int ub_prefix_check(const ub_state *S, size_t tailmax) {
  if (!S) return UB_E_ARG;
  if (tailmax > UB_BLOCKBYTES) return UB_E_GEOMETRY;
  /* update() is eager, so at most one block is pending. A digest costs one
   * compression exactly when the tail still fits that block. */
  return (S->buflen + tailmax <= UB_BLOCKBYTES) ? UB_OK : UB_E_GEOMETRY;
}

/* Hot path: copy the state, append the tail into the pending block, finalize.
 * One compression. */
static int finish(const struct ub_state *S, const uint8_t *tail, size_t taillen,
                  void *out, size_t outcap) {
  if (outcap < S->outlen) return UB_E_OUTCAP;
  if (S->buflen + taillen > UB_BLOCKBYTES) return UB_E_GEOMETRY;
  if (S->f[0] != 0) return UB_E_STATE;
  struct ub_state t = *S;
  memcpy(t.buf + t.buflen, tail, taillen);
  t.buflen += taillen;
  return ub_final(&t, out, outcap);
}

int ub_hash_tail(const ub_state *S, const void *tail, size_t taillen,
                 void *out, size_t outcap) {
  if (!S || !out || (taillen && !tail)) return UB_E_ARG;
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
  if (!S || !out) return ub_err(UB_E_ARG, "ub_hash_n", "NULL state or output");
  if (tailwidth != 4 && tailwidth != 8)
    return ub_err(UB_E_ARG, "ub_hash_n", "tailwidth must be 4 or 8");
  if (len == 0) len = S->outlen - off;          /* whole digest from off */
  if (off + len > S->outlen)
    return ub_err(UB_E_ARG, "ub_hash_n", "slice outside the digest");
  if (stride < len)
    return ub_err(UB_E_OUTCAP, "ub_hash_n", "stride below bytes written");
  if (S->f[0] != 0)
    return ub_err(UB_E_STATE, "ub_hash_n", "state already finalized");
  /* Geometry is a property of the state, so validate once, not per digest. */
  if (S->buflen + tailwidth > UB_BLOCKBYTES)
    return ub_err(UB_E_GEOMETRY, "ub_hash_n",
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
