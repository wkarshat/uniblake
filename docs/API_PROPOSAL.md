# Proposed API changes

Three additions, from the requirements review in `docs/CONSUMERS.md`. Nothing
here is implemented; this is the design to review before writing code.

## Terminology first: what "P5" is not

"P5" is a row number in the phase table of `uniblake-rs/docs/DESIGN.md` §6 --
the fifth of five planning phases (P1 compression + state, P2 `Params`
builder, P3 prefix layer, P4 benchmarks, P5 batch). It is a schedule label,
not an API name, and nothing in either library is or should be called `P5`.
P4's role was to decide whether P5 was worth building; P4's criteria passed,
so it is. (These were labelled M1-M5 until 2026-09-01; renamed to P for
"phase" because "M" read as "milestone" and collided with CPU names like
"Apple M4" in the same documents.)

`hash_n` is the name of the C entry point (`include/uniblake/prefix.h`) that
P5 mirrors. The `n` is the count of digests produced per call.

---

## 1. Rust `hash_n` -- batch over a shared prefix

### What the C entry point does

```c
int ub_hash_n(const ub_state *S, size_t tailwidth, uint64_t first, size_t n,
              size_t off, size_t len, void *out, size_t stride);
```

`n` digests over consecutive counters. Counter `i` is serialized little-endian
into `tailwidth` bytes (4 or 8), appended to the prefix state `S`, and digest
`i` is written to `out + i * stride`. `off`/`len` select a slice of each
digest so a caller reading fixed-width fields gets them packed with no second
pass. `S` is not modified, so it is shared read-only.

### Why this exact shape

It is what the consumer's loop already is. `equi_miner.c:528-534`:

```c
for (u32 block = id; block < NBLOCKS; block++) {
    state = eq->blake2b_clone(eq->blake_ctx);
    u32 leb = htole32(block);                 // consecutive counter, LE, 4 bytes
    eq->blake2b_update(state, &leb, sizeof(u32));
    eq->blake2b_finalize(state, hash, HASHOUT);
    eq->blake2b_free(state);
    for (u32 i = 0; i < HASHESPERBLAKE; i++) {
        const uchar *ph = hash + i * WN/8;    // digest consumed in fixed slices
        ...
    }
}
```

Consecutive counters, little-endian, 4-byte tail, fixed-size digest read in
slices -- `tailwidth`, `first`, `n`, and `off`/`len` respectively. The slicing
is structural, not incidental: `HASHOUT` is defined as
`HASHESPERBLAKE * WN/8` (`equi.h:33-34`), so one BLAKE2b digest is always
consumed as several fixed-width values. The loop is
also already strided by thread id, so a range split is the natural
parallel form.

### Not designing the signature yet

The shape above is what the consumer's loop needs. **The exact Rust signature
is deliberately not settled here** -- that is phase P5 work, and settling it
during a redesign of the C/Rust relationship would fix choices before the
things they depend on (parameter-block unification, whether multi-message SIMD
lands underneath) are decided.

What is settled: `hash_n` takes a **borrowed prefix state**, consecutive
counter tails, and writes strided output. Everything else -- how the tail
width is spelled, how output is passed, how errors are returned -- is open.

The four design questions that will have to be answered, with what is
actually at stake in each, are recorded below so the eventual decision is not
made from scratch.

#### Enum vs validated integer for the tail width

Either language can do either: C can have `typedef enum { UB_TAIL_4, UB_TAIL_8 }`
and Rust can take a `usize` and return `Err`. So "Rust has enums" is not a
reason.

The real trade is **where the check lives and what a wrong value costs**. A
validated integer moves the check to run time, so every call pays it (the C
`ub_hash_n` argument check is ~5 ns, about 5% of a single-digest call and
under 0.4% from n = 16) and a wrong value is a runtime error the caller must
handle. An enum moves it to compile time in both languages: no check, no error
path, and the mistake is unrepresentable.

