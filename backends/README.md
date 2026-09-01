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

The AVX2 kernel passes the oracle suites on x86-64 and is measured below.

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
| scalar (shipped) | 79.2 ns | 83.3 ns |
| NEON | 142.8 ns | 142.1 ns |
| 4 threads | 79.9 ns | 23.9 ns |
| 8 threads | 80.9 ns | 12.3 ns |

Figures from one machine; re-measure on the target.

Intel Skylake, 2 cores, GCC 13 -O2, libsodium 1.0.18. Same geometry: prefix
140 B, digest 50 B, median of 7 reps x 400k digests.

| build | streaming | `ub_hash_n` |
|---|--:|--:|
| scalar (shipped) | 218.9 ns | 225.3 ns |
| AVX2 | 136.9 ns | 147.5 ns |
| 2 threads | 215.1 ns | 118.7 ns |

AVX2 is 1.6x scalar. The threaded row moves only `ub_hash_n`: that call
computes independent digests and splits across cores, while streaming is a
single dependent chain and stays at scalar speed.

Measured in a VM; re-measure on the target.

## The scalar kernel now sets a higher bar

`src/compress.c` was hand-unrolled with literal sigma indices and moved from
89 to 79.2 ns/digest. The tables below are re-measured against it.

The change identifies a defect these kernels share.
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
| scalar (`src/compress.c`) | 79.2 |
| `compress_neon.c` | 142.8 |

BLAKE2b's G function exposes only two independent 64-bit lanes within one
message, so a 128-bit register buys one doubling while wide scalar issue
already extracts more.

An unrolled variant measured 187 ns -- worse still -- and was removed;
recover it from the `neon-both-kernels` tag. **The reason is not register
pressure.** Read from the generated code, that kernel spills *less* per
instruction than the looped one (4.1% against 6.0%) and eliminates the nine
sigma byte-loads entirely. What it adds is shuffling: 347 `ext` instructions
against the looped kernel's 8 `ld1`, because the donor's `LOAD_MSG` macros
assemble each message vector with `vext`/`vcombine` rather than lane inserts.
On this core that trade loses.

That correction matters because the same "unrolling exhausts the registers"
reasoning was recorded for the scalar kernel and proved wrong there too:
hand-unrolling with literal sigma indices gained 11% and *reduced* spilling.

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
| 1 | 83.4 | 1.0x |
| 2 | 42.6 | 2.0x |
| 4 | 24.0 | 3.5x |
| 8 | 12.3 | 6.8x |

Close to linear, which is what an embarrassingly parallel range should give.
Reproduce with `make bench-threads THREADS=<n>`.

Two details in the implementation worth keeping in a production version:
arguments are validated once, before any thread starts, so a bad call fails
without side effects; and the caller's own thread runs the last span rather
than idling.

Below `UB_THREAD_MIN` (512) digests the range runs inline — thread creation
costs more than the work.
