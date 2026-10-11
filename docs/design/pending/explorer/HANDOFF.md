# Roam — latest handoff

Updated: 2026-10-11. Design owner: Quinn. **Sol has E1A ready for ordinary review** on
`codex/explorer-e01a`; the popup review changes none of its action/status API
requirements. Native implementation remains unassigned.

## Resume point

Design branch: `codex/explorer-design`; checkout: `.worktrees/explorer-design`.
The foundation draft reviewed by Fable is commit `40077889`. His
[review](GUI_POPUPS_REVIEW.md) is retained unchanged; Quinn's
[disposition](GUI_POPUPS_DISPOSITION.md) and revised [E1K](GUI_POPUPS.md) select
the smaller owned-window architecture. Inspect git log for this revision's
checkpoint. The branch is local, with no design PR or merge into `userland`.

Runtime source audit: `userland` at `6d988565`; relevant sources match the design
branch's older runtime parent `f3f5f50e`. Sol's separate worktree is
`.worktrees/explorer-e01a`, based on `6d988565` at inspection. Do not merge this
design branch's old runtime base over active implementation or modify Sol's
working files to deliver a documentation update.

## Deliverables and next actions

| Work | State | Next action |
| --- | --- | --- |
| [E1 actions, menus and status](E01_MENUS.md) | Updated for focus-taking popup windows | E1B uses shared multi-window dispatch and closes menus before font adoption. E1C uses an unattached launcher root. |
| [E1A actions/status](E01A_HANDOFF.md) | Implementation ready for ordinary review | Review the action/status APIs, bindings/router and real gallery. [Host/QEMU evidence](evidence/e01a/README.md) and Chris's successful OS checkout are recorded; review and merge pending. |
| [E1K native popups](GUI_POPUPS.md) | First review dispositioned; whole packet not Ready | Resolve R1 safe focus acquisition/identity and R2 queue loss/reset with Fable. Draft is ready for that focused follow-up, not implementation approval. |
| E1K-W / E1K-C | Reviewed wait/capacity directions | Can prepare focused handoffs independently: ABI layouts, waiter races, consumer/capacity tests. No implementation assigned. |
| [E2 icons](E02_ICONS.md) | Provider/asset plan unchanged; visual choice open | Finalize provider, verify decoder accounting and choose a visual contact sheet with Chris before producing the family. |
| E6 message channels | Umbrella proposal only | Prepare its separate platform design and Fable review before implementation. |

E1B/E1C wait for the final E1K contract and implementation evidence. E1A does
not wait for R1/R2. No follow-up message to Fable has been sent by this update.
The [disposition](GUI_POPUPS_DISPOSITION.md#focused-follow-up-for-fable) is the
review handoff. No further product decision is needed for Sol to continue.

## Accepted decisions and boundaries

- **Roam** has independently hideable panes/bars, including the menu. Alt/F10
  temporarily reveal it, Escape dismisses it, and right-click offers Show Menu
  Bar. Hidden pane/tab/job models survive hiding.
- Shared popup widgets give grootmenu Interface font support. Menus extend
  beyond the owner window. Kernel-owned menu session transactions are removed;
  ordinary popup windows take focus and return it conditionally on destruction.
- Popup menus survive VT switches, matching the existing project ruling.
  Font changes close menus before owner adoption; they do not automatically
  reopen from Appearance or restore external focus.
- Icons remain a shared foundation for menus, files and future desktop use.
  Desktop placement/activation remains outside E2.
- Logical slices keep kernel work focused. Fable reviews message channels and
  serious kernel behavior changes; ordinary userland keeps normal review.

See [DECISIONS.md](DECISIONS.md), D007–D014. E1/E2 engineering defaults are
Quinn's design decisions, not additional customer rulings. File Open and Save
still share E4's browser through E5.

## Existing platform findings

The unsynchronized `task->heapEnd` reservation in `gui_window_create` and
`syscall_map` needs a separate focused fix with concurrency evidence. It is
outside this design revision and does not block E1A. The documented sibling-TLB
limitation remains; this design neither solves it nor enlarges the canvas-sharing
contract. Preserve existing GUI-lock → signal-lock cleanup.

The 32-slot global window table stays. Popup birth-size capacity reduces buffer
cost without changing ordinary-window stable resizing. Depth limits do not
promise available handles. E1K's diagnostic must test full-table refusal.

The icon provider keeps libos64 independent of libimage's decoders. Future E7
still needs explicit FAT/ext2 publication, identity and source-change contracts
before destructive transfers.

## Evidence and working state

This review revision changes documentation only. Source inspection supports the
dispositions; documentation verification belongs in [SESSION_LOG.md](SESSION_LOG.md).
No runtime/QEMU/P5 acceptance or review of Sol's code is claimed. Main checkout,
Sol's implementation worktree, installed files and icon artwork are untouched.
Record actual implementation commits, tests, review and merge state as they arrive.

## E1A implementation checkpoint

Local worktree `.worktrees/explorer-e01a`, branch `codex/explorer-e01a`, runtime
base `6d988565`; source/binary hashes identify the tested files. Chris requested commit and push
for review following his successful OS checkout.
Focused fake/real action-status, existing fake/real UI, Scribe and font-contract
checks passed with supported leak detection. PS/2 and USB-at-boot QEMU checks,
32px live Interface reflow, token ownership, stale targets, stable warmed heap
reports and exit code zero are in [the evidence packet](evidence/e01a/README.md).
No native transport, kernel change, production Scribe migration, review approval
or review approval is included. The OS checkout did not identify hardware or an
installed revision. Ordinary code review is the next E1A action.
The active design branch owns native review follow-up beyond snapshot `d33e651e`.