The counter-argument is ABI and forward compatibility. Adding a third width to
a C enum is a source-compatible change but not always ABI-compatible, and a
`switch` over it in a caller may warn or fall through. An integer absorbs new
values without recompiling anything.

For a value with exactly two legal settings that are fixed by the format, the
enum is right in both languages. This is a case where the two should agree,
not diverge.

#### Slice vs pointer + length

Rust's `&mut [u8]` and C's `void *out, size_t stride` differ in three ways
that matter, and one that does not.

- **Interface stability.** The slice bundles the length with the pointer, so
  the pair cannot drift apart across a version change. In C the caller
  restates the length at every call site, and a later change to what the
  function expects (bytes vs elements, say) is silent at every one of them.
- **Error resistance.** "Output buffer smaller than `n * stride`" cannot be
  expressed with a slice -- the length is known, so the check is an assertion
  the compiler or a bounds check enforces rather than an error code the caller
  may ignore. In C it is `UB_E_ARG` and a caller who ignores return codes gets
  a buffer overrun.
- **Performance.** A slice is a two-word value where C passes two arguments,
  so at the ABI level they are the same. The cost is *bounds checking* inside
  the loop, which for a strided write of known width is normally hoisted --
  but it is not free by construction, and a `hash_n` writing millions of
  digests must be checked for it in the generated code, not assumed.

C cannot adopt the slice without inventing a struct type, which would be a
worse fit than the existing convention. This is a case where the languages
should differ, and the C side compensates with the up-front argument check it
already documents.

#### `&mut Self` vs `Self` builders

`blake2b_simd::Params` methods take `&mut self` and return `&mut Self`;
uniblake-rs takes `mut self` and returns `Self`.

Chained use is identical. The difference appears in two places:

```rust
let mut p = Params::new();
p.hash_length(32);          // works with &mut Self, not with Self
let s = p.to_state();

const P: Params = ...;      // possible with Self (const-friendly), not with &mut
```

`Self` is the more idiomatic modern Rust builder and composes with `const`
construction, which matters because `Params` at a Zcash call site is
effectively a constant. `&mut Self` allows incremental mutation of a stored
builder, which no observed call site does -- all four chain.

**But the divergence is a migration tax on every consumer**, and it is the
only source-level incompatibility between the two crates. Two options:

- keep `Self` and document the one rewrite, or
- provide both: `fn hash_length(mut self) -> Self` plus a
  `fn set_hash_length(&mut self) -> &mut Self`.

The second removes the tax entirely at the cost of two names per setter. This
is worth deciding deliberately rather than inheriting: it is the difference
between a drop-in replacement and a near-drop-in one.

#### Errors

Open. C returns `int` codes with a documented enum; Rust has no error type
yet (`prefix_check` returns `bool`). Whatever `hash_n` returns should be
decided together with whether the crate gains a general error enum, not
in isolation.

---

## 2. C `ub_init_personal`

```c
/* Init with a digest length and a personalization tag, the shape every
 * observed consumer uses. Equivalent to ub_param_init + setting P.personal +
 * ub_init_param, which is what a caller must write today.
 *
 * `personal` is UB_PERSONALBYTES (16) bytes, or NULL for none. */
int ub_init_personal(ub_state *S, size_t outlen,
                     const uint8_t personal[UB_PERSONALBYTES]);
```

Purely additive: no existing symbol, struct, or behaviour changes. It exists
because personalization is the one parameter every Zcash call site sets and
the only one that requires assembling a `ub_param`, while `key` -- used by no
observed consumer -- already has `ub_init_key`.

---

## 3. Wiping: compile-time default on, runtime opt-in default off

The requirement is both controls, with opposite defaults:

- **compile-time `UB_WIPE`, default 1** -- whether the capability is built in.
- **runtime, default off** -- whether a given state actually wipes.

Reconciling that with the measurements (`docs/CONSUMERS.md`): the unkeyed path
pays under 0.14 ns for the existing branch, and a keyed state pays ~17.7 ns
for the wipe it performs.

### Proposed shape

