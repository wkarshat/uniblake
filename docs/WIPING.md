# Wiping: what four implementations do, and why uniblake chose what it did

API-level behaviour, not internals. If you are deciding whether uniblake
clears secrets for you, this is the page.

## What "wiping" means here

Two separate things, often conflated:

1. **Key material staged during init.** BLAKE2b absorbs a key as one
   zero-padded 128-byte block. Whoever builds that block holds a copy of the
   caller's key.
2. **The finalized state.** After `final`, the chaining value and the pending
   block are derived from everything absorbed, including a key.

## The four implementations

| | stages key in | wipes staged key | wipes final state |
|---|---|---|---|
| **libsodium** | stack buffer in `blake2b_init_key` | **always** (`sodium_memzero`) | **always** (`sodium_memzero` on `h` and `buf`) |
| **BLAKE2 reference (RFC 7693)** | stack buffer | `secure_zero_memory` | `secure_zero_memory` on the state |
| **`blake2b_simd`** | **`Params.key_block`, 128 B, held for the life of `Params`** | **never** | **never** |
| **uniblake C** | stack buffer in `ub_init_key` | **always**, regardless of build | `UB_WIPE=1` (default) **and** state was keyed |
| **uniblake-rs** | transient block in `to_state_keyed` | dropped, not zeroed | **never** |

### History, briefly

libsodium and the RFC reference both wipe unconditionally: they descend from a
tradition where a hash function is assumed to be handling secrets, and the
cost is not on anyone's critical path because neither was designed for a
shared-prefix batch workload.

`blake2b_simd` went the other way, and further than it looks: it keeps the
key block **inside `Params`** rather than on a stack frame, so a `Params`
constructed with a key holds 128 bytes of key-derived material for as long as
the caller keeps it, and never clears it. That is a deliberate trade for a
crate whose stated purpose is throughput, and it is a large part of why its
`Params` is 184 bytes against uniblake-rs's 34.

uniblake C sits between: the caller's key is **always** burned from the stack
because that copy is the library's own and no caller can reach it, while the
final-state wipe is a build option because it is the part a caller may
legitimately not want.

## uniblake's rules

**C.** `ub_init_key` burns its staged copy of your key unconditionally. That
is not configurable and should not be: the buffer is ours, the caller cannot
free it, and the cost is once per state.

The **final-state** wipe is controlled two ways:

- `UB_WIPE` (compile time, **default 1**) -- whether the capability exists at
  all. `UB_WIPE=0` removes the branch and the flag; a build hashing only
  public data pays nothing.
- `ub_set_wipe(S, on)` (runtime, **proposed**) -- per state. Default **off for
  unkeyed states, on for keyed ones**, so `ub_init_key` keeps giving you
  wiping without asking, and the call is an *override* in either direction.

  It is a configuration call, not a per-digest control: the flag is read once
  per `ub_final`, and leaving it constant across a batch keeps that branch
  perfectly predicted. Set it before the prefix state is shared; never toggle
  it inside a hashing loop.

Measured cost of the branch on the unkeyed path: **under 0.14 ns**
(95% CI [-0.14, +0.10], 60 alternating pairs). It is not why anything is slow.

**Rust.** Does not wipe, matching `blake2b_simd` -- which every observed
consumer already relies on. Unlike `blake2b_simd`, it does not *retain* key
material either: `Params` holds `key_length`, never a key block, which is why
it is 34 bytes rather than 184. If wiping is ever wanted it belongs behind a
non-default cargo feature as `impl Drop`, a build-level choice like `UB_WIPE`,
so the common path keeps its exact codegen.

## Choosing

- Hashing public data (every observed Zcash call site): defaults are right,
  and on the C side `UB_WIPE=0` is available if you want the code gone.
- Hashing with a key in C: you get the staged-key wipe always and the
  final-state wipe by default. Nothing to do.
- Hashing with a key in Rust: no wiping. If that matters, this crate is not
  yet the right choice and the feature above is the fix.
