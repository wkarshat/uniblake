# uniblake

BLAKE2b in portable C, for callers who need more than a one-shot hash:

- **Eager absorption, guaranteed.** Whole blocks are compressed as soon as
  more input follows, so a shared prefix is absorbed once. The standard API
  cannot express this and gives no diagnostic when an implementation does the
  opposite.
- **Controlled access to the internals.** The parameter block, the prefix and
  batch layer, a replaceable compression kernel, runtime-reported state size
  and alignment. Every entry point validates and returns a code; diagnosis is
  opt-in and costs nothing when unused.
- **Portable by construction.** C99, endian-neutral, no allocation, no POSIX,
  no threads, no floating point. Conformance is checked against an independent
  oracle, not asserted.

BLAKE2b only. Other algorithms are out of scope, not pending.

## What it does

Hashing one message is the usual three calls. What uniblake adds is the
repeated case:

    H(header || 0),  H(header || 1),  H(header || 2),  ...

Absorb `header` once, then produce each digest for the cost of a single
compression. Doing this through a plain streaming API works only if the
implementation compresses the shared bytes eagerly — which is not something
the BLAKE2 API lets a caller ask about. uniblake guarantees it and will tell
you when your sizes do not permit it.

## Quick start

```c
#include "uniblake/uniblake.h"

/* 32 = digest length in bytes. Anything from 1 to 64; 32 and 64 are the
   conventional choices. */
ub_state *S = aligned_alloc(ub_state_align(), ub_state_size());
ub_init(S, 32);
ub_update(S, msg, msglen);
ub_final(S, out, sizeof out);          /* out must hold 32 bytes */
```

Repeated-prefix hashing:

```c
#include "uniblake/prefix.h"

ub_param P;
ub_param_init(&P, 32);                 /* digest length, as above */
memcpy(P.personal, "my-app-v1\0\0\0\0\0\0\0", 16);   /* optional 16-byte
                                          domain separator; omit if unused */
ub_init_param(S, &P);
ub_update(S, header, headerlen);       /* the shared bytes, absorbed once */

/* count digests: H(header || 0), H(header || 1), ...
   4      = counter width in bytes (4 or 8)
   0      = first counter value
   0, 0   = write the whole digest (see the guide for partial output)
   stride = spacing between digests in out; 32 packs them back to back */
ub_hash_n(S, 4, 0, count, 0, 0, out, 32);
```

The values that vary between uses are the digest length, the personalization
string, the shared header, and the counter width. The rest are defaults.

## Build and test

Pick your platform, run the two commands, and see the "what a pass looks like"
block at the end of this section.

| platform | build | test |
|---|---|---|
| macOS | `make` | `make check SODIUM=$(brew --prefix libsodium)` |
| Ubuntu 24.04 | `make` | `make check SODIUM=/usr` |
| Ubuntu 18.04 | `make` | `make check SODIUM=/usr` |
| Windows 11 + WSL | `make` | `make check SODIUM=/usr` |
| Windows 11 native | `make CC=gcc EXE=.exe` | `make check CC=gcc EXE=.exe SODIUM=/mingw64` |

`SODIUM` points at libsodium, which `make check` and `make bench` use as an
independent reference. The library itself links nothing and needs only a C99
compiler. Prefer the libsodium the consuming project already uses, so
conformance is measured against the implementation being replaced.

The rest of the validation, same on every platform:

```
make check-portable CC2=<second compiler>   # C99 and C11, warnings are failures
make check-wipe-modes SODIUM=<prefix>       # secret-wiping compiled in and out
make check-negative SODIUM=<prefix>         # proves the suites can fail
make check-sanitize SODIUM=<prefix>         # ASan + UBSan (Linux and macOS)
make bench SODIUM=<prefix>                  # ns/digest for this machine
```

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
the one compiler-specific construct is guarded. Worth doing where MSVC is a
target -- it and MinGW disagree often enough that passing one says little
about the other.

#### Linux to Windows, cross-compiled

Produces Windows binaries on a Linux host. The library cross-compiles cleanly
because it links nothing; the suites need a libsodium built for the same
target, so without one, cross-check what does not need it and run the rest
natively.

