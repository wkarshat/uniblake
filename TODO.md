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

`ub_init_personal` and `ub_set_wipe` are in. Nothing else in this line is
blocked. What remains is presentational: the guide and README still show
`ub_param` assembly as the ordinary path for setting a personalization tag,
which is now the long way round.

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
**2263 instructions at `-O1` against 1550 at `-O2`**, a 1.46x ratio matching
the 282/193 = 1.46x timing.

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

### API additions, not yet built

Collected from the requirements review; none is implemented. Kept here rather
than in a permanent document because unshipped design is state, not
documentation.

- **Rust batch entry point over a shared prefix.** The consumer's solver loop
  is already this shape: clone a prefix state, append a little-endian counter,
  finalize, read the digest in fixed-width slices. The reference crate's batch
  API cannot express it, because its job type starts from the initial state
  and takes a whole input, so a shared prefix is re-absorbed per digest.
  Design questions recorded and unresolved: how the tail width is spelled, how
  output is passed, and what it returns.
- **Rust fallible constructors.** The builder must not panic on caller input.
  Follow the shape RustCrypto uses: validate in the public constructor, return
  `Result`, keep the assert as an internal invariant behind it. Needs a
  `Params` error enum naming the same conditions the C side names, which is
  also what any future fallible entry point returns — so design it once.

### Documentation refactor

The set has grown to 3,600 lines across six files, two of which are oversized
because material was appended to them rather than placed. Concrete state, so
the work is not re-derived:

| file | lines | problem |
|---|--:|---|
| `docs/INTERNALS.md` | 1192 | absorbed the C/Rust comparison, the x86 procedure and the parameter analysis; now spans kernel internals, cross-language design and a porting runbook |
| `docs/INTEGRATING.md` | 122 | **done 2026-09-01**: the 906-line API proposal moved here, leaving the migration guide |
| `docs/GUIDE.md` | 632 | absorbed the wiping material |
| `README.md` | 414 | carries scope, build, layout, the document contract and the naming rule |
| `docs/UniBlake.md` | 215 | the design argument; the least changed |
| `TODO.md` | 126 | this file |

Overlaps found by inspection: "state size" is discussed in five of the six;
`ub_state_size` appears in four; scope boundaries are stated in both `README`
and `UniBlake.md`.

**Target shape.** Each file gets one audience, one question, and a place in a
stated reading order — defined in `README`, which is the only file allowed to
point at the others:

1. `README` — what this is, how to build it, where things are. Audience:
   someone who just arrived. Should shrink: the naming rule and the document
   contract are policy, not orientation.
2. `UniBlake.md` — why it exists and what it will not do. Audience: someone
   deciding whether to adopt it. Absorbs the scope statements now duplicated
   in `README`.
3. `GUIDE.md` — how to call it. Audience: someone writing calling code.
   Wiping belongs here; internals do not.
4. `INTERNALS.md` — how it works and how to port it. Audience: someone
   replacing a kernel. **Split candidate:** the x86 runbook and the
   cross-language design are separate questions from the kernel, and are what
   pushed this past a thousand lines.
5. `INTEGRATING.md` — how to migrate an existing consumer. Audience: someone
   swapping out another BLAKE2b. Done: the unbuilt-API design moved to this
   file, which is where unshipped work lives.
6. `TODO.md` — state, and every figure.

**Rules to apply while doing it:** no restating what an adjacent paragraph
already said; no aphorisms; no "as we saw above"; cross-file pointers only
from `README`; a heading earns its place by answering a question a reader
actually has.

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
| libsodium reference, leaf | **282 ns at 1.0.21**, 185 ns at 1.0.22 | the standard oracle is 1.0.21; the versions differ by 1.5x on this shape, so any ratio must name which |
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
