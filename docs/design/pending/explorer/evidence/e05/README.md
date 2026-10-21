# E5 Scribe integration evidence

Scribe implementation `29252438` is built on `aa04123c` with the shared dialogs
from `42410e34`.
[Source and binary hashes](sha256.txt) identify the tested implementation.

## Host

- [Scribe](scribe-host.txt): 2,584 checks, zero failures. Controlled dialog
  results exercise pending operations, menu retirement, defaults, cancellation,
  unsaved edits, missing Open targets, exclusive-create races, approved
  replacement, write/sync refusal, delayed cleanup and save-before-quit.
- [Menu consumers](menus-host.txt): 1,655 checks, zero failures. Includes real
  File → Open/Save As dispatch and blocking F10 while the picker is active.
- [Shared dialogs](dialog-host.txt): 1,928 checks, zero failures, using the real
  dialog/browser/data-view composition and fonts with mocked system calls.

Reproduce with:

```sh
python3 tools/test_scribe_host.py --build-only --output /tmp/scribe-host
python3 tools/test_scribe_host.py --menus --build-only --output /tmp/scribe-menus
python3 tools/test_file_dialog_host.py --build-only --output /tmp/dialog-host
ASAN_OPTIONS=detect_leaks=1 /tmp/scribe-host/test_scribe userland/libfreetype/fixtures
ASAN_OPTIONS=detect_leaks=1 /tmp/scribe-menus/test_menu_apps userland/libfreetype/fixtures
ASAN_OPTIONS=detect_leaks=1 /tmp/dialog-host/test_file_dialog userland/libfreetype/fixtures /tmp/dialog-host/save.ppm
```

All three passed with ASan, UBSan and leak detection enabled outside the
sandbox. [The sandbox diagnostic](lsan-sandbox.txt) records the actual thread
attachment EPERM. The outside runs reused the built binaries; leak detection
was not disabled. Final Scribe/menu relinks reused unchanged FreeType objects.
Both runners include `shared/damage.c`, required by the merged UI repaint code.

## Cross-build and native QEMU

[Cross-build](cross-build.txt) links Scribe against the new libos64. The scratch
VM used copies of the repository disks and the October 19 kernel, 4 CPUs and
4 GiB RAM. Its root filesystem received the built Scribe and library payload
before boot; no P5 files or repository disk images were modified.

The final build exercised unnamed save-before-quit, explicit replacement of a
filename containing spaces, Open, the unsaved-document confirmation,
cancellation after that confirmation, and Save As to a second name. Offline
[byte comparisons](native-files.txt) verify the original and copy contents and
that the installed Scribe matches the final cross-built binary.
[Exit logs](native-exits.txt) include the preliminary run at 12:28–12:29 and the
final runs at 12:31–12:33; all three Scribe processes exit with code zero.

Screenshots: [Save](final-save-dialog.png), [replacement](final-replace-prompt.png),
[save and quit](final-save-and-quit.png), [Open](final-open-dialog.png),
[opened document](final-opened.png), [dirty Open](final-dirty-open-prompt.png),
[cancelled Open](final-open-cancel.png), [Save As defaults](final-saveas-ready.png),
and [saved copy](final-copy-saved.png).

A preliminary scripted interaction sent the next menu shortcut before the
previous dialog finished closing under TCG. The repeated interaction waits
for teardown and checks the resulting screen and file bytes. These runs do
not establish a P5 latency measurement.

After these captures, Chris reported a clean Codex review and successful P5
use, including resizing; no installed binary hash was captured. PR #319 merged
as `ddaad166`. The [slice record](../../E05_SCRIBE.md) contains the hardware
walkthrough and direct-write limitation.


The [stale-reference audit](stale-refs.txt) reports E4 document identifiers and
links moved by the status update. Those files still exist and the listed links
remain intentional, including historical session/evidence records. Each hit
was inspected; no retired code identifiers or new superlative claims remain.
