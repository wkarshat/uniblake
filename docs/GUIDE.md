# Using uniblake

For callers. Recipes, the full interface, and adapters for existing APIs.

Start with UniBlake.md for what the library is and why the
prefix case needs a specified absorption rule.

---

# Recipes

Recipes by message shape. Each says what to call, what sizes must hold, and
in what order.

### Terms

- **prefix** — the leading message bytes shared by many digests.
- **tail** — the trailing bytes that differ per digest.
- **prefix state** — a `ub_state` that has absorbed the prefix and nothing
  else. Not modified by the digest calls; safe to share read-only.
- **pending block** — bytes absorbed but not yet compressed. After any
  `ub_update` this is at most one block (128 B).

### Recipe 1 — one message, one digest

```c
ub_state *S = aligned_alloc(ub_state_align(), ub_state_size());
ub_init(S, 32);                 /* 32-byte digest */
ub_update(S, msg, msglen);
ub_final(S, out, sizeof out);   /* out must be >= 32 bytes */
```

Or in one call: `ub_hash(out, 32, msg, msglen, NULL, 0)`.

### Recipe 2 — many digests over a shared prefix

The shape: `H(prefix || tail_0)`, `H(prefix || tail_1)`, … The prefix is
absorbed once; each digest costs one compression.

```c
ub_init_param(S, &P);
ub_update(S, prefix, prefixlen);      /* absorb once */

if (ub_prefix_check(S, tailmax) != UB_OK) { /* geometry will not serve it */ }

ub_hash_tail(S, tail, taillen, out, outcap);   /* repeat; S unchanged */
```

#### Sizing rule

A digest costs one compression only if the pending block has room for the
tail:

    pending(prefixlen) + taillen <= 128

where `pending(n) = n ? ((n - 1) % 128) + 1 : 0`.

BLAKE2b retains a **full** trailing block rather than flushing it —
finalization must mark the last block — so a prefix that is a positive
multiple of 128 leaves 128 bytes pending, not 0. `pending(128) = 128`, and no
tail fits.

Worked sizes, 128-byte block:

| prefixlen | pending | max tail |
|--:|--:|--:|
| 0 | 0 | 128 |
| 100 | 100 | 28 |
| 128 | 128 | 0 |
| 140 | 12 | 116 |
| 256 | 128 | 0 |
| 260 | 4 | 124 |

`ub_prefix_check` returns `UB_E_GEOMETRY` rather than silently costing two
compressions. Call it once after absorbing the prefix: whether the geometry
serves a workload is then a fact known at startup rather than a slowdown found
later.

### Recipe 3 — consecutive counter tails

The shape: tails are `le(i)` for `i` in `first .. first+n-1`, each serialized
little-endian into 4 or 8 bytes.

```c
ub_hash_n(S, 4, first, n, out, stride, outcap);
```

- `stride` — byte spacing between successive digests in `out`. Must be
  `>= digest_length`. Set it to your row width to scatter directly into an
  existing layout with no second copy.
- `out` must hold `n * stride` bytes.
- `outcap` — capacity of one slot, `>= digest_length`.
- Geometry is validated once before the run, not per digest.

For a single counter digest, use `n = 1`:

```c
ub_hash_n(S, 4, i, 1, out, outcap, outcap);
```

### Recipe 4 — prefix changes, parameters do not

When the prefix changes, re-absorb:

```c
ub_init_param(S, &P);            /* same P */
ub_update(S, new_prefix, len);
```

A prefix state is valid only for the exact prefix it absorbed. If varying
bytes precede the tail in the message, they are part of the prefix, and the
state must be rebuilt whenever they change. Rebuilding costs two compressions
against however many digests follow.

### Parameters

`ub_param` is the RFC 7693 §2.5 parameter block. `ub_param_init(&P, len)`
sets `digest_length = len`, `fanout = 1`, `depth = 1`, everything else zero.

```c
ub_param P;
ub_param_init(&P, 48);
memcpy(P.personal, tag, 16);     /* personalization: 16 bytes, domain-separates */
ub_init_param(S, &P);
```

- `personal` and `salt` are 16 bytes each, absorbed into the IV at init.
  They are **not** message data and cost no compression.
- Keys are not settable through `ub_param`: the key bytes are absorbed as a
  zero-padded 128-byte first block. Use `ub_init_key(S, outlen, key, keylen)`.
