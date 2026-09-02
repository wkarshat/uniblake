# UniBench

Measurement format shared by uniblake and its consumers.
Storage: TSV. Viewer: tools/bench.py. Recorder: tools/record.py.

## File

measurements.tsv, one row per metric, newest first.
Header line first, format trailer last.

    utc project commit dirty platform cpu compiler oracle oracle_flags
    run metric value unit n reps note

    #unibench format=unibench/1 machine_uuid=<8 hex>

A row states what was measured, when, on what. It does not state what is true
now: a measurement holds for one commit, one machine, one compiler, one oracle
build. Only re-running gives the current answer.

Every row is citable. dirty=dirty weakens attribution, since the exact source
is not recoverable from a commit, but date, machine, compiler and oracle are
recorded and the figure remains meaningful.

## Failure modes this format guards against

Each of these produced a wrong published conclusion before the guard existed.

    stale binary       A binary older than the source measures a different
                       codebase. record.py --binary compares mtimes and
                       refuses. Caught a -0.72 ns "improvement" that was a
                       pre-merge build against a post-merge one.

    wrong oracle       oracle_flags is required whenever oracle is set. A
                       282-vs-185 ns gap was attributed to a libsodium
                       version; it was -O1 against -O2 of one source.

    false oracle       record.py rejects --oracle-version on a run that links
                       none, rather than recording a dependency the binary
                       does not have.

    emulation          exec=emulated marks a binary whose digests are correct
                       and whose timings are not. The viewer refuses to
                       compare across it.

    cumulative rate    kind=cumulative marks a running average. Its spread
                       over samples is meaningless; --agg skips it. Reporting
                       min/max of a cumulative Sol/s is a category error.

    incomparable pair  The viewer names the fields two runs differ on --
                       platform, compiler, oracle, oracle_flags, kind, exec --
                       and labels the difference reference-only.

    undersampling      reps is recorded per row. A difference smaller than the
                       run-to-run spread is unresolved, not zero: "no change"
                       was published for a change that resolved at -0.43 ns
                       once the sample count was raised.

Not guarded, because they are not measurements: citing a stale tag, describing
another project's code from memory, miscounting something without looking.
Those need verification before assertion, not a column.

What can be collected to raise suspicion where a guard is impossible:

    binary_mtime    Recorded next to the commit. A binary far older than the
                    commit is not proof of staleness -- ccache and untouched
                    sources both explain it -- but it is worth a second look.

    duration        Wall time of the whole run. A run that finishes far faster
                    than its siblings usually did less work: a filtered test,
                    a skipped arm, an early exit.

    checksum        A digest of the harness's full output. Two runs claiming
                    identical conditions and producing byte-identical output
                    were probably the same binary, not a reproduction.

    environment     One field for the machine's condition during the run:
                    load average, thermal or power state, governor, and
                    whatever else the platform exposes. A figure taken on a
                    busy or throttling machine is not wrong, but a spread that
                    will not close, or a long run that drifts monotonically
                    rather than converging, is explained by it. Collected
                    where the platform offers it, absent where it does not,
                    and never a reason to discard a measurement.

None of these decides anything on its own. Each is cheap to collect and turns
a silent wrong answer into a visible oddity, which is the most that can be
asked where the failure is a judgement rather than a fact.

## Columns

    utc           ISO 8601 UTC, second resolution
    project       uniblake | zeroperf | zero
    commit        short hash of the source repo
    dirty         clean | dirty
    platform      derived, see below
    cpu           model string as the OS reports it
    compiler      first line of cc --version
    oracle        libsodium version, empty when none is linked
    oracle_flags  its build flags; required whenever oracle is set
    run           harness name, matches a parser in record.py
    metric        <prefix>.<category>[ <qualifier>]
    value         as measured, unrounded
    unit          ns/digest | MB/s | s | pct | count | MB | ns
    n             problem size for the row, if any
    reps          timed repetitions the value is a median over
    kind          point | median | cumulative | series | share | count
    exec          native | emulated | cross
    note          free text

