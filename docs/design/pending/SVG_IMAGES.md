# SVG image route

2026-10-27. The [standalone document adapter](SVG_DOCUMENTS.md) now reaches
Yonder's ordinary picture jobs. With Modern Web enabled, HTTP(S) and file SVG
resources can appear in `<img>` elements and CSS/attribute backgrounds. The
P5 fixture is `/tests/pages/svg-images.html`, **SVG: special delivery**, with
four companion SVG files. `make stage-pages` includes `.svg` fixtures.

## Loading and lifetime

A picture product is tagged raster, sequence or SVG. The existing file reader
or libway fetch/cache supplies bytes on the worker. SVG MIME selects XML
validation; XML-shaped byte candidates (including UTF-8/UTF-16 BOMs) also
enter the bounded parser. An SVG-looking substring is insufficient: the
complete XML document must parse and its root must be SVG in the correct
namespace. HTTP error/empty bodies do not become images. SVG parsing retains
its 4 MiB encoded/decoded/expanded limits even though the shared fetch door
allows larger raster files. Modern Web off retains raster-only dispatch.

Raster decoding and GIF sequence ownership retain their existing paths.
The SVG worker returns an independently owned document, measured default
object size and retained-storage cost. It does not hand libimage a custom
pixel allocation. The window accepts a completion only for its current page
generation and waiting picture entry. Acceptance moves the document pointer
out of the product and charges it to the 256 MiB retained-picture account.
Cancelled/departed/unaccepted products are released by the worker's usual
release function. Page teardown frees accepted documents.

