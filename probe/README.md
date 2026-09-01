# CPU probe

`make probe` prints the CPU identity and ISA flags a measurement should be
reported with:

```
brand      Apple M4 Pro
arch       aarch64
vendor     Apple
midr       not exposed by this OS; identity is the brand string
features   neon=1 sse41=0 avx2=0 avx512f=0 sha_ni=0
```

Not part of the library. `include/` and `src/` have one scalar kernel, no
dispatch, and no platform-specific code; nothing there consults this. It is
built only by its own target.

## Why it exists

**Measurements need their machine.** A ns/digest figure is meaningless without
the exact CPU, and the brand string alone does not distinguish steppings.

**A selector would need core identity, not ISA flags.** NEON is base ISA on
every aarch64 core, so `neon=1` says nothing about whether a NEON kernel wins:
both kernels in `backends/` are correct and both measure slower than scalar
here, while published results have the same code faster on other cores. A
selector keying on "has NEON" would choose wrongly. What it needs is a list of
cores where a kernel has been *measured* to win, which requires
family/model/stepping on x86 and MIDR implementer/part on ARM.

## What it reports

| field | source |
|---|---|
| vendor, family, model, stepping | x86 `cpuid` leaf 0 and 1 |
| brand string | x86 `cpuid` leaves 0x80000002-4; macOS `sysctlbyname` |
| SSE4.1, AVX2, AVX-512F, SHA-NI | x86 `cpuid` leaves 1 and 7 |
| NEON | architecturally present on aarch64 |
| MIDR implementer, part | Linux/ARM; unavailable on macOS, which does not expose MIDR to userspace |

## Provenance

`ub_probe.c` and `ub_probe.h` are carried from this project's predecessor at
`Requihash/BLAKE/uniblake/src/`, unmodified. `ub_probe_main.c` is the driver
for the `probe` target and is original.
