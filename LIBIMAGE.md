# libimage and the image codecs — pictures stay in userland

*The design record for decoding image files in os64: where the format
boundaries live, how applications get one common pixel plane, and why PNG and
JPEG do not belong in either the kernel or the foundational C library. Design
settled 2026-09-03.*

## The ruling

**Image processing does not belong in libos64.** libos64 is the runtime every
program carries: heap, memory, strings, I/O, processes, time, and the thin GUI
ABI. Making it depend on an image decoder would make `cat`, `husk`, and every
other program load code for parsing pictures.

Serious formats get separate codec libraries. They have different algorithms,
dependencies, test populations, security surfaces, and—in JPEG's case—likely
different authors and licensing. A small `libimage` layer gives applications
one front door and one output representation without turning the codecs into
one source-code blob.

The split is by **codec**, not mechanically by filename extension:

- `libpng.so` owns PNG parsing and decoding.
- `libjpeg.so` owns JPEG parsing and decoding. `.jpg` and `.jpeg` are names for
  the same codec, not two libraries.
- BMP and PPM are small leaf decoders and can live directly in `libimage.so`.
  A shared object apiece would add machinery without buying an independent
  implementation boundary.
- `libimage.so` owns magic-byte detection, dispatch, common status mapping,
  and the normalized image returned to applications. It does not implement
  PNG or JPEG internally.

## Target dependency graph

```text
gview / desktop / future image consumers
                  |
             libimage.so
             |    |    |
      BMP + PPM   |    +--> libjpeg.so --------+
             |    |                             |
             |    +--> libpng.so                |
             |              |                   |
             |          libgzip.so              |
             |       (raw RFC 1951 DEFLATE)     |
             |              |                   |
             +--------------+-------------------+
                            |
                        libos64.so
                 (heap, memory and runtime primitives)
```

The arrows point toward dependencies. They never point back upward:

- libos64 must not acquire a `DT_NEEDED` on an image library.
- libgzip knows nothing about PNG; its raw inflater is useful beyond images.
- libpng knows nothing about windows, drawing, or the format dispatcher.
- libimage knows nothing about surfaces or the compositor. It produces pixels;
  libdraw decides where those pixels go.

This makes the codec libraries optional in the meaningful sense: programs
that do not process images never load them. A program linking `libimage.so`
will load its declared codecs even if a particular file is a PPM. That is an
acceptable trade: the loader shares their read-only pages between processes,
and only image-aware programs pay it. os64 does not need runtime codec plugins
or `dlopen` merely to avoid a few shared mappings; measurements must provide a
reason before that machinery exists.

## Where the tree stands

`userland/libimage/image.c` owns BMP/PPM decoding, complete-file loading, magic
selection and status mapping. Its public header is `image/image.h`.
`gview` and `desktop` link libimage; no image symbols remain in libos64.

`libpng.so` supplies non-interlaced PNG through libgzip's raw inflater.
`libjpeg.so` supplies the bounded libjpeg-turbo decoder and photo orientation
recorded in [JPEG.md](JPEG.md). The four formats share the image result below.
`pngtest` directly checks the PNG codec; `jpegtest` checks JPEG and the common
image entry points through their shared-library dependencies in ring 3.

## The common result

The format-neutral output remains the representation the GUI already speaks:

- width and height as bounded unsigned dimensions;
- one tightly packed `uint32_t` per pixel;
- top row first;
- `0xAARRGGBB` color;
- storage owned by the result and released through its library's matching
  free call.

PNG preserves real alpha. JPEG has no alpha and supplies `0xff`. BMP continues
to supply `0xff` until support for a variant with an actual alpha mask arrives.
The decoder produces pixels, not a surface: pitch, clipping, blending,
placement, damage, and publication remain libdraw concerns.

`os64_draw_blit` remains an opaque copy. gview therefore proves PNG decoding
and retains the alpha byte, but does not composite transparent pixels over its
mat. The browser needs the distinct source-over operation already recorded in
DEBTS; silently changing ordinary blit would break callers that require an
exact background copy.

The common status vocabulary must continue to distinguish:

- unknown format (no recognized magic);
- malformed input (the claimed file is broken or unsafe);
- unsupported variant (a valid form the codec deliberately lacks);
- resource refusal (allocation, file cap, dimensions, or expansion limit).

