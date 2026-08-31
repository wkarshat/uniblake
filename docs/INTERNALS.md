# uniblake internals

For implementers: porting to a new platform, writing a compression backend,
and the definitions the interface rests on.

Callers want GUIDE.md instead.

---

# State

A naming rule is worthless if "state" can mean four things. This fixes the
term, gives a test for applying it, and classifies every structure in the
BLAKE family against it.

### 1. Definition

> **state** — the complete mutable data an implementation carries *between*
> successive absorb operations of ONE hashing computation, and nothing else.

Three clauses, each doing work:

- **complete** — copying it and continuing from the copy yields bit-identical
  results. If a second structure must travel with it, the pair is the state
  and neither half is.
- **mutable, between absorbs** — what `update` reads and writes. Values fixed
  at `init` and never written again are *parameters*, not state.
- **one computation** — a structure holding several concurrent computations is
  a collection of states, not a state.

### 2. When it is NOT a state

Apply in order. The first that matches decides.

**(a) It is a parameter, not state.** Written once at `init`, read-only
thereafter, and reconstructible from the caller's arguments. `blake2b_param`
is a parameter block: 64 bytes describing *what to compute*. It is XORed into
the IV at init and never consulted again.

*Test:* if you can discard it after `init` and still finish the hash, it is a
parameter.

**(b) It is a collection of states.** Holds N independent hashing computations,
each with its own lifetime, advanced independently. `blake2bp_state` holds
`blake2b_state S[4][1]` — four leaf states plus a root — because BLAKE2bp is
a tree over four parallel lanes.

*Test:* if a field is an array of things each satisfying s1, it is a
collection. Name it for what it manages (`ub_pool`, `ub_tree`), never `state`.

**(c) It is a composite with parts of independent lifetime.** Contains a state
*and* something with its own parametrization and lifetime. `blake2xb_state` is
`{ blake2b_state S[1]; blake2b_param P[1]; }` — the param outlives the inner
state, which is reinitialized per output block. `blake3_hasher` is
`{ key[8]; blake3_chunk_state chunk; cv_stack[] }` — the chunk state is reset
per 1 KiB chunk while the CV stack accumulates across the whole message.

*Test:* if two fields are reset or replaced on different schedules, the
container is a composite. Name it for the machine (`hasher`, `ub_tree`), and
name the inner part `state`.

**(d) It is a snapshot.** A versioned byte image for transport or persistence.
Not live: it cannot be updated, only imported. Name it `image` or `snapshot`.

### 3. Classification

| Structure | Verdict | Why |
|---|---|---|
| `blake2b_state` | **state** | h, t, f, buf, buflen — all mutated by update. `outlen` and `last_node` are parameters carried inside it (see s4). |
| `ub_state` | **state** | same, minus `last_node`. |
| `blake2b_param` | parameter | (a) — discardable after init |
| `blake2bp_state` | collection | (b) — `blake2b_state S[4][1]` + root |
| `blake2sp_state` | collection | (b) — 8 lanes |
| `blake2xb_state` | composite | (c) — state + param, different lifetimes |
| `blake3_chunk_state` | **state** | cv, counter, buf, flags — one chunk's absorb state |
| `blake3_hasher` | composite | (c) — chunk state (per-chunk) + CV stack (per-message) + key (per-init) |
| libsodium `crypto_generichash_blake2b_state` | **state** | opaque `unsigned char[384]`; a `blake2b_state` behind a fixed-size array |
| uniblake snapshot image | snapshot | (d) — `ub_export`/`ub_import` |

### 4. The impurity in BLAKE2b, named

`blake2b_state` carries `outlen` and `last_node`, which are parameters by
test (a) — fixed at init, never written by update. They live inside the state
because `final` needs them and the reference API gives `final` no other way
to receive them.

This is worth stating rather than hiding: **`blake2b_state` is a state with
two parameter fields embedded for API convenience.** It is why
`sizeof(blake2b_state)` is not the mathematical state, and why uniblake's
drop of `last_node` is a scope decision, not a correctness one.

BLAKE3 removed the impurity: no `outlen` in the hasher, because output length
is chosen at `finalize` rather than `init`.

### 5. What a caller can rely on

- `ub_state` is a value. `ub_copy` duplicates it completely; both halves
  continue independently, and neither refers to the other or to any external
  storage.
- It holds no pointer, no handle, and no lock. It may live in automatic,
  static, or heap storage, and may be memcpy'd, provided alignment is at
  least `ub_state_align()`.
- Its size is `ub_state_size()`, reported at runtime. It is not a
  compile-time constant and may change between releases.
- There is one state type. Anything managing several concurrent computations
  is a different type with a different name.


---

# The compression kernel

`ub_compress` is the only function a SIMD, threaded, or offload backend
replaces. Three ways, in increasing order of runtime cost.

