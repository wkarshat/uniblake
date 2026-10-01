# Access patterns: which shape of work uniblake changes, and by how much

uniblake is not a faster BLAKE2b. On bulk data it is within 1-2% of libsodium,
because it runs a comparable scalar compression function. What it changes is
**how many times the compression function runs** for a given job.

This document groups workloads by that structural property, so a reader can
tell from the shape of their own problem whether the library helps.

All figures: Apple M4 Pro, macOS 26.3, clang `-O2`, oracle libsodium 1.0.22
built `-O3`. Prefix 140 B, digest 50 B unless stated.

## 1. The governing quantity

BLAKE2b compresses **128 bytes per block**. For a digest over prefix `P` and
tail `x`:

    blocks a streaming API pays  = ceil((|P| + |x|) / 128)
    blocks uniblake pays         = that, minus the whole blocks of P
                                   already absorbed into a kept state

The ratio of those two numbers is the whole effect. For `|P|`=140, `|x|`=4:

    libsodium   ceil(144/128) = 2 blocks
    uniblake    2 - 1         = 1 block
    predicted 2.00x     measured 2.03x

Matching to 1.5% is the evidence that nothing else material is going on.

**So the question for any workload is: what fraction of each digest's input is
a constant you have already hashed?**

## 2. The four patterns

### A. Fixed long prefix, short varying tail -- 1.8x to 2.0x

    H(P || x)  for many x,  |P| >> |x|

The pattern uniblake exists for. The prefix is absorbed once; each digest costs
only its tail block.

| Call | Use when | Measured |
|---|---|--:|
| `ub_hash_tail` | tails are arbitrary and caller-supplied | 83.0 ns (2.03x) |
| `ub_hash_n` | tails are a run of consecutive counters | 86.1 ns (1.95x) |
| `ub_hash_n`, n=1 | one tail; setup not amortised | 92.3 ns (1.82x) |

Note the n=1 row: the technique has a fixed setup cost, so a *single* digest
over a prefix gains least. The advantage is in the repetition.

**Where this shape occurs:** counter-mode key derivation; content-addressed
chunk indexing under a fixed manifest header; Merkle/tree hashing with a
domain-separation tag; deduplication indexes under a namespace prefix;
rendezvous and consistent hashing (fixed node id, varying key); log and ledger
commitments under an epoch header; expanding a PRNG from a seed; and the
general `H(domain_sep || item)` loop that most protocol code contains.
Domain separation is a fixed prefix **by construction**, which is why the shape
is so common.

### B. Fixed prefix, long tail -- approaches 1.0x

As `|x|` grows past `|P|`, the saved prefix blocks become a small fraction of
the total and the advantage decays toward parity. There is no penalty, but
little to gain.

### C. No shared prefix -- 1.0x

    H(m_i)  for unrelated m_i

Nothing is reusable; each digest is a fresh state. uniblake and libsodium do
the same work. **Use whichever library you already link.**

### D. Bulk, single long message -- 1.01x to 1.02x

| Message | libsodium | uniblake | ratio |
|---|--:|--:|--:|
| 1 KiB | 1625 MB/s | 1656 MB/s | 1.02x |
| 16 KiB | 1662 MB/s | 1675 MB/s | 1.01x |
| 1 MiB | 1670 MB/s | 1685 MB/s | 1.01x |
| 16 MiB | 1659 MB/s | 1683 MB/s | 1.01x |

This row is the control for every claim above: **it shows the compression
functions are equally fast**, so any larger ratio elsewhere is structural.

### E. Parallel across independent digests -- scales

| Threads | ns/digest | vs 1 thread |
|--:|--:|--:|
| 1 | 83.0 | 1.00x |
| 2 | 41.6 | 1.99x |

Near-linear, as expected for independent work. This composes with pattern A
rather than replacing it: 2 threads on pattern A is ~4.0x libsodium
single-threaded.

## 3. Memory

Measured, not estimated (`ub_state_size()` at runtime; `sizeof` for libsodium).

| Item | Bytes | Note |
|---|--:|---|
| `ub_state` | **216** | alignment 8; opaque, size is a runtime value |
| libsodium `crypto_generichash_blake2b_state` | 384 | for comparison |

uniblake's state is **44% smaller**. That matters less for footprint than for
what it enables: the state is small enough to copy cheaply, and copying the
post-prefix state per digest is the mechanism behind pattern A.

**Live memory for pattern A:**

| Role | Bytes |
|---|--:|
| Base state, prefix absorbed | 216 |
| Working clone, reused per digest | 216 |
| **Total live** | **432** |

**This does not grow with batch size.** A run of 400 000 digests holds the same
432 bytes as a run of two: the clone is overwritten, not accumulated.

**Whole-process, representative run** -- `ub_bench`, 400 000 digests over a
shared prefix, all four patterns measured:

| Metric | Value |
|---|--:|
| Peak RSS | 25.9 MiB |
| Hashing state within that | 432 B (0.0016%) |
| Wall | 1.68 s |

The peak is the harness's input buffers and the process image, not the hashing.
**BLAKE2b state is not a memory consideration at any batch size**; if a caller
sees growth, it is theirs, not the library's.

## 4. Choosing

| If your workload... | Then |
|---|---|
| hashes a long fixed prefix with many short tails | uniblake, pattern A, expect ~2x |
| hashes `H(tag \|\| item)` in a loop | same thing; check `\|tag\|` vs `\|item\|` |
| hashes unrelated messages | no benefit; either library |
| hashes a few large buffers | no benefit; either library |
| needs many independent digests at once | combine A with threads |
| is memory-constrained | uniblake's state is 216 B against 384 B |

The honest one-line summary: **uniblake pays off exactly when you are hashing
the same prefix over and over, and not otherwise.**