Each column earns its place by naming something that, if it differed
silently, would make two rows look comparable when they are not.

    seq           stable handle; utc collides within a second and
                  (project, run, metric) repeats. The file runs N..1 top to
                  bottom -- newest first, 1 at the end -- so a subset is
                  selected with --seq 5, 5-12 or 5,7,9-12
    utc           when; the only ordering that survives editing
    project       which codebase
    commit,dirty  which source; dirty says the source is not recoverable
    platform      grouping key, derived from cpu
    cpu           the raw fact platform is a slug for; two hosts can share a
                  platform id and differ in stepping
    compiler      the same machine builds with cc and gcc and they differ
    oracle        which reference implementation
    oracle_flags  288 against 178 ns/digest on one machine, same libsodium
                  version, different optimisation level
    run           which harness; selects the parser
    metric        what was measured
    value         as measured
    unit          derivable from metric via metrics.tsv, kept anyway so a row
                  reads without a lookup: nsols.equihash is count and
                  solve.equihash is s under one prefix rule
    n             problem size; leaf cost varies with it
    reps          sample count behind the median
    kind          cumulative against median is a category difference
    exec          emulated timings are meaningless and otherwise invisible
    dispersion    measured spread of the sample; the only honest source of
                  displayed precision
    disp_kind     which spread: iqr, stddev, range, ci95
    supersedes    which rows this replaces
    note          free text

Checked for redundancy: platform->cpu, run->kind, platform->compiler and
commit->dirty each hold in current data and none is an invariant -- the same
machine builds with two compilers, one run emits both a median and a count, a
commit is clean now and dirty later. Only metric->unit is a true dependency,
and unit stays for readability.

## Metric names

    <prefix>.<category>[ <qualifier>]

prefix from tools/metrics.tsv, category from tools/categories.tsv.
record.py rejects a name absent from either, so a typo fails loudly instead of
producing a row that never groups with its siblings.

The qualifier after a space carries free text: leaf.blake2b 2threads. The
structured part stays greppable; detail does not invent categories.

Figures:  leaf bulk phase isa solve nsols kernel
Series:   cpu mem io
Shares:   algo sync

algo.* is share of run time in an algorithm. sync.* is share of a node's wall
time in a subsystem -- a different question, kept apart so database and disk
rows never sort next to leaf and bulk.

Equihash is not one thing. equihash names the whole; equihash.gen, .sort,
.collide and .verify name the parts, because leaf generation is blake2b and
the rest is not.

## Kind

How the value relates to time. A cumulative average and an instantaneous rate
are different quantities and must not be pooled.

    point       one observation
    median      median over reps timed repetitions (the default)
    cumulative  running average since some start; spread over samples is
                meaningless and --agg skips it
    series      one sample of a time series, with t_offset in note
    share       fraction of a whole, measured not derived
    count       a tally, not a duration

## Exec

    native      ran on its own architecture; timings valid
    emulated    ran under emulation; digests valid, timings are not
    cross       cross-compiled, run elsewhere

## Values

Stored as measured, unrounded. Precision is the viewer's decision.

Derived quantities -- speedup, delta, share of a total -- are computed by the
viewer, never stored: a stored ratio goes stale when either input is
re-measured and cannot be recomputed from the file.

A percentage that is itself the measurement, such as a profiler's bucket
share, is a number and is stored.

## Repetitions

Menu: 5, 10, 100. Higher where the duration allows. Two runs at 7 and 9 reps
are not comparable and the difference buys nothing.

reps counts timed repetitions only. A warmup runs before any clock starts and
is not one of them.

## Platform

Derived by tools/platform_id.sh, not typed:

    <os>-<arch>-<model>-<cores>[-vm][-<uuid8>]
    mac-arm64-m4pro-14c
    linux-x86-xeone52680v4-2c-vm

A hypervisor is detected and marked -vm; contention is not noise that
averaging removes. --long appends eight characters of the machine UUID for
telling apart otherwise identical hosts. The full UUID is in the file trailer,
not in every row.

## Commands

    make record RUN=<target> [SODIUM=<prefix> ORACLE=<ver> ORACLE_FLAGS=<flags>]
    make results [ARGS='--runs | --agg | --compare | --metric <substr>']

## Adding a benchmark

1. Harness prints one metric per line.
2. Parser in tools/record.py, keyed by run name.
3. Prefix in tools/metrics.tsv, category in tools/categories.tsv.

The vocabulary files are also where a metric's meaning is written down.

## Mining figures: the s/solve question

Three measurements of one solver, all real:

    solve.equihash        5.6 s per solve, fixed nonce, one thread, kind=median
    zero.pow              1.6474 count/s, 4 threads, whole node, kind=cumulative
    derived               1.7 s per solve, computed from the second

