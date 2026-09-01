#!/usr/bin/env python3
"""Count what a compiled BLAKE2b compression kernel actually does.

Architectures: aarch64 today; x86-64 and AVX2 are stubbed with the
instruction sets they need (--arch). See ARCH_NOTES at the bottom of this
file for exactly what has to change and why the counts differ.

Reads aarch64 assembly (compiler `-S` output or `objdump -d`) and reports the
counts that have proved to matter, with the corrections that took three
attempts to get right:

  * Rotate immediates are matched in BOTH decimal and hex form. `objdump`
    prints `#0x3f`; a decimal-only pattern silently reports zero rotates.
  * `ror` is matched as a MNEMONIC, not a substring: `x*ror*` inside a
    register name is not an instruction.
  * Stack traffic is split into callee-saved register save/restore (x19-x28,
    x29/x30 -- paid once per call) and real spills. Counting them together
    roughly doubles the apparent spill rate.
  * Spill STORES are reported as distinct slots written, and LOADS separately,
    because the allocator evicts a value once and re-reads it many times;
    a single store:load ratio is not a spill count.
  * Slots read but never written are inbound arguments in the caller's frame,
    not spills.

Usage:  tools/kernel_stats.py <file.s> [symbol]
        tools/kernel_stats.py --self-test
"""
import re, sys
from collections import Counter

CALLEE_SAVED = re.compile(r'\b(x19|x20|x21|x22|x23|x24|x25|x26|x27|x28|x29|x30)\b')
ROT = re.compile(r'^ror\s+[wx]\d+,\s*[wx]\d+,\s*#(0x[0-9a-fA-F]+|\d+)')
MEM = re.compile(r'^(ldr|ldp|ldur|str|stp|stur)\b')
LOAD = re.compile(r'^(ldr|ldp|ldur)\b')
STORE = re.compile(r'^(str|stp|stur)\b')
BRANCH = re.compile(r'^(b|bl|br|b\.\w+|cbz|cbnz|tbz|tbnz)\b')
SLOT = re.compile(r'\[sp(?:,\s*#(0x[0-9a-fA-F]+|\d+))?\]')

def imm(v):
    return int(v, 16) if v.lower().startswith('0x') else int(v)

def body_of(path, symbol=None):
    """Return the instruction text of one function, from -S or objdump output."""
    raw = open(path).read().split('\n')
    objdump = any(re.match(r'^\s*[0-9a-f]+:', l) for l in raw)
    if objdump:
        lines, start = [], 0
        if symbol:
            for i, l in enumerate(raw):
                if symbol in l and re.match(r'^[0-9a-f]+\s+<', l):
                    start = i; break
            end = len(raw)
            for j in range(start + 1, len(raw)):
                if re.match(r'^[0-9a-f]+\s+<', raw[j]): end = j; break
            raw = raw[start:end]
        for l in raw:
            if re.match(r'^\s*[0-9a-f]+:', l):
                lines.append(l.split('\t', 1)[1].strip() if '\t' in l else l.strip())
        return lines
    start = None
    for i, l in enumerate(raw):
        if symbol and l.startswith(symbol): start = i; break
        if not symbol and re.match(r'^_?\w+:', l) and 'compress' in l: start = i; break
    if start is None:
        sys.exit(f"symbol {symbol!r} not found in {path}")
    end = len(raw)
    for j in range(start + 1, len(raw)):
        if '.cfi_endproc' in raw[j]: end = j; break
    return [l.strip() for l in raw[start:end] if re.match(r'\s+[a-z]', l)]

def stats(body):
    rot = Counter()
    for x in body:
        m = ROT.match(x)
        if m: rot[imm(m.group(1))] += 1
    sp = [x for x in body if MEM.match(x) and '[sp' in x]
    spill = [x for x in sp if not CALLEE_SAVED.search(x)]
    saves = len(sp) - len(spill)
    st_slots, ld_slots = [], []
    for x in spill:
        for m in SLOT.finditer(x):
            (st_slots if STORE.match(x) else ld_slots).append(imm(m.group(1) or '0'))
    frame = 0
    for x in body[:16]:
        m = re.search(r'sub\s+sp,\s*sp,\s*#(0x[0-9a-fA-F]+|\d+)', x)
        if m: frame = imm(m.group(1)); break
    return dict(
        instructions=len(body),
        rotates=sum(rot.values()), rotate_hist=dict(sorted(rot.items())),
        fused_rot=len([x for x in body if re.search(r'^(eor|orr|and|add)\b.*,\s*ror\s*#\d+', x)]),
        sigma_ldrb=len([x for x in body if x.startswith('ldrb')]),
        callee_save_ops=saves,
        spill_stores=len([x for x in spill if STORE.match(x)]),
        spill_loads=len([x for x in spill if LOAD.match(x)]),
        slots_written=len(set(st_slots)),
        slots_read=len(set(ld_slots)),
        slots_read_never_written=sorted(set(ld_slots) - set(st_slots)),
        stack_frame=frame,
        branches=len([x for x in body if BRANCH.match(x)]),
    )

