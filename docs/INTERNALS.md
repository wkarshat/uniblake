# uniblake internals

For implementers: porting to a new platform, writing a compression backend,
and the definitions the interface rests on.

Callers want GUIDE.md instead.

---

## Contents

- [How this kernel behaves](#how-this-kernel-behaves)
- [State](#state)
- [The compression kernel](#the-compression-kernel)
- [Porting and profiling](#porting-and-profiling)
- [Measurements and conformance](#measurements-and-conformance)
  - [Per-digest](#per-digest)
  - [Per workload](#per-workload)
  - [Conformance](#conformance)
  - [C and Rust: one design, two implementations](#c-and-rust-one-design-two-implementations)
  - [Running this work on x86-64](#running-this-work-on-x86-64)

# How this kernel behaves

Things that turned out to be true about BLAKE2b on this code, with the
command that shows each one. Not a log of what was tried -- the point is the
mechanism, so the next person reasons from it instead of re-measuring.

**Figures are not here.** Every measured number lives in `TODO.md`, which is
pruned each pass. These sections carry mechanisms, which do not rot.

**Reproduce:** `make kernel-stats` (what the compiled kernel does),
`make bench-phases` (where a digest's time goes), `make bench-compare`
(against libsodium, comparable to the Rust harness), `make ab` (A/B two
binaries with a resolution verdict -- required for any performance claim).

**Before tuning anything**, read the adoption document: it records what the
the observed consumers actually call, which decides what is a hot path and what is a
conformance obligation. Keyed hashing is the latter -- no observed call site uses
it. **On x86-64**, see *Running this work on x86-64* below: several results are
aarch64 codegen results and are expected to differ.

### AArch64 folds the last round's rotate away for free

BLAKE2b's output is `h[i] ^= v[i] ^ v[i+8]`, which reads **all sixteen** `v`
words -- no G call in round 12 is dead. What *is* removable is the closing
`b = ror(b^c, 63)` of the final round's **diagonal** half: those values flow
straight into the XOR, so the rotate can ride along with it.

clang already does this. `make kernel-stats` reports 96 each of
`ror #16/#24/#32` but only **92** of `ror #63`, and 4 fused rotates -- the
missing four appear as `eor x9, x9, x17, ror #63`, AArch64's free
shifted-operand form. Writing the deferral explicitly in C produces
**byte-identical assembly**.

This is a property of the *instruction set*, not of BLAKE2b. x86-64 has no
shifted-operand `xor`, so there the four rotates are real `rorq`
instructions and the transformation is unexplored. In a vector kernel it
matters more again: `ror 63` has no single-instruction form
(`vpaddq`+`vpsrlq`+`vpor`), so deferring the final round's is removing the
most expensive rotate in the kernel across every lane. See
`the parked-optimisations notes` §2b.

### Unrolling wins here, and the mechanism is the sigma lookup

Not register pressure. `ub_sigma` is `extern`, so a rolled loop cannot fold
`ub_sigma[r][2*i]` even once `r` is known: every message word costs an
address computation and a byte load. Hand-unrolling substitutes literals at
all 96 call sites and the loads disappear -- `sigma ldrb` goes 9 to **0**,
which `make kernel-stats` prints directly.

Spilling *falls* when you unroll, because the freed address registers are
worth more than the extra live values cost. That is the opposite of the usual
intuition and is why `#pragma clang loop unroll(full)` measures *slower*: it
replicates the body but leaves the lookup, paying the code growth without
collecting the saving.

### Spilling is real but small, and easy to overcount

Every round consumes all sixteen message words, so 16 `m` + 16 `v` = 32 live
64-bit values against 31 general-purpose registers. Some spilling is forced.

Measured, it is minor: **well under one spill operation per rotate**. The number is
easy to inflate three ways, all of which `tools/kernel_stats.py` separates:

- **Callee-saved registers.** Most of the stack operations are x19-x28/x29/x30
  save and restore -- paid once per call, not per round. Counting them with
  the spills roughly doubles the apparent rate.
- **Stores are not pairs.** The stores are distinct slots, each
  written once: the allocator evicts a value and then re-reads it, which is
  where the loads come from. A store:load ratio is not a spill count.
- **Inbound arguments.** Several slots are read but never written -- message words
  arriving in the caller's frame, not spills at all.

Because the rate is already this low, allocator-aimed rewrites have little to
recover: variants using named locals instead of `v[16]`, or re-loading message
words from the block instead of holding `m[16]`, all land within noise; see `TODO.md`.

### Passing state as separate arrays costs more than mutating it

`ub_final` mutates `S->t`, `S->f` and `S->buf` in place. Hoisting `h`, `t` and
`f` into locals and passing them to the kernel separately -- the shape
`uniblake-rs` uses, where `State::finalize` takes `&self` -- measures
**~1.7 ns slower**, and `make kernel-stats` shows why: spill traffic rises
measurably. Three independent pointers can alias where one
`struct ub_state *` cannot, so the compiler reloads more conservatively.

The Rust form is right for Rust, where `&self` is also what makes a prefix
state shareable without a copy. It does not follow that it is right for C, and
here it is not.

### The finalization flag does not have to live in the state

C carries `f[2]`, 16 bytes of a 232-byte state. `uniblake-rs` carries no flag:
`compress` takes `last: bool`. The Rust shape ports to C cleanly, and was
built and measured rather than argued about:

- `f[1]` is the tree-hashing last-node flag. This library hashes
  sequentially, so it is always zero and the kernel can hardcode it.
- `f[0]` is doing two jobs: the finalization mask the kernel XORs, and the
  marker that makes a second `ub_final` fail. Split them -- pass the mask to
  the kernel, keep a one-byte `fin` for the guard. That byte is free: it
  shares the slot `buflen`/`outlen`/`keyed` already occupy.

The flag is now a kernel argument and a one-byte `fin` carries the guard, so
the C and Rust states have the same 216-byte composition. It measured a small
improvement at the leaf and on the state copy; the figures are in `TODO.md`.

Worth knowing for two reasons. It is the correct layout -- the flag is a
property of one compression, not of the state between absorbs, which is the
definition this document opens with. And it sets the scale: **shrinking the
state is not where leaf time is**. Anyone reaching for the state size as an
optimisation should read that 0.09 first.

Not adopted here only because it changes a documented 232-byte public ABI;
`ub_state_size()` exists precisely so callers do not embed the literal, so
this is a decision about compatibility, not about speed.

### One struct pointer beats four array pointers

Two Rust design differences were ported to C. The state-size one above is a
small win. The other -- `State::finalize` taking `&self` and working on local
copies of `h`/`t`/`f`, with a kernel that takes them as separate arguments --
measured slower (see `TODO.md`).

`make kernel-stats` locates it: spill traffic rises.

The cause is register occupancy, not aliasing. `restrict` on all four
pointers was tried and changed **nothing** -- still 17/48 -- which rules the
aliasing explanation out. What the assembly shows is argument lifetime: the
split kernel keeps four incoming pointer registers live to the end of the
function, where `compress_block(struct ub_state *S, const uint8_t *block)`
needs one base register and reaches every field at a fixed offset from it.
Three registers permanently occupied is three fewer for the 32 live 64-bit
values the algorithm already cannot fit, so the allocator evicts two more and
re-reads them six more times.

**The general form:** on a register-starved kernel, passing a group of values
behind one struct pointer is not just tidier -- each extra pointer argument
costs a register for the whole function. Rust pays nothing here because
`&self` is a single pointer too; it is the *split into separate arrays* that
costs, not the immutability.

### A compression timed alone is not a component of a digest

`make bench-phases` reports an incremental build-up -- copy, copy+update, full
leaf -- so each stage is a difference between measured totals. It prints
`ub_compress` in a tight loop separately and **deliberately refuses to
subtract it**: the isolated loop gets better branch prediction and cache
behaviour, and measures *higher* (74.0) than the increment `ub_final` actually
adds (73.1). Summing the parts overshoots the whole.

So: finalization dominates the leaf, and the compression dominates
finalization, but no fixed percentage is attributable to the kernel. Any claim
of the form "N% is compression" is an artifact of measuring it in isolation.

### Saying what a measurement can support

"No change", "within noise" and "unchanged" are not measurements. Each claims
that two things are equal, which is a much stronger statement than the data
usually carries -- and it is the statement that stops further investigation,
so it is the one most worth getting right.

The failure is concrete. Removing `f[2]` was reported as "79.4 both,
differences within noise" from six single runs. At 24 alternating pairs the
same change resolves cleanly, with an interval that excludes zero (the
figures are in `TODO.md`). The
effect was always there; the harness could not see it, and the write-up
converted "could not see" into "is not there".

Use `tools/ab_compare.py`. It alternates A and B rather than batching them
(thermal and frequency drift over a session is monotonic and would otherwise
land entirely on whichever ran second), discards the first pair, and reports a
bootstrap interval on the **paired** difference. Pairing is what makes small
effects visible: the `f[2]` result resolves a 0.43 ns difference against a
3.87 ns within-variant spread, because drift affects both members of a pair
equally and cancels.

Three rules follow:

- If the interval spans zero, write **"unresolved at N pairs"**, with N. That
  is a statement about the harness and invites someone to raise N. "No change"
  is a claim about the code and closes the question.
- Quote the interval, not a bare median. `-0.43 ns [-0.86, -0.05]` and
  `-0.43 ns [-2.10, +1.20]` are the same median and opposite conclusions.
- Never state one number for two variants. Reporting "79.4 both" asserts an
  exact equality that no measurement produces; at minimum one of the two
  figures is being rounded into agreement with the other.

Design decisions inherit the weakest measurement behind them. An ABI change
justified by "no change" is justified by nothing.

### Benchmarks lie in three specific ways here

Each of these produced a wrong published number before it was caught:

- **Position in the run.** The first timed block after an idle machine runs
  ~2.5x slow and decays over milliseconds. It is positional, not per
  implementation: reordering the harness moves the penalty to whichever block
  now runs first. Both harnesses spin 300 ms before timing.
- **Timer against work.** A single 1 KiB hash is below clock resolution and
  reported an identical, implausibly round 1024 MB/s for two different
  implementations. Small inputs are repeated until the timed region is >= 20 ms.
- **Counting assembly with regexes.** Rotate immediates print as `#63` from the
  compiler and `#0x3f` from `objdump`; `ror` also appears inside register
  names. `tools/kernel_stats.py --self-test` asserts each of these, and runs as
  part of `make kernel-stats`.

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
| `blake2b_state` | **state** | h, t, f, buf, buflen — all mutated by update. `outlen` and `last_node` are parameters carried inside it, which is the impurity named below. |
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

`make probe` prints the identity to report it with: vendor, brand, and the
generation coordinates that distinguish cores of the same ISA -- x86
family/model/stepping, ARM MIDR implementer/part. It also lists ISA flags,
which say an instruction exists, not that a kernel using it is faster.

Measure the shape the caller actually uses. A bulk-throughput loop and a
short-message loop can rank implementations differently, and optimising
against the wrong one is how a kernel gets faster on a benchmark and slower in
use. `bench/bench_prefix.c` reports the repeated-prefix shape for that reason.

### Checking a replacement

`make check` is the test. Three kinds of evidence are admissible, in
decreasing strength:

0. **Published known-answer vectors.** `tests/test_kat.c` checks the BLAKE2
   authors' own vectors for input lengths 0-255, unkeyed and keyed, one-shot
   and streamed. Weaker than (1) as coverage -- a fixed list, one digest
   length -- but independent of any implementation, and the only conformance
   check that runs where libsodium is unavailable. `tests/gen_kat.py`
   regenerates the header from a reference-package clone.

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

Measured, the size is the same everywhere tested -- 232 bytes, 8-byte aligned,
on Linux x86-64, Windows x64 and Windows 32-bit -- because every field of
`struct ub_state` is a fixed-width type and none scales with pointer width.
That equality is a property of the current field types, not a guarantee of the
contract. Adding a `size_t`, a pointer, or a `long` would make the size differ
per target, as it did before commit `76b232f`, when `buflen` and `outlen` were
`size_t` and the state was 240 bytes on LP64 against 232 on ILP32. That build
was correct -- 240 worked -- but the same source no longer gave the same size
on every target, which quietly breaks any state shared across a cross-compile
boundary, a serialization, or a prebuilt binary paired with differently built
callers. No test catches it; `make check-portable` compiles, it does not
compare sizes. The maintainer note lives at the struct definition in
`src/internal.h`.

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

### Driving a build matrix

Every target takes `CC`, `AR`, `BUILD` and `EXE`, so a matrix is a loop over
those rather than a separate build system. `BUILD` is what keeps the runs from
colliding: each configuration gets its own output tree and none invalidates
another.

```sh
for cc in cc gcc-16 x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc; do
  for std in c99 c11; do
    make BUILD=build-$cc-$std CC=$cc CFLAGS="-O2 -std=$std -Wall -Wextra -Wpedantic"
  done
done
```

Cross targets additionally need `AR=<triple>-ar` and `EXE=.exe`. A build that
only compiles establishes portability of the source, not correctness of the
digests; for that the suites have to run on the target, natively or under an
emulator.

`make check-portable` automates the compile half for two compilers and both
standards, and treats a warning as a failure. It does not link or run, so it
is fast enough to precede every other check.

Recorded coverage on the development machine, all warning-free under
`-Wall -Wextra -Wpedantic` at both C99 and C11:

| toolchain | target | suites |
|---|---|---|
| Apple clang 21 | arm64 Mach-O | all, natively |
| Homebrew GCC 16 | arm64 Mach-O | all, natively |
| MinGW GCC 16 x86_64 | amd64 COFF | compile and link only |
| MinGW GCC 16 i686 | i386 COFF | compile and link only |

The i686 row is the one that earns its place: a 32-bit `size_t` is the only
check available here on the narrowing of `buflen` and `outlen` to `uint8_t`.
The two Windows rows link `tests/test_api.c` and `compat/test_blake2_alias.c`,
which need no oracle; the remaining suites need a libsodium built for the same
target.

### Running another architecture's kernel

Correctness of a SIMD kernel can be established without the target hardware;
speed cannot.

On Apple silicon, `clang -arch x86_64` plus Rosetta 2 runs an x86-64 binary
directly. Rosetta does not advertise AVX2 in CPUID -- a `cpuid` leaf-7 probe
reports `AVX2=0` -- but it executes AVX2 instructions correctly, so a kernel
built with `-mavx2` runs and can be checked against the suites that need no
oracle:

```sh
clang -arch x86_64 -mavx2 -O2 -std=c11 -Iinclude -Isrc -Itests \
  tests/test_kat.c src/core.c src/const.c src/prefix.c \
  backends/compress_avx2.c -o /tmp/kat_avx2 && /tmp/kat_avx2
```

The oracle suites additionally need a libsodium built for the same
architecture; a Homebrew arm64 build will not link.

Wine is the equivalent for a MinGW cross-build, and on Apple silicon it is
worth nothing here: a PE binary needs Wine *and* x86-64 translation, and the
`-arch x86_64` route above already gives the second without the first. Wine
earns its place on an x86-64 Linux host, where it runs the Windows binaries
that `make check-avx2 CC=x86_64-w64-mingw32-gcc EXE=.exe` produces.

**Cost.** Translation is cheap enough that this is a routine check, not a
last resort. On an M4 Pro the vector suite runs in 0.43 s the first time a
binary is seen -- Rosetta translating and caching it -- and in hundredths of a
second on every later run. Per-digest throughput is roughly 14x native
(~2,400 ns against ~174 ns), so even the 45,000-check prefix suite would be
seconds, not minutes, if an x86-64 oracle were available to link against.

**What it does not give.** Neither route measures. Translation rewrites the
instruction stream, so a timing from it says nothing about the target. And
the oracle suites need a libsodium built for the same architecture: a Homebrew
arm64 build will not link, which leaves the published vectors and the vendored
reference as the independent checks available under translation.

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

### Incremental builds

Objects carry compiler-generated header dependencies (`-MMD -MP`, emitted as
`.d` files beside them under `BUILD`). Editing `src/internal.h` or a public
header rebuilds every object that included it.

This matters more than usual here: `internal.h` defines `struct ub_state`, so
a stale object linked against a changed layout is a silent memory error, not a
compile failure.

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
and matters.

Figures from one machine and one compiler. Run the benchmarks on the target
rather than carrying these forward.

## Per-digest

Prefix 140 B, digest 50 B: the prefix crosses one block boundary, leaving 12
bytes pending and room for a 4-byte tail in the final block.

| build | streaming | batch (`ub_hash_n`) |
|---|--:|--:|
| libsodium 1.0.21 | 285.0 ns | — |
| libsodium 1.0.22 | 185.9 ns | — |
| uniblake scalar | 79.2 ns | 83.3 ns |
| uniblake NEON | 142.8 ns | 142.1 ns |
| uniblake, 4 threads | 79.9 ns | 23.9 ns |
| uniblake, 8 threads | 80.9 ns | 12.3 ns |

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
| `tests/test_kat.c` | none; published vectors | input lengths 0-255, unkeyed and keyed, one-shot and streamed |
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

## C and Rust: one design, two implementations

The goal is one design expressed twice, natively: **C consumers use the C
library, Rust consumers use the Rust crate.** No cross-language FFI, no
adapter in the middle. Identical sizes are the evidence that the composition
is identical, not an end in themselves.

### Status

| | uniblake C | uniblake-rs | agree? |
|---|--:|--:|:--:|
| state | **216 B**, align 8 | **216 B**, align 8 | **yes** |
| parameter type | `ub_param` 52 B | `Params` 34 B | **no** |
| digest output | caller's buffer | `Hash` 65 B | by design |

State parity was reached by removing `f[2]` from the C state (merged; measured
-0.43 ns [-0.86, -0.05] at the leaf) and by replacing the Rust `u128` counter
with `[u64; 2]`. Both states are now:

```
h[8]      64   chaining value
t[2]      16   counter
buf[128] 128   pending block
buflen     1
outlen     1
fin/…      1   finalized guard (+ wipe flag in C)
padding    5/6
         ---
         216
```

### The parameter block, and what a caller should see of it

The parameter block is 64 bytes and is XORed into the IV at init. Two
documents define it between them, and the split matters:

**RFC 7693 §2.5** specifies only two fields:

> "byte offset: 3 2 1 0 (otherwise zero) / p[0] = 0x0101kknn / p[1..7] = 0.
> Here the 'nn' byte specifies the hash size in bytes. The second
> (little-endian) byte of the parameter block, 'kk', specifies the key size in
> bytes. Set kk = 00 for unkeyed hashing. Bytes 2 and 3 are set as 01. All
> other bytes in the parameter block are set as zero."
> — RFC 7693, Saarinen and Aumasson, November 2015

and then says explicitly:

> "Note: [BLAKE2] defines additional variants of BLAKE2 with features such as
> salting, personalized hashes, and tree hashing. These OPTIONAL features use
> fields in the parameter block that are not defined in this document."

**So salt, personalization and the tree fields are not RFC features.** They
come from the BLAKE2 paper and are implemented by the reference code. Anything
this library says about them cites the reference implementation, not the RFC.
The full layout, as the reference implements it:

```
digest_length 1  key_length 1  fanout 1  depth 1
leaf_length   4  node_offset 8  node_depth 1  inner_length 1
reserved     14                       <- specified, must be zero
salt         16  personal 16
                                       = 64
```

The 14 reserved bytes are required to be zero, and are not slack: an
implementation that skipped them would XOR the salt and personalization into
the wrong words and produce different digests. Both libraries zero a 64-byte
buffer and write fields into it by offset, so the reserved region is correct
by construction. (This library splits the RFC's 8-byte `node_offset` into a
4-byte offset and a 4-byte `xof_length`, the BLAKE2X convention; the bytes
land in the same places.)

**How the reference models it, and why that settles the interface question.**
libsodium keeps its `blake2b_param` struct **entirely private** — it does not
appear in any public header. Its public API is two initializers, `_init` and
`_init_salt_personal`, taking the fields a caller actually sets. The twelve
fields are an implementation detail of the format, not an API.

The reference implementation's own answer is therefore: **do not put the
parameter block in the interface.** This library currently does, through
`ub_param`/`ub_init_param`, while the Rust crate exposes four fields via a
builder. That asymmetry is not defensible as "the languages differ" — it is
one library following the reference's shape and the other not.

`ub_param` is 52 bytes: its fields sum to 50, and the three `uint32_t` members
give the struct 4-byte alignment, which rounds 50 up to 52. That has nothing
to do with the 64-byte block, which is built separately.

**Why `ub_param` is nonetheless the right thing to keep**, on evidence rather
than on how often it is called:

- **The compatibility shim needs it, and needs it as a sequence.** libsodium's
  `crypto_generichash_blake2b_init_salt_personal` takes key, salt,
  personalization and digest length in one call. Reproducing that on this
  library is three steps, because the key is not a parameter-block field in
  the sense the others are:

  1. `ub_param_init`, then write `salt` and `personal` into the block, and set
     `key_length` -- the length goes in the block, the key bytes do not;
  2. `ub_init_param`, which XORs the completed block into the IV;
  3. build a zero-padded 128-byte block holding the key and `ub_update` it,
     because a key is absorbed as the first message block (RFC 7693 §3.3),
     not carried in the parameters.

  No two-argument constructor can express that, and getting it wrong is
  silent: setting `key_length` without absorbing the key produces a
  well-formed digest that is simply not the right one. The shim's comment
  says exactly this. That is the argument for keeping `ub_init_param` --
  it is the only entry point that lets a caller stop between steps 1 and 3.
- It is the only way to reproduce a digest made by another implementation with
  a non-default tree parameter. Removing it would make some third-party
  digests unreachable, which is a capability loss, not a tidy-up.

**What is not evidence for it:** none of its uses in this repository sets any
tree field. Every one of them sets only what a four-field builder would cover.
Frequency of use argues nothing about whether an interface is right.

Both surfaces are therefore kept as they stand: C exposes the twelve the
reference defines, Rust the four its builder sets. The asymmetry is real but
neither side is wrong, and closing it would break a public interface to buy
symmetry that nothing currently needs. `TODO.md` records the decision and what
would justify reopening it.

**The guarantee to test.** Equal `sizeof` between the two libraries' parameter
types proves nothing — the structs are never reinterpreted as bytes, since
both serialize field by field. What matters is that the two serializers emit
byte-identical 64-byte blocks from equivalent inputs. That test is the real
unification check and is on the backlog.

### Errors: a library does not panic

**C** returns `int`: `UB_OK`, or a negative code from a documented enum
(`UB_E_ARG`, `UB_E_OUTCAP`, `UB_E_GEOMETRY`, `UB_E_STATE`). Every entry point
validates before doing work, so a rejected call has no side effects. An
installable handler adds diagnostics without changing the return value.

**Rust, as it stands, panics** in the builder: a digest length outside
1..=64, a salt of the wrong length, and a key length mismatch are `assert!`.
That was chosen because the values are constants at a call site, so the assert
fires in development rather than in production.

**That reasoning is not good enough for a library.** A library detects,
mitigates where it can, reports, and lets the caller decide. A panic removes
the decision: it unwinds through code that did not choose it, and in a
consumer built with `panic = "abort"` it takes the process down. "The caller
should have passed a valid length" is an argument about whose fault it is, not
about who should handle it.

The ecosystem agrees on the primary surface. RustCrypto's `blake2`, the
standard consumers reach for, returns values from its trait API:

```rust
fn new(output_size: usize) -> Result<Self, InvalidOutputSize>
fn new_from_slice(key: &[u8]) -> Result<Self, InvalidLength>
```

It keeps `assert!` only in the inherent `new_with_params`, a lower-level
constructor. `blake2b_simd` asserts throughout its builder, which is the
convention this crate copied.

**Direction: move the builder to fallible constructors** — a `Params` error
enum with the same four conditions the C side already names, and `to_state`
returning `Result`. Panics remain acceptable only where a condition cannot be
produced by any input, such as an internal invariant. Tracked in `TODO.md`;
the same enum is what a batch entry point would return, so the two should be
designed together.

The contract both sides keep meanwhile: **a failed call changes nothing.** C
validates up front; the Rust asserts fire in the builder, before a state
exists.

### Scope: what this library is and is not

**In scope:** BLAKE2b, sequential mode, one digest at a time or many over a
shared prefix.

**Not in scope: BLAKE2bp and BLAKE2sp.** They are different algorithms --
tree modes producing different digests from the same input -- not faster
implementations of BLAKE2b. Nothing here will grow into them.

This matters because one optimisation direction sounds like it crosses the
line and does not. **Multi-message SIMD** -- hashing several independent
BLAKE2b messages in the lanes of one vector register -- is a *kernel
technique*, and every message still gets a plain BLAKE2b digest. It is in
scope, and is the natural implementation of a batch entry point. BLAKE2bp
uses the same lane trick but then combines the lanes into one digest, which
changes the output. That is the distinction: same technique, different
algorithm.

### Why alignment is a Rust worry and not a C one

The Rust state was 224 B before the counter change, because `u128` has
target-dependent alignment: 8 on x86-64 before rustc 1.77, 16 from 1.77 on,
and 16 on aarch64 throughout. Same source, different size per target and per
compiler version.

C has no equivalent exposure **in this state** because every field is a
fixed-width integer or an array of them: `uint64_t`, `uint8_t[]`. There is no
C type in `struct ub_state` whose alignment varies by target. C would have
exactly the same problem if the state held a `size_t`, a pointer, a `long`, or
a `__int128` -- and it did: `buflen`/`outlen` were `size_t` at the initial
commit, giving 240 on LP64 against 232 on ILP32. Narrowing them to `uint8_t`
fixed it for the same reason dropping `u128` fixed Rust.

So it is not that C is immune. It is that the C state was already audited for
ABI-dependent field types and the Rust one had not been. The C rule is stated
under *State* above; `tests/abi.rs` now carries the Rust one.

## Running this work on x86-64

Everything measured here is aarch64 on one machine. Several
results are *architecture* results, not algorithm results, and are expected to
come out differently here. This file is the whole procedure, so it does not
have to be reconstructed.

### First: much of this runs on an Apple-silicon Mac already

Before provisioning an x86-64 box, note that the AVX2 kernel **builds and runs
on Apple silicon**: Apple clang cross-compiles with
`--target=x86_64-apple-macos13`, and Rosetta 2 executes AVX2 despite not
advertising it in CPUID. `make check-avx2-rosetta` runs the published vectors
and the API suite that way, verified.

What still needs a real x86-64 host:

- **the oracle suites** -- they link libsodium, and a Homebrew libsodium is
  arm64, so it cannot go into an x86-64 binary;
- **every timing** -- Rosetta is emulation; no speed number from it means
  anything;
- **the scalar x86-64 codegen questions** below, which are about what a native
  compiler emits.

### Setup

```sh
sudo apt install build-essential libsodium-dev python3
git clone <this repo> && cd uniblake
make check SODIUM=/usr          # must pass before any measurement
```

For the Rust comparison:

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
cd ../uniblake-rs && cargo test
```

### What to run

```sh
make check SODIUM=/usr          # correctness first, always
make kernel-stats               # what the compiled kernel does
make kernel-stats-arch          # how to read those counts on x86 (read this)
make bench-phases               # where a digest's time goes
make bench-compare SODIUM=/usr  # against libsodium
make check-avx2 SODIUM=/usr     # AVX2 correctness -- never run here
make bench-avx2  SODIUM=/usr    # AVX2 speed -- never run anywhere
```

Then in `uniblake-rs`:

```sh
cargo run --release --example compare
python3 ../uniblake/tools/ab_compare.py \
    ../uniblake/build/ub_cmp target/release/examples/compare \
    --runs 21 --grep '^400000,'
```

Never compare a single A run against a single B run: see
*Reading a performance number*.

### What is expected to differ, and why

| result | aarch64 | x86-64 expectation |
|---|---|---|
| rotate count in `compress` | 380 + 4 fused | **384, 0 fused** — x86 has no shifted-operand `xor`, so the last-round rotate cannot ride along with the output XOR |
| last-round rotate deferral | free, compiler already does it | **untested and plausibly a real win** — the four rotates are real `rorq` there |
| AVX2 `ROT63` | n/a | 3 instructions (`vpsrlq`+`vpaddq`+`vpor`) against 1 for the others; deferring the last round's removes the most expensive rotate in the kernel |
| callee-saved registers | 10 (x19-x28) | 6 (rbx, rbp, r12-r15) — the spill subtraction is smaller |
| instruction counts | comparable | **not comparable** — x86 folds loads into arithmetic |

### The three open experiments

1. **Last-round rotate deferral, x86-64 scalar.** Apply the deferral to the
   final round's *diagonal* half only (the column half's `b` words are
   overwritten by the diagonal step) and fold the rotate into the output XOR.
   Verify with `make check`, measure with `tools/ab_compare.py`. On aarch64
   this produced byte-identical assembly; here it should not.

2. **Last-round `ROT63` deferral, AVX2.** Same idea in
   `backends/compress_avx2.c`. This is the most promising of the three:
   `ROT63` is 3 instructions against 1 for the other rotations, and the saving
   applies across four lanes on the final round's critical path.

3. **Confirm or refute the whole aarch64 table.** Every row of
   *How this kernel behaves* is one machine and one compiler. gcc and clang
   both, `-O2` and `-O3`.

### Prompt to use

Paste this verbatim:

> Read this document's *Running this work on x86-64* and *How this kernel
> behaves* sections before touching anything.
>
> This is x86-64 Linux. Every performance result currently in the docs was
> measured on aarch64 (Apple M4 Pro, clang) and several are architecture
> results that should come out differently here — the doc says which and why.
>
> Do, in order:
> 1. `make check SODIUM=/usr` and confirm it passes. Do not measure anything
>    until it does.
> 2. `make kernel-stats` and `make kernel-stats-arch`. The counting tool
>    currently only parses aarch64; extend it for x86-64 per the notes it
>    prints, and add the corresponding cases to its `--self-test`. Report the
>    rotate count and how many rotates are fused into a consumer — the
>    aarch64 answer is 380 + 4 fused, and x86 is expected to be 384 + 0.
> 3. `make bench-phases` and `make bench-compare SODIUM=/usr`, then the Rust
>    comparison and `tools/ab_compare.py` as shown in this file.
> 4. Run the three open experiments listed above.
>
> Rules, which exist because they were violated on aarch64:
> - Use `tools/ab_compare.py` for any A/B claim. Never compare single runs,
>   never batch all-A then all-B. If its verdict is UNRESOLVED, write
>   "unresolved at N pairs", never "no change" or "within noise".
> - Quote the confidence interval with every difference, not a bare median.
> - Do not report a code-shape conclusion without the assembly counts that
>   explain it, and do not assert a mechanism you have not tested — an
>   aliasing explanation was proposed on aarch64 and disproved by adding
>   `restrict`.
> - Anything you measure goes into this document as a mechanism, with
>   the `make` target that reproduces it. One-off scripts in `/tmp` do not
>   count as results.
