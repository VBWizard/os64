# E1B validation and P5 walkthrough

2026-10-11–12. Branch `codex/explorer-e1b`, fresh base `origin/userland` at
`b36e9ddc` (PR #257). [Concrete handoff](../../E01B_HANDOFF.md).

Tested implementation commit: `7c6de5cf` ([PR #259](https://github.com/VBWizard/os64/pull/259)).
Publication metadata after that commit changes documentation only.

Before publication, the branch fast-forwarded to `f7f2f508`; those upstream
CSS-transform fixes touch no menu, font or action files.

## Host results

The final menu suite passes **1,807 fake-font / 1,854 real-font checks**, including
its existing 495/542 action/status checks, with ASan, UBSan and LeakSanitizer.
The real run uses shipped DejaVu faces and production 24px/32px font adoption.

- Models: copied descriptors, separator normalization, duplicate/invalid/empty
  rejection and graph/depth bounds. Immutable public construction prevents cycles.
- Controller: both dismissal queue orders, matching down/up suppression and
  passing a later click, focus-only expiry, explicit close, stale and disabled
  targets, matched activation/repeats, PS/2/HID escape bursts, duplicate mnemonics,
  pointer cancellation, INPUT_LOST, Tab, editor selection and unclaimed right clicks.
- Lifetime: replacement before birth events drain, child-first destruction,
  handle tombstones across two full 64-event turns, standalone hosting with a
  distinct borrowed text context, close-before-callback and tracked references.
- Layout/fonts: real UTF-8 measurement, cluster ellipsis, signed edge placement,
  last-row scroll reachability, checked+icon columns, failed owner preparation
  retaining the old font with menus closed, then successful adoption. Checked
  popup paint and icon draw paths allocate nothing.
- Allocation sweeps: models, session creation, bar preparation and popup text,
  including partial cleanup. Existing actions/status/font transaction coverage
  remains in the reused suites.
- Six deliberate scratch-source mutations are detected: inverted enabled guard,
  accepting stale targets, callback before close, stale status-token clear,
  skipped font closure and arrow-prefix-as-Escape. A mutation requires a failing
  behavioral check; an unsupported sanitizer run alone does not count.

Existing suites pass **1,626 real-font UI/editor checks**, **2,470 Scribe checks**,
font contracts and strict full-userland cross build. Scribe/grootmenu behavior
is unchanged; their shared-menu migrations and production acceptance belong to E1C.

The sandbox denied LeakSanitizer attachment with `EPERM`; the contemporaneous
shell reported `TracerPid: 0`. [Diagnosis](lsan-diagnosis.txt) retains the actual
error. Supported runs used `ASAN_OPTIONS=detect_leaks=1` outside the sandbox;
leak checks were never disabled. The final real-font relink reused unchanged
FreeType objects; the complete driver below reproduces the result.

## Host reproduction

Run from the worktree, using normal escalation if attachment is denied:

```sh
make -C userland -j8
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --output /tmp/e1b-menus
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --real --output /tmp/e1b-menus-real
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_menu_mutations.py --output /tmp/e1b-mutations
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_text_host.py --real --output /tmp/e1b-ui-real
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_scribe_host.py --output /tmp/e1b-scribe
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_font_contracts.py
```

## Native results and limits

The retained four-core run uses the final gallery/library artifacts listed in
[binaries.sha256](binaries.sha256), on unchanged E1K kernel SHA-256
`067e5ab1fd7a876198f5a11325c8ae14ccfdea29449a57b63839277e45f3d081`.
The implementation is identified by [source hashes](sources.sha256); the PR
commit contains those sources and these reports.

| Check | Evidence |
| --- | --- |
| USB HID and PS/2 keyboard traversal, context edges and hidden-bar recovery | [HID trace](guest-hid.txt), [PS/2 trace](guest-ps2.txt), [controllers](controller-hid.txt); HID key flags are 128, PS/2 flags are 0. |
| Dismissing click consumed, subsequent click invokes; callback sees zero menu windows | [Dismiss](dismiss-click-final.png), [later click](later-click-final.png), HID invocation trace. |
| Checkmark and icon occupy separate columns; NULL provider remains usable | [Checked icon](checked-icon-final.png), [PS/2 without icons](ps2-actions-final.png). |
| Tall UTF-8 menu reaches its end, pages and routes wheel locally | [End](tall-end-final.png), [page](tall-page-final.png), [wheel before](wheel-before-final.png), [after](wheel-after-final.png). |
| Same-process real Interface change closes panels before adoption; 32px layout and complete overflow work | [Font report](guest-font32.txt), [closed](font-closed-final.png), [View](font32-view-final.png), [overflow](overflow-menu-final.png), [child](overflow-child-final.png). |
| Color-only change preserves live menus; VT preserves family; external focus dismisses | [Palette report](guest-palette.txt), [live color](font32-palette-final.png), [VT](vt-preserved-final.png), [Scribe focus](external-focus-final.png). |
| Popup close leaves owner alive; owner close releases family/font references | [Popup close](popup-close-final.png), [owner close](owner-close-final.png), [external trace](guest-external.txt). |
| Twelve warm cycles followed by 24 more return live heap to the same baseline | [Warm heap](heap-warm.txt), [after heap](heap-after.txt); mapped bytes, live bytes and block count match, heap audit is OK. All gallery exits report font bytes/references zero. |

Native panels use `os64_gui_popup_create`: trace reports pitch equal to popup
width, with explicit birth-size capacity; ordinary owner surfaces retain their
larger pitch. No kernel change is needed. Native heap measurements cover live
userland allocations, not the kernel's full accounting; E1K owns native backing
store and global-window admission evidence. Task VA high-water remains monotonic.

Tiny-screen refusal is exercised through the deterministic 70×40 screen seam,
not a separate tiny QEMU display. Owner INPUT_LOST and allocation/native-create
failure paths are injected in the host harness. Existing Scribe host checks
cover save/edit/unsaved-exit regressions; the native integration run checks its
help and external-focus behavior. Production menu migration and complete
Scribe/grootmenu acceptance belong to E1C. Hardware behavior is unclaimed until
Chris performs the P5 walkthrough below.

## QEMU reproduction

Four-core QEMU uses private FAT/ext2 copies under `/tmp/e1b-vm`; repository
disks are untouched. The tested ISO replays the fixed E1K ISO's boot metadata
with [this config](limine.conf), and the unchanged PR #257 kernel.
[Binary hashes](binaries.sha256) identify staged artifacts;
[source hashes](sources.sha256) identify toolkit/test sources.

```sh
mkdir -p /tmp/e1b-vm
cp --reflink=auto disk/os64.img /tmp/e1b-vm/os64.img
cp --reflink=auto disk/data.img /tmp/e1b-vm/data.img
xorriso -indev os64_kernel.iso -outdev /tmp/e1b-vm/e1b.iso -boot_image any replay \
  -map docs/design/pending/explorer/evidence/e01b/limine.conf /boot/limine/limine.conf \
  -map kernel/bin/os64_kernel /boot/os64_kernel
mcopy -o -i /tmp/e1b-vm/os64.img@@1048576 userland/bin/tests/actiongallery \
  userland/bin/tests/fontsettingstest ::/tests/
mcopy -o -i /tmp/e1b-vm/os64.img@@1048576 userland/bin/libos64.so userland/bin/libfreetype.so ::/lib/
mcopy -o -i /tmp/e1b-vm/os64.img@@1048576 userland/bin/scribe userland/bin/grootmenu \
  userland/bin/appearance ::/bin/
qemu-system-x86_64 -machine q35 -cpu qemu64,+rdrand,+rdseed -m 4g -smp 4 \
  -no-reboot -cdrom /tmp/e1b-vm/e1b.iso -boot d -display none \
  -serial file:/tmp/e1b-vm/serial-acceptance.log \
  -qmp unix:/tmp/e1b-vm/qmp.sock,server=on,wait=off \
  -drive file=/tmp/e1b-vm/os64.img,format=raw,if=none,id=nvme1 \
  -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=/tmp/e1b-vm/data.img,format=raw,if=none,id=data1 \
  -device nvme,drive=data1,serial=data1-serial \
  -device qemu-xhci -device usb-kbd,id=e1b_hidkbd
```

Copy the retained [controller](acceptance.py) to `/tmp/e1b-vm/acceptance.py`.
After `husk: started`, run its `hid`, `ps2-font` and `external` modes in that order. It uses
bounded relative mouse packets from a known origin, captures screenshots and
writes traces under guest `/home`; `quit` stops only this VM. The `external` mode exercises wheel routing, Scribe help, external focus
and popup/owner close.

After guest `sync`, retrieve reports:

```sh
debugfs -R 'cat /e1b-final-hid.txt' '/tmp/e1b-vm/data.img?offset=1048576'
debugfs -R 'cat /e1b-final-ps2.txt' '/tmp/e1b-vm/data.img?offset=1048576'
```

Early controller trials chose the wrong button or oversized relative mouse
packets and were discarded. The retained controller uses bounded packets and
an unobscured gutter to locate Record A. Its trace proves that dismissal leaves
one invocation, and the subsequent click makes two.

## P5 walkthrough for Chris

Build/install this revision's **whole userland** through the normal image
workflow, including libos64 and GUI consumers. Public UI structs gained hooks;
use consumers built with matching headers. Record the installed commit and,
if convenient, gallery/libos64 hashes. E1K is already merged.

1. Run `/tests/actiongallery --trace > /home/e1b-p5.txt`. F10, Enter, Enter
   invokes Record Target once. Hold Enter or Ctrl+D: no repeated invocation
   until a fresh press. Escape closes a child before its parent.
2. In View, checkmarks and synthetic icons use separate columns. Toggle panel,
   status, heading and either pane control; restore them and check retained
   target/message state. Hiding a focused control transfers focus to content.
3. Hide Show Menu Bar. Its row disappears. Bare left Alt and F10 temporarily
   reveal/reflow it; Escape hides it again. Right-click or Shift+F10 offers
   Show Menu Bar for permanent recovery.
4. Fixtures → Tall UTF-8 Commands: use arrows, Home/End, Page Up/Down, wheel and
   scroll arrows; reach row 49. Hover through cascades and back to another row.
   Resize narrowly, or run `/tests/actiongallery --narrow`; overflow must keep
   every top-level menu reachable.
5. Right-click near screen edges; cascades flip/clamp. Open a context menu,
   click Record A once to dismiss, then again to invoke. The first click must
   not invoke. A titlebar focus change must not swallow a later content click.
6. Select text in the editor. Right-click without changing selection, Escape,
   then use Ctrl+C/X/V/A. Enable/select B to check target-specific invocation;
   Make Target Stale must prevent mutations against that old generation.
7. Change Interface font/size with a menu open. It closes before adoption and
   stays closed; the next open uses the new face/size. Also try a color change,
   VT switching and another app: VT alone preserves, external focus dismisses.
8. Repeat open/close, test popup/owner Alt+F4, then exit. No stranded popup;
   trace ends with zero font bytes/references. Run `--no-icons` once: labels
   and reserved empty icon slots remain usable.

P5 acceptance awaits Chris's report. The gclock debt is untouched. Real icon
assets/catalog, production menu migrations, Roam browsing and IPC remain later work.
