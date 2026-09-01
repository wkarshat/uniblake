# What consumers actually need

Read from the Zebra and librustzcash trees, not assumed. This is the
requirements check that should precede any implementation tuning: it decides
which paths are hot, which are obligations, and where the C and Rust
interfaces have to agree.

## Every Zcash BLAKE2b call site

Four in Zebra and librustzcash use exactly one shape:

```rust
blake2b_simd::Params::new()
    .hash_length(32)
    .personal(b"ZcashAuthDatHash")
    .to_state()
    .update(h1).update(h2)
    .finalize()
```

librustzcash's Equihash additionally clones a prefix-absorbed state per leaf.
Note which way that FFI points: `components/equihash/src/blake2b.rs` exports
`blake2b_init`/`blake2b_clone`/`blake2b_free` as `extern "C"` so that
**tromp's C solver** (`components/equihash/tromp/equi_miner.c`) can call
Rust's BLAKE2b through function pointers -- `equi_miner.c:529` clones the
state per leaf inside the solver's hot loop. No Rust code calls into C here.

The shim exists only because the solver is C. A consumer using this library's
Rust crate directly has no reason to reproduce it, and the FFI-shaped
constraints below (opaque heap-allocated state, clone-and-free per leaf) are
therefore **not** requirements on the Rust API -- they are properties of an
arrangement that is expected to go away.

| feature | used by Zcash | notes |
|---|---|---|
| `hash_length` | **yes**, every site | always 32 |
| `personal` | **yes**, every site | 16-byte domain tag |
| `update`, `finalize` | **yes** | |
| `State: Clone` | **yes**, Equihash | the prefix workload this library targets |
| `salt` | no | BLAKE2b completeness only |
| **`key`** | **no** | searched both trees: zero keyed call sites |

## What this means for the keyed path

Keyed hashing is a **conformance obligation, not a hot path**. Nothing in the
Zcash lineage uses it, so it must be correct and must never cost the unkeyed
path anything.

The C library already resolves this the right way and measured it: wiping runs
only for a keyed state, so the unkeyed path pays 1.7 ns for the branch and the
keyed path pays 17.7 ns for the wiping it needs (`src/core.c`). The `keyed`
flag itself is free -- it shares an 8-byte slot with `buflen` and `outlen`.

**Consequence for tuning:** optimisation effort belongs on
clone -> update -> finalize with `personal` set and no key. A change that
helps the keyed path at the unkeyed path's expense is a regression against
every real consumer, even if the benchmark average improves.

## Wiping: what the setting actually controls

Four implementations, four behaviours:

| | wipes final state | wipes key from stack |
|---|---|---|
| libsodium | always | always |
| uniblake C | `UB_WIPE=1` (default) **and** state was keyed | **always**, regardless of `UB_WIPE` |
| uniblake-rs | never | n/a -- no key material is staged |
| `blake2b_simd` | never | n/a |

Two corrections to what `include/uniblake/uniblake.h` currently says:

- **The unkeyed cost is far smaller than documented.** The header cites 1.7 ns
  for the branch on the unkeyed path. Re-measured with `tools/ab_compare.py`
  at 60 alternating pairs, `UB_WIPE=1` against `UB_WIPE=0` is
  **+0.00 ns, 95% CI [-0.14, +0.10]** -- unresolved, but now *bounded*: the
  cost is under 0.14 ns, not 1.7. The older figure came from a whole
  init/update/final cycle where the branch could not be hoisted.
- **It does not change the state layout on LP64.** The header warns that a
  library and caller built with different settings must not be mixed because
  the layout differs. Measured, `ub_state_size()` is **232 bytes either way**:
  the `keyed` byte lands in padding that exists regardless (payload 226 vs
  227, both rounding to 232). The warning is sound as a rule for future
  fields, but as stated it describes a hazard that is not currently present.

Also worth stating plainly, because the flag's name suggests otherwise:
`ub_init_key` burns the caller's key from its stack buffer **unconditionally**
(`src/core.c:111`). `UB_WIPE=0` removes the final-state wipe, never the key
handling.

### Recommendation

**Keep the compile-time default at 1, and do not add a runtime argument.**

- The unkeyed path -- every Zcash call site -- now measurably pays under
  0.14 ns for it, so the performance argument for opting out is close to gone.
- A runtime flag would put a branch *and* a state field on the hot path to
  serve a case no consumer has. It also creates a footgun the compile-time
  form does not: a caller could construct a keyed state with wiping off and
  believe otherwise.
- `UB_WIPE=0` stays worth keeping for a consumer that hashes only public data
  and wants the branch and the flag gone entirely. That is a build-level
  decision about a whole binary, which is exactly what a compile-time switch
  expresses.

For **uniblake-rs**, not wiping matches `blake2b_simd`, which every current
consumer already relies on, so no change is needed. If wiping is ever wanted
there it should be `impl Drop` behind a non-default feature -- not a
constructor argument -- so the unkeyed path keeps its exact current codegen.

## Where the two interfaces diverge, and what to unify

