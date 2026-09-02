# uniblake. The library needs only a C11 compiler; libsodium is required
# solely by the check/bench targets, where it is an independent oracle.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
# _POSIX_C_SOURCE: the harnesses need clock_gettime, and posix_memalign under
# -std=c99 where ub_alloc.h falls back from C11 aligned_alloc. glibc declares
# both only at >= 200112L. On the command line, not in a header: it has to
# precede the first system header, and ub_alloc.h is included after <stdlib.h>.
INC      = -Iinclude -Isrc -Itests -D_POSIX_C_SOURCE=200112L   # tests/ub_alloc.h, used by every harness
SRC      = src/core.c src/compress.c src/const.c src/prefix.c

# libsodium is the conformance oracle for check/bench; the library links none.
# Point at whichever build you want to be checked against -- ideally the one
# the consuming project already uses, so conformance is measured against the
# implementation being replaced.
#   make check SODIUM=/usr                      # distro package
#   make check SODIUM=/path/to/project/depends/<triplet>
SODIUM  ?= /usr/local

# Where test and bench binaries are written. In-tree and gitignored, which is
# what the sibling projects do -- it needs no writable /tmp, so the same
# command works on Linux, WSL, MSYS2 and native Windows.
# EXE is the executable suffix: empty everywhere except native Windows.
BUILD   ?= build
EXE     ?=

# Objects and the archive live under BUILD too, not beside the sources, so a
# native build and a cross build can share one checkout without overwriting
# each other or silently reusing the other's objects. Give each its own BUILD.
OBJ      = $(SRC:src/%.c=$(BUILD)/%.o)
LIB      = $(BUILD)/libuniblake.a

SODINC   = -I$(SODIUM)/include
SODLIB   = -L$(SODIUM)/lib -lsodium

# `all` must stay the default goal: make takes the FIRST target in the file,
# and the library itself needs no libsodium. Declared before sodium-check so
# inserting rules above `all` cannot silently make a bare `make` need the
# oracle.
.DEFAULT_GOAL := all

# Fail with a useful message rather than a missing-header error.
sodium-check:
	@test -f "$(SODIUM)/include/sodium.h" || { \
	  echo "libsodium not found under $(SODIUM)"; \
	  echo "  no include/sodium.h there. Either the prefix is wrong, or it is"; \
	  echo "  a source build that has not been built yet -- building it is a"; \
	  echo "  separate step, see README, 'Ubuntu 24.04 and 18.04'."; \
	  echo "  Installed prefix:  make $@ SODIUM=/usr        (or /usr/local)"; \
	  echo "  No oracle needed:  make bench-phases, make bench-isa"; \
	  exit 1; }
	@printf 'oracle: libsodium %s at %s\n' \
	  "$$(sed -n 's/.*SODIUM_VERSION_STRING "\(.*\)".*/\1/p' $(SODIUM)/include/sodium/version.h)" \
	  "$(SODIUM)" 

# Created on demand; every target that writes a binary depends on it.
$(BUILD):
	@mkdir -p $(BUILD)

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

# -MMD -MP emits a .d file listing the headers each object actually included,
# so editing internal.h or a public header rebuilds what depends on it. Without
# this, a header change leaves stale objects -- and internal.h defines
# struct ub_state, so a stale object means a layout mismatch, not a warning.
# Makefile is a prerequisite so a CFLAGS or rule change rebuilds every object
# rather than leaving ones compiled with the old flags.
$(BUILD)/%.o: src/%.c Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

# The BLAKE2 author-reference alias shim, checked against the vendored
# reference (compat/ref). Needs no libsodium, so it joins the default `check`.
# Published BLAKE2b vectors: no oracle, so this runs where libsodium does not.
check-kat: | $(BUILD)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) tests/test_kat.c $(SRC) -o $(BUILD)/ub_kat$(EXE)
	$(BUILD)/ub_kat$(EXE)

check-alias: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -Icompat -Icompat/ref compat/test_blake2_alias.c \
	  $(SRC) compat/ref/ref_blake2b.c -o $(BUILD)/ub_alias$(EXE)
	$(BUILD)/ub_alias$(EXE)

