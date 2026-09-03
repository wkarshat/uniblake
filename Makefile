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
#
# BUILD renamed UB_BUILD.
ifeq ($(origin BUILD),command line)
$(error BUILD was renamed to UB_BUILD (it collided with the build triplet that \
Bitcoin-style depends trees export). Use: make UB_BUILD=$(BUILD) ...)
endif
UB_BUILD ?= build
EXE     ?=

# Objects and the archive live under UB_BUILD too, not beside the sources, so a
# native build and a cross build can share one checkout without overwriting
# each other or silently reusing the other's objects. Give each its own UB_BUILD.
OBJ      = $(SRC:src/%.c=$(UB_BUILD)/%.o)
LIB      = $(UB_BUILD)/libuniblake.a

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
$(UB_BUILD):
	@mkdir -p $(UB_BUILD)

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

# -MMD -MP emits a .d file listing the headers each object actually included,
# so editing internal.h or a public header rebuilds what depends on it. Without
# this, a header change leaves stale objects -- and internal.h defines
# struct ub_state, so a stale object means a layout mismatch, not a warning.
# Makefile is a prerequisite so a CFLAGS or rule change rebuilds every object
# rather than leaving ones compiled with the old flags.
$(UB_BUILD)/%.o: src/%.c Makefile
	@mkdir -p $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

# The BLAKE2 author-reference alias shim, checked against the vendored
# reference (compat/ref). Needs no libsodium, so it joins the default `check`.
# Published BLAKE2b vectors: no oracle, so this runs where libsodium does not.
check-kat: | $(UB_BUILD)
	@mkdir -p $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) tests/test_kat.c $(SRC) -o $(UB_BUILD)/ub_kat$(EXE)
	$(UB_BUILD)/ub_kat$(EXE)

check-alias: check-build | $(UB_BUILD)
	$(UB_BUILD)/ub_alias$(EXE)

# Build the alias suite without running it. Split out because a cross build
# cannot run its own output: the exit code otherwise reports a failure that is
# only the host refusing to execute a foreign binary.
#
#   make check-build UB_BUILD=build-win CC=x86_64-w64-mingw32-gcc EXE=.exe
#
# check-alias is the suite to cross-build: its oracle is vendored, so it links
# nothing for the target. The others need a libsodium built for it.
check-build: | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) -Icompat -Icompat/ref compat/test_blake2_alias.c \
	  $(SRC) compat/ref/ref_blake2b.c -o $(UB_BUILD)/ub_alias$(EXE)

# Run a cross-built Windows binary under Wine. WINEDEBUG=-all silences Wine's
# complaints about having no desktop, which a console program does not need.
#
#   make check-wine UB_BUILD=build-win
WINE ?= wine
check-wine:
	@test -f $(UB_BUILD)/ub_alias.exe || { \
	  echo "no $(UB_BUILD)/ub_alias.exe -- run check-build first, e.g."; \
	  echo "  make check-build UB_BUILD=build-win CC=x86_64-w64-mingw32-gcc EXE=.exe"; \
	  exit 1; }
	WINEDEBUG=-all $(WINE) $(UB_BUILD)/ub_alias.exe

# Everything that needs no oracle: the published KAT vectors, the blake2b
# alias suite, the public-API suite and the standards sweep. test_api.c links
# no libsodium (it exercises the API's own contracts, not conformance), so it
# belongs here rather than behind `check`'s sodium-check.
#
# This is the CI entry point for a runner with nothing installed, and the fast
# local check. `check` remains the full run: core, prefix and compat compare
# against libsodium and cannot be built without it.
check-nosodium: check-kat check-alias check-portable | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) tests/test_api.c $(SRC) -o $(UB_BUILD)/ub_api$(EXE)
	$(UB_BUILD)/ub_api$(EXE)

check: sodium-check check-kat check-alias | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_core$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_prefix$(EXE)
	$(CC) $(CFLAGS) $(INC)           tests/test_api.c  $(SRC)           -o $(UB_BUILD)/ub_api$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_compat$(EXE)
	$(UB_BUILD)/ub_core$(EXE) && $(UB_BUILD)/ub_prefix$(EXE) && $(UB_BUILD)/ub_api$(EXE) && $(UB_BUILD)/ub_compat$(EXE)

