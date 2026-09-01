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
state, which is reinitialized per output block. A tree hasher pairing a
per-chunk absorb state with a chaining-value stack that accumulates across the
whole message is the same shape: two parts on different schedules.

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
| libsodium `crypto_generichash_blake2b_state` | **state** | opaque `unsigned char[384]`; a `blake2b_state` behind a fixed-size array |
| uniblake snapshot image | snapshot | (d) — `ub_export`/`ub_import` |

### 4. The impurity in BLAKE2b, named

`blake2b_state` carries `outlen` and `last_node`, which are parameters by
test (a) — fixed at init, never written by update. They live inside the state
because `final` needs them and the reference API gives `final` no other way
to receive them.

**`blake2b_state` is a state with two parameter fields embedded for API
convenience.** That is why `sizeof(blake2b_state)` is not the mathematical
state, and why uniblake's drop of `last_node` is a scope decision, not a
correctness one. A design that
chose the output length at `final` rather than at `init` would not carry
`outlen` at all.

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

ISA support is the wrong question to ask it. The same NEON code
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

Unrolling loses to **register pressure**, not code size. Sixteen working
words plus sixteen message words already exceed the 31 general-purpose
registers: the rolled loop keeps one round live and spills nothing, while the
unrolled body presents twelve rounds at once and spills. Same reason `-O3`
measures slower than `-O2` here. Because the rolled loop spills nothing,
layout changes aimed at the register allocator have nothing to recover --
which is why the column-major experiment measured flat.

The lesson for a replacement: gains come from doing fewer or wider operations,
not from unrolling or from alignment hints. That means SIMD across independent
messages, or threads, not a differently-shaped scalar loop.

### Secret material after finalization

A keyed state's chaining value and pending block are derived from the caller's
key, so `ub_final` clears them, along with the digest staging buffer, which
holds all 64 bytes even when the digest is shorter. Only those fields: the
counter, flags and lengths stay, because the flags are what reject a second
`ub_final` on the same state -- zeroing the whole struct would silently
re-enable it.

Unkeyed states are not wiped. There is no secret to protect, and the check is
one predictable branch.

Build with `-DUB_WIPE=0` to compile the whole thing out -- no branch,
no flag in the state -- which is the right setting for a consumer hashing only
public data, where the clearing buys nothing. Digests are identical either
way; the setting changes what is left in memory after `ub_final`, not what is
computed. It does change the layout of the opaque state, so the library and
its callers must agree on it. `make check-wipe-modes` runs the oracle suites
both ways.

Clearing goes through `ub_wipe`, which calls `memset` through a `volatile`
function pointer so the store cannot be eliminated as dead. That is the
portable form: no `memset_s`, no OS-specific call, no inline assembly, so it
compiles under toolchains that reject GNU asm syntax.

Scope: this clears the named buffers. Copies the compiler placed in registers
or spilled to the stack, and pages written to swap, are outside its reach.

Verified zeroed at `-O2`, `-O3` and `-Os` on two compilers.

### Reading a performance number

A figure is meaningless without its machine. Code changes reorder between CPU
generations, so a number transferred from one host to another is unsupported.
State the exact CPU, compiler, flags and input shape with every measurement,
and re-run `make bench` on the target rather than citing a recorded figure.

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

C99. `ub_state_align()` uses `_Alignof` where C11 is available and the
`offsetof` probe otherwise; both report the same value, which is why the
alignment is queried at runtime rather than compiled in. No POSIX, no
allocation, no threading runtime, no floating point -- concurrency attaches at
`ub_hash_n` rather than inside the library.

Endian-neutral by construction: all serialization is explicit byte-shifting,
so the library runs unchanged on big-endian targets. Note what that claim
rests on -- it is a property of the code, not a test result, because the
conformance suites have only been run on little-endian hardware. A
cross-compile to a big-endian target is the cheap way to raise that from
argument to evidence.

Freestanding builds need `memcpy`, `memset`, and — unless the default error
handler is replaced — `fprintf`/`stderr`.

### Sizes

`ub_state_size()` and `ub_state_align()` are reported at runtime and are not
part of the ABI. Do not embed a literal.

`make check-sanitize` runs the oracle suites under AddressSanitizer and
UndefinedBehaviorSanitizer together, at `-O1 -g`. Not part of `make check`:
it is a separate build and roughly twice as slow, so it is run deliberately
rather than on every change.

