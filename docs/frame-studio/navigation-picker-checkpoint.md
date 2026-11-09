# Shared list navigation and stable picker hue

Implemented in the uncommitted `codex/frame-studio` worktree.

- The shared list already implements Up/Down, Home/End and Page Up/Down.
  A navigation command now reveals its selection even if that row was already
  selected but a companion scrollbar moved it out of view. It does not emit a
  redundant selection-change callback.
- Frame Studio focuses its color list when opening Finish or choosing Edit
  button color. The list caption names keyboard navigation instead of suggesting
  an unavailable scroll-wheel path. Built-in scrollbars are deferred.
- The shared picker treats setting its current opaque RGB as a no-op, retaining
  HSV coordinates that RGB quantization cannot recover. Frame Studio also skips
  the redundant setter. Other RGB values still synchronize normally.

## Validation and limits

The ASan/UBSan appearance suite passes, including shared widget tests and the
Appearance/Control Center consumers. New picker tests sweep pale/dark shades
at 52 hues with a callback echoing each RGB, and exercise hue changes at black
plus a different external RGB. The new test failed before the setter fix.
New list tests use both PS/2 and USB HID scancodes/modifiers for all six keys,
click/release focus, viewport following, boundary no-ops, empty and one-row
lists. The off-screen unchanged-selection case failed before its fix.

QEMU at 1920x1080, with a USB keyboard device configured: before these changes,
clicking the color list then using Home, End, Up and Page Up worked. The P5
report of arrows failing after a click has **not been reproduced or assigned a
proven cause**. No keyboard driver changes were made. Host event tests cover
both dialects; physical P5 confirmation is outstanding.

After the change, opening Pin's color followed immediately by Home showed
Active title without another click. Up/Down, Page Up/Down and End reached the
expected rows. A drag through pale and dark yellows retained the hue: the strip
and marker (excluding the mouse cursor) were pixel-identical before and after;
the upper palette sample was unchanged. Evidence is in
[navigation-picker](navigation-picker/).

Strict userland/kernel/ISO build and whitespace checks passed. Normal Limine
configuration was restored; the isolated test VM was stopped. The stale-reference
check reports the existing NO_DECORATIONS prose shorthand; its flag is live.
