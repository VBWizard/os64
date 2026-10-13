# Roam — latest handoff

Updated: 2026-10-13. Design owner: Quinn.

E1A is accepted. PR #254 merged into `userland` as `c6cc6e3c`; gallery relocation
`2bc9f2f4` followed. Chris confirmed the P5 test complete. The diagnostic launches
as `/tests/actiongallery`. Original host/QEMU results and three review follow-ups
remain under [E1A evidence](evidence/e01a/README.md); hardware acceptance is
user-reported, without a captured installed-binary hash.

## Resume point

E1K is complete: PR #257 is approved and merged as `b36e9ddc`. Chris explicitly
confirmed this when assigning E1B; old pending banners predated its approval.

E1B is merged as `88f8b672` (PR #259). E1C is approved and merged as
`faa321a5` (PR #260), including Scribe's menu and grootmenu migration.
Chris tested the production menus on P5; the last review reported no major issues.

E2 is merged as `f39b0ae0` through [PR #261](https://github.com/VBWizard/os64/pull/261).
It supplies the PNG provider, installable icon sets, Roam Classic artwork and
os64get delivery. Chris's P5 gallery visual acceptance follows `a3602b30` and
predates the manifest error-handling fix `4ccaf82f`; no installed binary hash
was captured. [E2 evidence](evidence/e02/README.md) preserves that distinction.

**Next: give Sol [E03_HANDOFF.md](E03_HANDOFF.md) and its governing
[E3 contract](E03_DATA_VIEW.md).** Implement the data/selection core, then the
Details widget and gallery. This is userland work; E4's real directory browser
follows it. No E3 implementation or runtime validation has been performed.

Design packet: `codex/explorer-e3-design`, worktree `.worktrees/explorer-e3`,
based on `f39b0ae0`. Published as a design branch; no separate PR or merge.
Start Sol's implementation from `origin/codex/explorer-e3-design` in a fresh
worktree, or incorporate the design commit into a branch based on current
`userland`. The eventual E3 implementation PR may carry this packet with it.

| Work | State | Next action |
| --- | --- | --- |
| [E1A actions/status](E01A_HANDOFF.md) | Complete | Reuse the existing APIs. |
| [E1K native popups](E01K_HANDOFF.md) | Approved and merged, PR #257 | Consume the merged E1K wrappers. |
| [E1B menus](E01B_HANDOFF.md) | Approved and merged, PR #259 | Reuse the shared controller and dispatcher. |
| E1C consumers | Approved and merged, PR #260; P5 tested | Reuse production menus. |
| [E2 icons](E02_ICONS.md) | Merged, PR #261 | Reuse provider and shipped assets; [evidence](evidence/e02/README.md). |
| [E3 data view and selection](E03_DATA_VIEW.md) | Ready; implementation unstarted | Assign [the implementation packet](E03_HANDOFF.md) to Sol. |
| E6 channels | Umbrella proposal only | Separate platform design and Fable review. |

Install another icon set by extracting its folder, selecting its absolute path
with `set = ...` in `icons.conf`, then starting a new app instance. Missing
icons use the shipped family. [Installation and format](../../../../assets/icons/README.md).

## Accepted decisions and boundaries

- Roam's panes and bars independently hide down to a bare directory view.
  Alt/F10 temporarily reveal the hidden menu; right-click offers Show Menu Bar.
- Shared menu widgets follow the Interface font. Native popups extend beyond
  their owner; immutable owner links support conditional focus return.
- VT switching preserves menus. Appearance changes close them before font
  adoption, without reopening or stealing external focus.
- Icons are a reusable foundation for menus, files and future desktop use;
  desktop placement and activation stay outside E2.
- Kernel work stays focused. E1K has Fable's architectural review; ordinary
  code review and testing still apply. Message channels retain a separate review.

[D015](DECISIONS.md) records second-review choices. E1K uses owned create,
nonconsuming any-window wait, same-task focus peer handles, content readback,
INPUT_LOST, child/waiter BUSY guards and birth-size popup capacity. There are no
kernel menu sessions, command semantics or IPC additions.

## Existing platform findings

The unsynchronized `task->heapEnd` reservation in `gui_window_create` and
`syscall_map` needs a separate focused fix with concurrency evidence. The
existing sibling-TLB limitation also remains outside this packet. E1B owns
window lifecycle on one UI thread and must not expand canvas sharing.

The global 32-window table remains. Depth limits do not promise free handles;
allocation refusal must leave the owner usable. Popup growth needs replacement,
so E1B must follow the post-destruction drain/tombstone protocol in the handoff.

E7 still needs explicit FAT/ext2 publication, identity and source-change
contracts before destructive transfers.
