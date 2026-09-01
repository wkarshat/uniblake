# Experiments: how they run and where they go

A measured variant that is deleted has cost more than it produced. This file
is the rule that prevents that, and the record of what exists.

## The rule

**Every variant that is built and measured lands somewhere durable before the
next thing starts.** Three outcomes, three destinations:

| outcome | destination |
|---|---|
| adopted | committed to `main` with the measurement in the commit message |
| rejected on evidence | one line in `docs/INTERNALS.md` *Changes that measured flat*, with the sample count |
| good but blocked (ABI, scope, timing) | **a branch**, committed, with the measurement and the blocker in the message |

The third is the one that was being mishandled: `f[2]` removal was built,
measured at -0.43 ns, described in chat, and then reverted with nothing but
prose left behind. It now lives on `state-216-drop-f`.

**What needs asking.** Anything touching `src/` or a public header on `main`
needs explicit approval before it is committed there. A branch does not: a
branch is how a result is preserved for that decision, and creating one is
never a substitute for asking. But it must be *said* -- "this is on branch X,
here is what it measured, main is unchanged" -- not left implied.

## Where the numbers in the docs came from

Every performance figure in `docs/INTERNALS.md` and `docs/API_PROPOSAL.md` was
produced by the targets below, on an Apple M4 Pro under clang -O2, and can be
re-run. Figures predating `tools/ab_compare.py` are marked in place with the
sample count they actually had -- several are single-run medians and say so.

The variants themselves are either on `main`, on a branch listed below, or
recorded as rejected with their mechanism. Nothing measured should exist only
as a number in a conversation; that was the failure this file exists to stop.

## Reproducing any measurement here

```sh
make check SODIUM=<prefix>     # correctness gate; never measure without it
make bench-phases              # where a leaf digest's time goes
make bench-compare SODIUM=...  # against libsodium; matches the Rust harness
make kernel-stats              # what the compiled kernel does (self-testing)
make ab BIN_A=<a> BIN_B=<b> GREP='full leaf' RUNS=25
```

`make ab` is required for any A/B claim. It alternates the two binaries,
discards the first pair, and reports a bootstrap CI on the paired difference.
If its verdict is UNRESOLVED, that is what gets written down -- with the pair
count -- never "no change".

## Branches holding measured work

| branch | what | measured | blocked on |
|---|---|---|---|
| `state-216-drop-f` | removes `f[2]`, state 232 -> 216 B, `fin` byte for the finalized guard, `last` passed to the kernel | leaf **-0.43 ns** [-0.86, -0.05]; copy **-0.13 ns** [-0.14, -0.10]; 24 pairs | changes a documented public state size; needs a deliberate ABI revision |

## Variants built, measured, and deliberately not kept

These are recorded rather than branched because the result was negative --
the code has no future use, only the finding does. Each is one line in
`docs/INTERNALS.md`; the mechanism is written up there, not here.

- Non-mutating `ub_final` over local `h`/`t`/`f` (**+2.12 ns** [+1.76, +2.25]);
  cause identified as four pointer arguments occupying four registers, with
  `restrict` tested and ruled out as an explanation.
- Explicit last-round rotate deferral (**byte-identical assembly** on aarch64;
  clang already does it). Still open on x86-64 and AVX2 -- see `docs/X86.md`.
- Rolled 12-round loop with an inline `g()` (**+5.4 ns**).
