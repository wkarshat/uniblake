# uniblake. The library needs only a C11 compiler; libsodium is required
# solely by the check/bench targets, where it is an independent oracle.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC      = -Iinclude -Isrc
SRC      = src/core.c src/compress.c src/const.c src/prefix.c
OBJ      = $(SRC:.c=.o)

# libsodium is the conformance oracle for check/bench; the library links none.
# Point at whichever build you want to be checked against -- ideally the one
# the consuming project already uses, so conformance is measured against the
# implementation being replaced.
#   make check SODIUM=/opt/homebrew
#   make check SODIUM=/path/to/project/depends/<triplet>
SODIUM  ?= /usr/local
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
	  echo "  set SODIUM=<prefix>, e.g. make check SODIUM=/opt/homebrew"; \
	  exit 1; }
	@printf 'oracle: libsodium %s at %s\n' \
	  "$$(sed -n 's/.*SODIUM_VERSION_STRING "\(.*\)".*/\1/p' $(SODIUM)/include/sodium/version.h)" \
	  "$(SODIUM)" 

all: libuniblake.a

libuniblake.a: $(OBJ)
	ar rcs $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

check: sodium-check
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o /tmp/ub_core
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o /tmp/ub_prefix
	$(CC) $(CFLAGS) $(INC)           tests/test_api.c  $(SRC)           -o /tmp/ub_api
	$(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c $(SRC) $(SODLIB) -o /tmp/ub_compat
	/tmp/ub_core && /tmp/ub_prefix && /tmp/ub_api && /tmp/ub_compat

bench: sodium-check
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c $(SRC) $(SODLIB) -o /tmp/ub_bench
	/tmp/ub_bench

clean:
	rm -f $(OBJ) libuniblake.a

.PHONY: all check bench clean sodium-check

# Prototype backends (see backends/README.md). Not part of `all`.
bench-neon:
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c \
	  src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o /tmp/ub_bench_neon
	/tmp/ub_bench_neon

bench-threads:
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL -DUB_THREADS=$(THREADS) \
	  bench/bench_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o /tmp/ub_bench_thr
	/tmp/ub_bench_thr
THREADS ?= 4

# Proves the conformance checks can fail: links a deliberately wrong
# compression function and requires every oracle comparison to reject it.
check-negative:
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_negative.c src/core.c src/const.c src/prefix.c $(SODLIB) -o /tmp/ub_negative
	/tmp/ub_negative

check-backends:
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o /tmp/ub_nc && /tmp/ub_nc
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o /tmp/ub_np && /tmp/ub_np
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL tests/test_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o /tmp/ub_tp && /tmp/ub_tp

.PHONY: bench-neon bench-threads check-backends check-negative
