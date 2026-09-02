#!/usr/bin/env python3
"""Read measurements.tsv and print it for a human.

The stored format is deliberately boring -- greppable, diffable, sortable.
This is the other half of that trade: the readable view is generated, not
stored, so the file never has to be both.

  tools/bench.py                    latest run, one table
  tools/bench.py --runs             what runs exist, newest first
  tools/bench.py --metric leaf      rows matching a metric, across runs
  tools/bench.py --compare          latest two runs of the same shape, side by side
"""
import argparse, sys, os
from collections import OrderedDict

def sig(v, unit, disp=""):
    """Print a value to the precision the measurement supports.

    Digits come from the measured dispersion, not from the magnitude of the
    number: rounding by size claims a precision the sample may not have, and
    hides one it may. The rule is one digit past the first significant digit
    of the dispersion, which is where the value stops being repeatable.

    Without a dispersion the value is printed as recorded, unrounded. That is
    the honest default: the harness did not say how well it knows this, so the
    viewer does not invent an answer."""
    try:
        d = float(disp)
    except (TypeError, ValueError):
        d = 0.0
    if d > 0:
        import math
        places = max(0, -int(math.floor(math.log10(d))) + 1)
        return f"{v:.{places}f}"
    if unit == "count":
        return f"{v:.0f}"
    return f"{v:g}"

def load(path):
    rows, fmt = [], ""
    with open(path) as f:
        head = None
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#unibench"): fmt = line; continue
            if not line: continue
            if head is None: head = line.split("\t"); continue
            rows.append(dict(zip(head, line.split("\t"))))
    return rows, fmt

def runs(rows):
    """Group by (utc, project, run) preserving file order -- newest first."""
    g = OrderedDict()
    for r in rows:
        g.setdefault((r["utc"], r["project"], r["run"]), []).append(r)
    return g

# Fields that must agree before two rows are comparable. Differing on any of
# these does not make one row wrong; it makes the pair a comparison of two
# different things, which is how most bad conclusions get made.
COMPARABLE = ("platform", "compiler", "oracle", "oracle_flags", "kind", "exec")

def incomparable(a, b):
    return [f for f in COMPARABLE if a.get(f, "") != b.get(f, "")]

