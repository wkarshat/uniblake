# Validating and benchmarking in a consuming codebase

A recipe for swapping the hash inside an existing proof-of-work node, in an
order that keeps a failure attributable at every step. The example is a
Zcash-lineage C++ tree hashing through libsodium; adapt the paths.

The rule throughout: **the digests must not change**. A different digest is a
consensus change, not an optimization.

## 1. Before touching the consumer

Build and check uniblake standalone on the target machine:

```
make check          # 4 suites, ~46k checks, oracle is libsodium
make bench          # per-digest figures for this machine
```

If `make check` fails here, nothing downstream is meaningful. If libsodium is
unavailable on the target, `tests/test_api.c` still runs and
`tests/test_core.c` still checks the RFC 7693 vector.

## 2. Establish the consumer's baseline

Record what the unmodified tree does, on the same machine, before changing
anything:

- total solve time, several nonces, median not mean;
- the hashing share of it. If the tree has no instrumentation, add a timer
  around the leaf-generation loop rather than guessing — the share decides
  whether this work is worth doing at all.

A tree in this lineage typically exposes a solver timing test through
environment variables, e.g.

```
SOLVE_TIMING_1927=8 SOLVE_TIMING_SOLVER=tromp ./src/test/test_bitcoin \
  --run_test=equihash_tests/solver_timing_192_7 --log_level=message
```

Run at least eight nonces per configuration and compare medians; single-solve
differences are noise.

## 3. Swap the hash, keeping call sites

Two changes to the tree:

```c
/* was: #include "sodium.h"  and
   typedef crypto_generichash_blake2b_state eh_HashState;            */
#include "uniblake/ub_sodium.h"   /* adapter: keeps the sodium spelling */
```

and, at every place one state is assigned to another:

```c
/* was: state = base_state;  */
ub_copy(&state, &base_state);
```

That second change is unavoidable — `ub_state` is opaque, so it has no
assignable value type. Search for assignments between two state variables to
find them; in a tree of this shape there are typically fewer than ten.

Nothing else moves: `_init_salt_personal`, `_update`, and `_final` map by
name through the adapter.

## 4. Prove the digests are unchanged

In order, cheapest first:

1. **The tree's own hash unit tests.** They compare against fixed vectors and
   fail immediately on a wrong digest.
2. **A known solution re-verified.** Take a block that already validates,
   re-run verification with the new hash, and require the same result. This
   exercises the real personalization and digest length.
3. **Solver output compared solution-for-solution.** Run the solver over the
   same nonces before and after and require identical solution sets. A
   consuming tree usually already has a dump mode for this, e.g.
   `DUMP_1927_SOLVER=vectors.txt`; if not, collecting the solution index
   vectors into a sorted set and diffing is a dozen lines.

Step 3 is the one that matters: it exercises the prefix path, the exact
geometry, and the consumer's own layout at once. It is also the check that
catches a wrong midstate, which unit vectors can miss.

## 5. Adopt the prefix interface

Only after step 4 passes. The adapter alone already gives most of the gain,
because the improvement is in eager compression, not in the batch call.

Replace the per-index hash:

```c
/* was: copy the base state, append the index, finalize */
ub_hash_n(&base_state, 4, first_index, count, 0, 0, out, row_stride);
```

and call `ub_prefix_check(&base_state, 4)` once after absorbing the header. If
it returns `UB_E_GEOMETRY`, the prefix length leaves no room for the counter
and the fast path does not apply — a fact worth knowing at startup rather than
discovering as a slowdown.

Where the consumer reads fixed-width fields out of each digest, `off`/`len`
write just that field, packed, with no second pass.

Re-run step 4 in full.

## 6. Measure the consumer, not the microbenchmark

Per-digest nanoseconds understate the consumer's cost. Measured inside one
solver, libsodium cost 337 ns per call against 285 ns in the microbenchmark —
18% more, from the state copy, the write into the solver's own layout, and
extracting more than one hash per digest.

So: re-run the step-2 timing, same nonce count, same machine, and compare
medians. Report the whole-solve number, not the hashing number — a large
speedup on a small share of runtime is a small speedup.

## 7. Consider threads last

`backends/hash_n_threads.c` splits the digest range across threads. It scales
near-linearly, which usually means the hashing phase stops being the limit
before the thread count runs out.

Two cautions in a node: the process may already run a thread per solve, in
which case per-solve threading oversubscribes; and a consensus-critical path
should not gain a thread pool without an ownership story for it. Measure
whether hashing is still a meaningful share of the solve before adding one.
