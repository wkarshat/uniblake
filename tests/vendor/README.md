# Third-party test data

`kat_blake2b.h` holds the BLAKE2 authors' published known-answer vectors for
BLAKE2b, extracted from `testvectors/blake2-kat.json` in the reference package
at [github.com/BLAKE2/BLAKE2](https://github.com/BLAKE2/BLAKE2), upstream pin
`ed1974e`. Copyright 2012 Samuel Neves; CC0 1.0 / OpenSSL / Apache-2.0, at the
user's option.

Derived, not vendored: the upstream file is JSON covering all four BLAKE2
variants, and this is the BLAKE2b subset re-expressed as C arrays. The digests
are unaltered. Regenerate with

```
python3 tests/gen_kat.py <clone>/testvectors/blake2-kat.json > tests/vendor/kat_blake2b.h
```

which is byte-reproducible from the same input. `tests/gen_kat.py` is ours;
the data is not.

Used by `tests/test_kat.c` (`make check-kat`), the only conformance suite that
needs no second implementation and therefore the one that runs on a target
without libsodium.
