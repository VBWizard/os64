# Source-over drawing on opaque canvases

`os64_draw_blend` in `os64/draw.h` draws straight-alpha ARGB pixels over a
GUI surface. It is a shared libdraw operation, shipped in `libos64.so`, and
knows nothing about image formats. Image decoders remain in their own
libraries. The browser, image viewers, icons and other graphical consumers
can use it. `os64_draw_blit` retains its verbatim-copy contract.

## Pixel contract

For source alpha `a` in 0..255, each RGB output channel is:

```
(source * a + destination * (255 - a) + 127) / 255
```

The division uses integer truncation after the rounding offset. This is
nearest-integer source-over for an opaque destination, mixing encoded color
bytes without linear-light conversion. The basis is the W3C
[simple alpha compositing equation](https://www.w3.org/TR/compositing-1/#simplealphacompositing)
with backdrop alpha set to one.

- Alpha 0 leaves the complete destination word unchanged, including its
  reserved high byte and regardless of the source's hidden RGB values.
- Alpha 255 copies the source word exactly. Consecutive opaque pixels use
  a copy loop without destination reads or blending arithmetic.
- Intermediate alpha mixes RGB and writes `0xFF` in the destination's
  reserved high byte. Destination alpha is not read as coverage.

This operation is for an opaque RGB canvas. It does not produce composited
RGBA images or make windows translucent. Premultiplied input, constant-alpha
fades and linear-light blending require separate contracts.

## Placement and storage

The signature matches blit: destination surface, signed x/y, source pointer,
width/height and source pitch in pixels. Negative origins crop the source's
top/left; the other edges crop at the destination bounds. Clipping precedes
source pointer arithmetic. Source pitch may exceed width for subimages, and
destination row padding is not written.

Source and destination must not overlap. Buffers must cover their declared
dimensions/strides; the destination is a valid GUI surface. Null source or
destination, zero size, source pitch narrower than width and source axes
above INT32_MAX are no-ops, as are fully offscreen placements. No allocation
or syscall is needed to blend pixels.

## Gview

Gview clears its canvas to the neutral mat and uses the shared operation for
image pixels. PNG fractional transparency blends with that mat; GIF binary
transparency skips or copies pixels. JPEG, PPM and supported BMP output is
opaque and follows the same copy path. Dispatch follows the decoded pixels,
so gview needs no format detection or new metadata field in the image API.
Animation timing, pause/restart and image geometry are unchanged.

The optional BMP bitfield/alpha-mask decoder work remains in DEBTS.md.

## Validation

`ASAN_OPTIONS=detect_leaks=0 python3 tools/test_jpeg_host.py` runs the extended
image/drawing harness. It compares all 16,777,216 alpha/source/background
channel combinations against an independent floating-point oracle, with
additional exact endpoint/rounding cases. It checks mixed-alpha rows,
clipping at each edge and extreme origins, source subimages, destination
padding/guards, invalid inputs, opaque equality with blit and the preserved
verbatim-copy behavior for alpha-zero/partial-alpha input.

Guest screenshots and recorded results belong in
[the source-over evidence](docs/source-over-evidence/README.md).