**Run it on Linux, not only on macOS.** LeakSanitizer is Linux-only; on
macOS/arm64 ASan reports memory errors but silently skips leaks, so a clean
run there is weaker evidence than it looks. The target prints which case
applies -- `leaks: checked` or `leaks: NOT CHECKED on this platform`. Leaks in
the test harness were found exactly this way, by an Ubuntu run, after the same
target had passed repeatedly on macOS.

`make check-portable` compiles the library under both C99 and C11 with
`-Wall -Wextra -Wpedantic` and requires zero warnings. Point `CC2` at a
second compiler to widen it: `make check-portable CC2=gcc-16`. A warning is
a failure there, because the standards claim above is only worth what the
strictest available compiler says about it.

### The platform matrix

Four machines are available: this Apple Silicon Mac, Ubuntu 18.04, Ubuntu
24.04, and an HP Windows 11 laptop. That is enough to cover every axis that
has historically broken a BLAKE2 implementation except one, and the gap is
worth naming rather than papering over.

What each unit is *for* -- one distinct risk apiece, so none is redundant:

| unit | covers | why it is not substitutable |
|---|---|---|
| Apple Silicon Mac | arm64, clang, 64-bit LP64 | the only arm64; also the only Apple toolchain |
| Ubuntu 24.04 | x86-64, current GCC, current glibc | the only current mainstream Linux |
| Ubuntu 18.04 | old GCC, old glibc, C99-era defaults | the only old toolchain; catches reliance on newer compiler behaviour |
| Windows 11 (HP) | MSVC, Windows LLP64, MinGW | the only non-POSIX target and the only LLP64 data model |

**LLP64 is the one that earns Windows a slot.** On Windows `long` stays 32
bits while pointers are 64, so any assumption that `long` holds a pointer or a
`size_t` breaks there and nowhere else in this set. The narrowed `buflen` and
`outlen` fields and the `size_t` casts around them are exactly the code that
would show it.

**Ubuntu 18.04 is not nostalgia.** Its GCC defaults to an older standard and
ships a glibc without newer built-ins, which is what catches an accidental
dependency on a modern compiler being generous. The `_Alignof` fallback exists
for precisely this case.

The realistic order on each unit, cheapest first. README carries the
per-platform invocations, including the `EXE` suffix native Windows needs;
this is what to run and what each step buys.

1. `make check-portable CC2=<other compiler>`. Compiles only, so it is fast,
   and it catches the largest class of problem: a construct one toolchain
   accepts and another rejects.
2. `make check`, needing libsodium on that unit. The real evidence -- byte
   agreement against an independent implementation, on that machine.
3. `make check-wipe-modes`, wherever `ub_final` or the state layout changed.
4. `make check-sanitize` on the two Ubuntu units, where ASan and UBSan are
   best supported.
5. `make bench`, recorded with the machine, compiler, and flags. Never compare
   a figure across units.

On Windows run (1) and (2) twice, once under MSVC and once under MinGW.

Cross-compiling from Linux to Windows covers step (1): it establishes the
source is portable to the target. It does not substitute for (2). Only running
the suites on the target shows the digests are right there, and a
cross-compiled binary needs Wine or the actual machine to run.

**The gap this matrix cannot close: big-endian.** All four units are
little-endian, so the endian-neutrality claim above stays an argument about
the code rather than a test result. Closing it needs a cross-compiler
(`gcc-powerpc-linux-gnu` on either Ubuntu unit) and, to actually execute
rather than merely compile, QEMU. Compile-only costs one package and one
`make`, and establishes less than execution; report it as such.

### Allocation in the harnesses

The library never allocates. The test and bench harnesses do, and they need
alignment, which is not portable: C11 `aligned_alloc` is absent from MinGW's
UCRT, which supplies `_aligned_malloc` and requires `_aligned_free` to release
it. `tests/ub_alloc.h` selects among `_aligned_malloc`, `aligned_alloc`, and
`posix_memalign`, pairing each with its correct deallocator.

Harness-only: nothing in `include/` or `src/` includes it. A consumer allocates
however it likes, sized by `ub_state_size()` and aligned to `ub_state_align()`.

### Bringing up a new platform

1. `make check` — core, prefix and adapter conformance. All must pass before
   any optimization work.
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

Apple M4 Pro (arm64, 14 cores), Apple clang, -O2. Median of 7 reps x 400k
digests. Reproduce with `make bench`; the oracle version is printed by the run
and matters (see below).

