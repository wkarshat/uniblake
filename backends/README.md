# Alternative compression and batch implementations

Prototypes. Not built by default; each replaces exactly one function and is
checked with the same suites as the shipped code.

| file | replaces | build |
|---|---|---|
| `compress_neon.c` | `ub_compress` | drop `src/compress.c`, add this |
| `hash_n_threads.c` | `ub_hash_n` | `-DUB_HASH_N_SERIAL -DUB_THREADS=N`, add this, `-lpthread` |

Both pass `tests/test_core.c` (630) and `tests/test_prefix.c` (45,531).

## Measurements

Apple M4 Pro (arm64, 14 cores), Apple clang -O2, libsodium 1.0.21.
Prefix 140 B, digest 50 B, median of 7 reps x 400k digests.

| build | streaming | `ub_hash_n` |
|---|--:|--:|
| scalar (shipped) | 92.9 ns | 96.4 ns |
| NEON | 138.6 ns | 141.4 ns |
| 4 threads | 93.4 ns | 26.2 ns |
| 8 threads | 93.2 ns | 13.4 ns |

Figures from one machine; re-measure on the target.

## NEON is slower here, and that is expected

2-lane NEON loses to this core's scalar execution: 138.6 ns against 92.9, a
1.5x loss. BLAKE2b's G-function gives only two independent 64-bit lanes within
one message, so a 128-bit register buys one doubling while wide scalar issue
already extracts more. Published BLAKE2b results show the same code faster
than scalar on some ARM cores and slower on others, so this is a property of
the core, not of the code.

The kernel is kept because it is correct, measured, and the right starting
point on a core where scalar is weaker. It should not be adopted without
measuring there.

Where SIMD does pay for BLAKE2b is across *independent* messages — one hash
per lane rather than one hash split over lanes. That is an AVX2-shaped
approach and needs a batch-aware compression interface this prototype does not
have; `ub_hash_n` is the entry point where such an implementation belongs.

## Threading scales, because the digests are independent

`ub_hash_n` splits cleanly: each digest is independent, the prefix state is
read-only, and each thread copies it and writes its own slice of the output.
No locks, no shared counters.

4 threads: 3.7x. 8 threads: 7.2x. Close to linear, which is what an
embarrassingly parallel range should give.

Two details in the implementation worth keeping in a production version:
arguments are validated once, before any thread starts, so a bad call fails
without side effects; and the caller's own thread runs the last span rather
than idling.

Below `UB_THREAD_MIN` (512) digests the range runs inline — thread creation
costs more than the work.

## Distributions surveyed as alternatives to libsodium

From the BLAKE reference notes, for anyone weighing a dependency:

- **libsodium** — maintained fork of the reference lineage behind
  `crypto_generichash*`, runtime CPU dispatch. Full parameter block. Used here
  as the conformance oracle.
- **BLAKE2 reference** (`github.com/BLAKE2/BLAKE2`) — the definitive C
  implementation, with SSE/AVX variants and `blake2bp` for parallel hashing.
- **OpenSSL** — BLAKE2b-512/BLAKE2s-256 since 1.1.0, EVP-only, no
  salt/personalization at the legacy API, so it cannot serve callers that need
  domain separation.
- **`blake2b_simd`** (Rust) — independent implementation with AVX2/SSE4.1 and
  runtime detection, plus a `hash_many` batch API: the closest existing thing
  to the lane-per-message approach described above.
- **`blake2`** (RustCrypto) — pure Rust from the specification, trait-shaped,
  the most-used BLAKE2 crate.
- **CPython `hashlib.blake2b`** — the fullest parameter-block exposure of any
  mainstream API; useful for generating test vectors.
- **`blake2-rfc`** — unmaintained since 2017; historical only.
