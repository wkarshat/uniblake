# Third-party kernel sources

Kernel sources copied from another project, unmodified, each subdirectory with
a note giving its upstream link and the commit taken.

| directory | contents |
|---|---|
| `libsodium/` | x86 SIMD compress paths; AVX2 built, SSE4.1 and SSSE3 vendored unbuilt |

Files here keep their upstream copyright and license. Files elsewhere in this
repository are Copyright (c) 2026 UniBlake Developers.

The BLAKE2 reference package's NEON macros were vendored here while an
unrolled kernel was evaluated; that kernel measured slower than the one in
`backends/` and was removed. Recover both from the `neon-both-kernels` tag.
