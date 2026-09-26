# Incremental image sequences and GIF playback

`image/sequence.h` adds an owned image-sequence handle to libimage. GIF
sequences decode frames on demand and apply disposal onto a persistent
straight-alpha ARGB canvas. PNG, JPEG, BMP and PPM become singleton sequences
using their existing decoders. The first-picture `os64_image_*` API remains
available with its existing behavior; callers opt into playback explicitly.

## API and ownership

- `os64_image_sequence_decode` copies GIF bytes; callers may release their
  input after the call. `load` reads a file using the usual image cap.
  Both prepare the first frame. Failure sets the handle output to NULL.
- `frame` returns a borrowed view: dimensions, pixels, current frame index,
  frame count, encoded delay in milliseconds, and total play count. Zero
  play count means indefinite looping; absent loop metadata means one pass.
  Pixels belong to the handle and must not be modified or independently freed.
- `next` decodes the next raster, disposes the old frame, and composites the
  new one. It honors finite and indefinite loops. `OS64_IMAGE_END` holds the
  last picture; it is a sequence result, not a failed image decode. A singleton
  holds its picture regardless of a GIF loop extension.
- `rewind` clears the canvas, renders frame zero and resets the pass count.
- `free` releases the handle and its storage. NULL is accepted.
- Failed advances preserve the last good frame, its metadata, and its saved
  rectangle. Callers may report the failure, hold that picture, or rewind.
  Separate handles are independent; callers serialize use of one handle.

## GIF composition

Frames have global or local palettes, their own position, interlace order,
transparency and delay. Transparent pixels leave the current canvas unchanged.
Disposal 0 and 1 retain the picture. Disposal 2 clears the prior frame's
rectangle to transparent black, preserving the browser-facing background
policy of the first-frame decoder. Disposal 3 restores the rectangle saved
before that frame was drawn. The next frame is drawn after that disposal.

A restart clears the canvas; a previous pass cannot leave pixels under a
partial first frame. NETSCAPE2.0 and ANIMEXTS1.0 loop extensions use their
repeat count: zero repeats indefinitely, and a positive count adds that many
passes after the initial pass. Duplicate loop directives and malformed loop
headers are refused.

The parser validates complete framing before opening a handle and records
frame metadata/offsets. Raster LZW is decoded on demand, so a malformed later
raster is reported when reached. Expansion first writes palette indices into
a staging buffer; the canvas and saved rectangle change only after successful
expansion. Playback and rewind allocate no memory.

Interactive GIFs (the Graphic Control user-input bit), Plain Text rendering,
and palettes inherited from earlier files are unsupported by the sequence
API. A caller that only wants a first picture can still use the original API,
which skips Plain Text as documented in GIF.md. Color profiles, pixel-aspect
resampling, seeking to arbitrary frames, and animation encoding are outside
this implementation.

The format basis is the [GIF89a specification](https://www.w3.org/Graphics/GIF/spec-gif89a.txt),
including Graphic Control scope/disposal and Appendix F's LZW rules.

## Resource contract

GIF sequences accept at most 20 MiB of encoded bytes, 4096 raster frames,
16,384 pixels on either axis, and 16 Mi pixels per logical screen. Total
requested owned storage is capped at 128 MiB, checked before allocation:

- the sequence handle and copied input;
- frame descriptors with their palettes;
- one full ARGB canvas;
- one byte per pixel of the largest raster for expansion staging;
- one ARGB rectangle as large as the largest disposal-3 frame;
- the 16 KiB LZW dictionary/stack.

This bounds memory independently of the sum of decoded frame sizes. It also
means a maximum-size image using restore-to-previous may exceed the working
budget and return LIMIT even though its pixel count is allowed. Borrowed
input and allocator bookkeeping are outside the requested-storage budget.
`load` temporarily holds its slurped input while `decode` copies it. Other
formats retain their existing codec-specific limits.

## gview playback

Gview uses sequences for its image input. Multi-frame GIFs start playing;
still images block for window events. `Space` pauses/resumes an active
animation, `R` restarts it, and `Q` closes the viewer. Finite animations hold
the final picture and block until an event; R restarts them.

Frame delays begin after presentation. Encoded delays of 0 or 10 ms use a
100 ms display delay; other delays are honored. The existing bound frame
clock pauses time while the window is covered, minimized, or behind a text
VT. Visible waits are sliced at 20 ms so long-delay frames remain responsive
to close, resize and pause. Slow decoding lengthens playback rather than
skipping disposal operations or running catch-up bursts.

Gview uses [source-over drawing](SOURCE_OVER.md) to composite straight-alpha
image pixels over its neutral mat. Transparent GIF pixels leave the mat
untouched, opaque pixels copy, and fractional-alpha PNG pixels blend. The
sequence API owns decoding; the shared drawing operation owns clipping and
pixel blending. The ordinary blit primitive retains its copy semantics.

## Proof

`python3 tools/test_gif_sequence_host.py` checks reference canvases, delays,
finite/infinite loops, disposal 0–3, successive restore-to-previous frames,
local palettes, transparent loop resets, singleton PPM/GIF, copied input
ownership, each allocation failure, rewind and malformed-later-frame rollback
under ASan/UBSan. Small valid GIFs are truncated at each byte; files over
100,000 bytes use 16,381-byte prefix steps. Each valid GIF also receives 128
single-bit mutations followed by up to eight advances. Resource refusals
assert zero decoder allocations, and playback asserts no allocation attempts.

`--real PATH` includes an external animation in the full-frame reference
comparison. The supplied Hayabusa2 file matches all 416 composed frames over
two loops while owning 9,786,191 bytes (about 9.3 MiB), including compressed
input. Transparent RGB is normalized to zero in the reference canvas because
those pixels have no painted color.

`/tests/giftest` adds exact composed-frame, disposal, loop, rewind, file-load
and heap checks through the actual shared library. Regenerate its sequence
vectors with `python3 tools/test_gif_sequence_host.py --guest-vectors userland/tests/giftest/sequence_vectors.h`. The original GIF corpus
and JPEG/image/drawing regressions continue to exercise the first-picture API.

Recorded host, build and QEMU results are in
[the animation evidence](docs/gif-animation-evidence/README.md).
