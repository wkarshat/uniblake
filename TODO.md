# TODO

Project state. The most transient document here: every number, count and
sample size lives in this file so the permanent documents do not carry
figures that rot. Prune each pass — finished, dropped and superseded items
come out, they do not accumulate.

## Active

**Reconcile `ub_param` with the Rust `Params`.** C exposes all twelve RFC 7693 parameter-block
fields through `ub_param`/`ub_init_param`; Rust exposes four
(`hash_length`, `key_length`, `salt`, `personal`). The reference
implementation settles the shape: libsodium keeps its `blake2b_param`
**entirely private** and its public API is `_init`, `_init_salt_personal` —
callers never see the twelve. Neither of ours should expose twelve either.
Blocked on: `ub_param` has 33 uses in tests and is the documented way to set
personalization, so narrowing is a public break. Next step is
`ub_init_personal` (additive), after which `ub_init_param` becomes the escape
hatch rather than the path.

**Oracle dependency floor.** `cargo test` needs a newer toolchain than the
library does, because `blake2b_simd 1.0.5` requires 1.85 through two
independent causes: edition 2024 in its own manifest, and
`constant_time_eq 0.4.2`, which declares `rust-version = 1.85.0`.

Proved that neither is the *code*: reverting the manifest to edition 2021 and
the dependency to 0.3, with no source change, builds `blake2b_simd 1.0.5`
cleanly on rustc 1.82 and 1.69. The 1.85 floor is packaging, not language
features.

Upstream position, from the tracker: issue #30 is open but the reporter
proposed closing it in 2024, and on the pull request that caused it the
maintainer wrote that there is no concrete MSRV policy, "but if we did it
would probably be something close to 'last three stable versions'". A pull
request adding `rust-version` would run against that stance, so **do not
file one**. Pull request #23, which proposed replacing `constant_time_eq`
with `subtle`, was also closed unmerged — the dependency is deliberately
aligned with the successor project.

**Resolution here: pin the oracle.** `blake2b_simd = "=1.0.2"` as a
dev-dependency. Verified: all 17 tests pass on **rustc 1.75**, which is what
the current Ubuntu LTS ships, and on current stable. The library itself builds
on 1.75 either way — only the test path was constrained.

**Slice versus pointer+length for a batch output.** Open, needs review.
Current position is that the two languages should differ, with the caveat that
bounds-check hoisting in a strided write loop must be *verified* in generated
code rather than assumed. No decision.

**Multi-message SIMD: disposition under consideration.** Not adopted, not
rejected. In scope as a technique (each message still gets a plain BLAKE2b
digest, unlike BLAKE2bp). The question is whether a batch entry point should
use it, which interacts with whether that entry point exists at all.

## Backlog

### Documentation refactor

The set has grown to 3,600 lines across six files, two of which are oversized
because material was appended to them rather than placed. Concrete state, so
the work is not re-derived:

| file | lines | problem |
|---|--:|---|
| `docs/INTERNALS.md` | 1192 | absorbed the C/Rust comparison, the x86 procedure and the parameter analysis; now spans kernel internals, cross-language design and a porting runbook |
| `docs/INTEGRATING.md` | 1028 | absorbed the consumer survey and the API proposal; now spans migration steps, requirements evidence and unbuilt-API design |
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
   swapping out another BLAKE2b. **The unbuilt-API design does not belong
   here** and should move to `TODO.md`, which is where unshipped work lives.
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
aarch64, unless stated. Reproduce with the `make` targets above; A/B figures
use `tools/ab_compare.py`, which alternates the two binaries, discards the
first pair, and reports a bootstrap interval on the paired difference.

| what | figure | conditions |
|---|---|---|
| leaf digest, C | 79.4 ns | 140 B prefix, 4 B tail, 50 B digest, median of 9 × 400k |
| leaf digest, Rust | 74.8 ns | same shape |
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
