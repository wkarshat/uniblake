# Unifying uniblake and uniblake-rs

The goal is one design expressed twice, natively: **C consumers use the C
library, Rust consumers use the Rust crate.** No cross-language FFI, no
adapter in the middle. Identical sizes are the evidence that the composition
is identical, not an end in themselves.

## Status

| | uniblake C | uniblake-rs | agree? |
|---|--:|--:|:--:|
| state | **216 B**, align 8 | **216 B**, align 8 | **yes** |
| parameter type | `ub_param` 52 B | `Params` 34 B | **no -- see below** |
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

## The parameter divergence, and why it is not the same kind of thing

`ub_param` (52 B) and `Params` (34 B) are **not two versions of one type**.
They are different concepts that ended up with similar names:

- **`ub_param` is the RFC 7693 §2.8 parameter block** -- the 64-byte structure
  that is XORed into the IV at init. It carries all twelve fields, including
  the tree ones (`fanout`, `depth`, `leaf_length`, `node_offset`,
  `xof_length`, `node_depth`, `inner_length`) that this library writes through
  but does not act on. 50 bytes of fields, 2 of padding.
- **Rust `Params` is a builder** -- a convenience for constructing a state,
  holding only what a caller can set through it: `hash_length`, `key_length`,
  `salt`, `personal`.

So the sizes differ because the *contents* differ, and the contents differ
because one is a wire format and the other is an API affordance. That is a
real divergence in the shape of the two libraries, and the fix is not to pad
one to match the other.

### Resolution

Give each language both concepts, with the same split:

| concept | C | Rust |
|---|---|---|
| RFC parameter block | `ub_param` (exists) | **add** `ParamBlock` -- the same twelve fields, same order |
| builder | **add** `ub_init_personal` and friends, or a small `ub_builder` | `Params` (exists) |

The builder is where the languages may differ in spelling, because it is
idiomatic surface. The parameter block must be **byte-identical in field order
and width** in both, because it is a specified serialization -- if the two
disagree there, one of them is wrong about BLAKE2b, not about style.

**Open:** whether Rust should expose `ParamBlock` publicly at all, given no
consumer sets tree parameters. The minimum is that it exists internally with
the same field order, and that a test asserts the serialized block matches the
C library byte for byte. That test is the real unification guarantee; equal
`sizeof` is only a proxy for it.

## Why alignment is a Rust worry and not a C one

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
ABI-dependent field types and the Rust one had not been. `docs/INTERNALS.md`
carries the C rule; `tests/abi.rs` now carries the Rust one.