### 1. Link-time (default)

`src/compress.c` defines `ub_compress`. Drop it from the build and compile a
different file defining the same symbol:

    cc -c src/core.c src/const.c src/prefix.c my_neon_compress.c

No source change, no indirection, no runtime dispatch. This is how a build
targeting a known machine should do it. It is a whole-library choice: one
kernel per binary.

### 2. Compile-time (`UB_COMPRESS_FN`)

    cc -DUB_COMPRESS_FN=my_avx2_compress ...

`ub_compress` becomes a macro naming your function, so the call inlines and
`src/compress.c` is not linked at all. Same cost as option 1, but selectable
from the build system without editing object lists.

### 3. Runtime (`ub_kernel_set`)

    int ub_kernel_set(ub_compress_fn fn);   /* NULL restores the built-in */

For a binary that probes the CPU at startup, or a test that forces a specific
kernel. Costs one indirect call per compression -- about 0.5% of a compress
on arm64, measured -- so it is off unless `UB_KERNEL_RUNTIME` is defined.
The library does not probe; selection policy belongs to the host.

### What a replacement must do

```c
void ub_compress(struct ub_state *S, const uint8_t *blocks, size_t n);
```

Compress `n` consecutive 128-byte blocks in order, updating `S->h`. For each
block:

1. **Add 128 to the counter first.** `S->t[0]` is the low half, `S->t[1]` the
   high half; increment the high half when the low half wraps. The counter is
   mixed into the compression, so an error here yields wrong digests rather
   than a crash. Every block passed here is counted here — the caller does not
   touch `S->t`.
2. Run the RFC 7693 §3.2 compression: build the 16-word working vector from
   `S->h`, the IV, `S->t` and `S->f`; twelve rounds; fold back into `S->h`
   with the feed-forward XOR.

Read `S->h`, `S->t`, `S->f`. Write only `S->h` and `S->t`. Leave `S->buf`,
`S->buflen` and `S->outlen` alone — they belong to the streaming layer.

`blocks` need not be aligned and may point into `S->buf`. `n` may be zero.

`ub_compress_final` handles finalization: one block whose counter and flags
the caller has already set, so it must **not** advance `S->t`. The shipped
version wraps `ub_compress` and corrects the counter afterwards.

### Batching

`ub_update` passes every whole block it holds in one call, keeping only the
last block pending. Hashing one megabyte is three calls, the largest covering
8190 blocks — not 8192 calls. An implementation spreading work across threads,
lanes, or a device sees the whole run and may split it freely, provided blocks
fold into `S->h` in order and the counter finishes where sequential processing
would leave it.

### Where acceleration actually pays

Vectorising the compression of a *single* message gives roughly 1.5–2x over
scalar: one BLAKE2b message has limited internal parallelism, and the wins
diminish quickly.

Running several *independent* messages across SIMD lanes gives considerably
more — 4x and up per core on AVX2 — because each lane holds a whole separate
hash. `ub_hash_n` is the entry point shaped for this: it hands over a run of
independent digests and the layout to write them into.

Two cautions from the BLAKE2b record:

- NEON is not a reliable win for BLAKE2b. The 2-lane width does not cover the
  cost against wide scalar execution on several ARM cores, and published
  results have the same NEON code faster than scalar on one core and slower on
  another. Measure before adopting.
- A hash speedup caps out at the share of runtime the hash occupies. If
  hashing is a fifth of a workload, a 2x hash is a 1.2x workload at best.

### Reading a performance number

A figure without its machine is an anecdote. The same code changes rank
between CPU generations, so any number carried from one box to another is a
guess. State the exact CPU, compiler, flags and input shape with every
measurement, and prefer re-running `make bench` on the target over trusting a
recorded figure.

Measure the shape the caller actually uses. A bulk-throughput loop and a
short-message loop can rank implementations differently, and optimising
against the wrong one is how a kernel gets faster on a benchmark and slower in
use. `bench/bench_prefix.c` reports the repeated-prefix shape for that reason.

### Checking a replacement

`make check` is the test. Three kinds of evidence are admissible, in
decreasing strength:

1. **Byte agreement with an independent implementation.** `tests/test_core.c`
   and `tests/test_prefix.c` compare every digest against libsodium across
   message lengths 0–600, digest lengths 1–64, key lengths, 29 update
   chunkings, and the whole prefix geometry range. Counter mistakes surface
   immediately at inputs longer than one block.
2. **Published test vectors.** `tests/test_core.c` also checks the RFC 7693
   Appendix-A `"abc"` vector, which needs no second implementation and so
   works where libsodium is unavailable.
3. **Agreement observed through a real caller.** Weakest as a unit check, but
   it catches integration drift the first two miss.

