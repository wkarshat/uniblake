#!/usr/bin/env python3
"""Turn benchmark output into measurement rows, newest first.

A figure without its conditions is unusable: "libsodium 1.0.21" named two
builds 64% apart on the same machine, because the version was recorded and the
optimisation level was not. Every row here carries what it takes to reproduce
or to refuse a comparison.

  tools/record.py --project uniblake --platform mac-m4 --run bench-phases \
      --oracle-version 1.0.21 --oracle-flags '-O1' < output.txt

Reads a harness's stdout, appends rows to measurements.tsv at the TOP of the
data section, so the newest run is what a reader sees first.

Rows are facts about one moment. They are authoritative about WHAT WAS
MEASURED, WHEN, UNDER WHAT -- never about what is true now. Only re-running
tells you that.
"""
import argparse, os, re, subprocess, sys, time

FORMAT = "unibench/1"

# Standard repetition counts. A menu, not a free number: two runs at 7 and 9
# reps are not comparable, and nothing is gained by the difference. Higher is
# better where the duration allows.
REPS_MENU = (5, 10, 100)

HEADER = [
    "seq", "utc", "project", "commit", "dirty", "platform", "cpu", "compiler",
    "oracle", "oracle_flags", "run", "metric", "value", "unit", "n", "reps",
    "kind", "exec", "dispersion", "disp_kind", "dispersion2", "disp2_kind",
    "supersedes", "note",
]

# How a value relates to time. A cumulative average and an instantaneous rate
# are different quantities; reporting a spread over samples of the first is
# meaningless, which is a mistake this column exists to prevent.
KINDS = ("point", "median", "cumulative", "series", "share", "count")

# How the binary ran. An emulated binary gives correct digests and meaningless
# timings; without this the two are indistinguishable in the file.
EXECS = ("native", "emulated", "cross")

def sh(cmd, cwd=None):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              cwd=cwd, timeout=20).stdout.strip()
    except Exception:
        return ""

def git_state(path):
    c = sh("git rev-parse --short HEAD", path) or "nogit"
    d = "dirty" if sh("git status --porcelain", path) else "clean"
    return c, d

def load_vocab(name, col=0):
    """Read a vocabulary file next to this script. Comments and blanks skipped."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    out = set()
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    out.add(line.split("\t")[col])
    except OSError:
        pass
    return out

def check_metric(metric, prefixes, cats):
    """A metric is <prefix>[.<category>][ <qualifier>]. Reject an unknown
    prefix or category so a typo fails loudly instead of producing a row that
    silently never groups with its siblings."""
    stem = metric.split(" ", 1)[0]
    parts = stem.split(".")
    if parts[0] not in prefixes:
        return f"unknown metric prefix '{parts[0]}' (see tools/metrics.tsv)"
    if len(parts) > 1 and cats:
        cat = ".".join(parts[1:])
        if cat not in cats and parts[0] in ("algo", "sync", "zero"):
            return f"unknown category '{cat}' (see tools/categories.tsv)"
    return None

def binary_is_stale(binary, src_dir):
    """True when the binary predates the newest source file.

    A comparison against a binary built before the change under test measures
    two codebases, not one change. That happened: a -0.72 ns "improvement" was
    a pre-merge build compared against a post-merge one."""
    try:
        bt = os.path.getmtime(binary)
    except OSError:
        return None
    newest = 0.0
    for root, _dirs, files in os.walk(src_dir):
        if any(part in root for part in (".git", "build", "target")):
            continue
        for fn in files:
            if fn.endswith((".c", ".h", ".rs")):
                try: newest = max(newest, os.path.getmtime(os.path.join(root, fn)))
                except OSError: pass
    return newest > bt

def next_seq(path):
    """Highest seq in the file plus one. Sequence numbers are the stable handle
    for a row: utc collides when two runs land in the same second, and a
    (project, run, metric) triple is not unique across repeats."""
    hi = 0
    try:
        with open(path) as f:
            for line in f:
                if line.startswith(("seq\t", "#")): continue
                head = line.split("\t", 1)[0]
                if head.isdigit(): hi = max(hi, int(head))
    except OSError:
        pass
    return hi + 1

def machine_uuid():
    """Stable per-machine id. Kept out of every row -- it answers "are these two
    hosts the same?", which is asked rarely and clutters the common case."""
    v = sh("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | "
           "awk -F'\"' '/IOPlatformUUID/{print $4}'")
    if not v:
        try:
            with open("/etc/machine-id") as f: v = f.read().strip()
        except OSError: v = ""
    return (v or "unknown").lower()[:8]