```
sudo apt install gcc-mingw-w64-x86-64
make CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar
make check-alias CC=x86_64-w64-mingw32-gcc EXE=.exe   # needs Wine to run
```

Cross-*compiling* proves the code reaches the target; it does not prove the
digests are right there. Only running the suites on the target does that.

The MSYS2 and cross-compile commands above are the shape of the work, not a
transcript -- they have not been exercised here, on a machine with no MinGW
toolchain. Everything else in this section has been run.

#### Variables

Test and bench binaries go to `build/` in the tree, gitignored and removed by
`make clean`.

| variable | default | set it when |
|---|---|---|
| `SODIUM` | `/usr/local` | libsodium is elsewhere |
| `EXE` | empty | executables need a suffix (native Windows) |
| `CC`, `AR` | `cc`, `ar` | cross-compiling, or selecting a second compiler |
| `BUILD` | `build` | the output should go somewhere else |

The Makefile recipes are POSIX shell -- no bash extensions -- so they run
under whatever `/bin/sh` a platform provides, including `dash` and MSYS2.
Nothing needs bash installed.

#### What a pass looks like

```
blake2-alias: checks=1218 fails=0 -> PASS
core: checks=630 fails=0 -> PASS
prefix: checks=45531 fails=0 -> PASS
api: checks=155 fails=0 -> PASS
compat: checks=56 fails=0 -> PASS
```

Any `FAIL`, or a nonzero exit, means stop: a wrong digest is not a platform
quirk. Report the machine, compiler and version with the failure.

## Documentation

Each document owns a question and carries that topic whole.

| document | owns | read it when |
|---|---|---|
| **README** (this file) | what the library is, its scope, how to build it, where everything lives | first |
| [docs/UniBlake.md](docs/UniBlake.md) | *why* — the design argument: what BLAKE2b guarantees, what it does not, and what this library adds | deciding whether to use it |
| [docs/GUIDE.md](docs/GUIDE.md) | *how to call it* — recipes, interface reference, sizing rules, adapters, the C++ wrapper | writing calling code |
| [docs/INTERNALS.md](docs/INTERNALS.md) | *how it works* — state, the compression kernel, porting, measurements | replacing a kernel or porting |
| [docs/INTEGRATING.md](docs/INTEGRATING.md) | *how to adopt it* — swapping the hash in an existing codebase, in a validated order | migrating a consumer |

### What does not go in these documents

Four kinds of material are absent because their lifecycle is not this
library's: they change on a different cadence, and a copy here goes stale
without anything failing to signal it.

- **History and provenance.** Contributor names, commit dates, upstream
  maintenance status. License and pin lines on vendored files are the
  exception.
- **Rejected and parked work.** `docs/INTERNALS.md` keeps only the short list
  that stops a reader repeating a measured mistake.
- **Other algorithms.** Named once in `docs/UniBlake.md` as a scope boundary,
  never compared.
- **Named downstream projects.** A consumer's domain is its own; examples
  here are generic.

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
compat/             adapter headers (compat/ref: vendored oracle)
backends/           alternative NEON and threaded implementations
tests/  bench/      conformance, measurement
docs/
```

`include/` and `src/` are self-contained: vendor those two directories and
nothing else.

## References

- [RFC 7693](https://www.rfc-editor.org/rfc/rfc7693) — the BLAKE2 specification
- [BLAKE2 reference implementation](https://github.com/BLAKE2/BLAKE2) — `blake2.h`, the API this one follows
- [libsodium](https://libsodium.org) — used as the conformance oracle, and adapted by `compat/ub_sodium.h`

## Size and coverage

The only place these numbers appear. They change; `make check` and `wc` are
the authorities.

| | |
|---|--:|
| library source (`include/` + `src/`) | 795 lines |
| conformance checks in `make check` | 47,590 |
| suites | 5, plus a negative suite that must fail |
| build dependencies | none |
| test dependencies | libsodium, for four of the five suites |

The negative suite links a compression function with one round removed and
requires every oracle comparison to reject it: a suite that cannot fail proves
nothing.

## License

See LICENSE.