```c
/* Compile-time: unchanged, default 1. UB_WIPE=0 removes the capability, the
 * branch, and the state field entirely. Everything below is absent. */

#if UB_WIPE
/* Runtime, per state. Default OFF: a state wipes only if asked.
 *
 * Returns UB_E_STATE if called after ub_final, UB_OK otherwise. Set it any
 * time before finalization; ub_copy propagates it, so a prefix state
 * configured once is inherited by every clone. */
int ub_set_wipe(ub_state *S, int on);
#endif
```

### What changes, and the one behaviour break

Today, wiping is implicit: `ub_init_key` sets an internal `keyed` flag and
`ub_final` wipes if it is set. Under the proposal the flag becomes
caller-controlled and **defaults to off**, so a keyed state no longer wipes
unless the caller opts in.

That is a security-relevant default change and must be stated plainly rather
than buried: **a caller who today gets wiping for free by calling
`ub_init_key` would silently stop getting it.** Two ways to take it:

- **(a) As specified.** `ub_set_wipe` defaults off for every state. Simple and
  uniform; breaks the existing keyed guarantee.
- **(b) Default off for unkeyed, on for keyed.** `ub_init_key` calls
  `ub_set_wipe(S, 1)` internally, so today's behaviour is preserved exactly
  and the runtime control is an *override* -- `ub_set_wipe(S, 0)` to decline
  it, `ub_set_wipe(S, 1)` to request it on an unkeyed state holding something
  the caller considers sensitive.

**(b) is ACCEPTED.** It satisfies the same requirement -- a runtime switch
whose default is off wherever nothing is known to be secret -- without
withdrawing a guarantee a caller currently relies on, and it keeps the
header's stated principle that "a caller hashing with a key should not have to
know to ask".

It is also the right shape for the branch predictor, which is the reason to
prefer it beyond compatibility. Wiping is decided once per state and read once
per `ub_final`. With (b) the unkeyed path -- every Zcash call site, and the
overwhelming majority of digests -- takes the *same* direction on every
iteration of a batch loop, so the branch is perfectly predicted and costs
nothing measurable; the measured bound is already under 0.14 ns. A default
that varied per state, or a flag flipped between digests, would put a
data-dependent branch in the one place the library is designed to be hot. The
guidance that follows: **`ub_set_wipe` is a configuration call, not a
per-digest control.** Set it before the prefix state is shared; never toggle
it inside a hashing loop.

### Cost

The state field already exists (`keyed`, one byte, sharing the slot with
`buflen`/`outlen`, measured at 232 bytes either way). `ub_set_wipe` only
writes it, so the hot path is unchanged: the same single branch in `ub_final`
that currently measures under 0.14 ns on the unkeyed path. No new per-digest
cost.

Rename note: with the flag caller-controlled, `keyed` becomes the wrong name
for it -- `wipe` is what it now means. That is an internal field, so the
rename is free.

### Rust side

No runtime control. `uniblake-rs` does not wipe, matching `blake2b_simd`,
which every observed consumer already depends on. If it is ever wanted it
should be `impl Drop` behind a non-default cargo feature -- a build-level
decision like `UB_WIPE`, not a per-state argument -- so the unkeyed path keeps
its exact current codegen.

---

## How these compare to the reference implementations

### Rust: against `blake2b_simd`

The four departures in `hash_n` are not novelty for its own sake. Three of them
are places where the reference itself is loose and the consumer pays for it;
one is a genuine capability gap.

| | `blake2b_simd` | proposed `hash_n` | which is better, and why |
|---|---|---|---|
| batch entry | `many::hash_many(jobs)` | `State::hash_n(...)` | **different capability, not a rival.** See below. |
| tail width | n/a -- caller builds the bytes | `enum TailWidth` | ours; the reference has no equivalent to get wrong |
| output | `HashManyJob::to_hash()` per job | `&mut [u8]` + stride + `Range` | ours for the strided case; theirs is simpler when each digest is consumed whole |
| errors | none; misuse panics or is impossible | `Result<(), BatchError>` | ours, marginally -- geometry failure is data-dependent |

