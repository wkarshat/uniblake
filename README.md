# uniblake

BLAKE2b in portable C, for callers who need more than a one-shot hash:

- **Eager absorption, specified.** Whole blocks are compressed as soon as
  more input follows, so a shared prefix is absorbed once. The standard API
  cannot express this and gives no diagnostic when an implementation does the
  opposite.
- **Controlled access to the internals.** The parameter block, the prefix and
  batch layer, a replaceable compression kernel, runtime-reported state size
  and alignment. Every entry point validates and returns a code; diagnosis is
  opt-in and costs nothing when unused.
- **Portable by construction.** C99, endian-neutral, no allocation, no POSIX,
  no threading runtime, no floating point. Conformance is checked against an
  independent oracle, not asserted.
- **Parallel where it pays, without owning your threads.** Digests over a
  shared prefix are independent, and `ub_hash_n` hands a kernel the whole
  range at once; splitting it across 8 threads measured 7.2x. The library
  spawns nothing itself, so it does not fight a caller that already has a
  scheduler.

BLAKE2b only. Other algorithms are out of scope, not pending.

## What it does

Hashing one message is the usual three calls. What uniblake adds is the
repeated case:

    H(header || 0),  H(header || 1),  H(header || 2),  ...

Absorb `header` once, then produce each digest for the cost of a single compression.
This requires the implementation to compress the shared bytes eagerly, which the BLAKE2
API neither specifies nor exposes. uniblake specifies it as interface behaviour, and
reports when the sizes do not permit it.

## Quick start

```c
#include "uniblake/uniblake.h"

/* Digest length in bytes: 1 to 64. 32 and 64 are conventional. */
ub_state *S = aligned_alloc(ub_state_align(), ub_state_size());
ub_init(S, 32);
ub_update(S, msg, msglen);
ub_final(S, out, sizeof out);          /* outcap; must be >= digest length */
```

Repeated-prefix hashing:

```c
#include "uniblake/prefix.h"

ub_param P;
ub_param_init(&P, 32);                 /* digest length */
/* Optional. BLAKE2 personalization: a 16-byte parameter-block field mixed into the
   initial state, yielding an independent hash function per value. */
memcpy(P.personal, "my-app-v1\0\0\0\0\0\0\0", 16);
ub_init_param(S, &P);
ub_update(S, header, headerlen);       /* shared bytes, absorbed once */

/* Digests of header || counter, for counter in [0, count).
   4      = counter width in bytes (4 or 8)
   0      = first counter value
   0, 0   = whole digest; a nonzero pair selects a slice (docs/GUIDE.md)
   32     = output stride; equal to the digest length packs them contiguously */
ub_hash_n(S, 4, 0, count, 0, 0, out, 32);
```

Four parameters vary between deployments: digest length, personalization, the shared
header, and counter width. The remainder are sequential-mode defaults.

## Build and test

Pick your platform, run the two commands, and see the "what a pass looks like"
block at the end of this section.

The table is the quick path, using whatever libsodium is already installed.
That is enough to prove conformance; for a *comparison figure* worth
publishing, use the reference oracle -- see the Ubuntu section below.

| platform | build | quick test |
|---|---|---|
| macOS | `make` | `make check SODIUM=$(brew --prefix libsodium)` |
| Ubuntu 24.04 | `make` | `make check SODIUM=/usr` |
| Ubuntu 18.04 | `make` | `make check SODIUM=/usr` |
| Windows 11 + WSL | `make` | `make check SODIUM=/usr` |
| Windows 11 native | `make CC=gcc EXE=.exe` | `make check CC=gcc EXE=.exe SODIUM=/mingw64` |

`SODIUM` points at libsodium, which `make check` and `make bench` use as an
independent reference. The library itself links nothing and needs only a C99
compiler.

**libsodium is a test oracle, not a dependency.** The library links nothing
and contains no libsodium code. Nothing in `src/` or `include/` is copied,
adapted, or derived from it, and no part of its documentation is a source for
this one. It is compiled into the test binaries only, so that every digest can
be checked against an independent implementation; a build of the library
itself neither needs nor references it. The optional `compat/ub_sodium.h`
maps libsodium's *function names* onto uniblake calls for callers migrating,
which is an adapter over this library's own code, not a use of theirs.

