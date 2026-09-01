# Vendored BLAKE2 NEON macros

Copied unmodified from the BLAKE2 reference package
([github.com/BLAKE2/BLAKE2](https://github.com/BLAKE2/BLAKE2)), `neon/`
subdirectory, pin `ed1974e`. CC0 1.0 / OpenSSL / Apache-2.0 at the user's
option; see the header of each file.

Used by `backends/compress_neon_unrolled.c`. `blake2b-round.h` defines `G1`,
`G2`, `DIAGONALIZE`, `UNDIAGONALIZE` and `ROUND(r)`; `blake2b-load-neon.h`
defines the 48 `LOAD_MSG_r_n` macros that resolve the sigma permutation at
compile time. The macros read `m0`..`m7`, `row1l`..`row4h`, `t0`, `t1`, `b0`
and `b1` from the enclosing scope; the kernel declares them.

Not part of the library: nothing in `include/` or `src/` refers to this, and
`backends/` is not built by `make all`.
