# Variant registry

Machine-checkable list of code variants that have been built and measured.
`tools/run_variant.sh` builds and measures any row against main.

| id | branch | files | measured | status |
|---|---|---|---|---|
| drop-f | (merged) | src/internal.h src/compress.c src/core.c src/prefix.c | leaf -0.43 ns [-0.86,-0.05], copy -0.13 [-0.14,-0.10], 24 pairs | **integrated 2026-09-01** |
| nomut-final | — | src/core.c src/compress.c | leaf +2.12 ns [+1.76,+2.25], 24 pairs | rejected; mechanism in INTERNALS |
| last8-explicit | — | src/compress.c | byte-identical asm on aarch64 | rejected on aarch64; **open on x86/AVX2** |
| rolled-g | — | src/compress.c | +5.4 ns | rejected |
