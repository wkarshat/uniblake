# UniBlake

## What BLAKE2b gives a caller

BLAKE2b is the 64-bit BLAKE2 variant: 128-byte blocks, digests of 1 to 64
bytes, specified by RFC 7693.

Its distinguishing feature is the **parameter block** -- 64 bytes of "what am
I computing" (digest length, key length, salt, personalization, tree layout)
mixed into the initial state rather than into the message. Two hashes with
different personalization strings are unrelated functions, at no cost per
message. This is why BLAKE2b is common in protocol work: domain separation is
free and built in.

### Two things called "the reference"

Two C implementations are both called "the reference", and the difference
decides what a caller can express.

| | RFC 7693 Appendix A | author reference |
|---|---|---|
| init | one call, key always passed | `init`, `init_key`, `init_param` |
| parameter block | not exposed | `blake2b_param` |
| salt, personalization | **absent** | present |
| `update` | returns `void` | returns `int` |
| `final` | no output capacity | takes capacity |
| SIMD, tree modes | none | SSE through AVX2, `blake2bp` |

The RFC sample is normative for the *algorithm* and deliberately small — about
150 lines demonstrating that the specification is implementable. It omits the
parameter block, so it cannot produce salted or personalized digests at all.

The author reference is what libraries follow: libsodium's
`crypto_generichash_blake2b_*` and this library both track its names and
argument order. "The reference implementation", in an argument about API
shape, means this one.

uniblake follows the author reference for the interface and cites RFC 7693 for
the algorithm. `compat/ub_rfc.h` adapts the Appendix-A shape for callers that
have it hard-coded, and `compat/ub_blake2.h` aliases the author reference's
own names.


## Why this library exists

A workload that hashes many messages sharing their opening bytes —

    H(header || 0),  H(header || 1),  H(header || 2),  ...

should absorb `header` once. BLAKE2b is a Merkle–Damgård construction over
128-byte blocks, so the state after the shared bytes is a legitimate starting
point for every message that begins with them.

Whether an implementation *reaches* that state is not specified anywhere. The
API offers init, update, final; nothing says when update compresses. An
implementation that buffers two blocks before compressing will, given a
140-byte prefix, have compressed nothing by the time the prefix is absorbed —
its byte counter still reads zero. Every digest then compresses those 140
bytes again: 2.7 block compressions where 1 suffices. An implementation that
compresses each whole block as soon as more input follows leaves 12 bytes
pending, and each digest costs one compression.

Both are correct BLAKE2b. The caller cannot tell them apart, cannot request
the second, and gets no diagnostic when the first silently triples the cost.

UniBlake makes that behaviour part of the interface: absorb eagerly, expose a
check that answers whether your sizes permit one-compression digests, and
refuse rather than degrade when they do not.

## Design parameters

**Track the reference, don't improve on it.** Names, argument order, and the
parameter block follow `blake2.h`. A reader who knows BLAKE2b should not have
to learn a second vocabulary, and an adapter for another library should be a
rename, not a translation.

**Caller owns all storage.** The library never allocates. State size and
alignment are reported at runtime, so the struct can change without breaking
a binary.

**One state type.** Anything that manages several concurrent computations
would be a different type with a different name; there is no hierarchy of
state-like objects to keep straight.

**One replaceable compression function.** Everything specific to a CPU,
thread pool, or accelerator lives behind a single function; the streaming
logic above it is written once.

**Errors are values, diagnosis is optional.** Every call returns a code.
A program that wants more can install a handler and get the failing function
and a reason; one that wants nothing pays for nothing.

## What a minimal interface cannot do

Deliberate omissions, and what to do instead.

**No tree hashing.** BLAKE2b's tree mode, and BLAKE2bp/BLAKE2sp, are not
implemented. The parameter block's tree fields are honoured — set them and
you get the correct initial state — but no traversal is performed. Callers
needing tree modes want the reference implementation.

**No extendable output.** Digests are 1–64 bytes. BLAKE2X is a separate
construction and is not provided; a caller needing a longer stream should use
a construction designed for it.

**No other algorithms.** BLAKE2b only. The 32-bit variant, the extendable
output construction, and the tree modes are different functions, not options;
the scope is settled rather than open. A caller who wants one of them wants a
different library.

**No parallel tree modes.** BLAKE2bp and BLAKE2sp are distinct algorithms --
tree modes producing different digests from the same input -- not faster
BLAKE2b. Nothing here grows into them, and no parameter, entry point or kernel
is shaped to leave room for one.

