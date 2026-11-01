# C6b — Canvas shadows

Baseline: `72e716de`, the integration merge of C6a PR #451.

## Behavior

Canvas contexts expose saved/resettable `shadowColor`, `shadowBlur`,
`shadowOffsetX` and `shadowOffsetY`. Invalid values leave the previous value
intact. Bindings finish script coercion before accessing the current native
context, including coercion that resizes the canvas.

Fill, stroke, image and outline-text draws prepare source alpha before
clipping, including off-bitmap geometry that can cast a visible shadow.
Offsets and blur remain in device coordinates. Source alpha includes paint
alpha and globalAlpha; shadow color alpha multiplies it once. Shadow and
foreground then use the selected compositor separately through the clip.
Clear and pixel transfer ignore shadows.

The shared uint16 blur from C6a supplies a bounded Gaussian approximation.
Adjacent integer three-box results are interpolated to match the requested
variance; sigma = shadowBlur / 2 is capped at 64 device pixels. Saved blur
values are not capped. Fractional offsets use bilinear alpha sampling. The
approximation is not pixel-identical to an exact Gaussian kernel.

Source and shadow patches are cropped and charged to the context ledger.
Allocation, geometry rasterization and source sampling finish before bitmap
writes. Allocation refusal leaves pixels and damage intact. Final composition
scans the bitmap, including transparent source pixels needed by destructive
operators, and reports the bounds of pixels that actually changed. This
whole-bitmap scan is a remaining optimization opportunity.

## Verification

- Native Canvas/geometry suite: **705,012 assertions**, including the baseline
  170,761 core assertions. New checks cover off-bitmap casters, post-cast
  clipping, transformed shapes with untransformed offsets, fractional blur
  and offsets, alpha multiplication, copy, transparent images, gradients and
  strokes. An independent floating-point oracle checks both composition
  passes for each supported operator, including antialiased clip edges. Both allocation-refusal modes traverse
  30 failing allocation positions without pixel changes or leaks.
- DOM Canvas suite: **136 checks in each of the target and sanitized engine
  profiles**, including reentrant coercion, same-value reset, save/restore,
  invalid setters, pixel-transfer isolation and real-font filled/stroked text.
- Complete browser suite: **16,555 checks, zero failures**. New shadow-only
  damage checks compare partial and full paints with scrolling, object-fit,
  overlapping content, transforms and opacity, and preserve layout identity.
- Gallery harness: **zero failures**. The real page's four buttons run and
  export retained bitmaps; scheduled frames move the light and shadow without
  changing layout or the DOM version, and pausing cancels the pending frame.
- Native Yonder and matching shared libraries build successfully.

Host suites retain AddressSanitizer, UndefinedBehaviorSanitizer and
LeakSanitizer. The isolated QEMU gallery renders the four examples and runs
its moving-light animation. This is a native smoke test, not P5 acceptance
or a performance measurement.

## Gallery and remaining acceptance

`tools/pages/canvas-shadows.html` presents a moving light, an off-canvas ring
casting into a clipped window, transparent canvas-to-canvas stamps, and
filled/stroked text. Its animation reuses its gradient and sprite drawing
reuses one source canvas.

P5 acceptance is pending for the frozen C6b delivery. Enable Scripts and
Modern Web, exercise the controls and animation, and inspect 25%–400% zoom.
Full signature-wall, scratch-game and selected cursor-effect acceptance
remain C6 work; this slice does not claim those sites work end to end.

## Upstream shadow profile

WPT pin: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`. The unmodified
shadow test pages and helpers report **51 passes, 8 failures, zero timeouts**.
The expectation file records those 51 passes and six required checks.
Four failures call the unsupported `createPattern`. Four image checks draw
at document load before their images have decoded: the existing image-load
lifecycle limitation recorded in DEBTS.md also affects these tests.

A separate private diagnostic changes the helper to wait for the source
image's load event. All six image-shadow checks then pass with the upstream
pixel assertions unchanged. Those diagnostic results are not added to the
unmodified WPT pass list. The native transparent-image and real gallery
canvas-source tests independently cover shadow alpha.
