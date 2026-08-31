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

### CPU detection is not part of this library

Runtime selection (`-DUB_KERNEL_RUNTIME`) takes a function pointer; it does not
decide which function. Deciding needs CPU identification — vendor, ISA
features, and the specific core — and that is not BLAKE2b's business: the same
detection serves any library, and BLAKE2b has no say in how a host wants to
probe.

If a binary needs it, put it beside the adapters as an ordinary
`sys_probe.c` / `sys_probe.h` with no `ub_` names and no dependency on this
library, and have the host call `ub_kernel_set()` with the result at startup.
That keeps the probe reusable and keeps the library free of platform
detection.

Note that ISA support is the wrong question to ask it. The same NEON code
measures faster than scalar on some ARM cores and slower on others — measured
here at 1.5x slower — so a probe that answers "has NEON" selects wrongly. What
a selector needs is the core identity plus a list of cores where a given
implementation has been measured to win.

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

### Code-shape changes that were tried and rejected

Measured on an Apple M4 Pro, prefix-digest shape, median of 9 runs of 400k
digests. A digest costs 98 ns, of which the single block compression is 79%
and the state copy 2%; the compression is where any real gain has to come
from.

| change | result |
|---|---|
| `#pragma clang loop unroll(full)` on the 12-round loop | **27% slower** (98 -> 114 ns; stack traffic 0 -> 398) |
| the same plus `__builtin_assume_aligned` on the block | 27% slower |
| hoisting the message-schedule row out of the round | no change |
| finalizing without copying the whole state | ~1%, inside noise |
| column-major working vector | no change (95.2-96.7 ns either way) |

Unrolling is the notable one, and the reason is register pressure, not code
size. The compiled body grows from 996 bytes to 8,912 — still far inside this
core's 192 KB L1 instruction cache, so instruction fetch is not the problem.
What changes is spilling: the rolled loop has **no** stack traffic at all,
while the unrolled body has 398 stack loads and stores. The working vector is
sixteen 64-bit words plus sixteen message words, already more than the 31
general-purpose registers; the rolled loop keeps one round's values live at a
time and fits, while the unrolled body presents the allocator with twelve
rounds' worth at once and it spills.

Since the rolled loop already spills nothing, layout changes aimed at the
allocator have nothing to recover — which is why the column-major experiment
measured no difference.

This matches `-O3` measuring slower than `-O2` on the same code.

The lesson for a replacement: gains come from doing fewer or wider operations,
not from unrolling or from alignment hints. That means SIMD across independent
messages, or threads, not a differently-shaped scalar loop.

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

Apple M4 Pro (arm64, 14 cores), Apple clang, -O2, libsodium 1.0.21.
Median of 7 reps x 400k digests. Reproduce with `make bench`.

Figures from one machine and one compiler. Run the benchmarks on the target
rather than carrying these forward.

## Per-digest

Prefix 140 B, digest 50 B: the prefix crosses one block boundary, leaving 12
bytes pending and room for a 4-byte tail in the final block.

| build | streaming | batch (`ub_hash_n`) |
|---|--:|--:|
| libsodium | 285.0 ns | — |
| uniblake scalar | 92.9 ns | 96.4 ns |
| uniblake NEON | 138.6 ns | 141.4 ns |
| uniblake, 4 threads | 93.4 ns | 26.2 ns |
| uniblake, 8 threads | 93.2 ns | 13.4 ns |

The gain over libsodium is in `ub_update`, which compresses a full pending
block as soon as more input follows: a state absorbed over a 140-byte prefix
already holds one compressed block, so each digest costs one more. libsodium
buffers 256 bytes and compresses only on overflow, so after the same prefix
its counter is zero and every digest re-absorbs the prefix — 2.7 compressions
per digest against 1.

## Per workload

Nanoseconds per digest are hard to weigh. The same numbers as the hashing time
for one proof-of-work solve, at two common parameter sets:

| parameters | digests per solve |
|---|--:|
| (192, 7) | 16,777,216 |
| (200, 9) | 1,048,576 |

| build | (192,7) hashing | (200,9) hashing |
|---|--:|--:|
| libsodium | 4.8 s | 0.30 s |
| uniblake scalar | 1.6 s | 0.10 s |
| uniblake NEON | 2.4 s | 0.15 s |
| uniblake, 4 threads | 0.44 s | 0.027 s |
| uniblake, 8 threads | 0.22 s | 0.014 s |

These are the hashing phase only, and they are a **lower bound**: a solver also
copies the state per digest, writes each result into its own layout, and
extracts more than one hash from each digest. Measured inside one such solver,
libsodium costs 337 ns per call against the 285 ns here — 18% more — so expect
the same margin on top of every row.

Against a whole solve of 8.3 s, of which 5.7 s was hashing, replacing the hash
alone predicts:

| build | solve | speedup |
|---|--:|--:|
| libsodium (baseline) | 8.3 s | 1.00x |
| uniblake scalar | 4.3 s | 1.95x |
| uniblake, 4 threads | 3.1 s | 2.70x |
| uniblake, 8 threads | 2.9 s | 2.90x |

Threading past four threads gains little because the hashing phase is no
longer the limit: at 8 threads it is 0.22 s of a 2.9 s solve. Everything
beyond that is in the code that consumes the digests.

## Conformance

| suite | checks | needs an oracle | covers |
|---|--:|---|---|
| `tests/test_core.c` | 630 | yes | streaming, parameters, keys, the RFC vector |
| `tests/test_prefix.c` | 45,531 | yes | prefix geometry, counters, slices, batching |
| `tests/test_api.c` | 24 | no | return codes, call ordering, the error handler |
| `compat/test_compat.c` | 53 | yes | the libsodium adapter |
| `tests/test_negative.c` | 7 | yes | that the checks above can fail |

`tests/test_negative.c` links a compression function with one round removed and
requires every oracle comparison — the RFC vector, unkeyed, keyed,
personalized, and the prefix path — to reject it. A suite that cannot fail
proves nothing, and a mistake that made a comparison vacuous would otherwise
look like success. Run it with `make check-negative`.

The NEON and threaded backends pass `tests/test_core.c` and
`tests/test_prefix.c` unchanged; `make check-backends` runs them.

Oracle is libsodium — an independent implementation. `include/` and `src/`
link nothing.
