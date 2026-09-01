# uniblake. The library needs only a C11 compiler; libsodium is required
# solely by the check/bench targets, where it is an independent oracle.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC      = -Iinclude -Isrc
SRC      = src/core.c src/compress.c src/const.c src/prefix.c
OBJ      = $(SRC:.c=.o)

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
	  echo "  set SODIUM=<prefix>, e.g. make check SODIUM=/usr"; \
	  exit 1; }
	@printf 'oracle: libsodium %s at %s\n' \
	  "$$(sed -n 's/.*SODIUM_VERSION_STRING "\(.*\)".*/\1/p' $(SODIUM)/include/sodium/version.h)" \
	  "$(SODIUM)" 

# Created on demand; every target that writes a binary depends on it.
$(BUILD):
	@mkdir -p $(BUILD)

all: libuniblake.a

libuniblake.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

# The BLAKE2 author-reference alias shim, checked against the vendored
# reference (compat/ref). Needs no libsodium, so it joins the default `check`.
check-alias: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -Icompat -Icompat/ref compat/test_blake2_alias.c \
	  $(SRC) compat/ref/ref_blake2b.c -o $(BUILD)/ub_alias$(EXE)
	$(BUILD)/ub_alias$(EXE)

check: sodium-check check-alias | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o $(BUILD)/ub_core$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o $(BUILD)/ub_prefix$(EXE)
	$(CC) $(CFLAGS) $(INC)           tests/test_api.c  $(SRC)           -o $(BUILD)/ub_api$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c $(SRC) $(SODLIB) -o $(BUILD)/ub_compat$(EXE)
	$(BUILD)/ub_core$(EXE) && $(BUILD)/ub_prefix$(EXE) && $(BUILD)/ub_api$(EXE) && $(BUILD)/ub_compat$(EXE)

bench: sodium-check | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c $(SRC) $(SODLIB) -o $(BUILD)/ub_bench$(EXE)
	$(BUILD)/ub_bench$(EXE)

clean:
	rm -f $(OBJ) libuniblake.a
	rm -rf $(BUILD)

.PHONY: all check bench clean sodium-check check-alias

# Prototype backends (see backends/README.md). Not part of `all`.
bench-neon: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o $(BUILD)/ub_bench_neon$(EXE)
	$(BUILD)/ub_bench_neon$(EXE)

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

check-backends: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o $(BUILD)/ub_nc$(EXE) && $(BUILD)/ub_nc$(EXE)
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o $(BUILD)/ub_np$(EXE) && $(BUILD)/ub_np$(EXE)
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
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/ub_san_core$(EXE)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/ub_san_pre$(EXE)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/ub_san_api$(EXE)

.PHONY: bench-neon bench-threads check-backends check-negative check-portable
.PHONY: check-wipe-modes check-sanitize
