# uniblake

BLAKE2b in portable C, with a fast path for hashing many messages that begin
with the same bytes.

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

## Build

```
make            # libuniblake.a
make check      # conformance suites
make bench      # measurements
```

`make check` and `make bench` need libsodium, which they use as an independent
reference; the library itself links nothing. Point at a non-default install
with `make check SODIUM=/opt/homebrew`.

## Documentation

| | |
|---|---|
| [docs/UniBlake.md](docs/UniBlake.md) | what it is and why the repeated case needs a guarantee — start here |
| [docs/GUIDE.md](docs/GUIDE.md) | for callers: recipes, interface reference, adapters |
| [docs/INTERNALS.md](docs/INTERNALS.md) | for implementers: state, compression backends, porting, measurements |
| [docs/CPP.md](docs/CPP.md) | the C++ wrapper, and the two BLAKE2 reference lineages |
| [docs/INTEGRATING.md](docs/INTEGRATING.md) | swapping the hash in an existing codebase: validation and measurement order |

## Drop-in use

Header-only adapters let existing call sites work unchanged:

- `compat/ub_sodium.h` — libsodium's `crypto_generichash_blake2b_*`
- `compat/ub_rfc.h` — the RFC 7693 sample-code API
- `compat/uniblake.hpp` — optional C++11 wrapper (no Boost)

See [docs/GUIDE.md](docs/GUIDE.md#adapters).

## Layout

```
include/uniblake/   public headers
src/                implementation
compat/             adapter headers
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

## License

See LICENSE.