**The capability gap is the point.** `hash_many` takes N *independent whole
inputs*: `HashManyJob::new(params, input)` starts from `params.to_words()` --
the initial state -- and absorbs the entire input. There is no way to hand it a
state that has already absorbed a shared prefix, so hashing N leaves over a
140-byte prefix re-absorbs that prefix N times. `hash_n` takes `&self`, a state
that already holds the prefix, and appends only the counter.

That is this library's whole reason to exist, expressed at batch scale, and it
is precisely what the reference cannot express. It is also why `hash_n` is not
a competitor to `hash_many`: a consumer with genuinely independent inputs
should use `hash_many` (or its SIMD multi-message path), and one with a shared
prefix cannot.

### Rust: everything except `hash_n`

Already a near-subset of `blake2b_simd`, deliberately. `Params::new()`,
`hash_length`, `personal`, `salt`, `to_state`, `update`, `finalize`,
`Hash::as_bytes` all match name-for-name, which is what makes the swap in
§"Shims" below a near-textual substitution. Ours omits the tree-hashing
builders (`fanout`, `max_depth`, `node_offset`, `inner_hash_length`,
`last_node`) that no observed consumer calls, and takes `Params` methods by
value rather than `&mut self`.

**One divergence worth flagging:** `blake2b_simd::Params` methods return
`&mut Self`; ours return `Self`. Chained calls look identical, but
`let mut p = Params::new(); p.hash_length(32);` compiles there and not here.
None of the four observed call sites uses that form -- they all chain -- but a
shim must account for it.

### C: against libsodium

| | libsodium | uniblake C |
|---|---|---|
| state | opaque `crypto_generichash_blake2b_state`, 384 B | opaque, `ub_state_size()`, 232 B |
| init | `_init`, `_init_salt_personal` | `ub_init`, `ub_init_key`, `ub_init_param`, **+ proposed `ub_init_personal`** |
| copy | struct assignment (state is public-size) | `ub_copy` |
| prefix reuse | none | `ub_hash_tail`, `ub_hash_n`, `ub_prefix_check` |
| errors | `int`, mostly 0 | `int` with a documented enum and an error handler hook |

libsodium's `_init_salt_personal` is the closest thing to the proposed
`ub_init_personal`, and it takes both salt and personal. Ours takes only
personal because that is what consumers set; a caller needing salt still has
`ub_init_param`.

---

## What each library still has to add

Exactly one entry point each. Neither is structural.

| | must add | why | status |
|---|---|---|---|
| **C** | `ub_init_personal` | personalization is the one parameter every consumer sets and the only one requiring a `ub_param`; `key`, used by none, already has a constructor | additive, no ABI change |
| **C** | `ub_set_wipe` | the runtime control, accepted shape (b) | additive; state field already exists |
| **Rust** | `hash_n` | the batch form over a shared prefix, which no reference implementation offers | new API, phase P5 |

Not additions but pending decisions, both already measured:

- **C `f[2]` removal** -- would take the C state 232 -> 216, matching Rust.
  Measured at **-0.43 ns [-0.86, -0.05]**. **Not yet applied**: the C state is
  still 232 bytes today. It changes a documented size, so it belongs in a
  deliberate ABI revision rather than slipped in with additive work.
- **Rust is already 216 B**, but from a different change: replacing the `u128`
  counter with `[u64; 2]` to make the size ABI-invariant. Do not confuse the
  two; only the Rust one is in the tree.

---

## Shims, adapters, and compatibility

Three distinct problems, three different mechanisms. The rule for all of them:
an adapter converts *interfaces*, never semantics -- if a shim would change
what bytes come out, it is not a shim.

### 1. Rust consumers swapping `blake2b_simd` -> `uniblake`

A near-textual substitution, because the names were chosen to match:

