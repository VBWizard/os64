# Canvas C4 validation

C4 adds HTML canvas/image sources, transformed and cropped drawImage, smoothing,
origin-clean enforcement, and PNG data-URL export. The gallery is
`tools/pages/canvas-images.html`; enable Scripts and Modern Web. Chris accepted
all four samples on the P5 at every tested zoom level (25%–400%), including
rapid clicks on the magnifier. The self-copy sample intentionally discards
pixels that move beyond its right edge; it does not wrap. This is the supported
C4 profile, not complete Canvas conformance.

## Automated checks

- Native Canvas: 413,144 assertions, zero failures under ASan/UBSan/LSan.
  Image cases cover exact nearest-neighbor colors, proportional crop clipping,
  negative extents, rotated destinations, bilinear alpha, overlapping self-copy,
  clip/COPY behavior, taint/reset (including singular transforms), saved smoothing state and allocation refusal
  preserving pixels, damage and origin cleanliness.
- Full DOM: 20,451 target-engine checks and 132,618 sanitized-engine checks,
  zero failures. Binding allocation sweeps cover 2,174 and 20,380 points.
  The full regression run preceded the final singular-transform taint guard;
  final native and focused Canvas binding runs cover that guard.
  Focused Canvas runs pass 118 checks in each engine profile. Image binding
  cases include reentrant conversions, invalid sources, loan release, blank
  canvas export, MIME fallback, PNG header decoding, and taint propagation.
- Yonder: 11,736 checks, zero failures, including existing SVG worker refusal
  and cancellation sweeps. Fetch jobs: 382 checks, zero failures, including
  native opaque image bodies, CORS acceptance/refusal, cross-origin redirects,
  and preservation of script fetch's opaque-body discard behavior.
- The gallery HTML and all four handlers execute through the real Yonder host.
  PNG export/decode/draw preserves every premultiplied pixel; image tasks settle,
  native allocations are released, and all eight captured bitmaps obey the
  premultiplied channel/alpha invariant.
- PNG writer tests cover exact admission, insufficient output space, output
  guards, alpha unassociation, and a 257×257 encode/decode round trip across
  stored-DEFLATE block boundaries. Independent Pillow decoding matches all
  66,049 RGBA pixels and verifies sRGB/96-dpi metadata. Existing PNG legal
  formats, filters, chunk splits, checksums, zlib and refusal tests pass.
- SVG painter: 1,835,108 assertions, zero failures after shared painter changes.
- Yonder and its library dependencies cross-build with warnings as errors.
- Sanitizer runs keep leak detection enabled outside the restricted sandbox.

Reproduction: `tools/test_svg_core.py --canvas --output <dir>`,
`tools/test_dom_host.sh`, `tools/test_yonder_scripts_host.sh`,
`tools/test_yonder_fetch_job_host.sh`, `tools/test_png_encode_host.sh <dir>`,
`tools/test_png_host.sh`, `tools/test_svg_core.py --paint --output <dir>`, and
`tools/test_canvas_gallery.sh <dir> tools/pages/canvas-images.html`.

The upstream image run exposed catalog allocations made before the shown page
owned the picture lifecycle. Source acquisition now waits for that ownership;
the final run is leak-free. Visual inspection exposed a straight/premultiplied
mismatch at the SVG provider boundary. The provider now labels the SVG output
correctly, its header states the output format, and gallery channel assertions
cover translucent edges. These were C4 defects corrected before delivery.

## Upstream census

WPT pin: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`. All prior recorded Canvas
passes remain. Image drawing adds 12 passing cases; compositing gains seven,
reset two, and fill/stroke styles one. Three required canvas-source/self-copy
cases fail with Modern Web off. The runner's selftests pass after adding a
separate concurrent picture-job queue and real PNG/SVG worker decoding.

| Family | Pass | Fail | Timeout |
| --- | ---: | ---: | ---: |
| drawing-images-to-the-canvas | 12 | 23 | 2 |
| canvas-host | 57 | 9 | 0 |
| canvas-context | 11 | 0 | 0 |
| compositing | 58 | 40 | 0 |
| drawing-rectangles-to-the-canvas | 30 | 2 | 0 |
| fill-and-stroke-styles | 136 | 120 | 1 |
| line-styles | 31 | 2 | 0 |
| path-objects | 117 | 86 | 0 |
| pixel-manipulation | 65 | 6 | 0 |
| reset | 13 | 16 | 0 |
| the-canvas-state | 53 | 15 | 0 |
| transformations | 21 | 1 | 0 |
| **Total** | **604** | **320** | **3** |

Most image-family failures require createImageBitmap; OffscreenCanvas is also
absent. The host image fake decodes PNG and SVG, not GIF/JPEG/WebP. HTML image
loading is incomplete: document-load timing does not wait for these source
requests, unwrapped markup handlers are not polled, and zero-size image cases
can time out. These limitations are recorded in DEBTS.md rather than claimed
as passing. The broader families retain missing C5/C6 features and the C3
wide-stroke/extreme-coordinate limitations.

## Independent pixels and guest checks

Chromium 140.0.7339.16 runs the same gallery without script errors. Initial
400×180 bitmaps compared in premultiplied 8-bit channels give:

| Card | Mean absolute channel difference | Maximum |
| --- | ---: | ---: |
| SVG stamps | 0.168681 | 79 |
| Crop/filter comparison | 0.674240 | 20 |
| Overlapping copy | 0 | 0 |
| PNG round trip | 0.018826 | 20 |

`c4-gallery-comparison.json` retains the measurements. Sampling and edge
rasterization differ; these are measurements, not exact browser-equivalence
claims. Literal pixel tests and independent PNG decoding provide other oracles.

The frozen cohort contains Yonder, 21 matching shared libraries and the gallery:
23 files, with 16 unchanged binaries reused from C3 by allocated ELF-section
comparison. Guest-installed files are read back and checked byte for byte.
The SHA-256 manifest is `c4-bundle.sha256`. QEMU exercises all four controls,
confirms PNG 2 after the asynchronous decode, and checks 25% and 400% zoom.
The gallery keeps card widths fixed so high zoom scrolls rather than squeezing
its image aspect ratios. It does not add a scheduled animation loop.
