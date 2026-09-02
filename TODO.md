# TODO

Project state. The most transient document here: every number, count and
sample size lives in this file so the permanent documents do not carry
figures that rot. Prune each pass — finished, dropped and superseded items
come out, they do not accumulate.

## Lines of development

Grouped by what they touch, with dependencies stated. A line is *decided* when
its shape is settled and only the work remains; *open* when a question must be
answered first.

### 1. C API completion — decided, no dependencies

`ub_init_personal` and `ub_set_wipe` are in. What remains is presentational:
the guide and README still show `ub_param` assembly as the ordinary path for
setting a personalization tag, which is now the long way round.

Also added since: `check-build` compiles a suite without running it, and
`check-wine` runs a cross-built Windows binary. The pair exists because
`check-alias` builds and runs, so a cross build reported a failure that was
only the host refusing to execute a foreign binary. Windows conformance is now
verifiable from Linux without a Windows machine.

### 2. Rust API correctness — decided, blocks line 3

Fallible constructors. The builder must not panic on caller input; follow the
shape the ecosystem standard uses -- validate in the public constructor,
return `Result`, keep the assert as an internal invariant behind it. Needs a
`Params` error enum naming the conditions the C side already names.

**Blocks line 3**, because a batch entry point's return type should be that
same enum rather than a second one invented alongside it.

### 3. Batch entry point over a shared prefix — open

The consumer's solver loop is exactly this shape: clone a prefix state, append
a little-endian counter, finalize, read the digest in fixed-width slices. No
reference implementation can express it -- their batch APIs take whole inputs
and re-absorb the prefix per digest.

Open questions, none answered: how the tail width is spelled, how output is
passed, what it returns. Depends on line 2 for the error type and on line 5
for whether a vector implementation sits underneath.

### 4. Cross-language parity — decided, independent

State sizes already match at 216 bytes. What remains is a test asserting the C
and Rust parameter-block serializers emit byte-identical 64 bytes from
equivalent inputs. That test is the parity guarantee; equal `sizeof` is a weak
proxy for it.

Parameter *surfaces* stay as they are -- see the decision below.

### 5. Vector kernels — partly open, informs line 3

- **NEON, single message: closed.** Measured to its ceiling. A scalar rotate
  is 4.6x cheaper than the NEON rot63 that replaces it, and 2-wide SIMD over
  one message does not shorten the dependency chain, so the vector path pays
  more latency per step to remove parallelism a wide core already extracts.
  `make bench-isa` reproduces the per-instruction figures. No further
  single-message NEON work.
- **NEON, two independent messages: open and untried.** The only formulation
  where lane width becomes real parallelism. Expected below 2x because the
  rot63 penalty is per lane-group, but worth measuring.
- **AVX2 last-round rotate deferral: open.** `ROT63` is three instructions
  against one for the other rotations, and the final round's is what the
  output XOR can absorb. Needs a real x86-64 host to measure.
- **x86-64 scalar last-round deferral: open.** Free on aarch64 because the
  compiler fuses it into the output XOR; x86 has no such encoding, so the four
  rotates are real instructions there.

### 7. Consumer integration — largely done, blocked on transfer

The proof-of-work code of a consuming node now hashes through this library:
per-digest state copy removed, optimised solver ported, batch path present
behind a default-off flag, depends package building from a local checkout.
Full node build and C++ suite pass.

Blocked on: the branch exists on one machine only. Nothing else in this line
moves until it is transferred.

### 6. Cross-platform measurement — open, no dependencies

Every figure is aarch64 on one machine. Needs a Linux x86-64 run of the same
harnesses, and a Linux aarch64 run to separate "this instruction set" from
"this core". The harnesses are portable C and Rust and need no porting; only
the assembly counter needs x86 patterns.