The implementation follows RFC 7693 and the BLAKE2 authors' reference package.
Where a constant is shared with other implementations -- the SIGMA schedule,
the IV, the rotation amounts -- it is shared because the specification fixes
it, not because it was taken from anyone.

**The reference version is libsodium 1.0.21.** That is what the consuming
project builds and links, on Linux and macOS alike, so conformance is measured
against the implementation actually being replaced rather than against
whatever a package manager happens to offer. The consuming build compiles it
from source with a pinned upstream tarball and SHA-256, `--enable-static
--disable-shared`; a distribution package is not the same artefact. Any other
version is a convenience, and a result from one should say so.

The rest of the validation, same on every platform:

```
make check-portable CC2=<second compiler>   # C99 and C11, warnings are failures
make check-wipe-modes SODIUM=<prefix>       # secret-wiping compiled in and out
make check-negative SODIUM=<prefix>         # proves the suites can fail
make check-backends SODIUM=<prefix>         # each alternative kernel, full oracle set
make check-sanitize SODIUM=<prefix>         # ASan + UBSan; leak checks are Linux-only
make bench SODIUM=<prefix>                  # ns/digest for this machine
make bench-neon SODIUM=<prefix>             # same, with the NEON kernel (aarch64)
make check-avx2 SODIUM=<prefix>             # AVX2 kernel conformance (x86-64)
make bench-avx2 SODIUM=<prefix>             # same, measured
make bench-threads THREADS=<n> SODIUM=<..>  # same, with the range split across threads
make probe                                  # CPU identity; report it with any figure
```

`make check-kat` and `make check-alias` are part of `make check` and need no
libsodium: the first runs the BLAKE2 authors' published vectors, the second
checks the `blake2.h` shim against the vendored reference. Run either alone on
a target with no libsodium.

### Setup and details, per platform

#### macOS

```
brew install libsodium gcc
make
make check SODIUM=$(brew --prefix libsodium)
make check-portable CC2=gcc-16
```

Apple's `gcc` is a clang shim, so pass a real one to `CC2` or the second
column tests nothing new.

#### Ubuntu 24.04 and 18.04

Quick check, using whatever libsodium the distribution provides:

```
sudo apt install build-essential libsodium-dev clang
make
make check SODIUM=/usr
make check-portable CC2=clang
make check-sanitize SODIUM=/usr
```

`/usr` is where the distribution package lands; a source build defaults to
`/usr/local`. On 18.04 the system GCC is older, which is the point of testing
there: it catches reliance on newer compiler behaviour.

**Which libsodium you link changes the comparison, and the build flags change
it more than the version does.** Conformance is unaffected -- a digest is a
digest, and every version agrees -- but the *comparison* rows in `make bench`
are against whatever was linked. Two builds of the same algorithm measured
282 ns and 193 ns here, a 46% spread, and the cause was `-O1` against `-O2`,
not the version: nothing between 1.0.21 and 1.0.22 changes BLAKE2b.

So state the oracle's version **and** how it was built with any published
figure. Ubuntu 24.04 packages 1.0.18 and 18.04 packages 1.0.16, both at the
distribution's own flags. To measure against the reference version:

```
curl -LO https://github.com/jedisct1/libsodium/releases/download/1.0.21-RELEASE/libsodium-1.0.21.tar.gz
echo "9e4285c7a419e82dedb0be63a72eea357d6943bc3e28e6735bf600dd4883feaf  libsodium-1.0.21.tar.gz" | sha256sum -c
tar xf libsodium-1.0.21.tar.gz && cd libsodium-1.0.21
./configure --prefix=$HOME/opt/libsodium-1.0.21 --enable-static --disable-shared
make -j"$(nproc)" && make install && cd ..

make check SODIUM=$HOME/opt/libsodium-1.0.21
make bench  SODIUM=$HOME/opt/libsodium-1.0.21
```

That `./configure` sets **`-O3`** plus its own hardening flags; libsodium chooses
them, not this project, and it is what an ordinary source build gives. To
reproduce a different build instead, pass `CFLAGS` explicitly -- for example
`CFLAGS="-pipe -O1"` to match a consumer whose dependency system standardises
on `-O1`.

Measured on one machine, libsodium 1.0.21 from identical source, leaf shape:

| build | ns/digest |
|---|--:|
| `CFLAGS="-pipe -O1"` | 288 |
| `CFLAGS="-pipe -O2"` | 178 |
| configure default (`-O3` + hardening) | 175 |

