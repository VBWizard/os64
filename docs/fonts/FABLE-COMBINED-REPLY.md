# Quinn's combined font review follow-up

Fable accepted `10e142d` with no P1/P2 findings in
[FABLE-REVIEW-COMBINED.md](FABLE-REVIEW-COMBINED.md). All four P3s were supported
and are addressed in this follow-up. His original report is preserved unchanged.
The fixes do not change the font contracts or add a syscall.

## Findings

| Finding | Correction | Verification |
| --- | --- | --- |
| P3-1: silent Restore refusal | `wm_set_maximized` logs the window ID, saved content dimensions, and current minimum through `printd(DEBUG_GUI, ...)`. Refusal retains the maximized flag and saved rectangle. | Guest log: `window 2 restore refused: saved content 958x696, minimum 1200x800`. Lowering the minimum then restores the original frame. |
| P3-2: titlebar toggle invalidates saved geometry | While maximized, `wm_set_decorated` applies the same chrome delta to the saved frame as to the current frame. Saved content size and position remain stable. | Reproduced before the fix. Afterward, both toggle directions and toggling twice restore 958x696 content; a real larger minimum still refuses Restore. |
| P3-3: malformed font settings print to stdout | `os64_font_settings_current` sends its diagnostic through `os64_debug_log`, reaching logd's system-log sink. | Malformed personal config, three production applications, and two palette Applies: 12 diagnostics in the guest log, zero bytes in each application's stdout. |
| P3-4: successful installation with refused preview leaves a stale list | Refresh discovery after successful installation regardless of preview adoption. Keep the prior draft/Undo/fonts on refusal, and report that installation succeeded. A subsequent discovery failure explicitly says the list refresh failed. | Added host regression fails against the old handler and passes with the fix, including retained preview/draft/Undo and failed-discovery handling. |

P3-3 preserves retries: subsequent appearance events still reread the file and
can log again while it remains invalid. This removes the launching-console
noise without suppressing recovery after the file is repaired.

## Accompanying notes

- Corrected the header comments that incorrectly described Restore as
  unconditional. Documented saved-frame handling when decorations change.
- Restored gterm's rationale at the relevant owners: preferred grid width for
  `top`, grabbed-drag edge clamping, client-painted cursor, and the
  WM → gterm → PTY/SIGWINCH resize path with its `PTY.md` reference.
- Startup-theme rejection now names the resolved `theme.conf`. The reader
  resolves once and reads that path through the bounded whole-file reader;
  the message cannot accidentally name a later ladder selection. Existing
  startup preservation and real-filesystem saved-theme suites pass.
- Removed Control Center's misleading `fit_text` wrapper and unused pixel
  parameter; callers copy labels and rely on widget clipping.
- Matched statement indentation in the added gterm/configuration code to the
  surrounding tab-indented files, retaining aligned argument continuations.

## Verification and limits

Durable receipts: [f5-evidence/combined-review/](f5-evidence/combined-review/).

- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_appearance_host.sh`: passed.
  The new install regression first failed at the missing catalog refresh on
  the pre-fix handler. The test drives the real page handler and adoption
  transaction with a refusing planner; installation/discovery filesystem
  boundaries use fixtures. This is not a new guest install-failure claim.
- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_appearance_saved_host.sh`: passed.
- Strict root `make`: passed. The booted ISO kernel and the tested applications
  and library in both FAT and ext2 roots match the built artifacts.
- Private 1920x1080 QEMU guest, 8 CPUs: titlebar/Restore sequences, genuine
  minimum refusal and recovery, malformed-font logging, and two live palette
  changes passed. The guest `windowmintest` reported zero failures.
- `git diff --check`: passed. `tools/stale_refs.sh` output was inspected;
  historical evidence paths are retained intentionally, as described in the
  evidence README.

The QEMU checks establish guest behavior, not a fresh P5 validation. The kernel
Restore fixes require the updated kernel; the userland fixes require updated
binaries/library and restarted consumers. No push or mainline merge is included
in this follow-up.