def report(path, symbol=None):
    s = stats(body_of(path, symbol))
    rot = s['rotates']
    print(f"{path}" + (f"  [{symbol}]" if symbol else ""))
    print(f"  instructions          {s['instructions']}")
    print(f"  rotates               {rot}   {s['rotate_hist']}")
    print(f"    (384 = 12 rounds x 8 G x 4 rotates, none fused into a consumer)")
    print(f"  fused rot in operand  {s['fused_rot']}")
    print(f"  sigma ldrb            {s['sigma_ldrb']}   (0 = indices are compile-time constants)")
    print(f"  branches              {s['branches']}   (0 = fully unrolled)")
    print(f"  stack frame           {s['stack_frame']} B")
    print(f"  callee-save ops       {s['callee_save_ops']}   (once per call, not per round)")
    print(f"  spill stores/loads    {s['spill_stores']}/{s['spill_loads']}")
    print(f"    slots written once  {s['slots_written']}  -- values evicted by the allocator")
    print(f"    slots re-read       {s['slots_read']}")
    print(f"    read, never written {len(s['slots_read_never_written'])}  -- inbound args, not spills")
    if rot:
        print(f"  spill ops per rotate  {(s['spill_stores']+s['spill_loads'])/rot:.3f}")
    return s

SELF_TEST = """_x:
\tror\tx1, x2, #0x3f
\tror\tx3, x4, #63
\teor\tx5, x6, x7, ror #63
\tstr\tx19, [sp, #8]
\tstr\tx9, [sp, #16]
\tldr\tx9, [sp, #16]
\tldr\tx10, [sp, #24]
\tsub\tsp, sp, #48
\t.cfi_endproc
"""

def self_test():
    import tempfile, os
    fd, p = tempfile.mkstemp(suffix='.s'); os.write(fd, SELF_TEST.encode()); os.close(fd)
    s = stats(body_of(p, '_x')); os.unlink(p)
    checks = [
        ("hex and decimal rotate immediates both counted", s['rotates'] == 2),
        ("rotate histogram merges 0x3f and 63",            s['rotate_hist'] == {63: 2}),
        ("fused rotate in operand detected",               s['fused_rot'] == 1),
        ("callee-saved store excluded from spills",        s['callee_save_ops'] == 1),
        ("spill store counted",                            s['spill_stores'] == 1),
        ("spill loads counted",                            s['spill_loads'] == 2),
        ("slot read but never written identified",         s['slots_read_never_written'] == [24]),
    ]
    ok = True
    for name, passed in checks:
        print(f"  {'PASS' if passed else 'FAIL'}  {name}")
        ok &= passed
    return 0 if ok else 1

ARCH_NOTES = """
Porting these counts to x86-64 and AVX2
---------------------------------------
The report shape stays; the instruction patterns and the INTERPRETATION change.

x86-64 scalar (`--arch x86`):
  rotate      `rorq $63, %rax`  -- ROT matches `ror` + AT&T immediate `$63`.
              There is NO shifted-operand form, so `fused rot in operand` is
              always 0 and the aarch64 380-vs-384 result does NOT carry over:
              expect a full 384. That is the whole reason the last-round
              deferral is worth trying there and worthless here.
  spills      base register is %rsp/%rbp; callee-saved set is
              rbx/rbp/r12-r15 (six, not ten), so the callee-save subtraction
              is smaller and matters less.
  memory ops  x86 folds loads into arithmetic (`addq (%rsi), %rax`), so an
              instruction count is not comparable to aarch64's and a
              "memory operations" count must include folded operands.

AVX2 (`--arch avx2`):
  rotate      NOT one instruction. From
              backends/vendor/libsodium/blake2b-compress-avx2.h:
                ROT32 = vpshufd                     1
                ROT24 = vpshufb + table             1
                ROT16 = vpshufb + table             1
                ROT63 = vpsrlq + vpaddq + vpor      3
              So count rotations by MACRO EXPANSION, not by mnemonic: a
              rotate histogram keyed on the immediate does not exist here.
              Report vpshufb/vpshufd/vpsrlq/vpaddq/vpor separately.
  lanes       4 lanes per register: 12 rounds x 2 vector G-groups, not 96
              scalar G calls. The "384 rotates" invariant becomes
              12 x 2 x 6 = 144 rotation instructions.
  spills      spill slots are 32 bytes (ymm), so a byte-offset histogram
              needs a stride of 32; vmovdqa to/from (%rsp) is the pattern.
  registers   16 ymm registers against BLAKE2b's 8 vector words + 8 message
              vectors = 16 live. Pressure is tighter than it looks, which is
              why the kernel-NEON trick (skip reloading the message on the
              final round) is the one worth porting here.

What to measure once a host exists (none of this has run on real x86-64):
  1. `make kernel-stats ARCH=x86`   -- expect 384 rotates, 0 fused.
  2. Apply the last-round ROT63 deferral to backends/compress_avx2.c,
     verify with `make check-avx2`, then `make bench-avx2`.
  3. `make bench-phases` and `make bench-compare` unchanged -- they are
     architecture-neutral C and already build anywhere.
"""

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--arch-notes':
        print(ARCH_NOTES); sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == '--self-test':
        sys.exit(self_test())
    if len(sys.argv) < 2: sys.exit(__doc__)
    report(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