check: sodium-check check-kat check-alias | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o $(BUILD)/ub_core$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o $(BUILD)/ub_prefix$(EXE)
	$(CC) $(CFLAGS) $(INC)           tests/test_api.c  $(SRC)           -o $(BUILD)/ub_api$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c $(SRC) $(SODLIB) -o $(BUILD)/ub_compat$(EXE)
	$(BUILD)/ub_core$(EXE) && $(BUILD)/ub_prefix$(EXE) && $(BUILD)/ub_api$(EXE) && $(BUILD)/ub_compat$(EXE)

bench: sodium-check | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c $(SRC) $(SODLIB) -o $(BUILD)/ub_bench$(EXE)
	$(BUILD)/ub_bench$(EXE)

clean:
	rm -f src/*.o libuniblake.a
	rm -rf $(BUILD)

.PHONY: all check bench clean sodium-check check-alias check-kat

# CPU identity and ISA flags. Not part of the library -- the core has no
# dispatch -- but a measurement should be reported with the machine it came
# from, and a future selector needs core identity, not just ISA flags.
probe:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iprobe probe/ub_probe_main.c probe/ub_probe.c -o $(BUILD)/ub_probe$(EXE)
	@$(BUILD)/ub_probe$(EXE)

.PHONY: probe

# Prototype backends (see backends/README.md). Not part of `all`.
# Per-instruction latency for the operations G needs. Explains a vector
# kernel's ceiling in terms of the instruction set rather than the code.
bench-isa: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) bench/bench_isa.c -o $(BUILD)/ub_isa$(EXE)
	$(BUILD)/ub_isa$(EXE)

bench-phases: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) bench/bench_phases.c $(SRC) -o $(BUILD)/ub_phases$(EXE)
	$(BUILD)/ub_phases$(EXE)

# What the compiled kernel actually does: rotate counts, unrolling, spills.
# tools/kernel_stats.py --self-test guards the counting rules themselves.
kernel-stats: | $(BUILD)
	@python3 tools/kernel_stats.py --self-test
	@$(CC) $(CFLAGS) $(INC) -S -o $(BUILD)/compress.s src/compress.c
	@python3 tools/kernel_stats.py $(BUILD)/compress.s _compress_block

# What has to change to run kernel-stats on x86-64 or the AVX2 kernel, and
# why the counts mean different things there. Read before porting.
kernel-stats-arch:
	@python3 tools/kernel_stats.py --arch-notes

# A/B two benchmark binaries with a resolution verdict. Use for every
# performance claim: BIN_A and BIN_B are built benchmark executables.
#   make ab BIN_A=build/ub_phases BIN_B=/tmp/variant GREP='full leaf'
GREP ?= full leaf
ab:
	@python3 tools/ab_compare.py $(BIN_A) $(BIN_B) --runs $(or $(RUNS),21) --grep '$(GREP)'

bench-compare: sodium-check | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_compare.c $(SRC) \
	  $(SODLIB) -lpthread -o $(BUILD)/ub_cmp$(EXE)
	$(BUILD)/ub_cmp$(EXE)

# bench-compare with the AVX2 kernel instead of scalar. Gives the bulk row
# the leaf-only bench-avx2 does not.
# Every benchmark and analysis target in one run, with machine, compiler,
# oracle version and tree state captured alongside. Redirect to a file.
collect:
	@sh tools/collect.sh "$(SODIUM)"

bench-compare-avx2: avx2-check sodium-check | $(BUILD)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) bench/bench_compare.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -lpthread -o $(BUILD)/ub_cmp_avx2$(EXE)
	$(BUILD)/ub_cmp_avx2$(EXE)

bench-neon: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o $(BUILD)/ub_bench_neon$(EXE)
	$(BUILD)/ub_bench_neon$(EXE)

# AVX2 kernel. Needs -mavx2 and an x86-64 host with AVX2, so it is separate
# from check-backends, which runs what the host can execute. `make probe`
# reports whether this CPU has AVX2.
AVX2FLAGS = -mavx2

# Fail with the reason rather than a bare compiler error.
avx2-check:
	@case "$$(uname -m)" in \
	  x86_64|amd64) ;; \
	  *) echo "AVX2 targets need an x86-64 host; this is $$(uname -m)."; \
	     echo "  cross-compile: make check-avx2 CC=x86_64-w64-mingw32-gcc EXE=.exe"; \
	     exit 1 ;; \
	esac

# AVX2 under Rosetta 2 on an Apple-silicon host.
#
# Apple clang is a cross-compiler: --target=x86_64-apple-macos13 produces an
# x86-64 binary, and Rosetta 2 executes AVX2 even though it does not advertise
# it in CPUID. So the vector kernel can be built AND run here.
#
# Limited to the suites that need no oracle: Homebrew's libsodium is arm64, so
# anything linking it cannot be part of an x86-64 binary. That leaves the
# published vectors and the API suite, which is enough to establish
# correctness -- it is NOT a speed measurement, because Rosetta is emulation.
X86TARGET ?= x86_64-apple-macos13
check-avx2-rosetta: | $(BUILD)
	@case "$$(uname -m)" in arm64) ;; \
	  *) echo "check-avx2-rosetta is for an Apple-silicon host; use check-avx2"; exit 0 ;; esac; \
	for t in test_kat test_api; do \
	  $(CC) --target=$(X86TARGET) $(AVX2FLAGS) $(CFLAGS) $(INC) tests/$$t.c \
	    src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	    -o $(BUILD)/ub_rose_$$t && $(BUILD)/ub_rose_$$t || exit 1; \
	done

check-avx2: avx2-check sodium-check | $(BUILD)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) tests/test_core.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -o $(BUILD)/ub_avx2c$(EXE) && $(BUILD)/ub_avx2c$(EXE)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) tests/test_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -o $(BUILD)/ub_avx2p$(EXE) && $(BUILD)/ub_avx2p$(EXE)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) tests/test_kat.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  -o $(BUILD)/ub_avx2k$(EXE) && $(BUILD)/ub_avx2k$(EXE)

bench-avx2: avx2-check sodium-check | $(BUILD)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) bench/bench_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -o $(BUILD)/ub_bench_avx2$(EXE)
	$(BUILD)/ub_bench_avx2$(EXE)

bench-threads: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL -DUB_THREADS=$(THREADS) \
	  bench/bench_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o $(BUILD)/ub_bench_thr$(EXE)
	$(BUILD)/ub_bench_thr$(EXE)
THREADS ?= 4

# Proves the conformance checks can fail: links a deliberately wrong
# compression function and requires every oracle comparison to reject it.
check-negative: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_negative.c src/core.c src/const.c src/prefix.c $(SODLIB) -o $(BUILD)/ub_negative$(EXE)
	$(BUILD)/ub_negative$(EXE)

# Each compression backend runs the full oracle set, not just core+prefix: a
# kernel that only replaces ub_compress still has to satisfy the adapter and
# the return-code contract. NEON kernels are skipped on non-aarch64, where they
# compile to an empty translation unit and the scalar compress is absent.
UB_KERNELS = compress_neon

check-backends: | $(BUILD)
	@case "$$(uname -m)" in \
	  arm64|aarch64) ;; \
	  *) echo "check-backends: NEON kernels skipped on $$(uname -m)"; exit 0 ;; \
	esac; \
	for k in $(UB_KERNELS); do \
	  echo "== $$k =="; \
	  for t in core prefix; do \
	    $(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_$$t.c \
	      src/core.c src/const.c src/prefix.c backends/$$k.c $(SODLIB) \
	      -o $(BUILD)/ub_bk$(EXE) && $(BUILD)/ub_bk$(EXE) || exit 1; \
	  done; \
	  $(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c \
	    src/core.c src/const.c src/prefix.c backends/$$k.c $(SODLIB) \
	    -o $(BUILD)/ub_bk$(EXE) && $(BUILD)/ub_bk$(EXE) || exit 1; \
	  $(CC) $(CFLAGS) $(INC) tests/test_api.c \
	    src/core.c src/const.c src/prefix.c backends/$$k.c \
	    -o $(BUILD)/ub_bk$(EXE) && $(BUILD)/ub_bk$(EXE) || exit 1; \
	done
	@echo "== hash_n_threads =="
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL tests/test_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o $(BUILD)/ub_tp$(EXE) && $(BUILD)/ub_tp$(EXE)

# Portability gate: the library must build warning-free under both compilers,
# at the oldest standard it claims (C99) and the one it prefers (C11).
# CC2 is a second compiler if one is installed; skipped silently if not.
CC2 ?= gcc
STRICT = -O2 -Wall -Wextra -Wpedantic -Iinclude -Isrc
check-portable: | $(BUILD)
	@for std in c99 c11; do \
	  for cc in $(CC) $(CC2); do \
	    command -v $$cc >/dev/null 2>&1 || continue; \
	    printf '%-10s %-5s ' "$$cc" "$$std"; \
	    for f in $(SRC); do \
	      $$cc -std=$$std $(STRICT) -c $$f -o /dev/null 2>>$(BUILD)/ub_warn_$$$$ \
	        || { echo FAIL; cat $(BUILD)/ub_warn_$$$$; rm -f $(BUILD)/ub_warn_$$$$; exit 1; }; \
	    done; \
	    test -s $(BUILD)/ub_warn_$$$$ \
	      && { echo WARN; cat $(BUILD)/ub_warn_$$$$; rm -f $(BUILD)/ub_warn_$$$$; exit 1; } \
	      || echo ok; \
	    rm -f $(BUILD)/ub_warn_$$$$; \
	  done; \
	done

# The library must behave identically with wiping compiled out, and the
# digests must not move. Runs the oracle suites both ways.
check-wipe-modes: sodium-check | $(BUILD)
	@for w in 1 0; do \
	  printf 'UB_WIPE=%s  ' $$w; \
	  $(CC) $(CFLAGS) -DUB_WIPE=$$w $(INC) $(SODINC) tests/test_core.c \
	    $(SRC) $(SODLIB) -o $(BUILD)/ub_wm_core$(EXE) && $(BUILD)/ub_wm_core$(EXE) >/dev/null || exit 1; \
	  $(CC) $(CFLAGS) -DUB_WIPE=$$w $(INC) $(SODINC) tests/test_prefix.c \
	    $(SRC) $(SODLIB) -o $(BUILD)/ub_wm_pre$(EXE) && $(BUILD)/ub_wm_pre$(EXE) >/dev/null || exit 1; \
	  $(CC) $(CFLAGS) -DUB_WIPE=$$w $(INC) tests/test_api.c \
	    $(SRC) -o $(BUILD)/ub_wm_api$(EXE) && $(BUILD)/ub_wm_api$(EXE) >/dev/null || exit 1; \
	  echo 'core+prefix+api ok'; \
	done

# Address and undefined-behaviour sanitizers over the oracle suites. Same
# pairing Zero uses (--enable-asan turns on both). Not part of `check`: it is
# a different build, roughly 2x slower, run deliberately.
check-sanitize: sodium-check | $(BUILD)
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o $(BUILD)/ub_san_core$(EXE)
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o $(BUILD)/ub_san_pre$(EXE)
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(INC)           tests/test_api.c    $(SRC)           -o $(BUILD)/ub_san_api$(EXE)
	@# LeakSanitizer is Linux-only: macOS/arm64 ASan reports memory errors but
	@# not leaks, so a clean run here does NOT mean leak-free. Say which it was.
	@if ASAN_OPTIONS=detect_leaks=1 $(BUILD)/ub_san_api$(EXE) >/dev/null 2>&1; then \
	  echo 'leaks: checked'; \
	else \
	  echo 'leaks: NOT CHECKED on this platform -- run this target on Linux too'; \
	fi
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/ub_san_core$(EXE)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/ub_san_pre$(EXE)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/ub_san_api$(EXE)

.PHONY: bench-neon bench-avx2 check-avx2 avx2-check bench-threads check-backends check-negative check-portable
.PHONY: check-wipe-modes check-sanitize
