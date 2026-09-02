# Alternative compression and batch implementations

Prototypes. Not built by default; each replaces exactly one function and is
checked with the same suites as the shipped code.

| file | replaces | build |
|---|---|---|
| `compress_neon.c` | `ub_compress` | drop `src/compress.c`, add this (aarch64) |
| `compress_avx2.c` | `ub_compress` | same, plus `-mavx2` (x86-64 with AVX2) |
| `hash_n_threads.c` | `ub_hash_n` | `-DUB_HASH_N_SERIAL -DUB_THREADS=N`, add this, `-lpthread` |

`make check-backends` runs the kernels this host can execute. `make probe`
reports whether a CPU has AVX2.

**The AVX2 kernel can be built and run on an Apple-silicon Mac.** Apple clang
is a cross-compiler and Rosetta 2 executes AVX2 even though it does not
advertise it in CPUID:

```
make check-avx2-rosetta        # builds x86-64, runs under Rosetta
```

That covers the published vectors (1,536) and the API suite (155). It cannot
cover the oracle suites, because a Homebrew libsodium is arm64 and cannot be
linked into an x86-64 binary -- so those need a real x86-64 host, or a
libsodium built for x86-64.

Emulation establishes correctness and nothing about speed. There is still no
AVX2 timing; `make bench-avx2` on real x86-64 is the only thing that produces
one. On a native x86-64 host use `make check-avx2` and `make bench-avx2`, or
cross-compile with `CC=x86_64-w64-mingw32-gcc EXE=.exe`.

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

## Why the scalar unrolling win does not transfer to NEON

`src/compress.c` was hand-unrolled with literal sigma indices and moved from
89 to 79.2 ns/digest. The tables below are re-measured against it.

The change suggested a defect these kernels might share.
`compress_neon.c` reads `ub_sigma[r]` inside its round loop and builds each
message vector one lane at a time. In scalar, removing exactly that cost 11%
and *reduced* spilling, because the address registers freed outweighed the
extra live values.

**Tried on NEON, and it does not transfer.** Resolving the permutation at
compile time -- twelve `ROUND()` invocations with literal indices, gather
otherwise unchanged -- passes every suite and measures **163.6 ns against the
shipped kernel's 136.0**. It does remove the sigma loads exactly as intended:
read from the generated code, all 9 `ldrb` become 0. It also takes the body
from 157 instructions to 1056 and introduces 24 spill stores where the looped
kernel has none.

The reason the scalar result does not carry over is that on NEON the two
changes are inseparable. Literal indices *are* unrolling here: each round's
`MP()` operands differ, so the compiler cannot share one body across rounds,
and the twelve copies exceed the 32 vector registers. In scalar the
permutation was folded into addressing that the register allocator then had
fewer, not more, values to track. That asymmetry, not the gather strategy, is
what separates the two kernels.

So the vendored AVX2 donor's shape -- `blake2b-load-avx2.h` has zero sigma
references, resolving all twelve rounds through 48 `BLAKE2B_LOAD_MSG_r_n`
macros -- is **not** the shape to copy on aarch64. It works on x86-64, where
AVX2 has 16 wider registers holding twice the lanes per register, so twelve
unrolled rounds fit where on NEON they spill. Any future NEON work should
start from the looped kernel, not the donor.

This also supersedes the earlier register-pressure explanation for the 187 ns
unrolled variant (recoverable from the `neon-both-kernels` tag): that kernel
spills *less* per instruction than the looped one, and what it added was
shuffling -- 347 `ext` against 8 `ld1`. Three separate formulations now land
at 163-187 ns. Unrolling costs 27-51 ns on this core regardless of how the
message is gathered.

## NEON is slower than scalar here

Both NEON kernels are correct and both lose to the scalar compression on an
Apple M4 Pro. Six formulations were tried; the fastest is the shipped one. The
question worth answering is not "which formulation" but **what the ceiling
is**, because that is what transfers to other cores and other projects.

### Where the time goes, measured per instruction

Serial-dependency micro-benchmarks on this core, one operation per iteration
so the number is latency rather than throughput:

| operation | ns | cycles @4.4GHz | role in BLAKE2b |
|---|--:|--:|---|
| `add.2d` | 0.96 | 4.2 | two of the four steps in every G |
| `tbl.16b` | 0.78 | 3.4 | rot24 and rot16 |
| `shl`+`sri` | 1.26 | 5.5 | **rot63 — no single-instruction form** |
| `ext.16b` | 0.55 | 2.4 | diagonalisation between round halves |
| scalar `ror` | 0.27 | 1.2 | the same rotate, scalar |

The last row is the finding: **a scalar rotate is 4.6x cheaper than the NEON
rot63 it replaces.** BLAKE2b needs one rot63 per G, on the critical path, and
NEON has no 64-bit rotate instruction at all -- it must be synthesised as a
shift pair. That is not a property of this kernel; it is a property of the
instruction set meeting this algorithm.

### Why 2-wide SIMD cannot win here

Counting operations, NEON should halve the work: 12 rounds x 8 G becomes 12 x
4 vector G-groups. Counting *dependencies*, it changes nothing. G is a serial
chain a -> d -> c -> b, eight dependent steps; per round the four column G are
mutually independent and then the four diagonal G are. The critical path is
therefore 16 dependent steps per round, 192 over twelve rounds -- **identical
for scalar and for 2-wide SIMD**, because pairing two lanes of the *same*
message does not shorten the chain.

So SIMD halves the instruction count on a core that was never issue-limited,
while lengthening every step on the chain. A wide out-of-order core running
scalar code already overlaps the independent G calls; the vector path pays
more latency per step to remove parallelism the machine was extracting anyway.

Summing the measured latencies over the chain predicts ~190 ns against a
measured 136, so the core does overlap more than the pure-chain model assumes
-- but the direction and the cause are clear, and no rearrangement of a
single-message kernel changes them.

### What would change the answer

**Two independent messages per register, not two halves of one.** That is the
one formulation not yet tried, and it is the only one where the lane width
buys real parallelism: two separate BLAKE2b chains have no dependency between
them, so the 2x lane width converts directly into 2x throughput and the
critical path stops mattering. It is the same technique the x86 kernels use at
4 lanes.

Expected ceiling on this core: below 2x, since the rot63 penalty is paid per
lane-group regardless. That is worth measuring before assuming, and it is the
direction any future NEON work should take. A single-message NEON kernel is a
dead end on this class of core and the numbers above say why.

### The kernels as they stand


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
