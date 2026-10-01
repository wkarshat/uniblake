# BLAKE2b on NEON: slower than scalar on Apple M4

**Scope warning, read first.**
Every number measured **one core design**:
Apple ARM M4 Pro lapton, 10 performance cores + 4 efficiency cores, macOS 26.3, clang arm64, `-O2`.
Results do not describe all ARM architectures and models, like Cortex-A7xx/A5xx, Neoverse N-series or V-series, Snapdragon, Graviton, Ampere, nor earlier Apple ARM silicon.
BLAKE2b NEON performance results here likely relate to issue width, vector-unit count, scheduling and register pressure.

## 1. The measurement

Prefix 140 B, tail 4 B, digest 50 B, N=400000, 10 reps, median ns/digest.
Correctness oracle: libsodium 1.0.22 built `-O3`.

| Test | ARM scalar | NEON | NEON vs scalar |
|---|--:|--:|--:|
| `ub_hash_tail` streaming | 83.0 ns | 134.4 ns | **62%** slower (1.62x) |
| `ub_hash_n`, counter run | 86.1 ns | 137.6 ns | **60%** slower (1.60x) |
| `ub_hash_n`, n=1 | 92.3 ns | 163.9 ns | **78%** slower (1.78x) |

Against libsodium BLAKE2b as the common reference, the scalar kernel measures 2.03x while
the NEON only 1.21x.

Reproduce:
    make bench       SODIUM=<prefix>     # scalar
    make bench-neon  SODIUM=<prefix>     # NEON kernel

## 2. Why

`backends/README.md` records the mechanism found while developing the kernel,
and it is consistent with the numbers above:

- The scalar kernel benefits from **round unrolling**: resolving `ub_sigma[r]`
  at compile time removes an indirection from the inner loop.
- The same unrolling benefit **does not transfer to NEON.** ARM unrolled rounds
  fit ARM registers, but the NEON **spills**. The added traffic costs more
  than the vectorisation saves.

BLAKE2b's round function is 128-bit-friendly, and a 128-bit NEON unit processes
two lanes where AVX2's 256-bit unit processes four. The arithmetic advantage on
NEON is therefore small to begin with, and on this core it does not survive the
register pressure.

**uniblake observation** for this workload uniblake advantage over libsodium comes
from prefix reuse rather than from vectorisation.

## 3. Future investigations

Ordered by importance and effort.

| # | Question | How | Why it matters |
|--|---|---|---|
| 1 | Confirm spilling | Compile both kernels with `-fverbose-asm` and count spill slots; build a non-unrolled NEON variant | Spill effects inferred from a development experience, not a register-allocation measurement |
| 2 | Different NEON schedule? | The `neon-both-kernels` tags an unrolled variant; interleave two independent digests to overcome latency | Two-message interleaving is how AVX2 wins; NEON may need it |
| 3 | Compiler matters? | Same kernels under GCC and clang, `-O2` and `-O3` | A 1.6x gap may reveal codegen differences |
| 4 | How E-cores differ from P-cores? | Pin the benchmark to `hw.perflevel1` cores | Vector-unit provisioning differs between core types |
| 5 | SVE/SVE2 change the picture? | Test on an SVE-capable core (Graviton 3+, Neoverse V1/V2) | Scalable vectors relax the 128-bit NEON constraint |
| 6 | Results hold on a non-Apple ARM core? | Run `make bench-neon` on Neoverse (Graviton/Ampere) or on a Cortex-A7xx phone | improve the guidance to "NEON loses on Apple" |

Items 1 and 3 are easy and only require a modern Apple laptop.

## 4. Implementation status

The NEON kernel is not deleted but **kept in the tree**, though not in the default build.
`make bench-neon` will measure on any core.

Consumers of this library may treat the scalar kernel as the default on arm64 unless they have
measured otherwise on their **own target**.
