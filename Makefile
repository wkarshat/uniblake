# uniblake. The library needs only a C11 compiler; libsodium is required
# solely by the check/bench targets, where it is an independent oracle.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
INC      = -Iinclude -Isrc
SRC      = src/core.c src/compress.c src/const.c src/prefix.c
OBJ      = $(SRC:.c=.o)

# Override for a libsodium in a non-default location:
#   make check SODIUM=/opt/homebrew
SODIUM  ?= /usr/local
SODINC   = -I$(SODIUM)/include
SODLIB   = -L$(SODIUM)/lib -lsodium

all: libuniblake.a

libuniblake.a: $(OBJ)
	ar rcs $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

check:
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   $(SRC) $(SODLIB) -o /tmp/ub_core
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c $(SRC) $(SODLIB) -o /tmp/ub_prefix
	$(CC) $(CFLAGS) $(INC)           tests/test_api.c  $(SRC)           -o /tmp/ub_api
	$(CC) $(CFLAGS) $(INC) $(SODINC) compat/test_compat.c $(SRC) $(SODLIB) -o /tmp/ub_compat
	/tmp/ub_core && /tmp/ub_prefix && /tmp/ub_api && /tmp/ub_compat

bench:
	$(CC) $(CFLAGS) $(INC) $(SODINC) bench/bench_prefix.c $(SRC) $(SODLIB) -o /tmp/ub_bench
	/tmp/ub_bench

clean:
	rm -f $(OBJ) libuniblake.a

.PHONY: all check bench clean

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

check-backends:
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_core.c   src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o /tmp/ub_nc && /tmp/ub_nc
	$(CC) $(CFLAGS) $(INC) $(SODINC) tests/test_prefix.c src/core.c src/const.c src/prefix.c backends/compress_neon.c $(SODLIB) -o /tmp/ub_np && /tmp/ub_np
	$(CC) $(CFLAGS) $(INC) $(SODINC) -DUB_HASH_N_SERIAL tests/test_prefix.c $(SRC) backends/hash_n_threads.c $(SODLIB) -lpthread -o /tmp/ub_tp && /tmp/ub_tp

.PHONY: bench-neon bench-threads check-backends