- Any value that varies per digest is message data, not a parameter. It goes
  through `ub_update` and becomes part of the prefix.

### Ordering

    ub_init* -> ub_update* -> ub_final
                          \-> ub_hash_tail / ub_hash_n   (repeatable, S unchanged)

- `ub_final` consumes the state: a second `ub_final`, or any `ub_update`
  after it, returns `UB_E_STATE`.
- `ub_hash_tail` and `ub_hash_n` do not consume. Call them any number of
  times on one prefix state, from any number of threads.
- `ub_init*` fully resets, so a state may be reused without clearing.
- `ub_copy(dst, src)` duplicates a state; both halves continue independently.

### Threading

Two rules:

- One `ub_state` must not be mutated from two threads.
- A prefix state used only through `ub_hash_tail` or `ub_hash_n` is never
  mutated, so sharing it read-only across threads is safe. To stream on
  several threads, give each its own state via `ub_copy`.

The library itself spawns no threads. Parallelism attaches at `ub_hash_n`,
which takes the whole digest range in one call so a caller's own scheduler can
split it.

### Errors

Every function returns `UB_OK` (0) or a negative `ub_status`. Batch entry
points validate once and return one code.

```c
ub_set_error_handler(my_handler, my_cookie);
```

Installs a callback receiving `(code, function_name, detail, cookie)`. Default
prints one line to stderr. `cookie` is passed back unmodified and never
interpreted. The library never aborts or exits.


---

# Interface reference

A minimal core mirroring the normative reference, one optional layer for
repeated-prefix hashing, and room to grow into parallel and offload backends
without changing the core.

### 1. Naming

- One prefix `ub_`; one state type `ub_state`.
- Verbs from the reference: `init`, `update`, `final`.
- `ub_init_param` takes a parameter block; `ub_init` and `ub_init_key` take
  the arguments they name.

### 2. Core API

Mirrors `blake2.h` one-for-one so a reader of the reference needs no
translation.

```c
typedef struct ub_state ub_state;          /* opaque, caller-allocated */
size_t ub_state_size(void);
size_t ub_state_align(void);

typedef struct {                            /* RFC 7693 s2.5 parameter block */
  uint8_t  digest_length, key_length, fanout, depth;
  uint32_t leaf_length, node_offset, xof_length;
  uint8_t  node_depth, inner_length;
  uint8_t  salt[UB_SALTBYTES];
  uint8_t  personal[UB_PERSONALBYTES];
} ub_param;

int ub_init      (ub_state *S, size_t outlen);
int ub_init_key  (ub_state *S, size_t outlen, const void *key, size_t keylen);
int ub_init_param(ub_state *S, const ub_param *P);
int ub_update    (ub_state *S, const void *in, size_t inlen);
int ub_final     (ub_state *S, void *out, size_t outlen);
int ub_hash      (void *out, size_t outlen, const void *in, size_t inlen,
                  const void *key, size_t keylen);
```

Six functions, one type, one struct. Every name and parameter is the
reference's with `blake2b` replaced by `ub`.

### 3. Prefix layer

For `H(prefix || tail_i)` over many `i`. The core cannot express "absorb the
prefix once" -- whether that happens is an unspecified buffering detail, and
libsodium gets it wrong (s6).

```c
int ub_copy(ub_state *dst, const ub_state *src);

/* One digest: H(prefix-absorbed S || tail). S is not modified. */
int ub_hash_tail(const ub_state *S, const void *tail, size_t taillen,
                 void *out, size_t outlen);

/* Does S admit one-compression digests with a tail of `tailmax` bytes? */
int ub_prefix_check(const ub_state *S, size_t tailmax);

/* n digests over consecutive counters, each serialized little-endian into
   `tailwidth` bytes (4 or 8), written at `stride` spacing. */
int ub_hash_n(const ub_state *S, size_t tailwidth, uint64_t first, size_t n,
              size_t off, size_t len, void *out, size_t stride);
```

`ub_hash_tail` and `ub_hash_n` take `const ub_state *` and do not mutate it:
one prefix state serves any number of digests and may be shared read-only
across threads.

`ub_update` compresses each whole block as soon as more input follows, so a
prefix state holds at most one partial block and `ub_hash_tail` costs exactly
one compression; the sizing rule above gives the condition.