```rust
-blake2b_simd::Params::new()
+uniblake::Params::new()
     .hash_length(32)
     .personal(b"ZcashAuthDatHash")
     .to_state()
     .update(h1).update(h2)
     .finalize()
```

What a consumer must check, in order of likelihood:

1. **`&mut Self` vs `Self` builders.** Non-chained use
   (`let mut p = Params::new(); p.hash_length(32);`) needs rewriting as a
   chain or a rebind. None of the four observed sites is affected.
2. **`Hash` type.** Both offer `as_bytes()`; confirm the `[u8; 32]`
   conversion a site uses exists.
3. **Tree-hashing builders.** If a site calls `fanout`, `node_offset`,
   `last_node` or similar, this library does not implement them -- that is a
   scope decision, not an oversight, and such a site should stay on the
   reference.

No compatibility module is proposed for this. A `blake2b_simd`-shaped facade
would freeze that crate's `&mut Self` convention into our API permanently to
save a handful of one-line edits.

### 2. C consumers swapping libsodium -> uniblake

Already built and tested: `compat/ub_sodium.h`, exercised by `make check`
(`compat: checks=56`). It maps `crypto_generichash_blake2b_*` onto the
uniblake calls. The one semantic difference it must not paper over is state
size: libsodium's state is a *public 384-byte array* callers may embed by
value, while uniblake's is opaque and sized by `ub_state_size()`. A caller
that embedded the sodium struct in its own must be changed, not shimmed --
this is exactly the case where an adapter would hide a real incompatibility.

### 3. The Equihash FFI, and why it should be deleted rather than adapted

`librustzcash/components/equihash/src/blake2b.rs` exports Rust's BLAKE2b as
`extern "C"` so tromp's C solver (`equi_miner.c:528`) can clone a state per
leaf. It is tempting to reimplement that surface --
`blake2b_init`/`clone`/`update`/`finalize`/`free` -- against uniblake and call
it a compatibility win.

**Don't.** That surface exists to make a C solver work, and it forces exactly
the shape this library is built to avoid: a heap allocation and free per leaf,
a state cloned through an opaque pointer, and no way to express that all N
leaves share a prefix. Reimplementing it would deliver uniblake's kernel while
discarding its entire advantage.

The correct move, if Equihash moves to this library, is for the *solver loop*
to call `hash_n` -- one call, one shared prefix state, no per-leaf allocation.
Whether that means porting the solver to Rust or giving the C solver a
`ub_hash_n` call directly is the consumer's decision; either way the shim
disappears rather than being ported.

If a drop-in FFI is nonetheless required as a transitional step, it should be
written **in the consumer**, not shipped here, and marked as temporary -- so
its cost is visible to whoever is paying it.

---

## Evidence: who actually uses what

Counted across every consumer in `ZKs/` whose `Cargo.toml` depends on
`blake2b_simd`. Reproduce with a grep over `ZKs/<repo>` for
`blake2b_simd::Params|use blake2b_simd` against `hash_many|HashManyJob|blake2b_simd::many`.

| consumer | `Params`/`State` sites | `many` sites |
|---|--:|--:|
| librustzcash | 17 | 0 |
| zcash | 4 | 0 |
| zebra | 3 | 0 |
| pirate | 3 | 0 |
| rusty-kaspa | 3 | 0 |
| ycash-zebra | 3 | 0 |
| ycash | 2 | 0 |
| zallet | 1 | 0 |
| Pirate-Unified-Light-Wallet | 1 | 0 |
| **total** | **37** | **0** |

The only `hash_many` references anywhere under `ZKs/` are the upstream crate's
own fuzz tests (`BLAKE/blake2_simd/tests/fuzz_many.rs`) and an unrelated
BLAKE3 CLI test. **No consumer in this lineage uses the batch API.**

### What `hash_many` actually is

Worth stating because the name invites the wrong assumption: it is **not
threaded**. `blake2b_simd` has zero dependencies -- no rayon, no `std::thread`.
`hash_many` is *multi-message SIMD interleaving*: it packs several messages
into the lanes of one vector register.

