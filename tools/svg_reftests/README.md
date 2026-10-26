# SVG picture comparisons

These are selected-fixture runners for SVG geometry and prepared-scene painting. It compares full opaque RGB
images and records evidence. It does not measure Yonder's SVG support or replace
WPT's server, discovery, dependency graph and scheduling machinery.

## Run

Pillow is needed by the comparator. Browser capture also needs Playwright and
its Chromium build. The recorded run uses Pillow 10.2.0 and Playwright 1.55.0 /
Chromium 140.0.7339.16. Install into an isolated environment, for example:

```sh
python3 -m venv /tmp/svg-reference-env
/tmp/svg-reference-env/bin/pip install Pillow==10.2.0 playwright==1.55.0
PLAYWRIGHT_BROWSERS_PATH=/tmp/svg-reference-env/browsers /tmp/svg-reference-env/bin/python -m playwright install chromium
/tmp/svg-reference-env/bin/python -m unittest discover -s tools/svg_reftests -p 'test_*.py'
PLAYWRIGHT_BROWSERS_PATH=/tmp/svg-reference-env/browsers /tmp/svg-reference-env/bin/python tools/svg_reftests/run.py --output /tmp/svg-browser
```

Use a new or empty output directory. The runner saves PNGs, difference images,
`results.json` and a gallery. It records browser/version, viewport, device
scale, locale, timezone, platform, loaded font faces, source digests and image
digests. The fixtures contain no rendered text; text fixtures are refused
until a pinned font configuration is supplied. Capture uses a fresh browser
context per URL and a localhost HTTP server. External requests and missing
resources are errors, except the browser's optional missing favicon.

Capture waits for load, fonts, image decoding and two animation frames. If the
root has `reftest-wait`, it dispatches `TestRendered`, waits for removal of the
class, then rechecks images/fonts and waits for two more frames. SVG `<image>`
resources, including legacy `xlink:href`, are checked using their resolved URLs
through an image decoder; actual SVG image errors are also tracked from before
navigation. An HTTP-200 corrupt resource is an ERROR, even if both pictures
would otherwise look identically blank. A retained wait class times out. This
is the static profile's paint synchronization; arbitrary animations and WPT
server-generated responses are outside its coverage.

The comparator has 18 self-tests. Seven real-browser resource regressions run
separately with the same Playwright environment:

```sh
PLAYWRIGHT_BROWSERS_PATH=/tmp/svg-reference-env/browsers /tmp/svg-reference-env/bin/python tools/svg_reftests/browser_checks.py -v
```

They cover corrupt bitmap/SVG resources, a broken reference, href/xlink:href,
post-TestRendered replacement, standalone SVG, data-URL SVG and the existing
HTML-image decode check. These are capture-contract checks; they do not assert
recursive resource correctness inside an independently loaded SVG document.

The local suite has ten expected matching/mismatching cases and eight
negative controls. Missing stroke, incorrect transform order, wrong winding
and per-child opacity must fail their reference comparisons on both light and
dark backgrounds. ERROR or UNSUPPORTED never counts as an expected negative
control rejection.

## Comparison rules

The RGB maximum channel difference and count of changed pixels follow
[WPT's reftest definitions](https://web-platform-tests.org/writing-tests/reftests.html).
A fuzzy value such as `15;300` specifies exact values; `0-15;0-300` specifies
inclusive upper allowances. Named parameters, positional parameters, and
per-reference overrides are supported. Exact equality follows the pinned
runner's special case when either fuzzy lower bound is zero. Match references
are alternatives; mismatch references must all differ. Missing references,
size mismatches and uncomposited alpha are errors. A one-unit blue-channel
change counts as a changed pixel; RGB differences are not rounded through
luminance. There is no edge exemption.

The runner reads selected HTML or namespace-correct XML metadata directly.
It refuses base elements, nonlocal references, XML DTDs and chained/cyclic
reference graphs. Cases are explicitly selected in a suite JSON; this is not
an automatic WPT manifest enumerator. A reference's own links are refused
instead of being silently ignored. Malformed metadata is an error. Unsupported
cases remain in the output and make the invocation unsuccessful.

## Native backend comparisons

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_geometry.py --backend --output /tmp/svg-native
/tmp/svg-reference-env/bin/python tools/svg_reftests/native.py --native /tmp/svg-native --browser /tmp/svg-browser --output /tmp/svg-comparison
```

The C harness calls the public geometry table and saves cap, non-scaling,
evenodd and nonzero coverage masks. It also records compiler/source/binary
provenance. These commands hand-construct equivalent path commands; they do
not parse or render an SVG document. `native.py` composites the masks over
specified backgrounds and compares them with the independently captured SVGs.
It verifies capture hashes and retains the native provenance. Its three
per-shape limits include a maximum channel delta, changed-pixel count and an
additional sum-of-maximum-channel-differences bound. Those cross-engine limits
are separate from WPT metadata. Native missing-stroke, wrong-transform and
wrong-fill-rule controls must be rejected too.

## Selected WPT sample

```sh
python3 tools/svg_reftests/fetch_wpt.py
PLAYWRIGHT_BROWSERS_PATH=/tmp/svg-reference-env/browsers /tmp/svg-reference-env/bin/python tools/svg_reftests/run.py --root .vendor/svg-reftests --suite tools/svg_reftests/wpt-sample.json --output /tmp/svg-wpt
```

The fetcher retains the shared pin in `tools/wpt_fetch.sh`; it downloads six
explicit files into `.vendor/svg-reftests`, verifies their SHA-256 digests and
prints the upstream license link. The runner checks those source digests again.
No upstream fixture or fuzzy metadata is rewritten. The sample is three tests,
not the full painting directory. Chromium 140 passes nested-viewBox and fails
the two non-scaling-stroke-through-image tests. The command therefore exits 1;
these remain ordinary FAIL results, not relabeled successes.

[Measured results and remaining work](../../docs/design/pending/SVG_REFTESTS.md).


## Native solid painting

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-paint
PLAYWRIGHT_BROWSERS_PATH=/tmp/svg-reference-env/browsers /tmp/svg-reference-env/bin/python tools/svg_reftests/paint.py --native /tmp/svg-paint --output /tmp/svg-paint-browser
```

The native harness parses paths, prepares provider-backed scenes and renders
through production libsvg/FreeType. It writes twelve scenes on two backgrounds,
equivalent browser SVG inputs and three independently sampled analytic RGBA
oracles. `paint.py` requires a successful native log and build provenance,
captures Chromium and saves a self-contained gallery plus source/image hashes
and comparison metrics. Fourteen deliberately wrong browser variants must fail.
The per-fixture maximum/count/sum limits and their analytic evidence are in
[SVG_PAINT.md](../../docs/design/pending/SVG_PAINT.md). These compare the native
prepared-scene API, not Yonder's document/style adapter.
