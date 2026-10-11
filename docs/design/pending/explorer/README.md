# Explorer project — start here

This is the entry point for a new session or a new contributor. The project
spans multiple sessions and may have different developers for different slices.
The repository records are the handoff; conversation memory is not required.

## Current position

Updated: 2026-10-11.

- Application name: **Roam**, confirmed by Chris (D007).
- Surrounding panes and bars, including the menu, must be independently
  toggleable down to a bare directory view (D008).
- Hidden-menu recovery is agreed. E1 must provide a shared popup-menu widget
  and migrate grootmenu onto it with Interface font support (D009).
- E1 must account for icons; E2 supplies the first asset family and shared icon
  support suitable for future desktop icons (D010).
- Slices follow logical outcomes rather than a size target; kernel changes stay
  focused, with explicit prerequisites and validation (D011).
- The channel foundation gets a standalone design and Fable review. Additional
  Fable review applies to serious kernel changes, not routine syscall additions
  following established contracts or ordinary userland work (D012).
- Native popups extending beyond their owner window are selected (D013).
  [E1K's revised GUI design](GUI_POPUPS.md) adopts Fable's smaller owned-window
  architecture; [disposition](GUI_POPUPS_DISPOSITION.md) records two focused
  follow-up questions (D014).
- [E1](E01_MENUS.md) and [E2](E02_ICONS.md) detailed drafts now exist. E1A's
  [action/status handoff](E01A_HANDOFF.md) is ready for ordinary code review from Sol on
  `codex/explorer-e01a`; this review changes none of its API requirements.
- Umbrella design and 15-slice roadmap written; E1A is under ordinary review in [PR #254](https://github.com/VBWizard/os64/pull/254).
- Design branch: `codex/explorer-design`, based on `userland` at `f3f5f50e`.
- Initial design commit: `337f4744`. Organization records follow on that branch.
- Local worktree: `.worktrees/explorer-design` under the repository root.
- No design PR has been opened. This local design checkpoint includes the
  2026-10-11 decisions and detailed packets; inspect git status/log for its
  commit state. Publication and integration are still pending, so it is not
  yet available on `userland`.
- Next work: close E1K R1/R2 with Fable, review Sol's E1A,
  and choose E2's visual family with Chris. Native implementation is unassigned.

## Read in this order

1. [Latest handoff](HANDOFF.md): where to resume, concrete next action, blockers.
2. [Umbrella design](../EXPLORER.md): product, architecture, dependencies and
   acceptance targets for the 15 slices.
3. [Decision log](DECISIONS.md): accepted requirements, proposed engineering
   choices and unresolved decisions. A proposal is not customer approval.
4. The active slice's record: [E1](E01_MENUS.md), its active
   [E1A packet](E01A_HANDOFF.md), or [E2](E02_ICONS.md). New slices use the
   [slice template](SLICE_TEMPLATE.md).
5. [Session log](SESSION_LOG.md) when the reasoning or evidence behind a change
   is needed. It is history, not the source of current status.

## Slice board

The umbrella design owns dependency details; do not duplicate that graph here.
Create slice records when work begins, rather than maintaining 15 empty designs.

| ID | Slice | State | Owner | Record / PR / implementation commit | Acceptance evidence |
| --- | --- | --- | --- | --- | --- |
| E1 | Actions, menus and status | E1A awaiting ordinary review; native R1/R2 follow-up | Quinn (design), Sol (E1A) | [E1A](E01A_HANDOFF.md), [E1K](GUI_POPUPS.md); `bd86ce4f`, [PR #254](https://github.com/VBWizard/os64/pull/254); branch pushed | [E1A host/QEMU evidence](evidence/e01a/README.md); Chris's successful OS checkout; no complete E1 acceptance |
| E2 | Icon vocabulary | Designing — visual choice open | Quinn (design) | [E2 contract](E02_ICONS.md) | Dependency/allocation inspection; no runtime evidence |
| E3 | Data view and selection | Planned | Unassigned | — | None |
| E4 | Directory browser | Planned | Unassigned | — | None |
| E5 | Open/Save picker and launch reuse | Planned | Unassigned | — | None |
| E6 | Local channel foundation | Planned | Unassigned | — | None |
| E7 | Safe operation engine | Planned | Unassigned | — | None |
| E8 | Transfer service | Planned | Unassigned | — | None |
| E9 | Transfers in the GUI | Planned | Unassigned | — | None |
| E10 | Tabs, split panes and tree | Planned | Unassigned | — | None |
| E11 | Rename, clipboard and removal | Planned | Unassigned | — | None |
| E12 | Drag and drop | Planned | Unassigned | — | None |
| E13 | Trash and truthful Undo | Planned | Unassigned | — | None |
| E14 | Views and previews | Planned | Unassigned | — | None |
| E15 | Search and finish | Planned | Unassigned | — | None |

States: **Planned → Designing → Ready → Implementing → Reviewing → Merged →
Accepted**. Ready means the slice contract is settled and prerequisites are
available. Reviewing includes fixing findings. Merged needs an actual merge
commit; Accepted additionally needs the slice's agreed acceptance evidence.
Record a blocker alongside the state, with the work needed to clear it, rather
than losing the stage the slice reached. A docs-only design commit does not
advance an implementation slice to Merged.

Host, QEMU and P5 results are separate evidence. If P5 acceptance is required,
a merged slice can wait for it without being called fully accepted. If a check
does not apply, record why. “Built” and “review approved” do not mean “tested.”

## Working agreement

- At session start, read the current handoff and inspect the branch/worktree
  state before editing. Confirm that prerequisite commits are actually present.
- Before coding a slice, maintain its linked record; create a numbered record
  from the template if none exists. Record its owner, branch, scope,
  shared interfaces and acceptance checks. Small slices can use a short record;
  complex slices can link a separate detailed design.
- Keep one owner per active slice. Record an explicit handoff when ownership
  changes. Opus or another contributor can take a slice without taking over the
  arc. Do not infer an assignment from a suggestion that someone might help.
- Parallel work needs independent worktrees and agreed shared interfaces.
  List any shared files/ABI changes in the slice records and coordinate those
  changes before editing. Separate branches alone do not settle interface conflicts.
- Record decisions when made, with rationale and affected slices. Update the
  umbrella or slice design in the same change if the decision changes its claims.
  Preserve superseded decisions with links to their replacements.
- Before handing off or ending a work session, update the board, latest handoff
  and an append-only session entry. Include commits/PRs, test evidence, review
  findings still open, known limitations and the exact next action. Link bulky
  logs rather than pasting them into the board; do not rely solely on `/tmp`.
- Commit the records with the work they describe where practical. Record whether
  a branch is local, pushed, under review or merged so another contributor knows
  where the work is available. Publishing a branch is not merging it.
- A slice may span sessions and PRs. A session may end after a design discussion.
  Neither is a reason to squeeze the rest of the project into the same chat.

Quinn maintains the arc and integration plan. The developer of a slice maintains
its record and evidence; Chris's product decisions are recorded with attribution.
This workflow does not replace repository review, testing or approval rules.