**A published figure must state the oracle's version and its build flags.**
The version alone is ambiguous by 64% on this shape, and the optimisation
level accounts for nearly all of it -- there is no BLAKE2b change between
1.0.21 and 1.0.22.

#### Linux performance and kernel analysis

The same targets as on macOS, all portable C:

```
make bench-phases                                   # where a leaf digest's time goes
make bench-compare SODIUM=$HOME/opt/libsodium-1.0.21
make bench-isa                                      # per-instruction latency
make kernel-stats                                   # what the compiled kernel does
make kernel-stats-arch                              # how to read those counts on x86
```

`kernel-stats` currently parses aarch64 only; `kernel-stats-arch` prints what
has to change for x86-64 and why the counts mean different things there. The
benchmarks need no porting.

On x86-64 also run the vector kernel, which cannot be measured on an
Apple-silicon host:

```
make check-avx2 SODIUM=$HOME/opt/libsodium-1.0.21
make bench-avx2 SODIUM=$HOME/opt/libsodium-1.0.21
```

For the Rust crate on Ubuntu 24.04, whose `rustc` is 1.75 -- the toolchain
this project targets:

```
sudo apt install rustc cargo
cd ../uniblake-rs && cargo test && cargo run --release --example compare
```

`docs/INTERNALS.md`, *Running this work on x86-64*, lists what is expected to
measure differently there and why.

#### Windows 11 with WSL

WSL is Linux, so the Ubuntu commands apply unchanged. Keep the checkout on the
WSL filesystem (`~/uniblake`), not under `/mnt/c` -- the cross-boundary build
is slow and file-mode handling differs.

```
sudo apt install build-essential libsodium-dev
make
make check SODIUM=/usr
```

To stay in PowerShell, prefix with `wsl -e`:

```powershell
wsl -e make
wsl -e make check SODIUM=/usr
```

#### Windows 11 native

The Makefile needs a POSIX shell and `ar`, which PowerShell and `cmd` do not
provide. Install MSYS2 and drive it from PowerShell, keeping one terminal:

```powershell
winget install MSYS2.MSYS2
$msys = "C:\msys64\usr\bin\bash.exe"
& $msys -lc "pacman -S --noconfirm mingw-w64-x86_64-gcc make mingw-w64-x86_64-libsodium"
& $msys -lc "cd /c/path/to/uniblake && make CC=gcc EXE=.exe"
& $msys -lc "cd /c/path/to/uniblake && make check CC=gcc EXE=.exe SODIUM=/mingw64"
```

`-lc` runs a login shell so the MinGW toolchain is on `PATH`. `EXE=.exe` is
the only Windows-specific setting; `build/` works unchanged.

Or from an MSYS2 MinGW64 shell directly, without the wrapper:

```
make CC=gcc EXE=.exe
make check CC=gcc EXE=.exe SODIUM=/mingw64
```

MSVC cannot build this Makefile, so an MSVC check means compiling `src/*.c`
into your own project or a direct `cl` invocation. The sources are MSVC-clean:
the one compiler-specific construct is guarded. Run it where MSVC is a target:
it and MinGW diverge often enough that one passing does not imply the other.

#### Linux to Windows, cross-compiled

Produces Windows PE binaries on a Linux or macOS host via MinGW-w64. The library
links nothing, so it cross-compiles without a target sysroot. Give the cross build its
own `BUILD` so it sits beside the native one; see "Two toolchains, one checkout".

```
sudo apt install gcc-mingw-w64-x86-64          # macOS: brew install mingw-w64

make BUILD=build-win CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar EXE=.exe
make check-build BUILD=build-win CC=x86_64-w64-mingw32-gcc EXE=.exe
```

`check-build` compiles the suite without running it. `check-alias` builds and
runs, which a cross build cannot do: the exit code would report a failure that
is only the host refusing to execute a foreign binary.

Pass the variables as `make` arguments on each line; collecting them in a shell
variable does not work.

The first command yields `build-win/libuniblake.a` containing COFF x86-64 objects. The
second builds `build-win/ub_alias.exe`, a PE32+ console executable. `check-alias` is the
suite to cross-build because its oracle is vendored; the other suites link libsodium and
need one built for the same target.

On Ubuntu, Wine runs the result in place, which verifies digests on the
Windows target without a Windows machine:

