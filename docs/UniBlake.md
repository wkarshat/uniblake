# UniBlake

## Where BLAKE2b comes from

BLAKE2 (2012) is a refinement of BLAKE, a SHA-3 finalist, by Aumasson,
Neves, Wilcox-O'Hearn and Winnerlein. It kept BLAKE's security margin while
cutting rounds and dropping padding overhead, on the argument that a hash
which is fast in software discourages people from reaching for faster but
weaker constructions. BLAKE2b is the 64-bit variant: 128-byte blocks, digests
up to 64 bytes. RFC 7693 specifies it, and the reference implementation at
github.com/BLAKE2/BLAKE2 defines the API most libraries follow.

Its distinguishing feature is the parameter block — 64 bytes of "what am I
computing" (digest length, key length, salt, personalization, tree layout)
mixed into the initial state rather than into the message. Two hashes with
different personalization strings are unrelated functions, at no cost per
message. This is why BLAKE2b is common in protocol work: domain separation is
free and built in.

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

**No BLAKE2s, no BLAKE3.** BLAKE2s (32-bit words, 64-byte blocks) suits small
machines and short digests; BLAKE3 replaces the parameter block with three
fixed modes and is intrinsically parallel. Both are different algorithms, not
options here.

**No shared segment except at the start.** This is inherent, not a shortcut:
the state depends on every byte absorbed so far, so in `varying || fixed` the
shared part cannot be precomputed. Only `fixed || varying` benefits, and the
state must be rebuilt whenever `fixed` changes.

**No thread safety inside a state.** One state, one thread. A prefix state
used only for hashing is never modified, so it may be shared read-only; to
stream on several threads, give each its own copy.

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
caller has: a nonce, a sparse index, a string. `ub_hash_n` cannot express it,
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

## Sizing

One rule governs the prefix case:

    pending(prefixlen) + taillen <= 128
    pending(n) = n ? ((n - 1) % 128) + 1 : 0

BLAKE2b keeps a *full* trailing block rather than compressing it, because
finalization has to mark the last block. A prefix that is an exact multiple of
128 therefore leaves 128 bytes pending and admits no tail at all — hash one
byte less, or accept two compressions per digest.

## Where to go next

The README lists the other documents and who they are for.
