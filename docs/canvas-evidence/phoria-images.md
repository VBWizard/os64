# Phoria image dimensions

The original Canvas 2D Phoria demos at
<https://kevs3d.co.uk/dev/phoria/test0.html> and
<https://kevs3d.co.uk/dev/phoria/test0t.html> were examined together.
The textured cube loaded its six PNGs but painted without textures: its
renderer uses `bitmap.width` and `bitmap.height` to calculate each triangle's
texture transform. Images made with `new Image()` and no dimension attributes
returned zero from those getters, producing invalid transforms.

The getters now use valid dimension attributes, including an explicit zero,
and otherwise borrow natural dimensions from the existing image provider.
Pending and broken images without valid dimensions return zero. Metadata
queries release their loans, including unsuccessful queries. The setters and
`drawImage` pixel acquisition remain unchanged. The unsigned 32-bit attribute
range retains its existing overflow result of zero.

This implements the non-rendered image case of
[HTML's dimension algorithm](https://html.spec.whatwg.org/multipage/embedded-content-other.html#dimension-attributes).
CSS-sized rendered images still need a content-box geometry read; that existing
limitation is recorded in DEBTS.md. The public DOM/host ABI is unchanged.

## Validation

The DOM image tests cover natural-size fallback, explicit dimensions and zero,
ASCII whitespace and a plus sign, invalid/negative attributes, removal, pending
and broken sources, and balanced metadata loans. The browser integration suite passes 15,942 checks. The DOM behavior run
(`tools/test_dom_host.sh --mutants`, which omits the broad installer allocation
sweeps) passes 8,646 checks with the target engine and 15,871 with the sanitized
engine. ASan, UBSan and LeakSanitizer remain enabled.
The captured textured demo also runs through the real PNG decoder, Canvas
renderer and painter. An isolated native QEMU run displays distinct textures
on the cube's visible faces. The basic cube also animates in that same guest.
The native records contain optional module/vendor-global probes and missing
font/CSS features, with no script failures.

## Basic cube timing investigation

Both original pages use a fixed 768 by 512 canvas and rotate by 0.5 degrees per
animation callback. Rotation speed therefore depends on callback frequency.
Yonder's nominal frame interval is 20 ms, with the existing post-paint pause;
this change does not alter that policy.

At 800 by 600 and 1920 by 1080 host viewports, eight basic-cube frames produce
the same final bitmap hash. After the initial paint, damage is confined to the
visible canvas (762 by 482 and 768 by 512 respectively), with no layouts per
frame. In this sanitizer replay, script/drawing work is about 4.7–5.3 ms and
painting about 1.9–2.5 ms. Enlarging the viewport did not increase those costs.
The native basic-cube record has 907 paints with only two whole-view paints.
These host timings exclude native scheduling, publication and the compositor;
they do not settle the reported P5 window-size effect or establish a hardware
frame-rate improvement. Hardware follow-up should hold zoom constant and
record small/maximized runs before changing scheduling or painting.