bench: sodium-check | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_bench$(EXE)
	$(UB_BUILD)/ub_bench$(EXE)

clean:
	rm -f src/*.o libuniblake.a
	rm -rf $(UB_BUILD)

.PHONY: all check check-nosodium bench clean sodium-check check-alias check-kat

# CPU identity and ISA flags. Not part of the library -- the core has no
# dispatch -- but a measurement should be reported with the machine it came
# from, and a future selector needs core identity, not just ISA flags.
probe:
	@mkdir -p $(UB_BUILD)
	$(CC) $(CFLAGS) -Iprobe probe/ub_probe_main.c probe/ub_probe.c -o $(UB_BUILD)/ub_probe$(EXE)
	@$(UB_BUILD)/ub_probe$(EXE)

.PHONY: probe

# Prototype backends (see backends/README.md). Not part of `all`.
# Per-instruction latency for the operations G needs. Explains a vector
# kernel's ceiling in terms of the instruction set rather than the code.
bench-isa: | $(UB_BUILD)
	@case "$$(uname -m)" in \
	  arm64|aarch64) ;; \
	  *) echo "bench-isa: aarch64 only (NEON intrinsics); this is $$(uname -m)"; exit 0 ;; \
	esac; \
	$(CC) $(CFLAGS) $(INC) bench/bench_isa.c -o $(UB_BUILD)/ub_isa$(EXE) && $(UB_BUILD)/ub_isa$(EXE)

bench-phases: | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) bench/bench_phases.c $(SRC) -o $(UB_BUILD)/ub_phases$(EXE)
	$(UB_BUILD)/ub_phases$(EXE)

# What the compiled kernel actually does: rotate counts, unrolling, spills.
# tools/kernel_stats.py --self-test guards the counting rules themselves.
kernel-stats: | $(UB_BUILD)
	@python3 tools/kernel_stats.py --self-test
	@$(CC) $(CFLAGS) $(INC) -S -o $(UB_BUILD)/compress.s src/compress.c
	@python3 tools/kernel_stats.py $(UB_BUILD)/compress.s _compress_block

# What has to change to run kernel-stats on x86-64 or the AVX2 kernel, and
# why the counts mean different things there. Read before porting.
kernel-stats-arch:
	@python3 tools/kernel_stats.py --arch-notes

# A/B two benchmark binaries with a resolution verdict. Use for every
# performance claim: BIN_A and BIN_B are built benchmark executables.
#   make ab BIN_A=build/ub_phases BIN_B=/tmp/variant UB_GREP='full leaf'
UB_GREP ?= full leaf
ab:
	@python3 tools/ab_compare.py $(BIN_A) $(BIN_B) --runs $(or $(RUNS),21) --grep '$(UB_GREP)'

bench-compare: sodium-check | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_compare.c $(SRC) \
	  $(SODLIB) -lpthread -o $(UB_BUILD)/ub_cmp$(EXE)
	$(UB_BUILD)/ub_cmp$(EXE)

# bench-compare with the AVX2 kernel instead of scalar. Gives the bulk row
# the leaf-only bench-avx2 does not.
# Every benchmark and analysis target in one run, with machine, compiler,
# oracle version and tree state captured alongside. Redirect to a file.
collect:
	@sh tools/collect.sh "$(SODIUM)"

# Append machine-readable rows to measurements.tsv, newest first.
#   make record RUN=bench-phases UB_PLATFORM=mac-m4-14c
#   make record RUN=bench-compare UB_PLATFORM=linux-x86-vps-2c SODIUM=<prefix> \
#        ORACLE=1.0.21 ORACLE_FLAGS='-O2'
# derived by tools/platform_id.sh when empty
UB_PLATFORM ?=
ORACLE ?=
ORACLE_FLAGS ?=
# Read measurements.tsv for a human. ARGS passes viewer flags:
#   make results                    latest run
#   make results ARGS=--runs        what runs exist
#   make results ARGS=--compare     latest two of each shape, side by side
results:
	@python3 tools/bench.py $(ARGS)

record:
	@$(MAKE) -s $(RUN) SODIUM=$(SODIUM) | python3 tools/record.py \
	  --project uniblake --platform "$(UB_PLATFORM)" --run "$(RUN)" \
	  --kind "$(or $(KIND),median)" --exec "$(or $(EXEC),native)" \
	  --oracle-version "$(ORACLE)" --oracle-flags "$(ORACLE_FLAGS)"

bench-compare-avx2: avx2-check sodium-check | $(UB_BUILD)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) bench/bench_compare.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -lpthread -o $(UB_BUILD)/ub_cmp_avx2$(EXE)
	$(UB_BUILD)/ub_cmp_avx2$(EXE)

bench-neon: | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o $(UB_BUILD)/ub_bench_neon$(EXE)
	$(UB_BUILD)/ub_bench_neon$(EXE)

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
check-avx2-rosetta: | $(UB_BUILD)
	@case "$$(uname -m)" in arm64) ;; \
	  *) echo "check-avx2-rosetta is for an Apple-silicon host; use check-avx2"; exit 0 ;; esac; \
	for t in test_kat test_api; do \
	  $(CC) --target=$(X86TARGET) $(AVX2FLAGS) $(CFLAGS) $(INC) tests/$$t.c \
	    src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	    -o $(UB_BUILD)/ub_rose_$$t && $(UB_BUILD)/ub_rose_$$t || exit 1; \
	done

check-avx2: avx2-check sodium-check | $(UB_BUILD)
	@mkdir -p $(UB_BUILD)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) tests/test_core.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -o $(UB_BUILD)/ub_avx2c$(EXE) && $(UB_BUILD)/ub_avx2c$(EXE)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) tests/test_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -o $(UB_BUILD)/ub_avx2p$(EXE) && $(UB_BUILD)/ub_avx2p$(EXE)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) tests/test_kat.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  -o $(UB_BUILD)/ub_avx2k$(EXE) && $(UB_BUILD)/ub_avx2k$(EXE)

bench-avx2: avx2-check sodium-check | $(UB_BUILD)
	@mkdir -p $(UB_BUILD)
	$(CC) $(CFLAGS) $(AVX2FLAGS) $(INC) $(SODINC) bench/bench_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_avx2.c \
	  $(SODLIB) -o $(UB_BUILD)/ub_bench_avx2$(EXE)
	$(UB_BUILD)/ub_bench_avx2$(EXE)

# Thread count for bench-threads; compiled in as UB_THREADS.
UB_THREADS_N ?= 4

bench-threads: | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL -DUB_THREADS=$(UB_THREADS_N) \
	  bench/bench_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o $(UB_BUILD)/ub_bench_thr$(EXE)
	$(UB_BUILD)/ub_bench_thr$(EXE)

# Proves the conformance checks can fail: links a deliberately wrong
# compression function and requires every oracle comparison to reject it.
check-negative: | $(UB_BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_negative.c src/core.c src/const.c src/prefix.c $(SODLIB) -o $(UB_BUILD)/ub_negative$(EXE)
	$(UB_BUILD)/ub_negative$(EXE)

# Each compression backend runs the full oracle set, not just core+prefix: a
# kernel that only replaces ub_compress still has to satisfy the adapter and
# the return-code contract. NEON kernels are skipped on non-aarch64, where they
# compile to an empty translation unit and the scalar compress is absent.
UB_KERNELS = compress_neon

check-backends: | $(UB_BUILD)
	@case "$$(uname -m)" in \
	  arm64|aarch64) ;; \
	  *) echo "check-backends: NEON kernels skipped on $$(uname -m)"; exit 0 ;; \
	esac; \
	for k in $(UB_KERNELS); do \
	  echo "== $$k =="; \
	  for t in core prefix; do \
	    $(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_$$t.c \
	      src/core.c src/const.c src/prefix.c backends/$$k.c $(SODLIB) \
	      -o $(UB_BUILD)/ub_bk$(EXE) && $(UB_BUILD)/ub_bk$(EXE) || exit 1; \
	  done; \
	  $(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c \
	    src/core.c src/const.c src/prefix.c backends/$$k.c $(SODLIB) \
	    -o $(UB_BUILD)/ub_bk$(EXE) && $(UB_BUILD)/ub_bk$(EXE) || exit 1; \
	  $(CC) $(CFLAGS) $(INC) tests/test_api.c \
	    src/core.c src/const.c src/prefix.c backends/$$k.c \
	    -o $(UB_BUILD)/ub_bk$(EXE) && $(UB_BUILD)/ub_bk$(EXE) || exit 1; \
	done
	@echo "== hash_n_threads =="
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL tests/test_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o $(UB_BUILD)/ub_tp$(EXE) && $(UB_BUILD)/ub_tp$(EXE)

# Portability gate: the library must build warning-free under both compilers,
# at the oldest standard it claims (C99) and the one it prefers (C11).
# CC2 is a second compiler if one is installed; skipped silently if not.
CC2 ?= gcc
STRICT = -O2 -Wall -Wextra -Wpedantic -Iinclude -Isrc
check-portable: | $(UB_BUILD)
	@for std in c99 c11; do \
	  for cc in $(CC) $(CC2); do \
	    command -v $$cc >/dev/null 2>&1 || continue; \
	    printf '%-10s %-5s ' "$$cc" "$$std"; \
	    for f in $(SRC); do \
	      $$cc -std=$$std $(STRICT) -c $$f -o /dev/null 2>>$(UB_BUILD)/ub_warn_$$$$ \
	        || { echo FAIL; cat $(UB_BUILD)/ub_warn_$$$$; rm -f $(UB_BUILD)/ub_warn_$$$$; exit 1; }; \
	    done; \
	    test -s $(UB_BUILD)/ub_warn_$$$$ \
	      && { echo WARN; cat $(UB_BUILD)/ub_warn_$$$$; rm -f $(UB_BUILD)/ub_warn_$$$$; exit 1; } \
	      || echo ok; \
	    rm -f $(UB_BUILD)/ub_warn_$$$$; \
	  done; \
	done

# The library must behave identically with wiping compiled out, and the
# digests must not move. Runs the oracle suites both ways.
check-wipe-modes: sodium-check | $(UB_BUILD)
	@for w in 1 0; do \
	  printf 'UB_WIPE=%s  ' $$w; \
	  $(CC) $(CFLAGS) -DUB_WIPE=$$w $(INC) $(SODINC) tests/test_core.c \
	    $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_wm_core$(EXE) && $(UB_BUILD)/ub_wm_core$(EXE) >/dev/null || exit 1; \
	  $(CC) $(CFLAGS) -DUB_WIPE=$$w $(INC) $(SODINC) tests/test_prefix.c \
	    $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_wm_pre$(EXE) && $(UB_BUILD)/ub_wm_pre$(EXE) >/dev/null || exit 1; \
	  $(CC) $(CFLAGS) -DUB_WIPE=$$w $(INC) tests/test_api.c \
	    $(SRC) -o $(UB_BUILD)/ub_wm_api$(EXE) && $(UB_BUILD)/ub_wm_api$(EXE) >/dev/null || exit 1; \
	  echo 'core+prefix+api ok'; \
	done

# Address and undefined-behaviour sanitizers over the oracle suites. Same
# pairing Zero uses (--enable-asan turns on both). Not part of `check`: it is
# a different build, roughly 2x slower, run deliberately.
check-sanitize: sodium-check | $(UB_BUILD)
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_san_core$(EXE)
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o $(UB_BUILD)/ub_san_pre$(EXE)
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(INC)           tests/test_api.c    $(SRC)           -o $(UB_BUILD)/ub_san_api$(EXE)
	@# LeakSanitizer is Linux-only: macOS/arm64 ASan reports memory errors but
	@# not leaks, so a clean run here does NOT mean leak-free. Say which it was.
	@if ASAN_OPTIONS=detect_leaks=1 $(UB_BUILD)/ub_san_api$(EXE) >/dev/null 2>&1; then \
	  echo 'leaks: checked'; \
	else \
	  echo 'leaks: NOT CHECKED on this platform -- run this target on Linux too'; \
	fi
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(UB_BUILD)/ub_san_core$(EXE)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(UB_BUILD)/ub_san_pre$(EXE)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(UB_BUILD)/ub_san_api$(EXE)

.PHONY: bench-neon bench-avx2 check-avx2 avx2-check bench-threads check-backends check-negative check-portable
.PHONY: check-wipe-modes check-sanitize
