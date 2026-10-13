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

Current work is E2 in `codex/explorer-e2`, worktree `.worktrees/explorer-e2`.
It supplies the shared PNG catalog, folder-based icon sets and Roam Classic
artwork in Scribe, grootmenu and the gallery. No kernel code changes.

| Work | State | Next action |
| --- | --- | --- |
| [E1A actions/status](E01A_HANDOFF.md) | Complete | Reuse the existing APIs. |
| [E1K native popups](E01K_HANDOFF.md) | Approved and merged, PR #257 | Consume the merged E1K wrappers. |
| [E1B menus](E01B_HANDOFF.md) | Approved and merged, PR #259 | Reuse the shared controller and dispatcher. |
| E1C consumers | Approved and merged, PR #260; P5 tested | Reuse production menus. |
| [E2 icons](E02_ICONS.md) | Ready for review; host, packaging and QEMU checks pass | Ordinary review and [P5 walkthrough](evidence/e02/README.md). |
| E3 data view and selection | Planned | Design the reusable view/selection contract after E2. |
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