A change that alters the operations or their order — the rounds, the G
function, the message schedule, the parameter block, finalization — needs (1)
in full plus a re-measurement. A change that only touches build glue,
`#ifdef`s, or linkage is byte-invariant by construction, so (1) alone
suffices.

Writing a scalar replacement from this section alone and running both suites
is a reasonable way to confirm the requirements are complete before starting
on a vectorised one.


# Porting and profiling

### Requirements

C99, plus `_Alignof` for `ub_state_align()`. No POSIX, no allocation, no
threads, no floating point. Endian-neutral: all serialization is explicit
byte-shifting, so the library runs unchanged on big-endian targets.

Freestanding builds need `memcpy`, `memset`, and — unless the default error
handler is replaced — `fprintf`/`stderr`.

### Sizes

`ub_state_size()` and `ub_state_align()` are reported at runtime and are not
part of the ABI. Do not embed a literal.

### Bringing up a new platform

1. `make check` — 630 core + 45,535 prefix + adapter conformance. All must
   pass before any optimization work.
2. `make bench` — establishes the baseline ns/digest on the target.
3. Replace the kernel (previous section) and re-run both.

The conformance suites compare against libsodium, so a target needs libsodium
to run them. If that is unavailable, `tests/test_core.c` also checks the RFC
7693 Appendix-A test vector, which requires nothing.

### Profiling

The cost of a digest is a whole number of compressions. Before profiling,
determine that number:

- streaming a message of `n` bytes: `ceil((n+1)/128)` compressions,
- a prefix digest: 1, if `ub_prefix_check` returns `UB_OK`.

If measured time per digest is not close to that count times the compression
cost, the geometry is wrong — check the sizing rule in the guide
before looking at the kernel.

Compression cost is measurable directly: hash a large buffer with `ub_hash`
and divide by `size/128`.

Higher optimization levels are not always faster: the compression body is
memory-bound and unrolling can cost more than it saves. Measure `-O1` against
`-O2` on the target rather than assuming.

### Writing a backend

See the requirements above. Two properties matter for
performance:

- `ub_update` hands the kernel every whole block in **one** call, so a
  batched, threaded, or offloaded kernel sees the entire range rather than
  one block at a time.
- `ub_hash_n` takes a count, a stride, and a digest slice, so a backend can
  distribute a range across lanes, threads, or devices and write results
  directly into the caller's layout.

A replacement must keep the counter rule: `S->t` advances by 128 before each
block the kernel is given.

### Cross-checking a port

A port that passes `make check` agrees with an independent implementation on
every shape this library exposes. `tests/test_api.c` needs no oracle, so it
runs anywhere and is a useful first signal on a bare target.


---

# Measurements and conformance

Apple M4 Pro (arm64), Apple clang, -O2. libsodium 1.0.21.
Median of 7 reps x 400k digests. Reproduce with `make bench`.
Prefix 140 B, digest 50 B: the prefix crosses one block boundary, leaving 12
bytes pending and room for a 4-byte tail in the final block.

Figures from one machine and one compiler. Run `make bench` on the target
rather than carrying them forward.

| path | ns/digest | vs libsodium |
|---|--:|--:|
| libsodium streaming | 280.2 | 1.00x |
| uniblake streaming | 90.6 | 3.09x |
| `ub_hash_n` | 89.7 | 3.12x |

The gain is in `ub_update`, which compresses a full pending block as soon as
more input follows: a state absorbed over a 140-byte prefix already holds one
compressed block, so each digest costs one more. libsodium buffers 256 bytes
and compresses only on overflow, so after the same prefix its counter is 0 and
every digest re-absorbs the prefix — 2.7 compressions per digest against 1.

`ub_hash_n` measures at parity with the streaming path. It provides the
geometry guarantee, leaves the prefix state unmodified, and is the entry point a
parallel backend replaces.

### Conformance

| suite | checks | needs an oracle | covers |
|---|--:|---|---|
| `tests/test_core.c` | 630 | yes | streaming, parameters, keys, the RFC vector |
| `tests/test_prefix.c` | 45,531 | yes | prefix geometry, counters, slices, batching |
| `tests/test_api.c` | 24 | no | return codes, call ordering, the error handler |
| `compat/test_compat.c` | 53 | yes | the libsodium adapter |

Core: RFC 7693 Appendix A "abc" KAT; input lengths 0..600 x digest lengths
1..64; keyed 1..64-byte keys; salt + personalization; chunked updates at 29
step sizes; copy independence; error surface.

Prefix: geometry sweep (prefix 0..260, 66 lengths) x digest lengths 1..64 x
personalization on/off; LE32/LE64 counter boundaries incl. 2^32 and ~0;
raw tails 0..28; batch vs single over 3000 digests; shared state verified
byte-identical after `ub_hash_n`; error surface.

Oracle is libsodium — an independent implementation. The `api/` tree links
nothing from `uniblake/` or `vendor/`.