| | C | Rust |
|---|---|---|
| construction | `ub_init(S, outlen)` / `ub_init_key(...)` / `ub_init_param(S, &P)` | `Params::new().hash_length(n).personal(p).to_state()` |
| personalization | via `ub_param` only | first-class builder method |
| copy | `ub_copy(dst, src)` | `Clone` |
| finalize | `ub_final(S, out, outcap)`, mutates `S` | `finalize(&self)`, non-mutating |
| state size | 232 B (216 B without `f[2]`) | 216 B |
| errors | `int` return codes | `Result` / panic split |

Three of these are real interoperability problems rather than style:

1. **Personalization is second-class in C.** The one parameter every Zcash
   call site sets requires building a `ub_param` and calling
   `ub_init_param`, while `outlen` and `key` -- one of which is never used --
   get dedicated constructors. A `ub_init_personal(S, outlen, personal)`
   would match how the library is actually called.

2. **`ub_final` mutates.** Raised because Equihash's FFI clones a state per
   leaf; **withdrawn** -- see the conclusion. That shim is tromp's C solver
   calling Rust, and it is expected to go away, taking the motivating case
   with it. The implementation also measured +2.12 ns [+1.76, +2.25].

3. **State size should agree.** Both are 216 bytes once `f[2]` leaves the C
   layout -- worth doing for the shared FFI story, and independently measured
   at **-0.43 ns [-0.86, -0.05]** at the leaf.

## Conclusion: what to build

Ranked, with the reasoning each rests on.

**1. Rust `hash_n` (batch over a shared prefix).** Planned but unbuilt -- it
is the fifth and last row of the phase table in
`uniblake-rs/docs/DESIGN.md` §6, whose gating phase (benchmarks against
the C library and `blake2b_simd`) has passed. "P5" is that row number, not an
API name. This is the one change with a
consumer waiting for it: if Equihash moves off the C solver and calls this
crate directly, the per-leaf `clone` -> `update` -> `finalize` loop is exactly
what a batch entry point replaces, and the C library's threaded `ub_hash_n`
already shows the shape and the ~1.95x it buys on two cores. Everything else
below is smaller.

**2. C `ub_init_personal(S, outlen, personal)`.** Personalization is the one
parameter every Zcash call site sets, and it is the only one that requires
building a `ub_param`; meanwhile `key`, which no consumer uses, has a
dedicated constructor. This is a five-line addition that makes the common case
the easy one. Purely additive to the ABI.

**3. Drop `f[2]` from the C state.** Measured at **-0.43 ns [-0.86, -0.05]**
at the leaf and -0.13 ns [-0.14, -0.10] on the copy, and it makes both
libraries 216 bytes. It is also the correct layout: the finalization flag is a
property of one compression, not of the state between absorbs. Held back only
because it changes a documented state size -- so it belongs in a deliberate
ABI revision, not slipped in.

**Explicitly not doing:**

- **A non-mutating `ub_final`.** Proposed earlier on the strength of Rust's
  `finalize(&self)`. The motivating case was the FFI shim's clone-per-leaf,
  which is going away; and the obvious implementation -- hoisting `h`/`t`/`f`
  into separate kernel arguments -- measured **+2.12 ns [+1.76, +2.25]**
  because four pointer arguments occupy four registers in a kernel that
  already spills. If it is ever wanted, it must keep the single-struct-pointer
  kernel signature.
- **A uniform runtime wipe default.** A per-state runtime control *is*
  proposed (`docs/API_PROPOSAL.md` §3), but defaulting it off for every state
  would silently withdraw the wiping a keyed caller gets today. The
  recommendation there is to default it off for unkeyed states and on for
  keyed ones, so the control is an override rather than a behaviour change.
- **Anything to serve the FFI shim's shape.** Opaque heap state, clone-and-free
  per leaf, and an `extern "C"` surface are properties of tromp's C solver, not
  requirements on either library.

**The API question this settles:** the two interfaces do not need unifying so
much as completing. They already agree on the operations that matter --
construct with a digest length and a personalization, absorb, copy, finalize.
Each is missing one entry point the other has, and neither gap is structural.

Concrete signatures for all three: `docs/API_PROPOSAL.md`.

## Proposed unified shapeConcrete signatures for all three: `docs/API_PROPOSAL.md`.

## Proposed unified shape

Same operations, same order, spelled naturally in each language. Nothing here
requires the C ABI to change except the additions marked NEW.

| operation | C | Rust |
|---|---|---|
| build params | `ub_param_init(&P, outlen)`; `P.personal = ...` | `Params::new().hash_length(n).personal(p)` |
| **construct, common case** | **NEW** `ub_init_personal(S, outlen, personal)` | `.to_state()` |
| construct, keyed | `ub_init_key(S, outlen, key, keylen)` | `.key(k).to_state()` |
| absorb | `ub_update(S, buf, len)` | `.update(&buf)` |
| copy | `ub_copy(dst, src)` | `.clone()` |
| finalize | `ub_final(S, out, outcap)` (mutates; a second call is rejected) | `.finalize()` (non-mutating) |
| batch over a prefix | `ub_hash_n(...)` | **NEW** `hash_n` (DESIGN.md P5, unbuilt) |

The gap in each direction is one entry point, not a redesign: C lacks a
personalized constructor; Rust lacks the batch call. Neither is implemented here -- this file is the requirements review, and
the API changes it implies should be a separate, reviewed change.
