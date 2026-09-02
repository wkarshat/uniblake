# Validating and benchmarking in a consuming codebase

A recipe for swapping the hash in an existing codebase, in an order that keeps
a failure attributable at every step. The example replaces libsodium in a C++
tree; adapt the paths.

The rule throughout: **the digests must not change**. Where the digest is part
of a protocol or a stored format, a different digest is a compatibility break,
not an optimization.

This procedure has been carried out once, against a proof-of-work node whose
Equihash implementation used libsodium's BLAKE2b. What that produced, as a
worked example of each step below:

| step | result |
|---|---|
| baseline | fixed-nonce solve timing, deterministic, identical work per arm |
| swap | a wrapper giving the opaque state value semantics, so call sites did not change |
| prove | 20,000 digests byte-identical, plus the chain's own genesis vectors |
| prefix interface | per-digest state copy removed; 1.15x on the per-digest cost |
| measure the consumer | leaf generation is about a quarter of a solve, so the digest-level gain is a few percent of the whole |
| threads | already threaded by the consumer; the batch entry point does not apply where counters are strided |

The measured figures are in `TODO.md`. The step that mattered most was
proving the digests first: on a consensus path a changed digest is a fork, and
no speedup is worth discovering that later.

## 1. Before touching the consumer

Build and check uniblake standalone on the target machine:

```
make check          # all conformance suites; oracle is libsodium
make bench          # per-digest figures for this machine
```

If `make check` fails here, nothing downstream is meaningful. If libsodium is
unavailable on the target, `tests/test_api.c` still runs and
`tests/test_core.c` still checks the RFC 7693 vector.

## 2. Establish the consumer's baseline

Record what the unmodified tree does, on the same machine, before changing
anything:

- total wall time for the whole operation, several runs, median not mean;
- the hashing share of it. If the tree has no instrumentation, add a timer
  around the hashing loop rather than guessing. The share bounds the
  achievable speedup.

Run at least eight repetitions per configuration and compare medians; single
runs are noise.

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
2. **A known-good artifact re-verified.** Take an input that already
   validates against a stored digest, re-run it with the new hash, and
   require the same result. This exercises the real personalization and
   digest length rather than a synthetic vector.
3. **Full output compared item-for-item.** Run the whole operation over the
   same inputs before and after and require identical output sets. If the
   tree has no dump mode, collecting the results into a sorted set and
   diffing is a dozen lines.

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

and call `ub_prefix_check(&base_state, 4)` once after absorbing the prefix. If
it returns `UB_E_GEOMETRY`, the prefix length leaves no room for the counter
and the fast path does not apply. Determined at startup rather than observed
later as a slowdown.

Where the consumer reads fixed-width fields out of each digest, `off`/`len`
write just that field, packed, with no second pass.

Re-run step 4 in full.

## 6. Measure the consumer, not the microbenchmark

Per-digest nanoseconds understate the consumer's cost. Measured inside one
real consumer, libsodium cost 337 ns per call against 285 ns in the
microbenchmark -- 18% more, from the state copy, the write into the caller's
own layout, and extracting more than one field per digest.

So: re-run the step-2 timing, same repetition count, same machine, and compare
medians. Report the whole-operation number, not the hashing number -- a large
speedup on a small share of runtime is a small speedup.

## 7. Consider threads last

`backends/hash_n_threads.c` splits the digest range across threads. It scales
near-linearly, which usually means the hashing phase stops being the limit
before the thread count runs out.

Two cautions: the process may already run a thread per operation, in which
case adding threading inside it oversubscribes; and a correctness-critical
path should not gain a thread pool without an ownership story for it. Measure
whether hashing is still a meaningful share before adding one.
