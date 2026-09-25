# Slider wheel validation

Wheel-up/right increases a hovered slider by its configured step per notch;
down/left decreases. Values clamp, callbacks fire on changes, keyboard focus
is preserved, and an active pointer grab consumes wheels without adjustment.

- Strict full `make -j8`: passed.
- Real-font UI suite with ASan/UBSan: 1,506 checks, zero failures. Slider
  dispatch checks step size, sign, multi-notch input, both bounds, callback
  counts, zero deltas, hidden/disabled controls, focus/grab preservation and
  large-step arithmetic across the full signed 32-bit range.
- Appearance host suite: passed.
- QEMU USB mouse, Frame Studio: hover Title padding without clicking;
  two up notches change 3 px to 5 px and repaint the preview; two down
  notches restore 3 px and the preview. Slider and preview crops both
  restore pixel-identically. Screenshots are before/up/down.png.
- `git diff --check` and `tools/stale_refs.sh`: clean.

Chris's P5 confirmation of console and title-font list scrolling preceded
this slider change. Slider hardware validation is not claimed here.
