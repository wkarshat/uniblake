# Vendored BLAKE2 NEON macros

Round and message-load macros used by `backends/compress_neon_unrolled.c`.

Source: the BLAKE2 reference package at github.com/BLAKE2/BLAKE2, `neon/`
subdirectory, files `blake2b-round.h` and `blake2b-load-neon.h`, copied
unmodified. Copyright 2012 Samuel Neves, under CC0 1.0 / OpenSSL License /
Apache-2.0 at the user's option; see the header of each file. The NEON code in
that package was contributed by Leigh Brown.

Pinned at upstream `ed1974e`. Re-copy from a clone and update this line to
change it.

`blake2b-round.h` defines `G1`, `G2`, `DIAGONALIZE`, `UNDIAGONALIZE` and
`ROUND(r)`; `blake2b-load-neon.h` defines the 48 `LOAD_MSG_r_n` macros that
resolve the sigma permutation at compile time. The macros read `m0`..`m7`,
`row1l`..`row4h`, `t0`, `t1`, `b0` and `b1` from the enclosing scope; the
kernel declares them.

Not part of the library: nothing in `include/` or `src/` refers to this, and
`backends/` is not built by `make all`.
