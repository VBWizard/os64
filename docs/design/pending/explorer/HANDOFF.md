# Roam — latest handoff

Updated: 2026-10-11. Design owner: Quinn.

E1A is accepted. PR #254 merged into `userland` as `c6cc6e3c`; gallery relocation
`2bc9f2f4` followed. Chris confirmed the P5 test complete. The diagnostic launches
as `/tests/actiongallery`. Original host/QEMU results and three review follow-ups
remain under [E1A evidence](evidence/e01a/README.md); hardware acceptance is
user-reported, without a captured installed-binary hash.

## Resume point

Quinn owns E1K in branch `codex/explorer-e1k`, checkout
`.worktrees/explorer-e1k`, based on `2bc9f2f4`. The working implementation and
[concrete handoff](E01K_HANDOFF.md) follow Fable's second review of native popups.
The original design branch is historical; do not merge its older runtime base
over this work. The first review snapshot arrived through E1A at `d33e651e`.

Sol's E1B starts after E1K's API, guest evidence and ordinary code review are
settled. Chris explicitly asked about the ordering; E1B is the dependent next
packet, followed by E1C. No E1B implementation is included here.

| Work | State | Next action |
| --- | --- | --- |
| [E1A actions/status](E01A_HANDOFF.md) | Accepted | Reuse the public action/status APIs. |
| [E1K native popups](E01K_HANDOFF.md) | Reviewing | Review the diff and [evidence](evidence/e01k/README.md); Chris confirmed QEMU and P5 validation after the readiness-test fix. |
| [E1B menus](E01_MENUS.md) | Waiting on E1K | Sol consumes the final APIs; exercise cross-queue dismissal, handle quarantine and unpaired-suppression expiry. |
| E1C consumers | Waiting on E1B | Migrate grootmenu and Scribe with their existing policies. |
| [E2 icons](E02_ICONS.md) | Provider/asset plan unchanged | Choose the visual family with Chris before producing assets. |
| E6 channels | Umbrella proposal only | Separate platform design and Fable review before implementation. |

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