```
sudo apt install wine64
make check-wine BUILD=build-win
blake2-alias: checks=1218 fails=0 -> PASS
```

`check-wine` sets `WINEDEBUG=-all`, which silences Wine's complaints about
having no desktop -- a console program does not need one. Without it the run
still passes; those messages are on stderr and the exit code is 0.

Compilation alone establishes only that the source is portable to the target.
The Wine run above establishes digest correctness there. Copying `build-win/`
to a Windows machine remains the stronger check, since Wine is an
implementation of the API rather than the platform itself.

Verified from macOS/arm64 with MinGW-w64 GCC 16.2.0: the library compiles warning-free
under `-Wall -Wextra -Wpedantic`, and both no-oracle suites link into PE32+ binaries.

The MSYS2 commands above have not been exercised here; everything else in this section
has been run.

#### Two toolchains, one checkout

Everything a build produces -- objects, the archive, test binaries -- goes
under `BUILD`. Nothing is written beside the sources, so a native build and a
cross build coexist in one clone; give each its own `BUILD` and they never
touch. One repository, one checkout, two output trees:

```
make                                          # -> build/
make check SODIUM=/usr                        # native suites

make BUILD=build-win CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar EXE=.exe
```

Pass the variables on each command line rather than collecting them in a shell
variable; `make` needs them as its own arguments.

Neither rebuilds nor invalidates the other, and `make` will not reuse the
wrong toolchain's objects. `make clean` removes only the tree named by
`BUILD`, so clean each separately:

```
make clean                       # removes build/
make clean BUILD=build-win       # removes build-win/
```

`build/` and `build-*/` are gitignored. Use a second clone only if you want
different *sources* -- for the same sources, a second `BUILD` is enough.

#### Variables

| variable | default | set it when |
|---|---|---|
| `SODIUM` | `/usr/local` | libsodium is elsewhere |
| `EXE` | empty | executables need a suffix (native Windows) |
| `CC`, `AR` | `cc`, `ar` | cross-compiling, or selecting a second compiler |
| `BUILD` | `build` | building with a second toolchain from one checkout |

The Makefile recipes are POSIX shell -- no bash extensions -- so they run
under whatever `/bin/sh` a platform provides, including `dash` and MSYS2.
Nothing needs bash installed.

#### What a pass looks like

```
blake2-alias: checks=... fails=0 -> PASS
core:         checks=... fails=0 -> PASS
prefix:       checks=... fails=0 -> PASS
api:          checks=... fails=0 -> PASS
compat:       checks=... fails=0 -> PASS
```

Any `FAIL`, or a nonzero exit, means stop: a wrong digest is not a platform
quirk. Report the machine, compiler and version with the failure.

## Documentation

Each document owns a question and carries that topic whole.

| document | owns | read it when |
|---|---|---|
| **README** (this file) | what the library is, its scope, how to build it, where everything lives | first |
| [docs/UniBlake.md](docs/UniBlake.md) | *why* — the design argument: what BLAKE2b specifies, what it does not, and what this library adds | deciding whether to use it |
| [docs/GUIDE.md](docs/GUIDE.md) | *how to call it* — recipes, interface reference, sizing rules, adapters, the C++ wrapper | writing calling code |
| [docs/INTERNALS.md](docs/INTERNALS.md) | *how it works* — state, the compression kernel, porting, measurements | replacing a kernel or porting |
| [docs/INTEGRATING.md](docs/INTEGRATING.md) | *how to adopt it* — swapping the hash in an existing codebase, in a validated order | migrating a consumer |
| [docs/UniBench.md](docs/UniBench.md) | *the measurement format* — columns, metric vocabulary, platform ids, commands | recording or reading a benchmark |
| [TODO.md](TODO.md) | *what is in flight* — backlog, active work, and every measured figure with its conditions | checking status, or looking up a number |

### What does not go in these documents

Four kinds of material are absent because their lifecycle is not this
library's: they change on a different cadence, and a copy here goes stale
without anything failing to signal it.

- **History and provenance.** Contributor names, commit dates, upstream
  maintenance status. License and pin lines on vendored files are the
  exception.
- **Rejected and parked work, and all figures.** These live in `TODO.md`,
  which is pruned every pass. A measurement is true of one machine, one
  compiler and one tree state; keeping figures out of the permanent documents
  is what stops them rotting silently. `docs/INTERNALS.md` keeps mechanisms
  and the short list that stops a reader repeating a measured mistake.
