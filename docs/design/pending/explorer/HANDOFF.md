# Roam — latest handoff

Updated: 2026-10-12. Design owner: Quinn.

E1A is accepted. PR #254 merged into `userland` as `c6cc6e3c`; gallery relocation
`2bc9f2f4` followed. Chris confirmed the P5 test complete. The diagnostic launches
as `/tests/actiongallery`. Original host/QEMU results and three review follow-ups
remain under [E1A evidence](evidence/e01a/README.md); hardware acceptance is
user-reported, without a captured installed-binary hash.

## Resume point

E1K is complete: PR #257 is approved and merged as `b36e9ddc`. Chris explicitly
confirmed this when assigning E1B; old pending banners predated its approval.

Quinn owns E1B in `codex/explorer-e1b`, worktree `.worktrees/explorer-e1b`,
from current `origin/userland` at that merge. The [concrete E1B handoff](E01B_HANDOFF.md)
describes the shared models/widgets/controller, native dispatcher, ownership,
font and context hooks. [Evidence and P5 walkthrough](evidence/e01b/README.md)
record what passed and what awaits Chris's hardware confirmation.

| Work | State | Next action |
| --- | --- | --- |
| [E1A actions/status](E01A_HANDOFF.md) | Complete | Reuse the existing APIs. |
| [E1K native popups](E01K_HANDOFF.md) | Approved and merged, PR #257 | Consume the merged E1K wrappers. |
| [E1B menus](E01B_HANDOFF.md) | Implemented; ordinary review/P5 pending | Review the toolkit and complete the linked P5 walkthrough. |
| E1C consumers | Waiting on E1B | Migrate grootmenu and Scribe with their existing policies. |
| [E2 icons](E02_ICONS.md) | Provider boundary available; visual choice open | Choose the family before producing assets/catalog. |
| E6 channels | Umbrella proposal only | Separate platform design and Fable review. |

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
