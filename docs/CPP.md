# C++ and the two reference lineages

## The optional C++ wrapper

`compat/uniblake.hpp` — header-only, C++11, no Boost. Nothing in `include/` or
`src/` requires it; the C interface works unchanged from C++.

It exists because three things are awkward at a C++ call site:

- allocating aligned storage by hand,
- checking a return code on every call when the surrounding code uses
  exceptions,
- proving to a reader that a shared prefix state really is not modified.

```cpp
#include "uniblake.hpp"

uniblake::Params P(48);
P.personal("my-app-v1");

uniblake::Prefix pre(P, header.data(), header.size(), /*max_tail=*/4);

auto digests = pre.hash_n(0, count);          // count digests, packed
auto field   = pre.hash_n_field(0, count, 24, 24);  // one 24-byte field of each
```

`Prefix` absorbs the leading bytes once in its constructor and exposes only
const methods, so sharing one across threads is a property of the type. The
constructor also runs the geometry check and throws if the sizes cannot give
one-compression digests — a mistake that is otherwise found at the first
hashing call.

`State` owns aligned storage and is copyable, with copy meaning what `ub_copy`
means: the computation continues independently on both sides. That makes the
long-tail case ordinary C++:

```cpp
auto st = pre.resume();      // a copy of the prefix state
st.update(part1); st.update(part2);
auto digest = st.final(48);
```

Errors are exceptions at construction and in the value-returning helpers,
because that is where a C++ caller cannot inspect a return code. `uniblake::
Error` carries the `ub_status`, so a caller that wants the code still has it.

Verified against libsodium: batch digests, field slices, resumed streaming,
geometry rejection, and copy independence.

## Fitting a Zcash-shaped codebase

Zcash-lineage code hashes through a typedef and a pair of free functions, uses
templates over `<N, K>`-style parameters, `BOOST_STATIC_ASSERT` for
compile-time invariants, and `std::invalid_argument` for bad parameters.

Three adaptations follow.

**Keep the typedef, change what it names.** A codebase that says
`typedef crypto_generichash_blake2b_state eh_HashState;` can say
`typedef ub_state eh_HashState;` and keep every signature. The only source
change the C interface forces is that assigning one state to another becomes
`ub_copy` — `ub_state` is opaque and has no assignable value type. With the
C++ wrapper, `typedef uniblake::State eh_HashState;` restores assignment.

**Parameters that are compile-time constants should stay compile-time.** Where
a digest length or personalization is derived from template parameters, a thin
class template keeps that:

```cpp
template <unsigned N, unsigned K>
struct PoWParams {
    static_assert(N % 8 == 0, "N must be a whole number of bytes");
    static uniblake::Params make() {
        uniblake::Params P((512 / N) * N / 8);
        char tag[16] = {};
        std::memcpy(tag, "MyPoW", 5);
        std::memcpy(tag + 8, &leN, 4);
        std::memcpy(tag + 12, &leK, 4);
        return P.personal(tag, 16);
    }
};
```

`BOOST_STATIC_ASSERT` and `static_assert` both work; the library imposes
neither.

**Return codes vs exceptions is a call-site choice.** The C interface returns
codes and never throws, so it suits a hot loop where a failure is impossible
by construction. The wrapper throws, which suits initialization. A codebase
can use both — `Prefix` for setup, and the raw `ub_hash_n` inside a solver
loop where an exception would be wrong.

**No Boost dependency either way.** The wrapper uses `<memory>`, `<vector>`,
`<string>`, `<stdexcept>`. A codebase already carrying Boost may prefer
`boost::optional` returns over exceptions; that is a fifty-line variant over
the same C calls, not a different library.

## Author reference vs RFC 7693 sample code

Two C implementations are commonly called "the reference", and they differ in
ways that matter when writing an adapter.

|  | RFC 7693 Appendix A | Author reference (`BLAKE2/BLAKE2`) |
|---|---|---|
| state type | `blake2b_ctx` | `blake2b_state` |
| init | `blake2b_init(ctx, outlen, key, keylen)` | `blake2b_init`, `_init_key`, `_init_param` |
| parameter block | not exposed | `blake2b_param`, via `_init_param` |
| salt, personalization | **absent** | present |
| update | returns `void` | returns `int` |
| final | `blake2b_final(ctx, out)` — no capacity | `blake2b_final(S, out, outlen)` |
| SIMD | none | SSE2/SSSE3/SSE4.1/AVX/AVX2/XOP variants |
| tree hashing | none | `blake2bp`, `blake2sp` |
| purpose | show the algorithm is implementable in ~150 lines | production code |

The RFC sample is normative for the *algorithm* and deliberately minimal: it
omits the parameter block entirely, so it cannot express salt or
personalization and cannot produce the domain-separated hashes protocols rely
on. Its `final` takes no output capacity, so an adapter must trust the
caller's buffer.

The author reference is the API almost every library follows — libsodium's
`crypto_generichash_blake2b_*` and this library both track its shape. When
"the reference implementation" appears in an argument about API design, it
means this one.

uniblake follows the author reference for naming and argument order, and cites
RFC 7693 for the algorithm and its section numbers. `compat/ub_rfc.h` adapts
the RFC sample's four-function shape for callers that have it hard-coded, with
the capacity caveat above.
