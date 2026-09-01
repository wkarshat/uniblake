# Agent Instructions

## Editing

- Identify the authoritative section for a topic before editing; do not
  duplicate its content elsewhere.
- Place new material in the nearest existing topical home. Do not create a
  file, section, or subsection whose contents would be a placeholder.
- One home per topic. Prefer combining and tightening over adding sections,
  tables, and lists. A subsection needs enough internal structure to justify
  a heading; fewer than three substantive sentences is a decomposition
  failure.
- Preserve existing content. Deleting a section, a finding, or a file needs
  explicit instruction.
- Update the table of contents when sections are added or renamed.

## Documents

- README carries the documentation map. No other file lists the documents.
- Cross-reference only where a reader must go elsewhere to act. Do not
  restate a fact another document owns, and do not add a pointer where the
  reader is already in the right place.
- State the durable fact, not the history: what the code does and why it must,
  not what it used to do or which approach was rejected. Keep measured
  evidence that justifies a constant; drop the narrative around it.
- Follow the scope and vendoring rules in README.md; they are project policy,
  not style.
- A number that would go stale needs a command that regenerates it.
- State what an omission costs the caller and what to do instead, rather than
  listing it as absent.

## Style

- ATX headers. No commentary, dates, or parenthetical qualifications in
  headings.
- No emoji or decorative Unicode. ASCII only: `--` not em-dash, `->` not
  arrow, `...` not ellipsis.
- No superlatives without evidence. Avoid hype and vague breadth
  ("comprehensive", "all platforms").
- Direct and factual. No concluding cheer, no redundant summary, no
  standalone acknowledgment sentence. Report what was verified or changed.
- No stock metaphors or security-marketing phrases ("defence in depth", "last
  line of defence", "free lunch", "silver bullet"). State the mechanism and
  its scope: what is covered, what is not.
- No filler qualifiers ("worth noting", "worth doing", "of course", "in
  practice", "it turns out"). If a thing is worth doing, state why.

## Code

- Wrap comments at 80 to 120 characters. A comment restating the line below
  it is noise; delete it.
- A parameter never reuses the name of a module-level function or constant in
  the same file. A local never rebinds a parameter to a different type.
- `include/` and `src/` stay self-contained and dependency-free: vendoring
  those two directories alone must work.
- Objects track header dependencies via `-MMD -MP`. A new source or include
  directory must keep that, or a header edit will leave stale objects.

## Verification

- **The digests must not change.** Any change to rounds, the G function, the
  message schedule, the parameter block, or finalization requires a full
  `make check` plus re-measurement.
- `make check SODIUM=<prefix>` needs libsodium as an independent oracle; the
  library links nothing. `make check-alias` needs no libsodium.
- `make check-portable` must stay warning-free; a warning is a failure.
- Changes touching `ub_final`, the state layout, or key handling also need
  `make check-wipe-modes`, which runs the suites with secret-wiping compiled
  in and out.
- Never report a performance number without its machine, compiler, flags, and
  input shape. Medians, not means. Re-measure on the target rather than
  carrying a figure across machines.
- Report a stage done when its named artifact exists and its checks pass, not
  when the code compiles.

## Operations

- Do not remove, overwrite, or add files without explicit confirmation.
  Prefer `trash` over `rm` on macOS; `mv -n` and `cp -n` where a destination
  may exist.
- Never discard uncommitted work: `checkout --`, `restore`, `reset --hard`,
  and `clean` destroy changes that exist nowhere else. To undo an edit, edit
  forward or copy the file aside first.
- Do not commit, push, or rebase unless instructed. Commit messages are
  present-tense imperative and state the measured effect, including changes
  tried and rejected with their numbers. No attribution trailers.
- If a tool call is denied or fails on a permission or syntax constraint,
  attempt at most one restructured retry, then stop and present options.
