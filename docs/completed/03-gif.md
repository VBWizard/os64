# 03 — GIF in libimage

## Completed — 2026-09-25

Delivered in [PR #135](https://github.com/VBWizard/os64/pull/135) (first-picture
GIF decoding) and [PR #136](https://github.com/VBWizard/os64/pull/136)
(incremental sequences and gview animation). Current contracts are
[GIF.md](../../GIF.md) and [GIF_ANIMATION.md](../../GIF_ANIMATION.md), with
[first-picture evidence](../gif-evidence/README.md) and
[animation evidence](../gif-animation-evidence/README.md).

The original planning handout follows as history. Its booked animation work
was delivered as the second PR. GIF binary transparency is displayed in
gview; general fractional-alpha compositing belongs to packet 02.

## Original packet

*libimage reads PNG, JPEG, BMP and PPM. The old web — the part a browser
without JavaScript reads best — is made of GIFs: every 1990s bullet,
divider, "under construction" sign and animated flame. A browser that
draws `[image]` for all of them is not showing the web it can actually
render.*

## Where it stands

- `os64_image_decode` (`userland/libimage/image.c`) dispatches on magic
  bytes: PNG and JPEG to their own libraries (`libpng.so`, `libjpeg.so`,
  each behind a status enum mapped onto `os64_image_status_t`), PPM and BMP
  decoded in `image.c` itself. Output is tightly packed ARGB, `os64_malloc`'d,
  freed with `os64_image_free`. Dimension cap `OS64_IMAGE_DIM_MAX`, a pixel
  and working-memory cap behind `OS64_IMAGE_LIMIT`, and the doctrine at the
  top of the file: a header that disagrees with the file is refused
  (MALFORMED), a legal variant not decoded is UNSUPPORTED.
- `tools/test_image_host.c` hand-builds files byte by byte and checks
  exact pixels under ASan; `tools/test_png_host.sh` and
  `tools/test_jpeg_host.py` run the two codecs against reference
  decoders on the host. That differential-against-a-reference pattern is
  the house's for codecs and it is what this packet reuses.
- Drawing a decoded GIF with transparency needs packet 02 (source-over);
  the DECODER does not, and the two land independently.

## Scope and ownership

Own a GIF decoder as `userland/libimage/gif.c`, dispatched from `image.c`
by the `GIF87a`/`GIF89a` signature, built into `libimage.so` (a second
object in `libimage/shared.mk`); its host harness; and the `os64get.conf`
routing line for `*.gif` beside the existing `*.bmp` one so a fetched GIF
lands where gview finds it. Not its own `.so`: it is a few hundred lines
with no dependency, where libpng and libjpeg earned separate libraries by
size and by sharing inflate.

## Deliverable

**GIF87a and GIF89a, first frame, to ARGB.** The whole format, in the
order the bytes come:

- Header, logical screen descriptor, global colour table (2..256 entries),
  background index (ignored for pixels: a browser paints the page behind).
- Extensions: the Graphic Control Extension is READ (transparency flag and
  index → alpha 0x00 for that index, 0xFF for everything else; disposal
  and delay noted and ignored for a first frame); Comment, Plain Text and
  Application extensions (`NETSCAPE2.0` looping) are skipped by their
  sub-block chain. An unknown extension label is skipped the same way,
  not refused: the sub-block framing is what makes that safe.
- The image descriptor: position and size within the logical screen (a
  first frame smaller than the screen is composed onto a transparent
  screen of the declared size, because that IS the image the file
  declares), local colour table if flagged, interlace flag (the four-pass
  row order, 8/8/4/2 starting at 0/4/2/1).
- **LZW**, variable code width from `min_code_size + 1` up to 12 bits,
  Clear and End-of-Information codes, the "deferred clear" quirk (a
  stream that keeps emitting at 12 bits without clearing is legal and
  common), and the first-code-after-clear and KwKwK cases that every
  fresh LZW decoder gets wrong once. Codes arrive in sub-blocks of at most
  255 bytes, LSB-first bit packing. Lineage worth a comment: LZW is
  Welch's 1984 refinement of Lempel and Ziv's 1978 scheme, CompuServe put
  it in GIF in 1987 without noticing Unisys's patent, and the 1994 licence
  demand is why PNG exists at all — its name was proposed as "PNG's Not
  GIF". The patent expired in 2004; the format outlived the quarrel.
- A table entry past the image's colour table is a malformed file
  (MALFORMED), not an out-of-bounds read.
- Truncation: a stream that ends before End-of-Information or before the
  trailer is MALFORMED under the file's doctrine, and the pixels are not
  published — "show what decoded" is a real browser behaviour and is
  BOOKED with its trigger below, because it needs a status the enum does
  not have and a policy decision about partial images that PNG and JPEG
  would then also owe.
- Limits: the same dimension and pixel caps as the other arms; the LZW
  table is fixed-size (4096 entries) so its memory is a constant.

Second and later frames are read past, not decoded (animation is booked).

## Required evidence

- `tools/test_image_host.c`: hand-built GIFs, every byte written in the
  test — a 2×2 with a global table, one with a local table, one interlaced
  (the row order is the whole test), one with a transparent index, one
  whose frame is offset inside a larger screen, one that hits the 12-bit
  width and a deferred clear, one that truncates in the middle of a
  sub-block, one that truncates before the trailer, one with an unknown
  extension label, and the huge-header case that must NOT allocate before
  it refuses (the vacuous-assertion trap the file's own comment names).
- A differential run against Pillow: a Python generator writes a corpus
  (bit depths 1..8, interlaced and not, transparent and not, local tables,
  sizes from 1×1 to a few thousand wide, frames offset in their screen)
  and the decoder's ARGB is compared pixel-exact with PIL's RGBA. Every
  file in the corpus is also truncated at EVERY byte offset and fed back:
  under ASan the outcome must be OK or MALFORMED and never a crash, never
  a read past the buffer, never an allocation the header did not justify.
- Guest: `os64get` a real GIF from the old web (textfiles.com and
  Floodgap's gopher menus both serve them), gview it, screendump.
- e2fsck stays green if anything in the run writes to the image (it does:
  os64get writes to /home).

## Booked out of this packet, by name

| What | Why | Trigger |
|---|---|---|
| Animation | a first frame is what a page needs to lay out; playing frames is a timer in a face, not a decoder question, and it wants an image type that holds frames | the first page whose meaning is in the animation (mostly it is the ads) |
| Show-what-decoded on truncation | needs a PARTIAL status and a policy PNG/JPEG would owe too | a truncated image worth seeing half of |
| The background colour index | a browser paints the page behind an image; a viewer paints its own | never for yonder |
