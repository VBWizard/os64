# Open With refinement — 2026-10-21

The application picker shows Back/Forward/Up/Refresh, location and Details,
with Open/Cancel and an initially unchecked remember option. Other file-dialog
modes keep their controls. Errors and incomplete-directory notices remain
visible when needed. Extensionless files do not offer association persistence.

## Host and build

- [Launch/config/Roam tests](launch-host.txt) pass with ASan, UBSan and LSan.
  They compile the production writer and verify inherited defaults on first
  save, custom absolute destinations, preserved comments and unrelated rules,
  duplicate replacement, invalid keys/values, missing or malformed input,
  failed reads/allocations/writes/sync/rename, cancellation, unchecked choices,
  failed launches, cleanup retries and config selection held through a picker.
- [File-dialog tests](dialog-host.txt): 4,760 checks, zero failures. Real widget,
  browser and font code covers compact/wrapped application layout, selection
  clearing, folder/Up navigation, navigation errors, hidden controls, mouse
  checkbox/Open events, and the existing Open/Save sizing/font regressions.
- [Cross-build](cross-build.txt) passes with warnings treated as errors.
  [Hashes](binaries.sha256) identify the clean app/library artifacts.

The sandbox denied LSan thread attachment with EPERM; [diagnosis](lsan-sandbox.txt).
Both existing test binaries passed outside the sandbox with
`ASAN_OPTIONS=detect_leaks=1`; leak checking was not disabled.

Reproduce with `tools/test_roam_launch_host.py` and
`tools/test_file_dialog_host.py` (both accept `--build-only --output DIR`), then
run the produced binaries with leak detection enabled. The dialog binary's
first argument is `userland/libfreetype/fixtures`.

## Native

Scratch ext2 root/home images, QEMU q35, four CPUs, 4 GiB, 1024×768. The clean
picker is captured in [picker.png](picker.png); selected Scribe with remember
checked in [selected.png](selected.png). Both keyboard activation and the Open
button launch Scribe ([clean launch](launched.png), [saved config](saved-config.png)). The saved custom config retains `.xyz=/bin/scribe` and
adds `.txt = /bin/scribe`. An ordinary Open uses the mapping after reboot.

The QMP driver’s standalone `input-send-event` button sequence was unreliable
for release-dependent controls. Targeted diagnostics distinguished cancellation
from acceptance; HMP `mouse_button 1` followed by `mouse_button 0` verified the
Open button. Diagnostics were removed before the clean build. No input-driver
or worker-lifetime change is included.

## P5 review

Chris accepted the original file actions before requesting this refinement;
no installed hash was captured. This refinement still needs P5 acceptance and
independent review. Try Open With with and without Remember, relaunch Roam,
and open the document normally. Confirm the compact layout with the preferred
Appearance/Frame. Ordinary Scribe Open/Save should retain their existing layout.

Saving requires a readable valid source config. Bare names preserve the found
config in a top-search-directory override; absolute paths update that file.
Write failure reports that the application started but its association was not
saved. Atomic publication protects complete files; concurrent saves can still
supersede each other.
