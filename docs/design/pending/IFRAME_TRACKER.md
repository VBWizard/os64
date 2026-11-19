# Iframe tracker

[IFRAME_TRACKER.csv](IFRAME_TRACKER.csv) records the implementation of
[IFRAME.md](IFRAME.md), initially as **14 proposed mergeable slices**.
F1/F2/F3 remain dependency stages; `F.1` through `F.14` identify individual
implementation slices. Child tasks use `F.n.m` (for example `F.4.1`) and
appear beneath their parent. The design refresh merged as PR #528; design
approval does not complete implementation.

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

- Keep one parent row per slice, with a short scope/acceptance summary.
  When a slice contains multiple tasks or additional work is discovered,
  add child rows immediately beneath it using stable `F.n.m` IDs. Record
  both completed work and known remaining work; do not hide new obligations
  in a growing parent note. Familiar brief labels such as F4a–d may appear
  in task names without changing the numeric IDs supported by the renderer.
- Give each child a concrete result, status, dependencies where needed,
  implementing PR references and a concise evidence/qualification note.
  Child statuses are `todo`, `doing`, `done` and `dropped`; use `doing` for
  active design/code/review and state the phase in notes when useful. Mark
  design tasks as design work; completing them does not complete the
  corresponding implementation. Detailed proofs remain in the slice briefs.
- Keep completed tasks visible. Append newly discovered tasks without
  renumbering existing IDs. Split a broad remaining task when concrete work
  emerges, preserving its acceptance obligations and dependency references.
  Tasks may span multiple coherent PRs; this breakdown does not prescribe
  smaller PRs. Task counts measure listed results, not effort percentages,
  fixed PR counts or estimates of remaining time.
- The builder updates its slice through `todo`, `design`, `design-review`,
  `code`, `review`, `p5` where hardware acceptance applies, and `done`.
  `todo` means a proposed cut without a completed implementation brief.
  Record a blocked state in notes while retaining the current lifecycle
  status. Retired plans become `dropped` with a reason rather than vanishing.
- A branch changes its own slice and child rows. A planned split or dependency change
  may update the affected plan rows together, with the reason documented.
  Carry forward predecessor updates when stacking or rebasing; do not reset
  another slice's status to an older snapshot.
- Prepare each implementing task's `done` flip and `done_in` PR number in
  its final commit after its required checks and acceptance. The parent
  retains its lifecycle status until the full slice contract is satisfied;
  completed foundations do not close remaining acceptance tasks.
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

F.2's accepted implementation comprises F.2a native ownership and F.2b
bindings. F.3's [retirement brief](IFRAME_F3_RETIREMENT.md) separates F.3a
libjs lifecycle/job ownership and F.3b DOM departure/failed-child isolation.
Their completed parent rows retain the implementing PRs and acceptance
receipts. The F.3 contract retains bounded slots and the runtime-wide overrun
fallback; it does not enable frame loading or independent reclamation.

F.4 and F.15 have task-level histories and remaining-work rows. Their parents
retain the slice lifecycle until full acceptance is met. Cross-slice dependencies
refer to the specific required task where possible; a task must not depend
on its own parent's completion.

F.15 and F.16 track the retained-realm reclamation and child-local overrun
recovery follow-ups. They are not prerequisites for F.3. F.11 has explicit
alternative evidence gates: reclamation proof or accepted Tier 1 session
realm-consumption/refusal measurements; recovery proof or accepted Tier 1
frame-budget measurements on the P5. Record the chosen evidence and retained
limitations in F.11 acceptance. Measurements satisfy the shipping gate but
do not mark an unimplemented reclamation/recovery row done. These alternatives
are in notes rather than unconditional dependency edges.
