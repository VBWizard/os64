# Iframe tracker

[IFRAME_TRACKER.csv](IFRAME_TRACKER.csv) records the implementation of
[IFRAME.md](IFRAME.md), initially as **14 proposed mergeable slices**.
F1/F2/F3 remain dependency stages; `F.1` through `F.14` identify individual
PR-sized cuts. The design refresh is already merged as PR #528; it is not
an implemented slice. The initial implementation rows are `todo`.

| Stage | Slices | Scope |
|---|---|---|
| F1 | F.1–F.5 | Realms and service routing; ownership/adoption; retirement; proxy security; messaging |
| F2 | F.6–F.11 | Page/context routing; navigation; scheduling/budgets; rendering; input; replay and integrated enablement |
| F3 | F.12–F.14 | Memory-fed documents; sandbox enforcement; coherent storage/events |

This is a working cut, not a claim that the ownership or budget decisions
are settled. A slice's brief defines those decisions and its required tests
before code starts. Split or revise a cut when that proof needs it, keeping
the dependency graph and acceptance notes current. Each slice includes its
tests, review and documentation; there is no separate testing slice that
licenses earlier work to go unverified. F.11 additionally proves the
integrated host and enables the supported frame path.

Preparatory host changes remain inert in normal browsing until the combined
security, lifetime, framing-policy, resource-budget and scheduling contracts
are satisfied. Harnesses exercise them as they land. Unsupported sandbox
and srcdoc loads are refused, as IFRAME.md requires; visible frame support
must not expose an intermediate path that ignores restrictions.

## Upkeep

- Keep one row per mergeable slice. Use the task column for scope and notes
  for the acceptance boundary, evidence location or a blocker. Detailed
  implementation steps belong in the slice brief; optional sub-task rows
  are supported by the tool but are not required for this arc.
- The builder updates its slice through `todo`, `design`, `design-review`,
  `code`, `review`, `p5` where hardware acceptance applies, and `done`.
  `todo` means a proposed cut without a completed implementation brief.
  Record a blocked state in notes while retaining the current lifecycle
  status. Retired plans become `dropped` with a reason rather than vanishing.
- A branch changes its own slice's row. A planned split or dependency change
  may update the affected plan rows together, with the reason documented.
  Carry forward predecessor updates when stacking or rebasing; do not reset
  another slice's status to an older snapshot.
- As with HTTP/2, prepare the `done` flip and `done_in` PR number in the
  implementing PR's final commit after its required checks and acceptance.
  On `userland`, `done` therefore means merged. A board rendered from a
  worktree describes that branch, including its prepared completion flips;
  use the merged tree for the accepted project status.
- Keep the short acceptance boundary in notes when adding results or links.
  Test details may live in the PR or brief. Security/lifetime review and
  leak-enabled host checks follow IFRAME.md; P5 evidence belongs to slices
  that change observable frame behavior.
- Run the checker before committing or pushing tracker changes:

  ```sh
  python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
  ```

Render/open the board with `tools/tracker.sh iframe [worktree]`.
[Shared tool documentation](../../../tools/TRACKERS.md) covers the format
and launcher. The iframe rows do not modify HTTP/2's tracker or its upkeep
rules. Workers and the remaining browser roadmap are separate arcs.

F.2 uses two implementation PRs under one row, as adopted in its design
review: F.2a for native ownership and F.2b for bindings. Keep the row in
progress after F.2a and name its evidence in notes. F.2b's completion flip
records both PRs; merging the brief or the native half alone does not finish
the slice.

F.3's [retirement brief](IFRAME_F3_RETIREMENT.md) proposes two implementation
PRs under its row: F.3a for libjs lifecycle/job ownership and F.3b for DOM
departure and failed-child isolation. Keep the row in progress between them;
its final completion records both implementation PRs. The brief selects
bounded retained slots and the design's runtime-wide overrun fallback, with
explicit proof obligations for a future recovery extension. It enables no
frame loading or independent reclamation of departed contexts.