All three get recorded. None is known faulty, so none is discarded.

The derived figure is useful and belongs in the system, but it must be
computed by the harness rather than inferred afterwards, because only the
harness knows the solutions each solve produced. A solve that yields 4
solutions and one that yields 2 are the same amount of work; converting a
solution rate to a solve rate needs the actual count, not an assumed one.

Four sampled nonces gave 4, 2, 3, 2. That is too few to characterise the
distribution, which is a reason to sample more -- 10 or more -- not a reason
to withhold the figure. Report it as the harness measures it, with its own
dispersion, and let the sample count speak for itself in reps.

The two direct measurements answer different questions and are not
interchangeable: one thread on a known nonce, deterministic and repeatable,
against four threads under the node's own scheduling averaged over hours. Both
are recorded; a ratio between them is not a speedup and the viewer will not
compute one, because they differ on kind and on thread count.

Rules:

    Record what the source reports, with its dispersion where the source
    computes one.

    A derived solve rate is a measurement when the harness computes it from
    counted solves and counted solutions. It is not one when a reader
    multiplies a rate by an assumed constant.

    Nonce is a condition, not noise. Record it in n so figures from different
    nonces are distinguishable rather than pooled.

    Blocks found are difficulty and luck. Context in note.

## Sequence and supersession

Every row has a seq: a stable handle. utc collides when two runs land in the
same second, and (project, run, metric) is not unique across repeats.

A later run can mark earlier rows as replaced:

    record.py --supersedes 1,2,11

Superseded rows stay in the file and are hidden by the viewer unless
--superseded is passed. Nothing is deleted: a wrong figure and the reason it
was replaced are both part of the record.

Use it when a row is known bad -- measured against a stale binary, an
emulated run, a misconfigured oracle -- not merely superseded by a newer
number. Newer is the default already; supersedes means do not trust this.

## Statistics: what to report, per data type

Chosen from the sample shape, not from convention. Evidence is 50 repetitions
of the leaf shape on an idle M4 Pro.

Timing, median of N reps -- leaf, bulk, phase, isa, solve

    report    median, iqr, mad
    why       the sample is right-skewed: a hard floor at the true cost and a
              tail from interference with nothing matching below it. Measured:
              min 87.4125, q1 87.5100, median 87.5737, q3 87.6900,
              max 87.9975; mean 87.6133 sits above the median and the upper
              half of the IQR is 1.82x the lower.
    not       stdev alone -- the tail inflates it, which is the gap the mad
              comparison exposes: 0.1186 (mad x 1.4826) against 0.1263.
              min-max -- one outlier moves it entirely.
              ci95 -- assumes a distribution this is not.

Counts -- nsols, kernel instruction counts

    report    the integer
    why       deterministic for a given input. A spread here means the input
              varied, which is a condition to record in n, not dispersion.

Cumulative rates -- zero.pow localsolps

    report    the value, kind=cumulative
    why       a running average since node start. It drifts monotonically and
              its spread over samples is an artifact of the averaging window,
              not of the thing measured. --agg skips it for that reason.

Shares -- algo.*, sync.*

    report    the percent as the profiler computes it
    why       the profiler's own sampling error, not ours. Recording a spread
              we did not measure would be inventing one.

Two arms, before and after -- ab_compare

    report    paired difference, bootstrap 95% interval, verdict
    why       pairing cancels drift, which is what makes a sub-nanosecond
              difference resolvable at all: -0.43 ns [-0.86, -0.05] resolved
              cleanly against a 3.87 ns within-arm spread. An unpaired
              comparison of the same data resolves nothing.

## Precision

Values are stored unrounded, always.

Displayed digits come from the measured dispersion, never from the magnitude
of the number:

    dispersion    disp_kind     the primary spread
    dispersion2   disp2_kind    a second estimator of a different kind

One digit past the first significant digit of the primary dispersion, which is
where the value stops being repeatable. A median of 87.5175 with an IQR of
0.155 prints as 87.52; one of 3.180 with an IQR of 0.075 prints as 3.180.

Without a dispersion the value prints as recorded. The harness did not say how
well it knows the figure, so the viewer does not invent an answer.

No approximation marks in output.

## Later

SQLite for querying across machines and months. Every row is already flat and
typed, so loading is trivial. Reserve unibench/2 if the columns change.