def show(key, rs, verbose=True):
    utc, proj, run = key
    h = rs[0]
    if verbose:
        print(f"{proj}  {run}   {utc}")
        print(f"  {h['platform']}  {h['cpu']}")
        print(f"  commit {h['commit']} ({h['dirty']})  {h['compiler'][:48]}")
        if h["oracle"]:
            print(f"  oracle libsodium {h['oracle']} [{h['oracle_flags']}]")
        print()
    w = max(len(r["metric"]) for r in rs)
    for r in rs:
        n = f"  n={r['n']}" if r["n"] else ""
        d, d2 = r.get("dispersion", ""), r.get("dispersion2", "")
        # Two robust estimators, printed together. IQR is a quantile spread and
        # MAD is a median of deviations; on a symmetric sample they track each
        # other, and a gap between them is the tail showing.
        sp = ""
        if d:
            sp = f"  {r.get('disp_kind','')} {d}"
            if d2: sp += f"  {r.get('disp2_kind','')} {d2}"
        print(f"  {r['metric']:<{w}}  {sig(float(r['value']), r['unit'], d):>10} {r['unit']}{n}{sp}")
    print()

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--file", default="measurements.tsv")
    p.add_argument("--runs", action="store_true")
    p.add_argument("--metric")
    p.add_argument("--compare", action="store_true")
    p.add_argument("--all", action="store_true")
    p.add_argument("--seq", default="",
                   help="subset by sequence number: 5, 5-12, or 5,7,9-12. "
                        "The file runs N..1 top to bottom, newest first.")
    p.add_argument("--superseded", action="store_true",
                   help="include rows a later run marked as replaced")
    p.add_argument("--agg", action="store_true",
                   help="aggregate repeated runs of the same shape: median and spread")
    a = p.parse_args()
    if not os.path.exists(a.file): sys.exit(f"no {a.file}")
    rows, fmt = load(a.file)
    if not rows: sys.exit("no measurements")
    if a.seq:
        want = set()
        for part in a.seq.split(","):
            part = part.strip()
            if "-" in part:
                lo, hi = part.split("-", 1)
                want.update(range(int(lo), int(hi) + 1))
            elif part:
                want.add(int(part))
        rows = [r for r in rows if r.get("seq", "").isdigit() and int(r["seq"]) in want]
        if not rows:
            sys.exit(f"no rows with seq in {a.seq}")
    if not a.superseded:
        dead = set()
        for r in rows:
            for x in (r.get("supersedes") or "").split(","):
                if x.strip(): dead.add(x.strip())
        rows = [r for r in rows if r.get("seq") not in dead]
    g = runs(rows)

    if a.runs:
        print(f"{'when':21} {'project':10} {'run':20} {'platform':24} rows")
        for (utc, proj, run), rs in g.items():
            print(f"{utc:21} {proj:10} {run:20} {rs[0]['platform']:24} {len(rs)}")
    elif a.metric:
        for (utc, proj, run), rs in g.items():
            m = [r for r in rs if a.metric in r["metric"]]
            if m:
                print(f"{utc}  {proj}/{run}  {rs[0]['platform']}")
                for r in m:
                    print(f"    {r['metric']:<34} {sig(float(r['value']), r['unit'], r.get('dispersion','')):>10} {r['unit']}")
    elif a.agg:
        # Repeated runs of one shape are the common case for a stable figure.
        # The file keeps every run; this shows what they agree on.
        import statistics as st
        by = OrderedDict()
        for r in rows:
            # Aggregating a cumulative average over samples is meaningless --
            # localsolps drifts monotonically and its spread says nothing.
            if r.get("kind") == "cumulative":
                continue
            by.setdefault((r["project"], r["run"], r["platform"], r["metric"], r["unit"]),
                          []).append(float(r["value"]))
        cur = None
        for (proj, run, plat, metric, unit), vs in by.items():
            if (proj, run, plat) != cur:
                cur = (proj, run, plat)
                print(f"\n{proj}/{run}  {plat}")
                print(f"  {'metric':<34} {'runs':>4} {'median':>10} {'min':>10} {'max':>10}")
            print(f"  {metric:<34} {len(vs):>4} {sig(st.median(vs),unit):>10} "
                  f"{sig(min(vs),unit):>10} {sig(max(vs),unit):>10}  {unit}")
        print()
    elif a.compare:
        shapes = OrderedDict()
        for k, rs in g.items(): shapes.setdefault((k[1], k[2]), []).append((k, rs))
        for (proj, run), lst in shapes.items():
            if len(lst) < 2: continue
            (k1, a1), (k2, a2) = lst[0], lst[1]
            diff = incomparable(a1[0], a2[0])
            print(f"{proj}/{run}   newer {k1[0]}   older {k2[0]}")
            if diff:
                print(f"  NOT COMPARABLE: differ on {', '.join(diff)}")
                for f in diff:
                    print(f"    {f}: {a1[0].get(f,'')!r} vs {a2[0].get(f,'')!r}")
                print("  Difference shown for reference only.")
            if a1[0].get("exec") != "native" or a2[0].get("exec") != "native":
                print("  One or both runs were not native; timings are not valid.")
            old = {r["metric"]: float(r["value"]) for r in a2}
            w = max(len(r["metric"]) for r in a1)
            for r in a1:
                nv, ov = float(r["value"]), old.get(r["metric"])
                if ov is None: continue
                d = nv - ov
                pct = (d / ov * 100) if ov else 0
                print(f"  {r['metric']:<{w}} {sig(nv,r['unit'],r.get('dispersion','')):>9} vs {sig(ov,r['unit'],r.get('dispersion','')):>9}"
                      f"  {d:+7.2f} ({pct:+.1f}%)")
            print()
    else:
        k = next(iter(g)); show(k, g[k])
        if a.all:
            for k in list(g)[1:]: show(k, g[k])
    if fmt: print(fmt.replace("#unibench ", "").strip(), file=sys.stderr)

if __name__ == "__main__":
    main()