**The oracle's build flags matter more than its version.** Two libsodium
builds on this machine measure 282 ns and 193 ns on the leaf shape. That is
**not** a version difference: nothing between 1.0.21 and 1.0.22 touches
BLAKE2b (1.0.22 adds ML-KEM768, X-Wing and SHA-3). It is the optimisation
level. The consuming project's dependency system builds everything at `-O1`;
a package manager builds at `-O2`.

Proved by compiling libsodium's own `blake2b-compress-ref.c` both ways:
**2263 instructions at `-O1` against 1550 at `-O2`**, matching the timing
ratio. Three builds of 1.0.21 from identical source, leaf shape:

| flags | ns/digest |
|---|--:|
| `-pipe -O1` (the consuming project's dependency system) | 288 |
| `-pipe -O2` | 178 |
| libsodium's own configure default (`-O3` + hardening) | 175 |

Note the default is **`-O3`**, not `-O2`: libsodium picks its own optimisation
and hardening flags unless `CFLAGS` is passed explicitly. `-O2` to `-O3` buys
almost nothing here; the whole effect is escaping `-O1`.

Consequences for any comparison figure:

- State the oracle's **build flags**, not only its version. "libsodium 1.0.21"
  is ambiguous by 46%.
- A figure against a distribution package (Ubuntu 24.04 ships 1.0.18, 18.04
  ships 1.0.16, both `-O2`) is not comparable to one against the consuming
  project's `-O1` build, and neither is wrong -- they answer different
  questions.
- The honest headline is a `-O2` comparison, since that is how a library is
  normally built. The `-O1` figure answers a narrower question: what the
  consuming node actually experiences today.

README's Ubuntu section carries the source-build recipe with the pinned hash.

## Decisions recorded

**Parameter surfaces: keep as they are, revisit later.** C keeps `ub_param`
with all twelve fields the BLAKE2 reference defines; the Rust `Params` keeps
its four.

- The twelve match the authors' own reference header exactly in name, type and
  order -- only `reserved[14]` is absent, because the reference memcpy's a
  packed struct as the 64-byte block while this library serializes field by
  field. The C surface is the reference's, not idiosyncratic.
- All twelve are reachable through exactly one function, `ub_init_param`. The
  convenience constructors are narrower: `ub_init` sets one field,
  `ub_init_key` two, `ub_init_personal` two. Nothing here sets a tree field.
- Two things depend on the twelve staying reachable: reproducing a third-party
  digest made with a non-default tree parameter, and the libsodium shim's
  salt+personal+key sequence, which must stop between building the block and
  absorbing the key.
- Narrowing either side is a public break, and equalizing them is not required
  for the state-size parity already achieved.

Revisit when: a consumer wants a tree parameter from Rust; the shim stops
needing the staged sequence; or a public-API revision opens for another reason.

**Consumer compatibility: confirmed by compilation, not by inspection.** An
observed call site compiles against this crate with one line changed -- the
crate name -- and produces a byte-identical digest to the reference. Verified
on rustc 1.75 by a test crate that transcribes the call site verbatim and
asserts equality against the reference crate.

One incompatibility exists and is the only one: builder methods take `self`
here and `&mut self` in the reference, so the non-chained form
(`let mut p = Params::new(); p.hash_length(32);`) does not compile -- confirmed
by compiling it and reading the error. Every observed call site chains, so none
is affected. **No adapter is needed**, and a compatibility facade would freeze
the reference's convention into this API permanently to save that one rewrite.

**Toolchain floor: rustc 1.75**, the version the current Ubuntu LTS ships and
the environment tests run in. The oracle is pinned to `blake2b_simd = "=1.0.2"`
because 1.0.3+ moved to edition 2024 and a dependency declaring 1.85; neither
is a code requirement -- 1.0.5's source builds on 1.69 with its manifest
reverted -- so the pin costs nothing. Revisit if a consumer requirement forces
it; two observed consumers already declare 1.85 and 1.88.

**Reference oracle: libsodium 1.0.21**, the version the consuming project
builds from source with a pinned tarball and hash. Not a distribution package,
and not a dependency of this library -- see the README.

## Backlog

### Warm up bench_prefix

`bench/bench_prefix.c` has no warmup spin, unlike `bench_phases.c` and
`bench_compare.c`. Its first timed rep pays the process startup transient,
measured at about 2.5x on an idle machine. A median over 10 reps absorbs most
of it, but the harness is not directly comparable with the other two, and it
is the one `make bench` runs.

Fix: add the same 300 ms spin the other two use, before the first clock start.
Cheap and mechanical. The reason to pause is that changing it shifts every
figure this harness has ever produced, so it wants doing in one step with a
before-and-after pair recorded, not slipped in.

### Consumer transition to the C library

A consuming node's proof-of-work code used libsodium's BLAKE2b directly. Its
inner loop is this library's target shape: a prefix state absorbed once, then
one digest per consecutive counter.

Done, on a branch in the consumer's tree:

- A wrapper giving this library's opaque state the value semantics the
  consumer's code already assumed -- stack declaration, copy-assignment.
- The per-digest helper now calls `ub_hash_tail` against the shared prefix,
  so the state is not copied per leaf. Measured 1.15x on the per-digest cost
  at the consumer's real geometry, digests byte-identical.
- The optimised solver ported the same way. Its counters advance by thread
  count, so they are not consecutive and the batch entry point does not apply
  there.
- A batch path behind a build flag, default off. Operational and
  byte-identical over half a million digests, and measurably slower: the
  vector kernels are single-message, so a batch call runs one per digest
  exactly as a loop does. It is the call shape a multi-message kernel would
  fill, not a win today.
- A depends package building uniblake from a local checkout.

Full node build succeeds, links this library, and passes its C++ suite
including the genesis validation vectors. One pre-existing failure in the
consumer's wallet tests, verified present on the parent commit and untouched
by this work.

Open:

- The branch is local to one machine. It needs pushing or transfer before the
  Linux side can build it.
- x86-64 measurement. The AVX2 kernel has never been timed on real hardware;
  everything so far is aarch64 or emulated.
- Two BLAKE2b call sites remain on libsodium: a serialization writer used on
  the sync path, and key derivation. The writer is the one that matters --
  sync is where the consumer's own profiling puts the cost.

### depends integration: what went wrong twice

Recorded because both failures were silent and the error message named the
wrong step. The mechanism and the debugging procedure are in the recipe's own
header comment, which is where someone hitting this will be looking.

1. The extract step copied into the extract directory while already inside it,
   nesting a second copy. The build found no Makefile and produced nothing.

2. depends names the compiler `$(HOST)-gcc`. That exists for a cross build and
   not for a native one -- on Ubuntu `x86_64-pc-linux-gnu-gcc` is absent and
   `gcc` is what there is. Packages with autotools never notice, because
   configure probes and falls back. uniblake has no configure step, so it ran
   a missing compiler, produced nothing, and depends stamped the step as
   built.

Both surfaced as `cp: cannot stat 'build/libuniblake.a'` at the staging step,
which is three steps downstream of the actual fault.

The general lesson, now enforced: depends stamps are claims, not evidence. Its
step dependencies are order-only, so a stamp left by a failed run makes make
skip the step entirely on the next attempt. Any package without a configure
step must assert its own output -- `test -f build/libuniblake.a` at the end of
the build commands -- or a silent failure travels downstream and is diagnosed
in the wrong place.

### Documentation refactor

Current state, measured:

| file | lines | owns | audience |
|---|--:|---|---|
| `docs/INTERNALS.md` | 1235 | kernel, state, porting, x86 runbook, C/Rust design | someone replacing a kernel |
| `docs/GUIDE.md` | 632 | calling it, wiping | someone writing calling code |
| `TODO.md` | 547 | state, backlog, figures | checking status |
| `README.md` | 536 | what, build, layout, oracle, naming | someone who just arrived |
| `docs/UniBench.md` | 361 | the measurement format | recording or reading a benchmark |
| `docs/UniBlake.md` | 215 | why it exists, what it will not do | deciding to adopt |
| `docs/INTEGRATING.md` | 122 | migrating a consumer | swapping out another BLAKE2b |

Two files are oversized and one is now right. `INTEGRATING.md` came down from
1028 lines when the unbuilt-API design moved to `TODO.md`, which is the shape
the rest should follow: a document holds what is true, `TODO.md` holds what is
proposed.

Remaining moves, in order of how much they help:

1. **Split `INTERNALS.md`.** It answers three questions with different
   audiences: how the kernel works, how to port to another architecture, and
   how the C and Rust implementations relate. The x86 runbook and the
   cross-language comparison are what pushed it past a thousand lines and
   neither is needed by someone replacing a kernel. Candidate: keep the kernel
   and state material, move porting to its own file, move the C/Rust
   comparison to `UniBlake.md` where the design argument already lives.

2. **`README.md` should shrink.** It carries the document contract, the naming
   rule and the oracle policy, which are project policy rather than
   orientation. Someone arriving needs what this is, how to build it, and
   where things are.

3. **The libsodium oracle deserves its own section in `README.md`**, placed
   after the per-platform subsections. It is currently split between the top
   of *Build and test* and the middle of the Ubuntu section, and the platform
   table points at "the Ubuntu section" for guidance that is not
   Ubuntu-specific. The source-build recipe also does not say which directory
   each command runs in, which has already produced a `make bench` in the
   wrong tree.

4. **`GUIDE.md` at 632 lines** absorbed the wiping material. Check whether it
   still reads as one document or two.

Rules while doing it. Say the thing once, in as few words as carry it. No
restating what an adjacent paragraph said, no aphorisms, no sentence whose job
is to introduce the next one. Prefer a table to a paragraph and a sentence to
a table when the sentence is enough. Cross-file pointers only from `README.md`.
Figures live in `TODO.md` and nowhere else; a permanent document names the
`make` target that reproduces a number rather than quoting digits.

### Visual material

None of the documentation has a diagram, and several things it explains are
structural and would be clearer drawn than described. Proposals, roughly in
order of value:

- **State layout diagram** — the 216-byte state as a byte map (`h`, `t`,
  `buf`, the length bytes, padding), with the same drawing for both libraries
  side by side. This is the clearest possible statement that the two
  compositions match, and it replaces a paragraph.
- **Parameter block** — a 64-byte map showing every field at its
  offset, including the reserved region, annotated with which fields this
  library exposes and which it only serializes.
- **The prefix property** — a before/after showing N digests over a shared
  prefix as N full absorptions versus one absorption plus N tails. This is the
  library's entire reason for existing and is currently only prose.
- **Compression data flow** — the working vector as 4x4, with the column and
  diagonal G steps and the diagonalisation between them. Standard in BLAKE2
  literature and worth reproducing.
- **Benchmark charts** — leaf cost and bulk throughput across the four
  implementations; thread scaling. Generated from the harness output so they
  cannot drift from the numbers in this file.
- **State-size comparison** — a simple bar chart of 216 / 216 / 224 / 384 with
  the field composition stacked, which makes the libsodium difference
  self-explaining.
- **Decision flow for choosing an entry point** — one-shot versus streaming
  versus prefix versus batch, as a small flowchart in `GUIDE.md`.
- **Project mark** — a simple wordmark or glyph for the README header. Low
  priority, but the repository currently has no visual identity at all.

Constraint: anything generated from measurements must be produced by a
committed script from committed data, or it will rot exactly the way the
inline figures did.



- `ub_init_personal(S, outlen, personal)` — additive; personalization is the
  one parameter every observed call site sets and the only one requiring a
  `ub_param`.
- `ub_set_wipe(S, on)` — runtime per-state control, default off for unkeyed
  and on for keyed, so existing keyed behaviour is preserved and the call is
  an override.
- Rust batch entry point over a shared prefix — planned as phase P5.
- Cross-library test asserting the C and Rust serializers emit byte-identical
  64-byte parameter blocks. This is the real unification guarantee; equal
  `sizeof` is a weak proxy.
- **Evaluate RustCrypto `blake2` for comparison and possible compliance.**
  Already tracked in the source lists and actively maintained. It is the
  ecosystem's standard interface (`Digest`, `KeyInit`, `VariableOutputCore`),
  so the questions are: does its trait surface fit this library's shape, and
  is a `RustCrypto` adapter behind a feature flag worth providing? Its error
  convention has already been adopted as the model for fallible constructors.
- Review upstream closed issues and rejected pull requests for the other
  vendored sources, as was done for the Rust reference.
- Last-round rotate deferral on x86-64 and in the AVX2 kernel. Free on
  aarch64 (the compiler already fuses it); untested elsewhere, and `ROT63` is
  three instructions in the vector kernel against one for the others.
- **Rust fallible constructors.** Decided: the builder must not panic on
  caller input. Follow RustCrypto's shape -- validate in the public
  constructor, return `Result`, keep the assert as an internal invariant
  behind it. Needs a `Params` error enum naming the same conditions the C side
  names (`UB_E_ARG`, `UB_E_OUTCAP`, `UB_E_GEOMETRY`, `UB_E_STATE`), which is
  also what any future fallible entry point returns, so design it once.

## In test

- AVX2 kernel under Rosetta 2 (`make check-avx2-rosetta`): passes the
  published vectors and the API suite. Correctness only — emulation says
  nothing about speed, and the oracle suites cannot run because a Homebrew
  libsodium is arm64.

## Done (recent only)

- `ub_init_personal(S, outlen, personal)` — digest length plus a 16-byte tag,
  the shape every observed caller uses, without assembling a parameter block.
  Tested for byte equality with the long spelling it replaces, for equality
  with `ub_init` when the tag is NULL, and that a different tag changes the
  digest.
- `ub_set_wipe(S, on)` — runtime per-state control over final-state wiping,
  defaulting on for keyed states and off otherwise. Tested that it overrides
  in both directions, does not change the digest, is carried by `ub_copy`, and
  is rejected after finalization. Absent when built `-DUB_WIPE=0`. The
  internal `keyed` field is renamed `wipe`, for what it controls rather than
  for what usually sets it.

- C state 216 bytes, matching Rust. Dropped `f[2]`; the finalization mask is a
  kernel argument and a one-byte `fin` carries the guard.
- Rust state ABI-invariant at 216 bytes across targets and toolchains
  (`u128` counter replaced with `[u64; 2]`).
- Benchmark and analysis tooling: `make bench-phases`, `make bench-compare`,
  `make kernel-stats`, `make ab`, `tools/run_variant.sh`.

## Dropped

- Non-mutating `ub_final` over separate `h`/`t`/`f` arguments. Four pointer
  arguments occupy four registers in a kernel that already spills.
- Explicit last-round rotate deferral **on aarch64**: byte-identical assembly,
  the compiler already does it. Still open on x86-64 and AVX2.
- Rolled twelve-round loop with an inline `g()`.
- A `blake2b_simd`-shaped compatibility facade for Rust consumers: it would
  freeze that crate's `&mut Self` builder convention into this API permanently
  to save a handful of one-line edits.

## Figures

Measured numbers live here and nowhere else. The format, columns, metric
vocabulary and commands are specified in `docs/UniBench.md`; this section is
the data.

### x86-64 Linux, VM

First x86-64 run. Ubuntu VM, libsodium 1.0.21 from a `$HOME/opt` source build.
CPU model not captured; a VM, so absolute figures carry hypervisor overhead
and the drift within a run is larger than on bare metal.

| | ns/digest |
|---|--:|
| scalar leaf | 184 |
| AVX2 leaf | 151 |
| libsodium leaf | 246-270 across rows in one run |
| 2 threads, n=100k/400k | ~95-100 |
| 2 threads, n=10k | 199 — slower than 1 thread, thread setup dominates |

| bulk | MB/s |
|---|--:|
| uniblake scalar | 784-826 |
| libsodium | 1104-1158 |

Leaf phases: state copy 6.90, `update(4B)` +15.54, `ub_final` +164.01;
`ub_compress` alone 158.62.

Open from this run:

- **AVX2 gain is 1.22x**, against 1.6x in the older Skylake row. Our scalar
  path is also faster there (184 vs 219), so there may be less to recover --
  unconfirmed.
- **libsodium is 1.4x faster on bulk**, consistently. It ships x86 SIMD
  compress kernels and we run scalar there; `bench-compare-avx2` would say
  whether our vector build closes it. Not yet run.
- **State copy 6.90 ns and `update(4B)` 15.54 ns** are 2.4x and 5.4x the
  aarch64 figures, worse than the compression ratio. Suspect memory or VM
  rather than codegen. Needs `lscpu` and a bare-metal comparison.
- **CPU model and `bench-isa` not captured.** `bench-isa` is what decides the
  last-round rotate question on x86.

Use `make collect SODIUM=<prefix>` for the next run; it captures all of the
above plus the machine and oracle details in one file.

### aarch64, macOS

Every measured number, with the conditions. Apple M4 Pro, clang -O2,
aarch64, unless stated. Reference oracle is libsodium **1.0.21**, the version
the consuming project builds; figures taken before that was fixed used the
1.0.22 available from a package manager, which affects only the libsodium
comparison rows, not any uniblake figure. Reproduce with the `make` targets above; A/B figures
use `tools/ab_compare.py`, which alternates the two binaries, discards the
first pair, and reports a bootstrap interval on the paired difference.

| what | figure | conditions |
|---|---|---|
| leaf digest, C | 78.6 ns | 140 B prefix, 4 B tail, 50 B digest, median of 9 × 400k |
| libsodium reference, leaf | 1.0.21: **288 ns `-O1`**, 178 `-O2`, 175 configure-default `-O3` | the spread is optimisation level, not version; no BLAKE2b change between 1.0.21 and 1.0.22 |
| leaf digest, Rust | 74.4 ns | same shape |
| NEON instruction latency | `add.2d` 0.98, `tbl` 0.79, `shl+sri` (rot63) 1.25, `ext` 0.57, scalar `ror` 0.27 ns | `make bench-isa`; serial chains, so latency not throughput |
| C versus Rust | −4.95 ns, 95% CI [−5.20, −4.65] | 20 alternating pairs |
| dropping `f[2]`, leaf | −0.43 ns, CI [−0.86, −0.05] | 24 pairs |
| dropping `f[2]`, state copy | −0.13 ns, CI [−0.14, −0.10] | 24 pairs |
| separate `h`/`t`/`f` arguments | +2.12 ns, CI [+1.76, +2.25] | 24 pairs |
| `UB_WIPE` on the unkeyed path | +0.00 ns, CI [−0.14, +0.10], unresolved | 60 pairs; bounds the cost under 0.14 ns |
| two threads | 1.95× (C), 1.96× (Rust) | leaf shape, range split |
| bulk throughput | ~1720 MB/s (C), ~1750 (Rust) | 1 KiB to 16 MiB |
| NEON kernel | 136.0 ns, 1.71× slower than scalar | six formulations tried |
| state sizes | C 216, Rust 216, libsodium 384, `blake2b_simd` 224 | LP64 |
| params sizes | `ub_param` 52, Rust `Params` 34, `blake2b_simd` 184 | the last holds a 128 B key block |
| kernel instructions | C 1516, Rust 1546 | 380 rotates each, 4 fused, no sigma byte-loads |
| spill traffic | 0.150 (C), 0.153 (Rust) ops per rotate | callee-saved registers excluded |
| suites | kat 1536, alias 1218, prefix 45531, core 630, api 155, compat 56, negative 7 | |
| consumer survey | 37 `Params`/`State` sites, 0 batch-API sites | nine applications, five chains |