Its width is `many::degree()`, which is `guts::MAX_DEGREE`:

```rust
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
pub const MAX_DEGREE: usize = 4;
#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
pub const MAX_DEGREE: usize = 1;
```

Measured on this machine (aarch64): **`degree() == 1`**. On aarch64 `hash_many`
degenerates to a sequential loop with extra bookkeeping. On x86-64 with AVX2 it
processes 4 messages per pass, single-threaded.

### Decision: keep `hash_n`

- No consumer uses `hash_many`, so there is no compatibility argument for
  adopting its shape.
- It cannot express a shared prefix at all (see below), which is this
  library's entire purpose.
- It is a no-op on aarch64, one of the two target architectures.

`hash_n` stays. That is not a rejection of multi-message SIMD -- see the
composition note.

**Not checked: upstream discussion.** This session has no network access, so
GitHub issues, PRs and online mentions of `hash_many` were not reviewed. The
local clone (`ZKs/BLAKE/blake2_simd`, pinned `6a94ae2`, "version 1.0.4")
carries code and tests only, no issue history. The evidence above is
consumer-usage evidence, which is the stronger kind for this decision, but if
upstream has deprecated `hash_many`, reshaped it, or documented a threading
story elsewhere, that is unknown here. **To do when online:** check
`oconnor663/blake2_simd` issues and PRs for `hash_many`/`degree`, and whether
any Zcash-lineage project has an open request for a batch API.

### Can one be layered on the other?

No, in both directions, and the reason is informative:

- **`hash_n` over `hash_many`.** You would build N inputs of
  `prefix || counter` and pass them as independent messages -- re-absorbing
  the 140-byte prefix N times. Possible, and self-defeating: that cost is
  exactly what `hash_n` exists to remove.
- **`hash_many` over `hash_n`.** `hash_n`'s domain is one prefix state plus
  consecutive counters, strictly narrower than N arbitrary messages. Four
  unrelated inputs cannot be expressed as one `hash_n` call.

They are different problems, not different spellings of one.

**They do compose in the direction that matters, at the implementation
level.** A `hash_n` *implementation* can use multi-message SIMD internally --
absorbing the shared prefix once, then processing 4 counter-tails per vector
pass, which is what `hash_many`'s guts already do. That gets the prefix saving
*and* the lane parallelism. It is an implementation strategy for `hash_n`, not
a layering of interfaces, and it is the natural home for the AVX2 work parked
in `Requihash/BLAKE/OPTIMIZATIONS.md` §1.

---

## Interface-level struct sizes, all four implementations

aarch64, LP64. C via `ub_state_size()`/`sizeof`; Rust via `size_of`.

| type | uniblake C | libsodium | uniblake-rs | `blake2b_simd` |
|---|--:|--:|--:|--:|
| **state** | **232** (216 on `state-216-drop-f`) | **384** | **216** | **224** |
| state alignment | 8 | 64 | 8 | 16 |
| **params** | 52 (`ub_param`) | n/a -- no params type | **34** | **184** |
| **hash/output** | n/a -- caller's buffer | n/a | 65 | 65 |
| batch job | n/a | n/a | n/a | 112 (`HashManyJob`) |

What each difference is, and whether it should be closed:

- **libsodium 384 vs everyone else's ~220.** Its buffer is `buf[2 * 128]` --
  two blocks, so the last can be handled as final -- plus a `size_t buflen`.
  The internal struct is 361 B; the public opaque array rounds to 384 at
  64-byte alignment. **Not closable and not ours to close**; it is the cost of
  its deferred-compression design.
- **C 232 vs Rust 216.** The C state still carries `f[2]`. Measured at
  -0.43 ns and sitting on `state-216-drop-f`; **should be closed** in a
  deliberate ABI revision. Nothing else differs.