def detect_cpu():
    v = sh("sysctl -n machdep.cpu.brand_string")
    if v: return v
    v = sh("grep -m1 'model name' /proc/cpuinfo")
    return v.split(":", 1)[1].strip() if ":" in v else (v or "unknown")

def detect_compiler():
    v = sh(f"{os.environ.get('CC','cc')} --version")
    return v.split("\n")[0] if v else "unknown"

# Each harness gets one parser. Adding a harness means adding one function,
# not changing the format.
# Dispersion reported by the harness, when it reports one. A median without a
# spread says nothing about how well it is known, and digits inferred from
# magnitude claim a precision the measurement may not have.
DISP_KINDS = ("", "iqr", "stddev", "range", "ci95")

PHASE_NAMES = {
    "state copy": "phase.copy",
    "+ update(4B)": "phase.update",
    "+ ub_final (full leaf)": "phase.leaf",
    "ub_compress, tight loop": "phase.compress_alone",
}

def parse_phases(text):
    for line in text.splitlines():
        m = re.match(r"\s+(state copy|\+ update\(4B\)|\+ ub_final \(full leaf\)|ub_compress, tight loop)\s+([\d.]+)", line)
        if m:
            d = re.search(r"iqr\s+([\d.]+)", line)
            d2 = re.search(r"mad\s+([\d.]+)", line)
            yield (PHASE_NAMES[m.group(1).strip()], float(m.group(2)), "ns/digest",
                   ("", "", d.group(1) if d else "", "iqr" if d else "",
                    d2.group(1) if d2 else "", "mad" if d2 else ""))
    m = re.search(r"N=(\d+) reps=(\d+)", text)
    if m: yield "_meta", None, None, (m.group(1), m.group(2))

def parse_compare(text):
    section = None
    for line in text.splitlines():
        if line.startswith("[leaf]"): section = "leaf"; continue
        if line.startswith("[bulk]"): section = "bulk"; continue
        if line.startswith("#") or not line.strip() or line[0].isalpha(): continue
        f = line.split(",")
        if section == "leaf" and len(f) == 4:
            n = f[0]
            yield "leaf.blake2b", float(f[1]), "ns/digest", (n, "")
            yield "leaf.blake2b 2threads", float(f[2]), "ns/digest", (n, "")
            yield "leaf.blake2b reference", float(f[3]), "ns/digest", (n, "")
        elif section == "bulk" and len(f) == 3:
            b = f[0]
            yield "bulk.blake2b", float(f[1]), "MB/s", (b, "")
            yield "bulk.blake2b reference", float(f[2]), "MB/s", (b, "")

def parse_solve_tsv(text):
    # ZeroPerf solver_timing: solver, nonce, secs, nsols, raw
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) >= 4 and f[1].isdigit():
            yield f"solve.equihash {f[0]}", float(f[2]), "s", (f[1], "")
            yield f"nsols.equihash {f[0]}", float(f[3]), "count", (f[1], "")

PARSERS = {"bench-phases": parse_phases, "bench-compare": parse_compare,
           "bench-compare-avx2": parse_compare, "solve-timing": parse_solve_tsv}

