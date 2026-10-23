# E5D — remembered placement implementation evidence

2026-10-23, Quinn. Architecture: [revision 2](../../E05D_WINDOW_PLACEMENT.md),
confirmed in [Fable's disposition](../../E05D_FABLE_DISPOSITION.md).
Implementation review and Chris's P5 acceptance remain pending.

## Behavior and API

`os64_ui_window_open` in `os64/ui_window.h` combines native creation, draw setup
and UI initialization. Its `remember` option names a dedicated configuration
file and selects position+size or size only. Roam uses `roam-window.conf`, Scribe
uses `scribe-window.conf`, and the shared file dialogs retain `file-dialog.conf`
and its existing `size` value. The system configuration ladder selects the
directory; no application home path is compiled into the helper.

The compositor publishes event 17 when a user gesture completes. Payloads name
changed components; the queue merges them without replacing untouched fields.
The event remains 32 bytes. Desktop windows (`GUI_WINDOW_DESKTOP`) and windows
with `popup_owner_id != 0` are excluded; ordinary file dialogs receive it.
Maximize/minimize, minimum-size changes, decoration changes and VT transitions
end an active gesture before automatic geometry changes. Destruction discards
the gesture without publishing to its retiring window.

Appearance priority and pointer-after-ring-drain remain intact. Doorbell,
placement and ring/loss share a service cursor. A loss and its retained control
are one ring turn; the old empty-ring doorbell priority remains when placement
is absent. Persistence runs before menu/application filters or widget guards.
There is no close-time save, polling worker, timer or new syscall.

Full records use `geometry = 1,x,y,width,height`, with frame position and content
dimensions. Saved preferences survive automatic fitting; an explicit edit
re-reads the record and merges only changed components. Checked first publication
uses NOREPLACE. Syntax/read/write failures preserve the old file. Same-role
concurrent saves remain last complete publication wins, including possible loss
of edits to different components; the feature gate is in the root debt ledger.

## Builds and host checks

Kernel and full userland cross-builds passed with `CC=x86_64-elf-gcc`.
[Binary hashes](binaries.sha256) identify the tested production kernel, library,
applications and native diagnostic. The public `os64_ui_t` storage changed:
install a matching rebuilt kernel and userland, including other GUI consumers.

| Check | Result |
| --- | --- |
| `tools/test_window_placement_host.py` | Real config parser/writer, component merging, display fitting, invalid records, I/O/allocation failures, NOREPLACE collision, copied options and initializer ownership passed; nine queue mutants killed. [Log](host.txt) |
| `tools/test_file_dialog_host.py` | 4,840 checks, zero failures; existing behavioral assertions retained, gesture fixtures now supply completion events. Includes direct dispatch without a root and dispatcher interception before modal/menu filters. [Log](dialog.txt) |
| `tools/test_scribe_host.py` | 2,584 checks, zero failures. [Log](scribe.txt) |
| `tools/test_scribe_host.py --menus` | 1,655 checks, zero failures. [Log](scribe-menus.txt) |
| `tools/test_appearance_host.sh` | Appearance, existing queue, input-loss, settings and layout regressions passed. [Log](appearance.txt) |
| `tools/test_roam_launch_host.py` | Launch/config/failure paths and chooser lifecycle passed. |

Host runs retained ASan, UBSan and LeakSanitizer. The sandbox denied LSan thread
attachment with EPERM while `TracerPid` was zero. Leak-enabled runs outside the
sandbox passed; no `detect_leaks=0` fallback was used. For Python harnesses,
`--build-only --output DIR` permits reusing binaries outside the sandbox.
The placement runner also supports `--run-only --output DIR`.

## Native compositor evidence

Isolated QEMU q35, four CPUs, 4 GiB, copied ext2 root/home images. The native
fixture is `/tests/placementtest`; it syncs `/home/placement-events.log` after
events. `a`/`s` select wait-any/single-window wait, `d` recreates the window,
`m` raises the minimum, and `q` exits. HMP button press/release and bounded
relative mouse movements drove the gestures. The first oversized calibration
and an incorrect VT shortcut were excluded from acceptance.

[Accepted gesture trace](gestures.events):

| Generation | Case | Placement result |
| --- | --- | --- |
| 1 | Titlebar move, blocked wait-any | mask 3, position 220,150 |
| 3 | Ctrl+Alt move, blocked single-window wait | mask 3, position 200,140 |
| 4 | Northwest resize | mask 15, position 90,80, content 530×320 |
| 5 | Northeast resize | mask 14, Y=80, content 540×320 |
| 6 | Southwest resize | mask 13, X=90, content 530×340 |
| 7 | Southeast resize | mask 12, content 540×340 |
| 8 | No-op titlebar gesture | none |
| 9 | Decoration change during an applied move | one mask-3 event, position 160,120 |
| 10 | Decoration change cancels resize outline | none |
| 11→12 | Destroy during move; native handle reused | no retiring-window or stale replacement event |
| 13 | Automatic minimum-size growth | resize event, no placement |

Unmasked payload fields in the trace are unspecified, often zero. The move
cases publish placement without a resize event, exercising both native wait
paths rather than merely their queue predicate.

[Additional edge trace](edge.events) verifies a resize from 750×480 clamped to
the window's 650×400 minimum, a further shrink that emits no placement, and a
resize out of maximized state reporting all four fields (922×647 content at
0,0). A no-op resize while maximized emits nothing; the following Restore
remains available.

[VT trace](vt.events) uses the actual GUI departure chord, Ctrl+Alt+F1, followed
by release on the console and Alt+F8 to return. The applied move publishes once
before WINDOW_COVERED; returning does not publish it again.

For the automatic-geometry mutation, a scratch kernel injected a placement
notification into `wm_resize`. Starting `/tests/placementtest --fit` through
the guest shell grows its initial minimum: the [mutant trace](automatic-mutant.events)
contains the forbidden mask-12 placement; the restored production kernel's
[clean trace](automatic-clean.events) contains resize but no placement. The
mutant was not committed or included in the production hashes.

## Real application round trip

Scribe saved `(260,170), 700×480`; Roam saved `(92,68), 920×650` after real moves
and corner resizes. Scribe was closed normally. Both applications then launched
after reboot at 800×600: they fitted the screen and both files stayed unchanged.
[Smaller-screen capture](apps-small.png).

Moving the clamped Roam window to `(20,15)` saved that position while retaining
its preferred 920×650 content size. Rebooting at 1024×768 restored both preferred
sizes and their stored positions. [Restored capture](apps-restored.png),
[Roam record](roam-restored.conf), [Scribe record](scribe-restored.conf).
Maximize/restore and minimize/return commands left those records unchanged.
The scratch VM was stopped after testing. No P5 result is claimed for E5D.