- **`blake2b_simd` 224 vs our 216.** `Count = u128` forces 16-byte alignment
  and pads the tail; it also keeps four config bytes we do not
  (`last_node`, `hash_length`, `implementation`, `is_keyed`). Ours dropped the
  `u128` for `[u64; 2]` to make the size ABI-invariant across targets.
  **Already resolved in our favour**, for correctness reasons rather than size.
- **`Params` 34 vs 184.** The gap is almost entirely
  `key_block: [u8; BLOCKBYTES]` -- `blake2b_simd` stages the key inside
  `Params`, so every `Params` carries a 128-byte buffer whether keyed or not,
  plus the tree-hashing fields. Ours stores no key material and no tree
  parameters. **Deliberate, keep.** It matters because `Params` is commonly a
  `const`-like value constructed per call site.
- **`ub_param` 52 vs Rust `Params` 34.** The C struct carries the full RFC
  7693 parameter block including the tree fields (`fanout`, `depth`,
  `leaf_length`, `node_offset`, ...) because `ub_init_param` is the general
  entry point. This is the gap the proposed `ub_init_personal` makes
  irrelevant for the common case -- the caller stops touching `ub_param` at
  all rather than the struct shrinking.
- **Alignment 64 (libsodium) vs 8/16.** libsodium over-aligns for its SIMD
  paths. Ours is 8 because nothing in the state requires more; a caller
  allocating with `ub_state_align()` is correct on every target.

---

## libsodium's public fixed-size state array: pro and con

libsodium declares its state as a public, fixed-size, over-aligned array:

```c
typedef struct CRYPTO_ALIGN(64) crypto_generichash_blake2b_state {
    unsigned char opaque[384];
} crypto_generichash_blake2b_state;
```

uniblake declares an incomplete type and reports its size at run time:

```c
typedef struct ub_state ub_state;
size_t ub_state_size(void);
size_t ub_state_align(void);
```

**In favour of the public array:**

- Callers can declare a state on the stack, embed it in their own struct, or
  put it in an array, with no allocator and no indirection. This is the single
  biggest practical advantage and it is not small.
- `sizeof` works, so C++ callers get value semantics, containers, and RAII for
  free.
- Copy is `a = b`. No function call, no error code.
- No ABI break when internals change, **as long as they still fit**. That is
  the trick: 384 is generous padding around a 361-byte struct.

**Against:**

- The size is a permanent public constant. libsodium can never let the state
  exceed 384 bytes, and the padding it reserves against that is real memory in
  every caller.
- It hides layout changes rather than preventing them: a caller who
  serialized the array, or memcpy'd it between builds, gets no diagnostic.
- Over-alignment to 64 is paid by every caller including those on targets
  where it buys nothing.
- The type carries no information. `sizeof` is right but says nothing about
  whether two builds agree on the contents.

**Why uniblake chose opaque + `ub_state_size()`:** the deciding factor is that
this library expects its state to be *copied on the hot path* -- one copy per
digest in the prefix workload -- so 168 bytes of reserved padding is not a
neutral cost, and an alignment of 8 rather than 64 keeps a batch of states
dense in cache. The measured state is 216 bytes; a libsodium-style declaration
would have to reserve more than that for future growth and would then be stuck
with it.

**What is given up, and the mitigation:** stack allocation and `sizeof` are
genuinely convenient, and losing them is the real cost of this choice. The
mitigation is that `ub_state_size()`/`ub_state_align()` are callable at
compile time in practice (constant-folded), and `compat/ub_sodium.h` maps the
libsodium spelling for callers migrating. A caller who *embedded* the sodium
struct by value must change; that is the one case the shim deliberately does
not paper over, because doing so would hide a real incompatibility.

---

## `hash_many`: what it is for, who wrote it, and why single-threaded

Asked directly, and the answer is in the upstream documentation rather than
in any inference from the code.

**Author and origin.** `blake2_simd` is Jack O'Connor's (`oconnor663`) -- the
same author as BLAKE3, of which this crate is a direct ancestor. `many` is a
first-class part of its stated purpose, not an afterthought: the repository
README lists as a headline feature

