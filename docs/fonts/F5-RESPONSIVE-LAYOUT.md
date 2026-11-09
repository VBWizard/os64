# Responsive settings windows and Restore

This follow-up to `78c4b0b` addresses Chris's P5 report: Workshop and Control
Center retained their previous interface font above 16 pixels even with ample
screen space. Their fixed-height controls were the limiting factor. Chris also
requested that a maximized window refuse Restore when its saved rectangle is
below the new font's minimum usable size.

## Behavior

- Workshop measures candidate interface rows and important button captions.
  Its pages, columns and footer adapt together. Apply, Save and Undo stay
  visible. The minimum retains a 958x696 floor and increases with the font.
- Control Center measures its title/navigation and adapts the menu page size.
  Entries remain reachable through Previous/Next; hiding a row clears focus
  from it. Its builtin minimum is installed before configured-font adoption,
  so even a refused startup font leaves a usable window.
- A font may grow a normal window when the new content plus chrome fits on
  screen at its current position. Otherwise adoption refuses locally; the user
  can maximize and retry. No automatic repositioning is introduced.
- Successful adoption installs the measured minimum using the existing syscall.
  Resize reuses the accepted metrics. Font reduction lowers the minimum again.
- The kernel's existing Restore path checks the saved rectangle before resizing.
  Size clamping refuses Restore without clearing the maximized flag or changing
  the saved rectangle. A later valid Restore returns to that rectangle. Both
  title-bar double-click and Ctrl+Alt+M reach this path. No new syscall or ABI.

The default fixed-row guard remains available for other libui consumers.
Workshop's specimen remains an independent local font draft; its custom menu
and button-state painters still use builtin text. This change does not migrate
custom-painted controls or window chrome to scalable fonts.

## Transaction boundary

Application planners measure the candidate and stage bounds without moving
live widgets. Abort preserves fonts, bounds, preview viewport and minimum.
Commit applies precomputed dimensions without measurement or allocation.
Changing the minimum uses the window's reserved screen-sized canvas; planning
checks the screen/chrome constraints before accepting growth. Unexpected loss
of the owned window during commit quits that app with a diagnostic rather than
continuing with mismatched geometry. The confirmation dialog has a separate
measured planner; a dialog which cannot fit keeps its old font.

## Evidence

Receipts and inspected screenshots are in
[f5-evidence/responsive-layout/](f5-evidence/responsive-layout/).

- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_appearance_host.sh` passed with
  ASan/UBSan. The real Workshop planner accepted scalable fixture sizes
  17/24/28/32, and all four pages fit at their exact computed minimums with
  footer controls visible. Preparation followed by abort left editor and
  preview bounds unchanged; 96-pixel refusal retained the accepted state;
  reducing the font restored the builtin-size minimum. The fixture uses a
  1920x1003 available surface, covering the height of a 1920x1024 desktop.
- Control Center's host fixture checks 28-pixel adoption, reduced rows,
  cleared focus on a hidden row, navigation through all nine entries,
  refusal and lowering the minimum.
- Strict root `make` passed, rebuilding userland, kernel and bootable images.
  `git diff --check` and `tools/stale_refs.sh` passed. The old fixed-cell layout
  description in `APPEARANCE.md` was stale and is updated with the code.
- Private 8-CPU QEMU images booted at 1920x1080. Requested 1920x1024 fell back
  to 1024x768 in this QEMU setup, so that attempt is not high-resolution proof.
  Production boot configuration and personal disk images were not changed.
- Workshop's real selector applied DejaVu Sans at 17, then 28 pixels. All four
  tabs and the confirmation dialog were inspected. At 28 pixels its measured
  minimum was 1804x864; a new Workshop grew to that size. Double-click Restore
  stayed maximized. Reducing to 16 and using Restore recovered the saved frame.
- Existing Control Center followed live 17/28/16 changes; a new instance and
  a fresh cold boot with saved 28-pixel settings both displayed the larger text.
  Its 28-pixel minimum was 493x273. The final cold boot used the final binary,
  including focus cleanup and the initial builtin minimum.
- `windowmintest --hold` passed with zero failures. Raising the minimum to
  1200x800 while maximized preserved frame 1920x1080 and flags 8 on Restore.
  Lowering it to 64x32 allowed the original frame `(32,24) 960x717`, flags 0.
  `windowmin.txt` is the guest-written output extracted after shutdown.

The action files can be replayed through `f5-evidence/run_guest.py` with private
images and a private ISO whose GUI resolution is 1920x1080. The `final` boot
uses `fonts-final.conf` as `/home/fonts.conf`; the earlier interactive boot
starts without a personal font configuration. `payload-sha256.txt` identifies
final production binaries and source. The initial interactive run preceded
Control Center's final focus-cleanup/startup-floor adjustments; Workshop and
kernel bytes were unchanged. Large desktop pointer resets in the action file
use extra negative moves to reach the origin before clicking.

P5 validation and Fable review remain pending. Very large fonts can still be
refused when the usable layout cannot fit on screen; this is not a promise
that every face supports every size on every display.


## Incidental close-status comparison

The exploratory shell reported status 137 after closing Control Center.
Before treating it as a regression, the previous `78c4b0b` Control Center source
was compiled separately against the same current libraries and booted as
`ccbaseline`. Alternating current/baseline/baseline/current closes each reported
137 (two of each), without an exception in the serial log. This observation
therefore predates the layout change; its underlying exit-status behavior is
not corrected here. `commands-exit.jsonl`, `serial-exit.log` and
`close-comparison.png` retain the comparison. The baseline executable was only
in the private guest image.
