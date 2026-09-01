# Alternative compression and batch implementations

Prototypes. Not built by default; each replaces exactly one function and is
checked with the same suites as the shipped code.

| file | replaces | build |
|---|---|---|
| `compress_neon.c` | `ub_compress` | drop `src/compress.c`, add this |
| `hash_n_threads.c` | `ub_hash_n` | `-DUB_HASH_N_SERIAL -DUB_THREADS=N`, add this, `-lpthread` |

Both pass the core and prefix suites unchanged.

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

Measured on the M4 Pro, prefix 140 B, digest 50 B:

| threads | ns/digest | speedup |
|--:|--:|--:|
| 1 | 93.8 | 1.0x |
| 2 | 49.5 | 1.9x |
| 4 | 25.5 | 3.7x |
| 8 | 13.0 | 7.2x |

Close to linear, which is what an embarrassingly parallel range should give.
Reproduce with `make bench-threads THREADS=<n>`.

Two details in the implementation worth keeping in a production version:
arguments are validated once, before any thread starts, so a bad call fails
without side effects; and the caller's own thread runs the last span rather
than idling.

Below `UB_THREAD_MIN` (512) digests the range runs inline — thread creation
costs more than the work.

## Where to source a kernel

Only two of these matter to someone writing a backend here:

- **libsodium** — its `crypto_generichash*` BLAKE2b carries SSSE3/SSE4.1/AVX2
  compress paths behind a function-pointer install, the same replaceable-kernel
  seam this directory uses. It is the donor to start from for x86, and the
  conformance oracle for `make check`.
- **`blake2b_simd`** (Rust) — has a `hash_many` batch API, the closest existing
  thing to the lane-per-message approach described above. Read it for the
  interleave structure, not to link against.

Which implementations are still maintained, who wrote them, and how the
alternatives compare as dependencies belong to the program's BLAKE record,
not here -- those facts age on a different clock than this code.