> "Support for computing multiple BLAKE2b and BLAKE2s hashes in parallel,
> matching the efficiency of BLAKE2bp and BLAKE2sp. See the `many` module in
> each crate."

**What it is for.** From `blake2b/src/many.rs`:

> "The throughput of these interfaces is comparable to BLAKE2bp, about twice
> the throughput of regular BLAKE2b when AVX2 is available."

So the point of `hash_many` is **not** threading. It is to reach BLAKE2bp-class
throughput *without* using the BLAKE2bp tree construction -- BLAKE2bp gets its
speed by hashing four independent lanes and combining them, which changes the
output. `hash_many` gets the same lane parallelism while every input stays
plain BLAKE2b. That is a genuinely useful thing and explains why it exists in
a crate with no dependencies: threading is the caller's business, SIMD lane
occupancy is the crate's.

Upstream's own benchmark table (README, Kaby Lake i5-8250U) records
`blake2b_simd many::hash` at 1.43 cpb against `blake2s_simd BLAKE2sp` at 1.32
-- the parity the README claims.

**Why it does nothing on ARM.** `degree()` is `guts::MAX_DEGREE`, which is 4
on x86/x86-64 and 1 everywhere else, because the crate's multi-message kernels
are written for SSE4.1 and AVX2 only. Measured on this machine: `degree() == 1`.
There is no NEON multi-message kernel, so on aarch64 `hash_many` is a
sequential loop with job bookkeeping. That is a gap in the crate, not a design
intent.

*(Upstream issues and PRs were not reviewed: the GitHub search API returned
"rate limit exceeded" for this host. The README and module documentation above
are authoritative for intent; open issues might still show a deprecation or a
NEON effort. Worth one retry when the limit resets.)*

## `hash_n` implemented with multi-message SIMD

This is the composition that matters, and it differs by architecture.

**On x86-64 with AVX2 (`degree() == 4`).** The natural implementation:

1. Absorb the shared prefix **once** into a state `S`. In the leaf shape that
   is one compression for a 140-byte prefix, done before the loop.
2. For each group of 4 consecutive counters, build 4 final blocks -- each is
   `S`'s pending bytes plus that counter's tail, zero-padded. These differ
   only in a few bytes.
3. Broadcast `S`'s chaining value into all 4 lanes, load the 4 blocks
   interleaved, and run **one** 12-round compression across the vector.
4. Extract 4 digests, write each at its stride.

The saving is multiplicative rather than additive: the prefix is absorbed once
instead of N times (which is `hash_n`'s existing advantage), *and* the final
compressions run 4-at-a-time (which is `hash_many`'s). Neither interface can
express both -- `hash_many` cannot take a pre-absorbed state, and today's
scalar `hash_n` cannot use lanes.

The donor code exists: `backends/vendor/libsodium/` has the AVX2
single-message kernel, and Samuel Neves' `blake2-avx2` (see
`ZKs/BLAKE/README.md`) has the interleaved multi-message BLAKE2bp/BLAKE2sp
kernels that are the right shape to adapt. This is
`Requihash/BLAKE/OPTIMIZATIONS.md` §1, and it is where AVX2 work should go
rather than into a faster single-message kernel.

**On aarch64 (`degree() == 1` in the reference).** There is no multi-message
NEON kernel anywhere in the ecosystem to borrow, and this project's own
measurements say why one is hard here: the single-message NEON kernel is
**1.71x slower than scalar** on an M4 Pro (136.0 against 79.4 ns), because
BLAKE2b's G function exposes only two independent 64-bit lanes and a 128-bit
register buys one doubling where wide scalar issue already extracts more.

A 2-lane multi-message NEON kernel would not have that problem -- two
independent *messages* per 128-bit register is real parallelism, unlike two
halves of one message -- so it is the more promising NEON direction and has
never been tried. But the expected ceiling is 2x lanes on a core whose scalar
path is already strong, against 4x on x86. **`hash_n` on ARM should stay
scalar until that is measured**, and the prefix saving alone remains its
entire benefit there.