Collapsing these answers would make a missing feature look like file
corruption—or make hostile input look like an innocent missing feature.

## libpng's boundary

`libpng.so` owns everything between the eight-byte PNG signature and the
normalized pixel plane:

- chunk framing and ordering, critical CRC enforcement, and discard of
  ancillary chunks whose CRC fails;
- IHDR validation and bounded dimension arithmetic;
- concatenating IDAT's logical zlib stream across chunk boundaries;
- zlib header validation and Adler-32 verification around libgzip's raw
  inflater;
- scanline filter reversal;
- supported color types and bit depths, palette expansion, and transparency;
- Adam7 interlace when that variant enters the supported set;
- rejecting unknown critical chunks while safely skipping unknown ancillary
  chunks.

The library currently decodes only. Encoding, metadata editing, color
management, gamma correction, and APNG each require a real consumer before
they enlarge the contract.

Like DEFLATE and the existing BMP/PPM decoders, PNG parsing is hostile-input
work. Every size is checked before multiplication or allocation, decompressed
output is bounded from IHDR before inflate begins, and no chunk length is
trusted beyond the bytes actually present. CRC and Adler checks are part of a
successful decode, not optional diagnostics after pixels have been published.

The codec API uses os64-owned names (`os64_png_*`) even though the shared
object has the conventional `libpng.so` name. That prevents an accidental
claim of source compatibility with the established third-party libpng API.

## JPEG: import the codec, keep the boundary ours

JPEG is a poor candidate for another house implementation. Baseline decoding
alone brings marker parsing, Huffman entropy decoding, quantization, inverse
DCT, chroma upsampling, and color conversion; real files also make progressive
JPEG support valuable. The useful os64 work is the freestanding adaptation,
memory/error boundary, normalized pixel output, and hostile-file proof—not
re-deriving decades of codec behavior.

The JPEG implementation uses a pinned decoder-only import of
[libjpeg-turbo](https://libjpeg-turbo.org/). It supports baseline and
progressive JPEG, has in-memory APIs, and retains the widely used libjpeg API
alongside its simpler TurboJPEG API. Its code is covered by compatible
BSD-style licenses, with attribution requirements recorded in its
[license file](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md).

The port keeps an explicit source manifest and os64-owned adapters:

- decoder and in-memory source path only;
- scalar C first, with SIMD disabled until the ABI and CPU-feature boundary is
  proven;
- os64 allocation and error adapters, with no hosted `stdio`, filesystem, or
  process-exit assumptions escaping the library;
- the smallest auditable source set that still accepts the JPEG population we
  claim to support;
- upstream license texts and modification notices shipped with the source and
  binary distribution.

`stb_image` can be built with only its JPEG decoder and could fit this same
library boundary. libjpeg-turbo was selected for its JPEG-specific controls
and optimization options. Its public upstream API stays private to our codec;
applications use os64-owned names and normalized pixels.

## Proof before integration

Each codec earns its place with a host-side pure-buffer harness under ASan and
UBSan before QEMU is involved. `tools/test_png_host.sh` supplies the PNG
harness, including:

- independently produced images for every claimed color type and bit depth;
- all five scanline filters, including first-row and one-byte-per-pixel edges;
- IDAT split at hostile byte positions and across multiple chunks;
- stored, fixed-Huffman, and dynamic-Huffman DEFLATE supplied by the existing
  inflater tests;
- palette and transparency cases with exact expected ARGB pixels;
- bad signature, chunk order, length, CRC, zlib header, Adler-32, filter byte,
  and truncated stream refusals;
- dimensions and expansion claims that prove refusal occurs before an
  oversized allocation;
- exact pixel cross-checks against an independent host decoder.

QEMU then proves the integration facts the host cannot: shared-library loading,
the real heap, filesystem reads, drawing, and freeing through the public ABI.
The visual check is the last check, not the first; a symmetrical picture can
look correct while rows, channels, or alpha are wrong.

## The durable rule

**libos64 provides the world an image library needs; it does not become the
image library. libimage chooses a codec; it does not absorb one. Each serious
codec owns its format all the way from magic bytes to normalized pixels.**
