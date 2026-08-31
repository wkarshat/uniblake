/* Internal layout, shared by the .c files in this directory.
 *
 * This header is not part of the interface: it is not copied to an include
 * directory when the library is installed, and nothing outside src/ should
 * include it. The struct below may change between releases — callers get its
 * size from ub_state_size() at runtime instead. */
#ifndef INTERNAL_H
#define INTERNAL_H

#include "uniblake/uniblake.h"
#include <string.h>

/* Everything one hashing computation carries between calls.
 *
 * h, t, f, buf and buflen all change as the message is absorbed. `outlen`
 * does not: it is set at init and only read, by ub_final, which needs to know
 * how many digest bytes to emit and has no other way to learn it. The BLAKE2b
 * reference does the same.
 *
 * The reference also carries a `last_node` flag used only when hashing a
 * tree; this library hashes sequentially, so it is omitted. */
struct ub_state {
  uint64_t h[8];                 /* chaining value */
  uint64_t t[2];                 /* message byte counter */
  uint64_t f[2];                 /* finalization flags */
  uint8_t  buf[UB_BLOCKBYTES];   /* pending block; bytes >= buflen unused */
  size_t   buflen;
  size_t   outlen;
};

extern const uint64_t ub_iv[8];
extern const uint8_t  ub_sigma[12][16];

/* --- the compression function ---
 *
 * ub_compress is the only thing a SIMD, threaded, or accelerator
 * implementation replaces. Everything above it — buffering, the parameter
 * block, finalization, the prefix calls — is written once and does not care
 * which one is loaded.
 *
 * WHAT A REPLACEMENT MUST DO
 *
 *   void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t n);
 *
 * Compress n consecutive 128-byte blocks starting at `blocks`, in order,
 * updating S->h. Specifically, for each block:
 *
 *   1. Add 128 to the 128-bit counter in S->t first, before compressing that
 *      block. S->t[0] is the low half, S->t[1] the high half; increment
 *      S->t[1] when S->t[0] wraps. The counter is mixed into the compression,
 *      so getting this wrong produces wrong digests rather than a crash. The
 *      caller does not touch S->t: every block handed here is counted here.
 *
 *   2. Run the RFC 7693 §3.2 compression: initialise the 16-word working
 *      vector from S->h, the IV, S->t and S->f, apply twelve rounds, and fold
 *      the halves back into S->h with the feed-forward XOR.
 *
 * Read S->h, S->t and S->f. Write only S->h and S->t. Do not touch S->buf,
 * S->buflen or S->outlen — those belong to the streaming layer.
 *
 * `blocks` need not be aligned, and may point into the caller's buffer or
 * into S->buf. n may be zero; return without doing anything.
 *
 * ub_compress_final is finalization: exactly one block whose counter and
 * flags the caller has already set. It must NOT advance S->t. Implementing it
 * as one uncounted compression is the whole job; the shipped version wraps
 * ub_compress and corrects the counter afterwards.
 *
 * A replacement is correct when tests/test_core.c passes: it compares against
 * an independent implementation across message lengths, digest lengths, key
 * lengths and update chunkings, and mismatches in counter handling show up
 * immediately at inputs longer than one block.
 *
 * BATCHING
 *
 * ub_update hands over every whole block it has in a single call, keeping
 * only the last block pending. Hashing one megabyte is three calls, the
 * largest for 8190 blocks — not 8192 calls. An implementation that spreads
 * work across threads, lanes, or a device therefore sees the whole run and
 * can split it however it likes, provided the blocks are folded into S->h in
 * order and the counter ends where sequential processing would leave it.
 *
 * HOW TO SUPPLY ONE
 *
 * Three mechanisms, chosen when the library is built.
 *
 * Default, neither macro defined: ub_compress is an ordinary function in
 * compress.c. Leave that file out of the build and compile one that defines
 * ub_compress instead. No indirection, nothing here changes.
 *
 * -DUB_COMPRESS_FN=my_function: ub_compress becomes a macro naming your
 * function, so calls go straight to it and compress.c is not needed. Useful
 * when a build system selects an implementation and managing object lists is
 * awkward.
 *
 * -DUB_KERNEL_RUNTIME: calls go through a pointer that ub_kernel_set() can
 * change while the program runs, for a binary that inspects the CPU at
 * startup or a test that forces one implementation. Costs an indirect call
 * per compression, so it is off unless asked for. ub_kernel_set(NULL)
 * restores the built-in.
 */
typedef void (*ub_compress_fn)(struct ub_state *S, const uint8_t *blocks,
                               size_t nblocks);

#if defined(UB_COMPRESS_FN)
  void UB_COMPRESS_FN(struct ub_state *, const uint8_t *, size_t);
# define ub_compress UB_COMPRESS_FN
#elif defined(UB_KERNEL_RUNTIME)
  extern ub_compress_fn ub_active_compress;
  int  ub_kernel_set(ub_compress_fn fn);
# define ub_compress ub_active_compress
#else
  void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t nblocks);
#endif
void ub_compress_final(struct ub_state *S, const uint8_t *block);

static inline uint64_t ub_load64(const uint8_t *p) {
  return (uint64_t)p[0]        | ((uint64_t)p[1] <<  8) |
        ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
        ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
        ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static inline void ub_store64(uint8_t *p, uint64_t v) {
  p[0]=(uint8_t)v;       p[1]=(uint8_t)(v>>8);  p[2]=(uint8_t)(v>>16);
  p[3]=(uint8_t)(v>>24); p[4]=(uint8_t)(v>>32); p[5]=(uint8_t)(v>>40);
  p[6]=(uint8_t)(v>>48); p[7]=(uint8_t)(v>>56);
}
static inline uint64_t ub_rotr64(uint64_t w, unsigned c) {
  return (w >> c) | (w << (64 - c));
}
int ub_err(ub_status code, const char *fn, const char *detail);
#endif
