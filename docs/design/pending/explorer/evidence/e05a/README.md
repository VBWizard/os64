# E5A dialog polish evidence

Built from `codex/file-dialog-polish`, based on `5fb5f7a6`.
The [R1 follow-up](review-r1/README.md) records the creation-limit fix, revised
binary hashes, 4,621 passing dialog checks and a native smaller-display check.
The original captures and hashes below predate that correction.
[Source and binary hashes](sha256.txt) identify the tested implementation.

## Host and build

[Host results](host.txt): 4,223 shared-dialog and 2,584 Scribe checks, zero
failures, with ASan, UBSan and LeakSanitizer. The [sandbox diagnostic](lsan-sandbox.txt)
shows actual thread-attachment EPERM; both suites passed outside the sandbox
with leak detection enabled. No leak-disabled result is substituted.

The dialog suite exercises compact/wrapped controls, conditional messages,
font adoption and allocation-failure recovery; shared Open/Save sizes,
Cancel, unchanged older dialogs, maximize/restore, failed writes, malformed
preferences, screen limits and initial growth for a larger font. Filesystem,
native windows and config calls are mocked. The native checks below exercise
the real configuration writer and distinct processes.

Reproduce builds with `tools/test_file_dialog_host.py --build-only --output DIR`
and `tools/test_scribe_host.py --build-only --output DIR`; then run their
binaries against `userland/libfreetype/fixtures` with `ASAN_OPTIONS=detect_leaks=1`.
Cross-build Scribe and filedialogtest through `userland/GNUmakefile`; both
link against the rebuilt libos64 with warnings treated as errors.

## Native QEMU

Disposable copies of the repository disks, Q35, 4 CPUs, 4 GiB RAM, 1024 × 768,
and the kernel built 2026-10-21 01:34:18 UTC. The root copy received the rebuilt
Scribe, filedialogtest and library dependencies. The scratch ISO's first boot
entry enabled GUI, BACKSTOP=10 and DEBUG_GUI. Repository disks and P5 files
were not modified by this QEMU run. Chris's subsequent hardware report is
recorded separately below.

1. Scribe [Open initially](open-full.png), then Ctrl+Alt+right-drag resized it
   to [680 × 500 content pixels](open-resized.png). Cancel closed it.
2. Scribe [Save restored 680 × 500](save-restored.png). Choosing `gui.conf`
   showed the [replacement message](replace-prompt.png); Escape refused
   replacement and removed the message row. No destination file was written.
3. With that Save dialog still open, `/tests/filedialogtest open /home` launched
   from VT1. Its [Open window restored 680 × 500](other-app-restored.png),
   confirming sharing across applications/processes.
4. That second app resized to [480 × 440 with wrapped buttons](other-app-wrapped.png),
   then cancelled. Scribe's older, unchanged Save dialog then cancelled too.
5. Scribe [Open restored 480 × 440](scribe-newest-size.png), demonstrating
   that closing the older dialog did not overwrite the newer choice.
   After both apps closed, [the persisted file](saved-setting.png) still read
   `size = 480,440`.

[Window log](native-windows.txt) records frame creation and content resizing;
frame dimensions include decorations (2 × 21 pixels in this setup).
The VM was shut down before [offline inspection](native-config.txt): the saved
pair was `480,440`, and read-only `e2fsck -fn` passed with exit status zero.
The [stale-reference audit](stale-refs.txt) lists live E5 document links; each
was inspected and remains valid.

## P5 report

Chris tested at 2560 × 1440 with an Appearance Workshop appearance and a Frame
Studio composition. He reported both dialogs looking correct; resizing Open
was retained when reopening Open and opening Save As, including after reboot.
Navigation and New Folder also passed his checks. No installed binary hash or
hardware screenshot was captured. The plain QEMU frame above does not show
his customized appearance/composition.

He noted that the Pin button appears in the dialog titlebar. This is inherited
from the selected frame's ordinary-window actions. Parent-owned stacking and
a corresponding dialog Pin policy are separate follow-up work.
Independent review and merge remain pending. The [slice record](../../E05A_DIALOG_POLISH.md)
contains delivery and acceptance instructions.
