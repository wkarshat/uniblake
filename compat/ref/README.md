# Vendored BLAKE2 author reference, renamed

The oracle for `compat/ub_blake2.h`. Source: the reference C implementation
at github.com/BLAKE2/BLAKE2, copyright 2012 Samuel Neves, under CC0 /
OpenSSL / Apache-2.0 at the user's option -- see the header of `ref_blake2.h`.

Pinned at upstream `ed1974e`. Re-copy from a clone and update this line to
change it.

Copied from `Requihash/BLAKE/vendor/blake2` (`blake2.h`, `blake2b-ref.c`,
`blake2-impl.h`), with every public symbol and constant mechanically
prefixed `ref_` / `REF_`:

    blake2b_state -> ref_blake2b_state      BLAKE2B_OUTBYTES -> REF_BLAKE2B_OUTBYTES
    blake2b_init  -> ref_blake2b_init       (and so on)

The rename exists so the reference and the aliasing shim -- which defines the
*unprefixed* names -- can both be called from one translation unit. That is
the same technique `test_compat.c` uses against real libsodium. The algorithm
is untouched; only identifiers changed.

Used by `compat/test_blake2_alias.c` (`make check-alias`). Not part of the
library: nothing in `include/` or `src/` refers to it.