### 4. Drop-in replacement for libsodium

Exact mapping. Types, storage, and ownership included.

| libsodium | uniblake | notes |
|---|---|---|
| `crypto_generichash_blake2b_state` | `ub_state` | libsodium: `unsigned char opaque[384]`, `CRYPTO_ALIGN(64)`. uniblake: opaque, `ub_state_size()` / `ub_state_align()`. Both caller-allocated; neither library allocates. |
| `crypto_generichash_blake2b_init(S, key, keylen, outlen)` | `ub_init_key(S, outlen, key, keylen)` | Argument ORDER DIFFERS. libsodium puts key first; the reference and uniblake put `outlen` first. A shim must reorder. `key=NULL, keylen=0` -> `ub_init(S, outlen)`. |
| `crypto_generichash_blake2b_init_salt_personal(S, key, keylen, outlen, salt, personal)` | `ub_init_param(S, &P)` | Fill `P.digest_length`, `P.salt`, `P.personal`; leave the rest zero except `fanout = depth = 1`. |
| `crypto_generichash_blake2b_update(S, in, inlen)` | `ub_update(S, in, inlen)` | `inlen` type DIFFERS: libsodium `unsigned long long`, uniblake `size_t` (the reference's type). Identical on LP64. |
| `crypto_generichash_blake2b_final(S, out, outlen)` | `ub_final(S, out, outlen)` | `outlen` is the output CAPACITY in both, checked against the init-time digest length. |
| `state_b = state_a;` (struct assign) | `ub_copy(&b, &a)` | libsodium's state is a fixed-size array, so plain `=` works. uniblake's is opaque, so copying needs a call. This is the one source change a consumer cannot avoid. |
| `crypto_generichash_blake2b_PERSONALBYTES` (16) | `UB_PERSONALBYTES` (16) | same value |
| `crypto_generichash_blake2b_SALTBYTES` (16) | `UB_SALTBYTES` (16) | same value |
| returns `int`, 0 = ok | returns `int`, 0 = ok, negative `ub_status` | Existing `!= 0` checks keep working. |

#### Shim

A consumer keeping libsodium call sites needs this and nothing else:

```c
#define crypto_generichash_blake2b_state              ub_state
#define crypto_generichash_blake2b_PERSONALBYTES      UB_PERSONALBYTES
#define crypto_generichash_blake2b_SALTBYTES          UB_SALTBYTES

static inline int
crypto_generichash_blake2b_init(ub_state *S, const unsigned char *k,
                                size_t klen, size_t outlen)
{ return klen ? ub_init_key(S, outlen, k, klen) : ub_init(S, outlen); }

static inline int
crypto_generichash_blake2b_init_salt_personal(ub_state *S,
    const unsigned char *k, size_t klen, size_t outlen,
    const unsigned char *salt, const unsigned char *personal)
{
  if (klen > UB_KEYBYTES) return UB_E_ARG;   /* else the cast below truncates */
  ub_param P; ub_param_init(&P, outlen);     /* checks outlen; sets fanout/depth */
  if (salt)     memcpy(P.salt, salt, UB_SALTBYTES);
  if (personal) memcpy(P.personal, personal, UB_PERSONALBYTES);
  if (klen) {
    P.key_length = (uint8_t)klen;
    int rc = ub_init_param(S, &P);
    if (rc != UB_OK) return rc;
    /* The key is absorbed as ONE ZERO-PADDED 128-byte block (RFC 7693 §2.9).
     * Feeding the raw key here instead -- ub_update(S, k, klen) -- sets the
     * parameter block correctly but produces a wrong digest for every keyed
     * call, because the padding is part of the hashed message. */
    unsigned char blk[UB_BLOCKBYTES];
    memset(blk, 0, sizeof blk);
    memcpy(blk, k, klen);
    return ub_update(S, blk, UB_BLOCKBYTES);
  }
  return ub_init_param(S, &P);
}

#define crypto_generichash_blake2b_update  ub_update
#define crypto_generichash_blake2b_final   ub_final
```

`update` and `final` map by name alone. Only the two `init` forms need a
wrapper, because libsodium orders arguments differently and encodes the
parameter set in the function name.

**The one thing a shim cannot hide:** `ub_state` is opaque, so
`state_b = state_a;` must become `ub_copy(&b, &a)`: `ub_state` is opaque, so
it has no assignable value type.

Storage: a caller may place `ub_state` in automatic, static, or heap
storage; the library holds no pointer into it and never allocates. It is not
internally locked -- one state per thread, copied via `ub_copy`.

### 5. Extension points

Added later without touching s2 or s3.

```c
/* backend selection: scalar, SIMD, threaded, offload */
typedef enum { UB_BACKEND_AUTO = 0, UB_BACKEND_REF, UB_BACKEND_SIMD,
               UB_BACKEND_THREADED, UB_BACKEND_DEVICE } ub_backend;
int          ub_backend_set(ub_backend b);
ub_backend   ub_backend_get(void);
const char  *ub_backend_name(ub_backend b);
```

`ub_hash_n` is where a parallel backend attaches. It takes `(first, n, off, len, out, stride)` -- a
range of independent digests and where to scatter them -- which is exactly
the shape a thread pool, a SIMD kernel processing k digests per lane, or a
GPU dispatch needs. A parallel backend changes what `ub_hash_n` does, not
its signature.

`stride` exists so a backend can write directly into a caller's row layout
with no second copy. That matters most for the offload case, where a copy is
a device transfer.

For device backends the state must be transferable: `ub_export` / `ub_import`
(versioned byte image) serialize a prefix-absorbed state to upload. GPU
implementations already do this by hand, uploading the eight chaining-value
words directly.

### 6. Why the prefix layer exists

libsodium buffers 256 bytes and compresses only on overflow. After absorbing
a 140-byte prefix its counter is 0 — nothing compressed — so every digest
re-absorbs the whole prefix: 2.7 compressions per digest against 1. Measured
289.3 ns vs 110.3 ns on the machine recorded with the measurements.

`ub_update` compresses eagerly, so the prefix is absorbed once.

### 7. Prefix validity

A prefix state is valid for exactly the bytes it absorbed. In a message
`fixed || varying || tail`, the `varying` field is part of the prefix: when it
changes, the state must be rebuilt. Only `tail` may differ between digests
served by one state.

Rebuilding costs the prefix's compressions once, amortized over the digests
that follow.

Sizing: for a 140-byte prefix the first 128-byte block is fully inside the
prefix and 12 bytes remain pending, leaving room for tails up to 116 bytes in
one final block. The general rule and a size table are in the recipes above.


---

# Adapters

Header-only shims in `compat/`, letting existing call sites use uniblake
without source changes. Opt-in: nothing in `include/` or `src/` refers to
them, and a caller that ports its call sites never includes one.

### libsodium

`compat/ub_sodium.h`

Covers `crypto_generichash_blake2b_*`. Include it instead of `sodium.h` for
BLAKE2b use.

| libsodium | maps to | note |
|---|---|---|
| `crypto_generichash_blake2b_state` | `ub_state` | opaque; see below |
| `..._init(S, key, keylen, outlen)` | `ub_init` / `ub_init_key` | argument order differs: libsodium puts the key first |
| `..._init_salt_personal(S, key, keylen, outlen, salt, personal)` | `ub_init_param` | key absorbed as a zero-padded 128-byte block |
| `..._update(S, in, inlen)` | `ub_update` | `inlen` widens from `unsigned long long` to `size_t` |
| `..._final(S, out, outlen)` | `ub_final` | `outlen` is output capacity in both |
| `..._PERSONALBYTES`, `..._SALTBYTES` | `UB_PERSONALBYTES`, `UB_SALTBYTES` | both 16 |

**One source change is unavoidable.** libsodium's state is a fixed-size array,
so `state_b = state_a;` copies it. `ub_state` is opaque and has no assignable
value type; that becomes `ub_copy(&b, &a)`.

Conformance: `compat/test_compat.c` checks the adapter against real
libsodium across digest lengths, key lengths, and salt/personalization.

### RFC 7693 sample code

`compat/ub_rfc.h`

Covers the Appendix-A API: `blake2b_ctx`, `blake2b_init`, `blake2b_update`,
`blake2b_final`, `blake2b`.

`blake2b_final(ctx, out)` takes no output capacity — the RFC fixes the digest
length at init and the sample code trusts the caller's buffer. The adapter
must pass `UB_OUTBYTES` and cannot check, so a caller whose buffer is smaller
than 64 bytes gets no diagnosis. Prefer the core API where possible.

### BLAKE2 reference (`blake2.h`)

`compat/ub_blake2.h`

uniblake already follows this API's names and argument order, so the shim is
aliases rather than translation. Include it instead of `blake2.h` and
sequential BLAKE2b call sites compile unchanged.

| reference | uniblake |
|---|---|
| `blake2b_state` | `ub_state` |
| `blake2b_init(S, outlen)` | `ub_init` |
| `blake2b_init_key(S, outlen, key, keylen)` | `ub_init_key` |
| `blake2b_init_param(S, P)` | `ub_init_param` |
| `blake2b_update(S, in, inlen)` | `ub_update` |
| `blake2b_final(S, out, outlen)` | `ub_final` |
| `blake2b_param` | `ub_param` |

`ub_param` is not packed and carries no reserved bytes: `ub_init_param`
serializes the 64-byte block field by field, so no packing pragma is needed
and `sizeof(ub_param)` is not part of the interface.

The reference's `blake2b_state` carries a `last_node` field used only for tree
hashing; `ub_state` omits it.

Not covered, because it cannot be shimmed: `blake2bp`, `blake2sp` and
`blake2s` are different algorithms or tree modes; assigning one state to
another becomes `ub_copy`, the one unavoidable source change.

Conformance: `compat/test_blake2_alias.c` (`make check-alias`) checks the
shim against the vendored reference itself, across digest lengths, message
lengths, key lengths, and the salt/personalization parameter block. The oracle
is vendored under `compat/ref`, so this suite needs no libsodium.

### C++ wrapper

`compat/uniblake.hpp` -- header-only, C++11, no Boost. Optional: nothing in
`include/` or `src/` requires it, and the C interface works unchanged from
C++. It exists because three things are awkward at a C++ call site --
allocating aligned storage by hand, checking a return code on every call where
the surrounding code uses exceptions, and proving to a reader that a shared
prefix state is not modified.

```cpp
#include "uniblake.hpp"

uniblake::Params P(48);
P.personal("my-app-v1");

uniblake::Prefix pre(P, header.data(), header.size(), /*max_tail=*/4);

auto digests = pre.hash_n(0, count);                // count digests, packed
auto field   = pre.hash_n_field(0, count, 24, 24);  // one 24-byte field of each
```

`Prefix` absorbs the leading bytes once in its constructor and exposes only
const methods, so sharing one across threads is a property of the type. The
constructor runs the geometry check and throws if the sizes cannot give
one-compression digests -- a mistake otherwise found at the first hashing
call.

`State` owns aligned storage and is copyable, with copy meaning what `ub_copy`
means: the computation continues independently on both sides. That makes the
long-tail case ordinary C++:

```cpp
auto st = pre.resume();      // a copy of the prefix state
st.update(part1); st.update(part2);
auto digest = st.final(48);
```

Errors are exceptions at construction and in the value-returning helpers,
because that is where a C++ caller cannot inspect a return code.
`uniblake::Error` carries the `ub_status`, so a caller that wants the code
still has it. The C interface never throws, so a hot loop where failure is
impossible by construction can call `ub_hash_n` directly while using `Prefix`
for setup.

The wrapper uses `<memory>`, `<vector>`, `<string>`, `<stdexcept>`. A codebase
preferring optional-returns over exceptions can write that variant over the
same C calls.

Where a digest length or personalization is fixed at compile time, a class
template keeps it there:

```cpp
template <unsigned N>
struct Params {
    static_assert(N % 8 == 0, "N must be a whole number of bytes");
    static uniblake::Params make() {
        uniblake::Params P((512 / N) * N / 8);
        return P.personal("my-app-v1");
    }
};
```

### Writing a new adapter

Adapters are header-only and live in `compat/`. Three points to check:

1. **Argument order** — distributions disagree on where the key and digest
   length go.
2. **Capacity vs length** — some APIs pass the digest length at finalize,
   others the buffer capacity, others nothing.
3. **State copying** — any API whose state is a value type needs its
   assignments rewritten to `ub_copy`.

Verify against the distribution being replaced, not against uniblake: the
adapter's job is to reproduce the original byte-for-byte.

