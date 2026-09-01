# Vendored libsodium x86 SIMD

Copied unmodified from
[libsodium](https://github.com/jedisct1/libsodium), path
`src/libsodium/crypto_generichash/blake2b/ref/`, pin `9608bca8`. ISC.

Three independent paths, each a compress translation unit, a header of round
macros, and one message-load header:

| path | files | built |
|---|---|---|
| AVX2 | `blake2b-compress-avx2.{c,h}`, `blake2b-load-avx2.h` | by `backends/compress_avx2.c` |
| SSE4.1 | `blake2b-compress-sse41.{c,h}`, `blake2b-load-sse41.h` | no |
| SSSE3 | `blake2b-compress-ssse3.{c,h}`, `blake2b-load-sse2.h` | no |

All nine are vendored so the set comes from one commit; only AVX2 is built.
The other two paths wait on a consumer that needs pre-AVX2 x86, or a
measurement showing a narrower path wins. An unbuilt file is not compiled by
any target and costs nothing but disk.

Only `blake2b-compress-avx2.h` is included by our code, and it pulls
`blake2b-load-avx2.h`. The `.c` translation units are libsodium's own entry
points and are not compiled here: `backends/compress_avx2.c` supplies the
`ub_compress` interface instead, using the same macros.
