# GIF in libimage

`os64_image_decode` recognizes `GIF87a` and `GIF89a` and returns the first
image raster on a logical-screen-sized ARGB canvas. `os64_image_load` supplies
file I/O; `os64_image_free` releases the pixels. The decoder lives in
`userland/libimage/gif.c`, built into `libimage.so`. First-picture decoding
uses the ordinary image API; animation adds the sequence API described in
[GIF_ANIMATION.md](GIF_ANIMATION.md). Neither requires a separate shared
library, kernel code, or libos64 dependency on an image codec.

## Pixel and stream contract

- Global and local palettes, interlaced four-pass rows, frame offsets, and
  transparent palette entries are supported. Output is straight-alpha
  `0xAARRGGBB`, tightly packed, top row first. Transparent palette pixels
  retain their palette RGB with alpha zero.
- Pixels outside the first raster are transparent black. The logical-screen
  background index is deliberately ignored. This browser-facing policy
  differs from filling the canvas with the GIF background color.
- LZW supports minimum code sizes 2 through 8, widths through 12 bits, Clear,
  End of Information, dictionary self-reference (KwKwK), and a full dictionary
  that continues without Clear. A one-bit palette uses minimum code size 2.
  An initial Clear is accepted but not required; the specification recommends
  it to encoders rather than requiring it of a decoder.
- The first raster must produce its exact pixel count and reach End of
  Information. Palette references outside the active table are malformed,
  including a used transparent index outside that table.
- A structure pass validates block lengths, fixed extension headers, image
  rectangles, code-size bytes, and a final trailer before pixel allocation.
  Bytes after the trailer are refused. Duplicate or dangling graphic controls,
  reserved image/GCE bits, and reserved disposal methods are malformed.
- Comment, Application, Plain Text, and unknown extensions are skipped by
  their sub-block framing. Plain Text is not drawn. It consumes pending graphic
  control settings, as do unknown labels in the graphic-rendering range;
  comments and application extensions leave those settings pending.
- Later image descriptors, palettes and sub-block framing are checked, but
  their compressed codes are skipped. Success proves a valid first raster
  and complete surrounding framing, not semantic validity of later LZW data.
- A stream without an image raster is malformed for this API. A first raster
  that requires a palette inherited from another stream is unsupported;
  this decoder has no cross-file palette state.

These choices implement the first-frame work packet, with the framing and
LZW rules grounded in the [GIF89a specification](https://www.w3.org/Graphics/GIF/spec-gif89a.txt),
particularly its deferred-clear cover sheet, extension scope, and Appendix F.
GIF is CompuServe's Graphics Interchange Format and service mark.

## Bounds and failure behavior

Both logical-screen axes are limited to `OS64_IMAGE_DIM_MAX` (16,384), and
the screen has at most 16 Mi pixels, matching the PNG/JPEG default pixel
budgets. Zero dimensions are malformed; exceeding a resource cap returns
`OS64_IMAGE_LIMIT` before allocating. Raster rectangles must fit the screen.

The output costs at most 64 MiB. Scratch is one 16 KiB allocation containing
4096 LZW prefixes, suffixes, and the expansion stack, plus fixed local parser
state. Compressed sub-blocks are read directly from borrowed input; no
concatenation or per-frame allocation is needed. The load API's default
encoded-file cap remains 20 MiB. The decode API accepts caller-owned buffers
without imposing a separate encoded-byte cap; processing is bounded by input
length and the first raster's pixel budget.

Failure leaves the public result zeroed and frees decoder-owned allocations.
No partial image is returned. File truncation after a recognized six-byte
signature is malformed; shorter prefixes follow the dispatcher's
`OS64_IMAGE_UNKNOWN_FORMAT` contract.

## Validation

Run `python3 tools/test_gif_host.py` for the ASan/UBSan corpus. It includes
hand-built code streams and independently Pillow-encoded pictures, palettes
of 1 through 8 bits, interlace at short-height boundaries, offsets,
transparency, sub-block splits, dictionary growth through 12 bits, deferred
clear, KwKwK, extensions, malformed input, and resource caps. For valid files
it checks every truncated prefix, forced failure of each allocation, and
400 deterministic mutations. Live allocation tracking checks cleanup.

Pillow supplies exact first-frame reference pixels. For offset images, the
harness explicitly clears only the margins outside that raster to match our
canvas policy; it preserves Pillow's frame RGB and alpha. Plain Text control
scope has a hand-selected oracle because that scope is part of our parser's
contract even though the text itself is not rendered.

`tools/test_image_host.c`, linked by `tools/test_jpeg_host.py`, additionally
checks GIF dispatch, exact pixels, truncation before allocation, limits and
missing EOI alongside BMP/PPM, drawing and JPEG integration.

`/tests/giftest` checks exact pixels through the real shared library, local
and global palettes, transparency, interlace, offset canvases, KwKwK,
truncation, file loading, and heap integrity. Its checked-in vectors are
selected from the host corpus; no host decoder is needed in the guest. Regenerate
them with `python3 tools/test_gif_host.py --guest-vectors userland/tests/giftest/vectors.h`.

Build and runtime results are recorded in
[docs/gif-evidence/README.md](docs/gif-evidence/README.md).

## Integration and follow-up

The `*.gif` rule in `etc/os64get.conf` routes valet downloads to `/home/images`.
`gview` uses the incremental sequence API described in
[GIF_ANIMATION.md](GIF_ANIMATION.md), including frame disposal, delays and
looping. Fully transparent runs show the viewer's mat. Fractional-alpha
compositing remains the separate source-over drawing feature.

The first-picture API described here does not animate or return partial
images. Sequence playback is a separate opt-in API; Plain Text rendering,
cross-stream palettes and partial-image status remain outside these APIs.