- **Other algorithms.** Named once in `docs/UniBlake.md` as a scope boundary,
  never compared.
- **Named downstream projects.** A consumer's domain is its own; examples
  here are generic.
- **Alternatives and selection criteria.** These documents describe what the
  library uses, with its strengths and limitations. Which implementations were
  considered, and on what grounds, is program-level material.

A number that would go stale needs a command that regenerates it. Prefer
"run `make check`" over quoting a check count.

## Drop-in use

Header-only adapters let existing call sites work unchanged:

- `compat/ub_sodium.h` — libsodium's `crypto_generichash_blake2b_*`
- `compat/ub_blake2.h` — the BLAKE2 author reference `blake2.h`
- `compat/ub_rfc.h` — the RFC 7693 sample-code API
- `compat/uniblake.hpp` — optional C++11 wrapper (no Boost)

See [docs/GUIDE.md](docs/GUIDE.md#adapters).

## Layout

```
include/uniblake/   public headers
src/                implementation
compat/             adapter headers (compat/ref: derived oracle)
backends/           alternative kernels (backends/vendor: donor macros)
tests/  bench/      conformance, measurement (tests/vendor: published vectors)
probe/              CPU identity and ISA flags (make probe)
docs/
```

`include/` and `src/` are self-contained: vendor those two directories and
nothing else.

### Original, derived, and vendored

Every source file is one of three things, and which one decides its copyright
header.

**Original.** Written from scratch, using this project's file structure,
naming conventions, layout and comment style. Including a vendored header is
allowed and does not change this: what makes code original is that we wrote
it, not that it references nothing. Carries

```c
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 UniBlake Developers */
```

**Derived.** Modifies existing third-party code, leaving sections intact and
following the original's naming and layout. Retains the prior copyright
statement, with ours added alongside it -- never replacing it.

**Vendored.** Copied unmodified into a `vendor/` directory, keeping its
upstream name, layout and comments, with a note giving the upstream link and
the commit taken. Untouched: a local edit makes it derived.

Present now: `backends/vendor/libsodium/`, x86 SIMD compress macros copied
unmodified, is vendored. `compat/ref/` (the BLAKE2 author reference with
identifiers renamed) and `tests/vendor/` (the authors' published vectors
re-expressed as C arrays) are derived. All three keep the upstream copyright
alone.

## Licensing

MIT, Copyright (c) 2026 UniBlake Developers. Original files carry a one-line
notice; a standalone `LICENSE` file is under consideration.

Third-party and derived code retains its own terms and copyright, stated in
the README of the directory it sits in. `compat/ref/` is the only such code
currently present.

## References

- [RFC 7693](https://www.rfc-editor.org/rfc/rfc7693) — the BLAKE2 specification
- [BLAKE2 reference implementation](https://github.com/BLAKE2/BLAKE2) — `blake2.h`, the API this one follows
- [libsodium](https://libsodium.org) — used as the conformance oracle, and adapted by `compat/ub_sodium.h`

## Size and coverage

The only place these numbers appear. They change; `make check` and `wc` are
the authorities.

| | |
|---|--:|
| library source (`include/` + `src/`) | 825 lines |
| conformance checks in `make check` | 49,126 |
| suites | 6, plus a negative suite that must fail |
| build dependencies | none |
| test dependencies | libsodium, for four of the six suites |

### Memory

The library allocates nothing. A caller needs one state plus its own buffers.

| | |
|---|--:|
| `ub_state` | 232 bytes, 8-byte aligned; query with `ub_state_size()` |
| static constants | 256 bytes |
| compiled code | 8 KB |
| each conformance suite, peak RSS | 2 MB |
| `make bench`, peak RSS | 26 MB |

Measured on Apple M4 Pro, macOS 26.3, arm64, Apple clang 21, dynamically
linked. An empty C program is 1 MB of that 2 MB, so the suites cost little
beyond process overhead; a static build on another platform will differ. The
benchmark figure is one 25 MB output buffer, 400,000 digests at a 64-byte
stride, and scales with `N` in `bench/bench_prefix.c`; it also links libsodium
for comparison. Reproduce with `/usr/bin/time -l`.

The negative suite links a compression function with one round removed and
requires every oracle comparison to reject it: a suite that cannot fail proves
nothing.
