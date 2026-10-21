# Roam — latest handoff

Updated: 2026-10-20. Design owner: Quinn.

E1A is accepted. PR #254 merged into `userland` as `c6cc6e3c`; gallery relocation
`2bc9f2f4` followed. Chris confirmed the P5 test complete. The diagnostic launches
as `/tests/actiongallery`. Original host/QEMU results and three review follow-ups
remain under [E1A evidence](evidence/e01a/README.md); hardware acceptance is
user-reported, without a captured installed-binary hash.

## Resume point

**E5's Scribe consumer is accepted.** [PR #319](https://github.com/VBWizard/os64/pull/319)
merged as `ddaad166`; Chris reported a clean review and successful P5 use,
including resizing. No installed binary hash was captured.

Active follow-up: [E5A compact footer and shared size memory](E05A_DIALOG_POLISH.md),
owned by Quinn on `codex/file-dialog-polish`, worktree `.worktrees/file-dialog-polish`,
based on `5fb5f7a6`. Implementation `5efff386` is in
[draft PR #322](https://github.com/VBWizard/os64/pull/322), with host/native checks
complete. Chris reported P5 acceptance at 2560 × 1440 with his Appearance
Workshop appearance and Frame Studio composition: size sharing survived
reopening and reboot; navigation and New Folder passed. No installed binary
hash was captured. Review R1 fixes restored sizes exceeding the native creation ceiling after a
display change; [details and evidence](E05A_DIALOG_POLISH.md#review-r1--saved-size-after-a-display-change).
The correction awaits review and targeted P5 retesting; merge remains pending. Parent-owned
stacking is a separate window-manager task; Chris's observation about the
inherited Pin button is recorded in the E5A follow-up notes.
Scribe is the first app consumer; Yonder remains part of the Modern Web work.

E4 is merged through PR #267 as `a3dd4b65`. Its [design](E04_ROAM.md) and
[evidence](evidence/e04/README.md) describe the browser foundation. The old E4
worktree reference is historical.

Chris settled association storage: Roam configuration must select the path of
the associations file. No further storage-format discussion is required.
File launching/associations are separate from this Scribe integration.

E3 PR #264 is merged and its first review reported no major issues at
`75662a5c`. Quinn's architectural check independently reran the core and real-font
suites with ASan/UBSan/LSan. Hardware evidence remains recorded separately
from merge and review status.


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

E3A/E3B are merged through [PR #264](https://github.com/VBWizard/os64/pull/264).
[Evidence](evidence/e03/README.md) records host, native, packaging and delivery
scope. The E3 design and implementation worktrees remain separate historical
checkpoints; active dialog work belongs in `.worktrees/file-dialog-polish`.

| Work | State | Next action |
| --- | --- | --- |
| [E1A actions/status](E01A_HANDOFF.md) | Complete | Reuse the existing APIs. |
| [E1K native popups](E01K_HANDOFF.md) | Approved and merged, PR #257 | Consume the merged E1K wrappers. |
| [E1B menus](E01B_HANDOFF.md) | Approved and merged, PR #259 | Reuse the shared controller and dispatcher. |
| E1C consumers | Approved and merged, PR #260; P5 tested | Reuse production menus. |
| [E2 icons](E02_ICONS.md) | Merged, PR #261 | Reuse provider and shipped assets; [evidence](evidence/e02/README.md). |
| [E3 data view and selection](E03_DATA_VIEW.md) | Merged PR #264 | Reuse the shared model and Details view; [P5 packet](E03_HANDOFF.md). |
| [E4 directory browser](E04_ROAM.md) | Merged PR #267 | Reuse the shared browser. |
| [E5 Scribe dialogs](E05_SCRIBE.md) | Accepted; PR #319 merged, P5 tested | [E5A layout and size follow-up](E05A_DIALOG_POLISH.md). |
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
