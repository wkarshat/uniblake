#!/usr/bin/env python3
"""A/B two benchmark binaries and report what the data can and cannot resolve.

Exists because "no change" and "within noise" were being written as if they
were measurements. They are not: a difference smaller than the run-to-run
spread is *unresolved*, which is a statement about the harness, not about the
code. This prints the resolution alongside the estimate so the distinction
cannot be skipped.

Method: alternate A and B (never all-A then all-B -- thermal and frequency
drift is monotonic over a session and would land entirely on whichever ran
second), discard the first pair, report medians, the paired differences, and
a bootstrap interval on the median difference.

Usage: tools/ab_compare.py <binA> <binB> [--runs N] [--grep PATTERN]
"""
import subprocess, sys, re, random, statistics as st

def sample(binary, pattern):
    out = subprocess.run([binary], capture_output=True, text=True).stdout
    for line in out.split('\n'):
        if re.search(pattern, line):
            nums = re.findall(r'\d+\.\d+', line)
            if nums: return float(nums[0])
    raise SystemExit(f"no line matching {pattern!r} in {binary} output")

def bootstrap_ci(diffs, iters=20000, alpha=0.05):
    meds = []
    n = len(diffs)
    for _ in range(iters):
        meds.append(st.median([diffs[random.randrange(n)] for _ in range(n)]))
    meds.sort()
    return meds[int(iters * alpha / 2)], meds[int(iters * (1 - alpha / 2))]

def main():
    a_bin, b_bin = sys.argv[1], sys.argv[2]
    runs = 12
    pattern = r'full leaf'
    if '--runs' in sys.argv: runs = int(sys.argv[sys.argv.index('--runs') + 1])
    if '--grep' in sys.argv: pattern = sys.argv[sys.argv.index('--grep') + 1]

    A, B = [], []
    for i in range(runs):
        A.append(sample(a_bin, pattern))     # alternate, never batched
        B.append(sample(b_bin, pattern))
    A, B = A[1:], B[1:]                      # discard first pair: startup transient

    diffs = [b - a for a, b in zip(A, B)]
    ma, mb = st.median(A), st.median(B)
    spread = max(max(A) - min(A), max(B) - min(B))
    lo, hi = bootstrap_ci(diffs)
    d = st.median(diffs)

    print(f"pairs (first discarded)  {len(A)}")
    print(f"A  {a_bin}   median {ma:7.2f}   spread {max(A)-min(A):.2f}")
    print(f"B  {b_bin}   median {mb:7.2f}   spread {max(B)-min(B):.2f}")
    print(f"paired difference B-A    {d:+.2f}   95% CI [{lo:+.2f}, {hi:+.2f}]")
    print(f"harness resolution       ~{spread:.2f} ns (worst within-variant spread)")
    print()
    if lo <= 0 <= hi:
        print(f"VERDICT: UNRESOLVED. The interval spans zero, so this harness at")
        print(f"  {len(A)} pairs cannot tell these apart. Report as unresolved --")
        print(f"  NOT as 'no change', which claims a measurement that was not made.")
        print(f"  To resolve a difference this small, raise --runs or lower the")
        print(f"  per-sample noise; |d| = {abs(d):.2f} vs resolution {spread:.2f}.")
    else:
        direction = "SLOWER" if d > 0 else "FASTER"
        print(f"VERDICT: RESOLVED. B is {direction} by {abs(d):.2f} ns")
        print(f"  (95% CI excludes zero).")

if __name__ == '__main__':
    if len(sys.argv) < 3: sys.exit(__doc__)
    main()
