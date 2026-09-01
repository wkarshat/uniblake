# Alternative compression and batch implementations

Prototypes. Not built by default; each replaces exactly one function and is
checked with the same suites as the shipped code.

| file | replaces | build |
|---|---|---|
| `compress_neon.c` | `ub_compress` | drop `src/compress.c`, add this (aarch64) |
| `compress_avx2.c` | `ub_compress` | same, plus `-mavx2` (x86-64 with AVX2) |
| `hash_n_threads.c` | `ub_hash_n` | `-DUB_HASH_N_SERIAL -DUB_THREADS=N`, add this, `-lpthread` |

`make check-backends` runs the kernels this host can execute. The AVX2 kernel
needs an x86-64 host: `make check-avx2` and `make bench-avx2` there, or
cross-compile with `CC=x86_64-w64-mingw32-gcc EXE=.exe` and run the binaries
on the target. `make probe` reports whether a CPU has AVX2.

The AVX2 kernel is **correct but unmeasured**. On Apple silicon it runs under
Rosetta 2, which executes AVX2 despite not advertising it in CPUID: the
published vectors (1,536), the API suite (155) and the alias suite against the
vendored BLAKE2 reference (1,218) all pass there. The oracle suites need a
libsodium built for x86-64, which Homebrew's arm64 build is not.

Rosetta is emulation, so it establishes correctness and nothing about speed.
No AVX2 timing is recorded; `make bench-avx2` on real x86-64 is the only thing
that would produce one.

## Measurements

Apple M4 Pro (arm64, 14 cores), Apple clang -O2, libsodium 1.0.21.
Prefix 140 B, digest 50 B, median of 7 reps x 400k digests.

A row compares builds on **this** part, at this core count, cache and memory
configuration, on this working-set size. It is not a comparison between
instruction sets: NEON here is one implementation on one Apple core, and says
nothing about AVX2 on an x86-64 part of some other generation. The concurrent
rows likewise measure this chip's cores and memory system, not threading in
general.

| build | streaming | `ub_hash_n` |
|---|--:|--:|
| scalar (shipped) | 92.9 ns | 96.4 ns |
| NEON | 138.6 ns | 141.4 ns |
| 4 threads | 93.4 ns | 26.2 ns |
| 8 threads | 93.2 ns | 13.4 ns |

Figures from one machine; re-measure on the target.

## The scalar kernel now sets a higher bar

`src/compress.c` was hand-unrolled with literal sigma indices and moved from
89 to 79.5 ns/digest. Every figure below predates that and is measured against
the old 89 ns baseline; the gap to scalar is now wider than the tables say.

More usefully, the change identifies a defect these kernels share.
`compress_neon.c` still reads `ub_sigma[r]` inside its round loop and builds
each message vector one lane at a time. In scalar, removing exactly that cost
11% and *reduced* spilling, because the address registers freed outweighed the
extra live values. The NEON kernel has never been tried with the permutation
resolved at compile time.

The vendored AVX2 donor already does this: `blake2b-load-avx2.h` contains zero
references to sigma, resolving all twelve rounds through 48
`BLAKE2B_LOAD_MSG_r_n` macros. That is the shape to copy, and it is the reason
the donor is worth vendoring rather than writing a kernel from scratch.

A caveat before assuming this transfers. An unrolled NEON kernel *was* built
and measured slower -- 187 ns against the looped kernel's 144 -- and removed;
recover it from the `neon-both-kernels` tag. But that kernel unrolled the
rounds while still assembling message vectors by lane insert, so it paid the
code growth without collecting the saving. Whether literal-index loads change
the result is untested.

## NEON is slower than scalar here

Both NEON kernels are correct and both lose to the scalar compression on an
Apple M4 Pro:

| kernel | ns/digest |
|---|--:|
| scalar (`src/compress.c`) | 90 |
| `compress_neon.c` | 141 |

BLAKE2b's G function exposes only two independent 64-bit lanes within one
message, so a 128-bit register buys one doubling while wide scalar issue
already extracts more. Unrolling the rounds makes it worse rather than better:
holding the eight message vectors live across twelve rounds exhausts aarch64's
32 vector registers and spills. A kernel built that way measured 187 ns; it is
recoverable from the `neon-both-kernels` tag.

The sign of the loss depends on the core, so the kernel is kept: correct,
measured, and the right starting point where scalar is weaker. Do not adopt it
without measuring there.

Where SIMD pays for BLAKE2b is across *independent* messages -- one hash per
lane rather than one hash split over lanes. That needs a batch-aware
compression interface these prototypes do not have; `ub_hash_n` is the entry
point where such an implementation belongs.

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
