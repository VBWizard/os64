# 02 — Source-over compositing in libdraw

## Completed implementation — 2026-09-25

Delivered for review in [PR #137](https://github.com/VBWizard/os64/pull/137).
The current API contract is [SOURCE_OVER.md](../../SOURCE_OVER.md), with
[host, QEMU and P5 evidence](../source-over-evidence/README.md).

`os64_draw_blend` composites straight-alpha pixels over opaque GUI canvases,
and gview uses it for image drawing. Opaque pixels take its copy path;
`os64_draw_blit` retains its verbatim-copy behavior. The optional BMP
alpha-mask decoder remains separate work in [DEBTS.md](../../DEBTS.md).

The packet below is the original planning handout, preserved as history.
Implementation completion does not imply independent review or merge approval.

## Original packet

*The DEBTS row "`os64_draw_blit` is an OPAQUE COPY — no source-over
blending" named the browser as its customer. yonder is here.*

## Where it stands

- `os64_draw_blit` (`userland/libos64/include/os64/draw.h`, `draw.c`)
  copies XRGB pixels verbatim, clipping at every edge, with a source pitch
  so a sub-image blits correctly. Its contract says out loud that the
  alpha byte is not consulted, and that changing that would break every
  background that relies on an exact copy. **That contract stays.**
- libpng preserves real alpha in its ARGB output (straight alpha, not
  premultiplied); libjpeg and PPM produce 0xFF; BMP produces 0xFF because
  libimage refuses `BI_BITFIELDS`, which is how a BMP carries an alpha
  mask. GIF (packet 03) will produce 0x00 for the transparent index and
  0xFF elsewhere.
- gview blits a decoded image over `GVIEW_BACKGROUND` verbatim, so a
  transparent PNG shows whatever its transparent pixels' colour bytes
  happen to hold.
- `tools/test_image_host.c` already tests the blit with exact pixel values;
  it is the harness this packet extends.

## Scope and ownership

Own a new operation in `draw.c`/`draw.h`, its host tests, gview's use of
it for images that carry alpha, and (optional second half) libimage's BMP
arm for `BI_BITFIELDS` with an alpha mask. Do not change `os64_draw_blit`.
Do not touch the kernel compositor: window-to-window translucency is
GRAPHICS.md's limitation #7 and a different thing.

## Deliverable

**`os64_draw_blend`** — Porter and Duff's *over* operator (SIGGRAPH 1984,
"Compositing Digital Images", the paper that gave every graphics system its
compositing algebra): `dst = src + dst × (1 − α)`, with `src` in STRAIGHT
alpha as libpng hands it over, so the multiply by α happens here. Same
signature and clipping contract as `os64_draw_blit` (destination, x, y,
source, w, h, source pitch); a negative origin clips top and left the same
way. The destination's alpha byte is written 0xFF — it is the framebuffer's
reserved byte and nothing reads it. Integer arithmetic only, rounding so
that α = 255 reproduces the source byte-exactly and α = 0 leaves the
destination byte-exactly; the two fast paths (a run of 0xFF rows copies,
a 0x00 pixel skips) are worth having because most of a "transparent" icon
is one or the other. Blending is done in the sRGB byte space browsers blend
in — NOT linearised first — because matching what a page's author saw
matters more here than being physically right (booked below).

**gview** uses it for a decoded image whose format can carry alpha (PNG,
GIF), keeps `blit` for the rest, and the difference must be visible: a
transparent PNG over the viewer's background shows the background through.

**Optional second half, libimage BMP:** `BI_BITFIELDS` with the four masks
read from the header, so a 32-bit BMP with an alpha mask decodes to real
alpha instead of being refused UNSUPPORTED. Same host harness, a hand-built
file whose every byte is written in the test, cross-checked against the
PNG of the same picture the way the PPM/BMP cross-check works today.

## Required evidence

- `tools/test_image_host.c`: exact pixel values for α = 0, 255, 128 and 1
  over known backgrounds (the α = 1 and α = 254 cases catch the rounding
  direction); clipping at all four edges and at a negative origin; a
  sub-image via pitch; a source row that is all 0xFF equals `blit`'s
  result byte for byte; a 0x00 pixel leaves the destination untouched.
  Under ASan, as the suite runs today.
- Guest: gview a PNG with alpha in QEMU, screendump, and see the
  background through it. A checkerboard PNG with half its squares
  transparent is the fixture that makes a wrong alpha channel impossible
  to miss.
- No change in any existing screendump-based test: `blit` is untouched
  and nothing else calls the new verb yet.

## Booked out of this packet, by name

| What | Why | Trigger |
|---|---|---|
| Linear-light (gamma-correct) blending | the web blends in sRGB space and so must a browser that wants to match; a correct-physics blend would make every page's soft edge look different from every other browser's | never for yonder; a compositor or an image editor that wants it asks for a flag |
| Premultiplied alpha as an input format | libpng gives straight alpha; a second convention is a second bug | a decoder that produces it |
| Destination alpha / window translucency | the compositor's, GRAPHICS.md #7 | that arc |
| A blend with a constant α (fade) | no consumer | a UI transition that wants one |
