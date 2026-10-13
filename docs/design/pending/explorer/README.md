# Explorer project — start here

This is the entry point for a new session or a new contributor. The project
spans multiple sessions and may have different developers for different slices.
The repository records are the handoff; conversation memory is not required.

## Current position

Updated: 2026-10-13.

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
  architecture. His second review closes R1/R2; D015 records the implementation
  refinements and [E1K handoff](E01K_HANDOFF.md) fixes the public API.
- E1A is accepted: PR #254 merged as `c6cc6e3c`; gallery relocation
  `2bc9f2f4` is on `userland`. Chris confirmed the P5 test complete.
  Launch the diagnostic as `/tests/actiongallery`.
- E1K is complete: [PR #257](https://github.com/VBWizard/os64/pull/257)
  is approved and merged as `b36e9ddc`. Chris confirmed this status at E1B
  authorization; old pending-review banners preceded approval.
- E1B is approved and merged as `88f8b672` (PR #259), confirmed by Chris.
- E1C is approved and merged as `faa321a5` (PR #260); Chris tested Scribe
  and the desktop menu on P5.
- Current work: E2 is pushed in `codex/explorer-e2`, with
  [PR #261](https://github.com/VBWizard/os64/pull/261) open against `userland`.
  The file-backed provider, installable
  sets and Roam Classic artwork pass host, FAT/ext2 packaging and QEMU checks.
  [E2 evidence and P5 walkthrough](evidence/e02/README.md) record the result;
  ordinary review and installed P5 acceptance remain.
- The initial design snapshot `d33e651e` arrived through E1A.
- D016 settles E2's visual family and installable sets. E6 still needs its
  standalone channel design and Fable review.

## Read in this order

1. [Latest handoff](HANDOFF.md): where to resume, concrete next action, blockers.
2. [Umbrella design](../EXPLORER.md): product, architecture, dependencies and
   acceptance targets for the 15 slices.
3. [Decision log](DECISIONS.md): accepted requirements, proposed engineering
   choices and unresolved decisions. A proposal is not customer approval.
4. The active slice's record: [E1](E01_MENUS.md), its active
   [E1K packet](E01K_HANDOFF.md), [E1B handoff](E01B_HANDOFF.md), or [E2](E02_ICONS.md). New slices use the
   [slice template](SLICE_TEMPLATE.md).
5. [Session log](SESSION_LOG.md) when the reasoning or evidence behind a change
   is needed. It is history, not the source of current status.

## Slice board

The umbrella design owns dependency details; do not duplicate that graph here.
Create slice records when work begins, rather than maintaining 15 empty designs.

| ID | Slice | State | Owner | Record / PR / implementation commit | Acceptance evidence |
| --- | --- | --- | --- | --- | --- |
| E1 | Actions, menus and status | E1A/E1K/E1B/E1C merged; production menus P5 tested | Quinn (E1C), Sol (E1A/E1B) | [E1A](E01A_HANDOFF.md), [E1K](E01K_HANDOFF.md), [E1B](E01B_HANDOFF.md); PR #254 `c6cc6e3c`, PR #257 `b36e9ddc` | [E1B evidence/walkthrough](evidence/e01b/README.md); P5 report precedes review fixes |
| E2 | Icon vocabulary | Reviewing; P5 pending | Quinn | [E2 contract](E02_ICONS.md), `codex/explorer-e2` | [Host, packaging and QEMU evidence](evidence/e02/README.md) |
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