Figures from one machine and one compiler. Run the benchmarks on the target
rather than carrying these forward.

## Per-digest

Prefix 140 B, digest 50 B: the prefix crosses one block boundary, leaving 12
bytes pending and room for a 4-byte tail in the final block.

| build | streaming | batch (`ub_hash_n`) |
|---|--:|--:|
| libsodium 1.0.21 | 285.0 ns | — |
| libsodium 1.0.22 | 185.9 ns | — |
| uniblake scalar | 92.9 ns | 96.4 ns |
| uniblake NEON | 138.6 ns | 141.4 ns |
| uniblake, 4 threads | 93.4 ns | 26.2 ns |
| uniblake, 8 threads | 93.2 ns | 13.4 ns |

The oracle's own version moves the comparison: 1.0.22 is a third faster than
1.0.21 on this machine, so the same uniblake build reads as 2.0x rather than
3.1x against it. Record which libsodium a figure was taken against, and
compare only against the version the consumer actually links.

The gain over libsodium is in `ub_update`, which compresses a full pending
block as soon as more input follows: a state absorbed over a 140-byte prefix
already holds one compressed block, so each digest costs one more. libsodium
buffers 256 bytes and compresses only on overflow, so after the same prefix
its counter is zero and every digest re-absorbs the prefix — 2.7 compressions
per digest against 1.

## Per workload

Nanoseconds per digest are hard to weigh. The same numbers as total hashing
time for a batch, at two sizes:

| build | 16.8M digests | 1.05M digests |
|---|--:|--:|
| libsodium | 4.8 s | 0.30 s |
| uniblake scalar | 1.6 s | 0.10 s |
| uniblake NEON | 2.4 s | 0.15 s |
| uniblake, 4 threads | 0.44 s | 0.027 s |
| uniblake, 8 threads | 0.22 s | 0.014 s |

These are the hashing phase only, and they are a **lower bound**: a real
caller also copies the state per digest, writes each result into its own
layout, and often extracts more than one field per digest. Measured inside one
such caller, libsodium costs 337 ns per call against the 285 ns here -- 18%
more -- so expect the same margin on top of every row.

Amdahl's law bounds the result.
Against a whole operation of 8.3 s, of which 5.7 s was hashing, replacing the
hash alone predicts:

| build | total | speedup |
|---|--:|--:|
| libsodium (baseline) | 8.3 s | 1.00x |
| uniblake scalar | 4.3 s | 1.95x |
| uniblake, 4 threads | 3.1 s | 2.70x |
| uniblake, 8 threads | 2.9 s | 2.90x |

Threading past four threads gains little because the hashing phase is no
longer the limit: at 8 threads it is 0.22 s of a 2.9 s total. Everything
beyond that is in the code that consumes the digests.

## Conformance

| suite | oracle | covers |
|---|---|---|
| `tests/test_core.c` | libsodium | streaming, parameters, keys, the RFC vector |
| `tests/test_prefix.c` | libsodium | prefix geometry, counters, slices, batching |
| `tests/test_api.c` | none | return codes, call ordering, the error handler |
| `compat/test_compat.c` | libsodium | the libsodium adapter |
| `compat/test_blake2_alias.c` | vendored reference | the `blake2.h` alias shim |
| `tests/test_negative.c` | libsodium | that the checks above can fail |

Coverage is exhaustive over the ranges that matter rather than sampled: every
message length from empty to past two blocks, every digest length the
parameter block admits, every key length, many update chunkings, and the whole
prefix geometry range including the boundary cases where no tail fits.

`tests/test_negative.c` links a compression function with one round removed and
requires every oracle comparison — the RFC vector, unkeyed, keyed,
personalized, and the prefix path — to reject it. A suite that cannot fail
proves nothing, and a mistake that made a comparison vacuous would otherwise
look like success. Run it with `make check-negative`.

The NEON and threaded backends pass `tests/test_core.c` and
`tests/test_prefix.c` unchanged; `make check-backends` runs them.

Oracle is libsodium — an independent implementation. `include/` and `src/`
link nothing.

`compat/test_blake2_alias.c` is the exception: its oracle is the BLAKE2
author reference itself, vendored and renamed under `compat/ref`, so it runs
without libsodium. An alias shim has to match the implementation it is
aliasing, which makes that the right oracle for it.