# Harnesses that link no oracle. Recording an oracle version against one of
# these attributes a dependency the binary does not have, which is worse than
# leaving the column empty: it invites a comparison that means nothing.
NO_ORACLE = {"bench-phases", "bench-isa", "solve-timing"}

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--project", required=True)
    p.add_argument("--platform", default="",
                   help="stable id; derived by tools/platform_id.sh when omitted")
    p.add_argument("--run", required=True, choices=sorted(PARSERS))
    p.add_argument("--src", default=".", help="repo the commit is read from")
    p.add_argument("--oracle-version", default="")
    p.add_argument("--oracle-flags", default="")
    p.add_argument("--kind", default="median", choices=KINDS,
                   help="how the value relates to time; see KINDS")
    p.add_argument("--exec", dest="execmode", default="native", choices=EXECS,
                   help="native, or emulated/cross when timings are not valid")
    p.add_argument("--binary", default="",
                   help="the harness binary; checked against source mtimes")
    p.add_argument("--supersedes", default="",
                   help="comma-separated seq numbers this run replaces, e.g. 1,2,11")
    p.add_argument("--note", default="")
    p.add_argument("--out", default="measurements.tsv")
    a = p.parse_args()

    if a.run in NO_ORACLE and (a.oracle_version or a.oracle_flags):
        sys.exit(f"record.py: --run {a.run} links no oracle; drop --oracle-version/--oracle-flags")
    if a.binary:
        stale = binary_is_stale(a.binary, a.src)
        if stale:
            sys.exit(f"record.py: {a.binary} is older than the newest source in "
                     f"{a.src}; rebuild before measuring")
    text = sys.stdin.read()
    if not a.platform:
        here = os.path.dirname(os.path.abspath(__file__))
        a.platform = sh(os.path.join(here, "platform_id.sh")) or "unknown"
    commit, dirty = git_state(a.src)
    utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    cpu, cc = detect_cpu(), detect_compiler()

    n = reps = ""
    prefixes = load_vocab("metrics.tsv")
    cats = load_vocab("categories.tsv")
    seq = next_seq(a.out)
    rows, bad = [], []
    for metric, value, unit, extra in PARSERS[a.run](text):
        if metric == "_meta":
            n, reps = extra; continue
        err = check_metric(metric, prefixes, cats)
        if err: bad.append(f"{metric}: {err}")
        ex = extra if isinstance(extra, tuple) else ("", "")
        en, ereps = ex[0], ex[1]
        disp, dkind = (ex[2], ex[3]) if len(ex) >= 4 else ("", "")
        disp2, d2kind = (ex[4], ex[5]) if len(ex) >= 6 else ("", "")
        rows.append([str(seq), utc, a.project, commit, dirty, a.platform, cpu, cc,
                     a.oracle_version, a.oracle_flags, a.run, metric,
                     f"{value:g}", unit, en or n, ereps or reps,
                     a.kind, a.execmode, disp, dkind, disp2, d2kind,
                     a.supersedes, a.note])
        seq += 1
    if bad:
        sys.exit("record.py: " + "; ".join(sorted(set(bad))))
    if not rows:
        sys.exit(f"record.py: no rows parsed for --run {a.run}; is this the right harness output?")

    body = ""
    if os.path.exists(a.out):
        with open(a.out) as f:
            old = f.read()
        # Strip the header and the format trailer; both are rewritten below.
        lines = [l for l in old.split("\n")
                 if l and not l.startswith("seq\t") and not l.startswith("#unibench")]
        body = "\n".join(lines) + ("\n" if lines else "")
    with open(a.out, "w") as f:
        f.write("\t".join(HEADER) + "\n")
        for r in rows: f.write("\t".join(r) + "\n")   # newest first
        f.write(body)
        # Trailer, not header: the top of the file belongs to the newest
        # measurement, so metadata sits where it is out of the way.
        f.write(f"#unibench format={FORMAT} machine_uuid={machine_uuid()}\n")
    print(f"recorded {len(rows)} rows to {a.out}", file=sys.stderr)

if __name__ == "__main__":
    main()