One nearby technique is easily confused with them and is *not* excluded:
multi-message SIMD, where several independent BLAKE2b messages occupy the lanes
of one vector register. Every message still receives a plain BLAKE2b digest;
only the kernel is shared. BLAKE2bp uses the same lane trick and then combines
the lanes, which changes the output. Same technique, different algorithm.
Whether this library adopts the technique is an open question, not a commitment.

**No shared segment except at the start.** This is inherent, not a shortcut:
the state depends on every byte absorbed so far, so in `varying || fixed` the
shared part cannot be precomputed. Only `fixed || varying` benefits, and the
state must be rebuilt whenever `fixed` changes.

**No threads inside the library.** The core spawns nothing, locks nothing, and
links no threading runtime. That is a decision about *where* concurrency
lives, not a claim that hashing is serial -- see "Concurrency" below.

**No key in the parameter block.** BLAKE2b absorbs the key as a padded first
block, not as initial-state material, so keying is a separate init call.

## Scenarios

| you have | you want | call |
|---|---|---|
| a message in one buffer | its digest | `ub_hash` |
| a message arriving in pieces | its digest | `ub_init`, `ub_update`…, `ub_final` |
| a shared prefix, one trailing value | one digest | `ub_hash_tail` |
| a shared prefix, a run of counters | many digests | `ub_hash_n` |
| a shared prefix, unrelated tails | many digests | `ub_hash_tail` per tail |
| digests read as fixed-width fields | one field of each | `ub_hash_n` with `off`/`len` |
| a shared prefix, a long or multi-part tail | a digest | `ub_copy`, then `ub_update`/`ub_final` |

### Why these four hashing calls

`ub_hash` — one call instead of four for a message already in memory. The only
one that is pure convenience; it exists because that case is overwhelmingly
common.

`ub_hash_tail` — the general prefix form. Trailing bytes are whatever the
caller has: a counter, a sparse index, a string. `ub_hash_n` cannot express it,
because `ub_hash_n` generates its own tails.

`ub_hash_n` — a run of consecutive counters. A loop over `ub_hash_tail` gives
the same bytes, but it cannot be handed to a thread pool, SIMD kernel, or
accelerator: those need the whole range and the output layout in one call, or
they spend more on coordination than they save. `off`/`len` belong here for
the same reason — a caller reading a 48-byte digest as two 24-byte fields
gets the field it wants written directly, with no second pass.

`ub_prefix_check` — not a hashing call but the reason the others can be
trusted: it reports, before any hashing, whether the sizes permit
one-compression digests.

Removing any of the three hashing calls forces a caller into a slower or more
error-prone shape; adding a fourth would duplicate one of them.

## Concurrency

The work is embarrassingly parallel and the library still spawns no threads.
Both halves of that are deliberate.

**Why the digests parallelize.** Each digest over a shared prefix is
independent: it reads the prefix state, appends its own tail, and finalizes.
Nothing is carried between digests, so a range of them has no ordering
constraint and needs no coordination -- no locks, no shared counter, no
accumulator. A thread takes a span of the range, copies the read-only prefix
state, and writes its own slice of the output. Measured on an 8-core machine,
that is a 7.2x speedup at 8 threads.

**Why the library does not do it for you.** A hash library that creates
threads makes three decisions that are not its to make: how many threads,
when to spawn them, and which runtime to link. Those belong to the program.

- A caller that already runs one task per core does not want a second pool
  underneath it -- that oversubscribes and usually runs slower than serial.
- The right count depends on what else the process is doing, which the
  library cannot see.
- Linking pthreads, or OpenMP, or a platform pool, forces that dependency on
  every consumer including the ones hashing a single short message.

So the core stays free of it: no allocation, no POSIX, no threading runtime.
That is what makes it usable on a bare target and inside a caller that has
its own scheduler.

**Where concurrency attaches instead.** `ub_hash_n` is the seam. It takes the
whole range and the output layout in one call, which is exactly what a
parallel implementation needs and what a loop over single digests cannot
provide -- a loop hands out one digest at a time and spends more on
coordination than it saves. Replacing that one function distributes the range;
the streaming logic above it is untouched.

Two rules the caller owns, and they are simple because the state is small:

- One `ub_state` must not be mutated from two threads.
- A prefix state used only through `ub_hash_tail` or `ub_hash_n` is never
  mutated, so sharing it read-only is safe. To stream on several threads,
  give each its own copy via `ub_copy`.

`backends/hash_n_threads.c` is a working pthreads implementation of exactly
this, kept as a prototype rather than shipped for the reasons above.