Pictures remain indexed by address. Several elements using one URL share
one fetched document. A source change can load a second URL; changing back
reuses the retained first entry. Neither sharing nor source replacement lets
the host page's CSS or script reach into an image's XML tree. Picture scripts,
animation and resource references retain the secure-static policy documented
in [SVG_DOCUMENTS.md](SVG_DOCUMENTS.md#static-image-policy-and-root-coordinates).

## Size and pixels

Natural sizing uses absolute root CSS width/height (presentation attributes
participate in that cascade), with viewBox ratio and a 300 by 150 default
object rectangle when dimensions are absent. Absolute units include px, in,
cm, mm, Q, pt and pc. Percentage/auto and unsupported relative/calc dimensions
do not supply intrinsic lengths. The current libflow callback takes a pair
of dimensions, so this adapter supplies a default object size rather than
extending the layout ABI with optional dimensions/ratio fields.

Measurement uses a 300 by 150 CSS media viewport and memoizes the result for
the current dark/reduced-motion preference pair. Container-dependent intrinsic
sizing, font-relative/calc dimensions and a general ratio-only layout contract
remain explicit follow-ups. Zero intrinsic dimensions are preserved.

The window computes the object-fit/object-position rectangle or background
tile using its existing picture placement. It then renders SVG for that
rectangle's dimensions. Fractional CSS viewport dimensions are separate from
integer output pixel dimensions: zoom changes the output map, not the CSS
media-query width. Thus the same image remains vector-rendered at 25% and
400%, and each differently sized use receives its own viewport. The image's
own viewBox/preserveAspectRatio operates inside that viewport.

Rasterization is synchronous on the window, serialized with document size
queries and teardown. The output uses the existing 64 MiB geometry/pixel
ledger and is released after compositing/tiling. Unsupported or over-budget
painting produces a diagnostic and an image frame (or no background tile).
There is no raster-variant cache or asynchronous SVG paint worker in this
increment; large/many images can therefore affect paint latency. Retained
XML/styles are reused, but cascades/scenes/pixels are rebuilt for painting.

Root CSS sizing follows [SVG2 intrinsic sizing](https://www.w3.org/TR/SVG2/coords.html#IntrinsicSizing).
Pinned Chromium 140 reports the attribute dimensions in the two fixtures
where root CSS overrides them; those differences are recorded separately
from agreement. The [CSS Images default sizing algorithm](https://www.w3.org/TR/css-images-3/#default-sizing)
provides the default-object-size basis.

## Working allowances

The picture pool admits 512 MiB per job, covering its raster codec path or
SVG parse/measurement path; the pool's total remains 2 GiB. This conservative
reservation can admit fewer simultaneous pictures than the raster-only
reservation. The window has a separate serialized 512 MiB SVG working
allowance, in addition to the retained-picture account and other page owners.
These are admission/accounting allowances, not a process-wide allocator cap.

`garb_cascade_limited` supplies caller-selected state/read/attribute arena
caps and a retained attribute-parse count. `garb_parse_block_limited` and
`garb_parse_values_limited` retain their literal cap for subsequent reads.
The original CSS entry points retain their default limits and behavior.

SVG measurement and rendering select 32 MiB cascade state, 64 MiB sheet
reads, at most 128 retained inline-style parses of 2 MiB each, and 2 MiB for
a temporary presentation-value parse. This closes the uncounted multiplicity
of independently retained style-attribute arenas; counting only cascade
state/read arenas was not a complete working estimate. XML/tree/sheet storage
retains its 32 MiB aggregate post-parse cap. A sheet parse may temporarily use
the existing 96 MiB parser arena before aggregate rejection. Slot tables,
match/keyframe arrays and parser/tokenizer scratch are separate from arena
caps and are covered by the conservative working allowance; they are not
reported as geometry-ledger allocations.

XML parsing and scene/render work poll cancellation. Bounded CSS phases
remain non-preemptible inside a call; their surrounding phases check worker
cancellation. The window repaint is synchronous and has no outstanding worker
reference to a document it may free.

## Evidence and checkpoint

- Native standalone fixtures: 20 exact pixel pairs; existing inline geometry,
  scene, paint, mutation and allocation-failure regressions pass. Eleven size
  fixtures cover absolute units, missing dimensions, viewBox ratios, root CSS
  and zero width. Separate CSS/raster extents preserve media behavior at
  quarter, normal and quadruple output size. Attribute-count and parse limits
  refuse cleanly.
- Production picture worker: 64 allocation positions and 600 cancellation
  positions, file/HTTP transport fixtures, MIME/byte recognition, malformed
  and wrong-namespace XML, HTTP errors, feature-off and raster dispatch.
- The Yonder scripted-page suite passes 10,937 checks. Window integration covers shared-address ownership, natural/explicit sizes,
  private styles, 400% rerasterization, contain placement, SVG backgrounds,
  exhausted geometry budget, stale completions and teardown.
- Full CSS parser/cascade regression suite passes, including its corpus,
  failure sweeps, repeated-cascade isolation and read-budget checks. Host
  suites run with ASan/UBSan and LeakSanitizer enabled outside the sandbox.
- Pinned Chromium: 60 expected standalone pixel outcomes and 11 expected
  sizing outcomes (nine matches, two explicitly pinned root-CSS differences).
  The static-versus-SMIL difference remains explicit in the pixel report.

Visible QEMU checks cover file loading, the source toggle in both directions,
contain/cover placement, style isolation, the static shield and background tiles,
and 25%/400% zoom. A second boot of the final delivery cohort loads the page
and three SVG resources over HTTP through virtio-net/libway. Both boots shut
down normally and both ext2 volumes pass read-only filesystem checks.

Evidence: [HTTP gallery](../../svg-evidence/yonder-images-http-top.png),
[source changed](../../svg-evidence/yonder-images-changed.png),
[contain/cover and isolation](../../svg-evidence/yonder-images-middle.png),
[shield and tiles](../../svg-evidence/yonder-images-bottom.png),
[25%](../../svg-evidence/yonder-images-25-percent.png),
[400%](../../svg-evidence/yonder-images-400-percent.png).
Browser reports retain source hashes and native/compiler provenance:
[document pixels](../../svg-evidence/images-document-results.json),
[image dimensions](../../svg-evidence/images-sizing-results.json).
The [delivery manifest](../../svg-evidence/images-delivery.sha256) identifies
33 tested binaries, matching libraries and fixtures; it includes the earlier
galleries for convenient regression checks.

Reproduce the host and browser checks (Chromium 140 / Playwright 1.55.0):

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-images-native
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
python3 tools/svg_reftests/documents.py --native /tmp/svg-images-native --output /tmp/svg-images-doc-browser
python3 tools/svg_reftests/images.py --native /tmp/svg-images-native --output /tmp/svg-images-size-browser
```

The gallery has six cards: a reversible source toggle, repeated uses at two
sizes, contain/cover crops, host-style isolation, a static green shield, and
SVG background tiling. The shield deliberately requests script, SMIL and an
external stylesheet; in Yonder it remains green. Chromium's animated image
mode can turn it red, so that card is a processing-mode check rather than a
cross-browser color reference.

Chris verified the `a013caf7` delivery on the P5: the complete gallery passes
from 25% through 400% zoom. The frozen binary/library/fixture cohort contains
33 files; the update transferred 26 changed files.

[Linear](SVG_GRADIENTS.md) and [radial gradients](SVG_RADIAL.md) extend the image
profile. Raster-variant caching and corpus-led latency/fidelity work remain
in 6c; broader graphics remain in 6d. SVGZ,
data-URL picture dispatch, fragment-view selection and full intrinsic sizing
are not delivered by this route.
