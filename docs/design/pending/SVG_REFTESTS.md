# SVG comparison harness and browser evidence

Implemented as the next slice 6a increment above the production geometry
backend. Worktree/branch: `svg-reftests` / `codex/svg-reftests`, initially
stacked on #359 at `06d28997`. The comparison prerequisite now has executable
self-tests, browser captures, native/backend comparisons and explicit negative
controls. This does not complete SVG rendering or claim WPT conformance for
Yonder. [Commands and supported profile](../../../tools/svg_reftests/README.md).

[Native/browser comparison gallery](../../svg-evidence/reference-comparison.html).
Each row shows the native result, independently captured browser SVG and raw
RGB difference. Deliberately broken controls appear alongside ordinary cases.

## What is measured

The capture environment is Chromium 140.0.7339.16 from Playwright 1.55.0,
Pillow 10.2.0, device scale 1, en-US locale, UTC timezone, light browser color
scheme, and 192×128 viewport for local/native fixtures. Both light and dark
painted backgrounds are tested. The selected WPT sample uses 800×600. No
rendered text occurs in these fixtures; no font-layout result is claimed.
Capture records the browser/platform/settings, loaded font faces, source
SHA-256 digests and screenshot digests. Native artifacts additionally record
compiler flags, input-source/header digests and executable digest.

The public geometry harness emits coverage for an ordinary round stroke under
scale(4,1), a device-space non-scaling stroke, and nested filled contours.
The Python tool composites those masks with the fixture's foreground and
background colors. SVG parsing, viewport selection and scene construction
are still hand-specified in these C fixtures; their success is geometry
comparison evidence, not an SVG document render through libsvg or Yonder.

The local browser suite compares stroke shapes with CSS rounded rectangles,
evenodd paths with rectangular references, group opacity with a single shape,
and exercises mismatch and TestRendered/reftest-wait behavior. Ten ordinary
cases pass. Eight deliberate defects fail: missing strokes, wrong transform
order, wrong winding and per-child opacity, each on both backgrounds.

The comparator's 18 self-tests include inclusive fuzzy bounds, literal single
values, named and positional arguments, per-reference overrides, mismatch
inversion, multiple references, XML namespaces, one-unit blue differences,
size/alpha/file errors, and errors that must not count as negative-control
success. It follows the documented flat-reference rule: any match and all
mismatches. Chained reference graphs are explicitly unsupported, rather than
approximated by comparing only their first edge. Its exact-equality special
case for a zero fuzzy lower bound follows the project's pinned WPT runner.

## Cross-engine limits and observations

| Native fixture | Maximum RGB channel delta, light / dark | Changed pixels | Allowed maximum / pixels / total pixel equivalents |
|---|---:|---:|---|
| Stretched round cap | 38 / 33 | 150 | 40 / 200 / 12 |
| Non-scaling round cap | 38 / 32 | 59 | 40 / 100 / 4 |
| Evenodd nested rectangles | 0 / 0 | 0 | exact |

The total is the sum of the largest RGB channel delta at each pixel divided
by 255. It is an additional native-comparison bound; WPT comparisons retain
their two specified metrics. No edge pixels are excluded. Six native/browser
comparisons pass these limits. Six native negative controls fail even with
the same allowances: missing stroke, wrong stroke-transform order and wrong
fill rule on both backgrounds. Their maximum channel differences are 210 on
light and 184 on dark; changed-pixel counts are 2398, 698 and 1600 respectively.

The initial cross-engine trial reused the analytic oracle's 24/255 maximum
and failed four cap comparisons (observed 32–38). That did not establish a
geometry regression: after identical color compositing, the independent
64×64 capsule reference differed from FreeType by at most 11 steps and
2.654902 pixel equivalents, while Chromium differed by at most 38 steps and
8.235294 pixel equivalents. Native-versus-Chromium stretched-cap error was
9.764706 pixel equivalents on light. The retained per-fixture limits therefore
accommodate the observed browser coverage approximation while remaining far
below the deliberately wrong geometry. They are empirical fixture envelopes,
not an accuracy guarantee for arbitrary curves. The tighter independent
analytic checks in the C harness remain unchanged.

A separate initial browser-only group-opacity comparison differed by one
step across the 4000-pixel rectangle: offscreen group composition versus
single-shape composition rounded differently. That fixture and its negative
control use an explicit `0-1;0-4000` allowance. Wrong per-child opacity still
fails by 53 steps on light and 46 on dark over the 800-pixel overlap. This
allowance is recorded in those fixtures rather than globally relaxing images.

## Selected WPT baseline

The sample retains shared pin `a699fb16767be39ba3e61a8e8b6bef60383f08ee` and
six upstream source digests in `tools/svg_reftests/wpt-sample.json`. The fetcher
places those unmodified files in ignored `.vendor/svg-reftests`. It refuses a
pin mismatch with `tools/wpt_fetch.sh`; the runner rechecks source digests.
Only these explicitly selected pages are run, not the entire SVG directory.

| Chromium test in `svg/painting/reftests` | Result | Maximum channel delta | Different pixels |
|---|---|---:|---:|
| `non-scaling-stroke-non-uniform-scale.html` | FAIL | 98 | 792 |
| `non-scaling-stroke-with-none-preserve-aspect-ratio.html` | FAIL | 128 | 9900 |
| `small-nested-viewbox.html` | PASS | 0 | 0 |

The WPT invocation exits nonzero and preserves both failures. No upstream
metadata is widened, no failure is excluded, and neither result is attributed
to Os64. These are browser baseline disagreements in non-scaling strokes
inside stretched SVG images. They also demonstrate why a browser screenshot
cannot substitute for an independent geometry oracle.

## Verification and remaining boundaries

Seven real-browser resource regressions cover decode readiness, including two
matching pages with the same corrupt HTTP-200 image. That case reported PASS
before the SVG image fix and now reports ERROR. SVG image error events are
tracked before navigation; direct href/xlink:href resources are also decoded,
and readiness is rechecked after TestRendered mutations. Valid bitmap,
standalone SVG and data-URL SVG fixtures remain supported. This checks direct
resource decoding, not recursive correctness of a referenced SVG's contents.

The modified public-backend harness passes ASan, UBSan and LeakSanitizer,
including its 32 allocation and 1045 cancellation cuts. This increment changes
host tools and fixture output; production geometry/font code is unchanged.
No new P5, QEMU, font regression or performance run is claimed.

Capture supports selected local static pages. Full WPT manifest enumeration,
reference chains, wptserve dynamic handlers, a pinned text/font environment,
animation timestamps, cross-browser coverage and Yonder raster capture remain
separate work. A missing image, HTTP/decode error, timeout, invalid metadata
or unsupported construct cannot produce a passing comparison. Output records
keep ordinary FAIL, ERROR, UNSUPPORTED and intentional negative-control
rejection distinct. A larger renderer corpus must establish its own feature
expectations and tolerances rather than inherit these capsule allowances.

Source semantics: [WPT reftests](https://web-platform-tests.org/writing-tests/reftests.html),
[pinned fuzzy parsing](https://github.com/web-platform-tests/wpt/blob/a699fb16767be39ba3e61a8e8b6bef60383f08ee/tools/manifest/sourcefile.py),
[pinned comparison implementation](https://github.com/web-platform-tests/wpt/blob/a699fb16767be39ba3e61a8e8b6bef60383f08ee/tools/wptrunner/wptrunner/executors/base.py).
