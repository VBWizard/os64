# Image memory I2: responsive selection

2026-11-08. Implementation evidence for I2 in
[IMAGE_MEMORY.md](IMAGE_MEMORY.md). The P5 gallery zoom checks, original
metadata/Canvas regressions and Microsoft live-page check are accepted; the
live-site comparison is recorded below. The separate replacement-button
currentSrc assertion remains unconfirmed. I3's metadata ownership change is
not part of this slice.

## Behavior

Modern Web image elements share one selected URL, density, request mode and
generation between display and native acquisition. `img.srcset`, `img.sizes`,
preceding `picture > source` elements, source media/type conditions and
`currentSrc` participate. Detached `new Image()` and source-only markup use
the same selector. Classic mode retains its `src` behavior.

The pure parser lives in libpage. It handles density/width descriptors,
default `src` participation, commas inside URLs, duplicate densities, invalid
descriptors and width/height descriptor pairs. It chooses the lowest density
at least the effective device pixel ratio, otherwise the largest candidate;
equal densities keep source order. A valid overflowing descriptor saturates
at the largest finite binary64 value. Source-size zero has the same bounded
infinite-density representation. Invalid candidates are skipped; a request
failure does not trigger downloads of the remaining candidates.

Yonder resolves the selection against the document base and uses `css_view`
for CSS viewport dimensions and effective DPR. Page zoom is applied once.
The existing CSS parser and media evaluator supply `sizes` conditions and
lengths. The supported length units are px, em, rem, ex, ch, vw, vh, vmin,
vmax, pt, pc, cm, mm, in and q; font-relative units use the initial 16px font.
Typed `calc`, `min`, `max` and `clamp` lengths work. Percentages, custom-property
substitution, container/dynamic viewport units and unsupported math expressions
fall through to the next list entry. Native lazy loading and automatic sizing
are absent: ineligible `auto` is skipped, so `auto, 160px` selects 160px.
An exhausted list uses the CSS viewport width.

Eligible picture MIME types reflect the implemented PNG, JPEG, GIF, BMP, PPM,
WebP and secure-static SVG decoders. AVIF is skipped. Media/type mismatch moves
to the next preceding source, then the img selection. This does not add codecs,
CSS image-set, responsive preload or source-element width/height sizing hints.

DOM/source/base/environment changes reevaluate the selection. Metadata getters
can discover a changed selection without starting intermediate requests;
submission occurs at the existing task/layout boundaries. A replacement keeps
the completed image available while pending. An obsolete job may finish in
the bounded catalog but cannot replace the newer element selection. Hidden
geometry reads can use an already-completed selected catalog entry without
submitting work or mutating the displayed current/pending pair.
Synchronous geometry, hit, text-rectangle and computed-layout reads consume
an owed image/font relayout before answering, including inside onload.

Source raster dimensions, natural dimensions and reduced display dimensions
remain distinct. Density corrects layout, naturalWidth/naturalHeight and the
intrinsic fallback for width/height. Canvas default destination dimensions and
explicit source crops use natural CSS coordinates, converted to source pixels
at draw time. A loaded image whose density-corrected size truncates to zero
keeps a zero-sized box rather than an unknown-image placeholder.

Explicit anonymous/credentialed display requests now use the same header-aware
fetch/CORS policy as native images, while retaining display reduction. Ordinary
display requests still use the existing display fetch path. Request mode is
part of the catalog key; display bytes cannot establish native readback access.
I1's cache behavior remains, including misses for headerless legacy entries.

## Bounds and remaining ownership

The selector admits 64 KiB of srcset text, 128 candidate attempts and 127 bytes
of numeric descriptor text plus its suffix. `sizes` and source `media` text
have 64 KiB limits; a picture scan admits 4096 preceding siblings. Limits refuse
the selection explicitly in diagnostics. CSS syntax outside the supported
length/media profile follows fallback behavior instead.

The page holds at most 4096 selected-element records and 4 MiB of their URL
strings, separately from the existing 4096-entry/4 MiB picture catalog. The
page census reports selected-element state as `image sources`. Element records
hold their DOM nodes until teardown; obsolete catalog entries and source states
are not reclaimed during a long visit. Source-state/catalog allocation refusal
is explicit. This does not raise the retained-picture budget.

Native metadata access still retains full rasters. Wrapped image load/error
delivery and synchronous drawing in the load callback remain supported;
unwrapped markup handlers, document-load dependencies and Image.decode remain
open. Rendered img.width/height from resolved CSS content boxes remains the
separate layout debt. This slice makes no claim that those contracts are complete.

## Validation

The host suite exercises the production selector, picture worker, PNG decoder,
DOM bindings and Canvas drawing. Cases include source/type/media fallback,
srcset-only and detached images, base changes, effective zoom, coalesced
mutations, stale completions, failure without alternative downloads, no-op
selection changes, explicit CORS display enforcement and density-corrected
Canvas readback. Existing hidden-layout reuse and diagnostic-settlement
regressions remain covered.

`tools/pages/image-responsive.html` and its deterministic 160×90 / 320×180
PNGs run through their delivered script in the host suite. Tests select at
100% and 200%, replace a detached image, check natural dimensions and execute
the immediate crop in onload. The host transports the same PNG bytes through
data URLs because its ordinary display HTTP fixture is text-only.

The WPT pinned at `a699fb16767be39ba3e61a8e8b6bef60383f08ee`,
`html/semantics/embedded-content/the-img-element/current-pixel-density/basic.html`,
also has separate original
and adapted runs. The original reads metadata in window.onload and uses the
runner's stubbed display HTTP path, so it is not a passing conformance gate.
The density-only adaptation supplies the pinned PNG through a data URL and
runs the original assertions after the wrapped images' load/error callbacks.
This separates density arithmetic from the deferred metadata/document-load
contract. Adapted results must not be reported as an unmodified WPT pass.

- Full Yonder host: 24,044 checks, zero failures.
- libpage host: 254,435 checks, zero failures, including the allocation sweep.
- Density-only WPT adaptation: 14 passes, zero failures. Its PNG bytes and
  assertions are from the pin; transport and callback timing are adapted.
- Unmodified WPT: 0 passes, 14 failures. Adapting only PNG transport still
  gives 0/14; the metadata/document-load contract remains open.
- Target Yonder and matching dependencies build successfully.
- ASan, UBSan and LeakSanitizer enabled for the host and WPT runs.

The original M0 PNG ownership readings remain: 57,600 B of display pixels,
no native pixels for untouched images, and 8,294,400 B of native pixels for
metadata and Canvas pages. Responsive selection does not release those owners.

## Hardware comparison

Install the frozen matching runtime cohort, restart Yonder, and enable Modern
Web and scripts. Open `/tests/pages/image-responsive.html` at 100%, then 200%.
Each result should say PASS. Width selection changes from blue/orange to
green/yellow while natural dimensions stay 160×90. Use the replacement button;
the detached result should pass again. Save a diagnostic record at each zoom.
Both PNGs must be beside the page in `/tests/pages`.

Repeat the original metadata and Canvas pages to preserve the ownership
baseline. Then visit Microsoft wallpapers at the prior 100% window geometry,
save a settled diagnostic, leave and revisit, and save another. Compare
selected source sizes, retained display/native bytes, transfer counters and
budget refusals to I1. Live page contents and cache state remain confounders;
do not attribute their variation solely to responsive selection. I3 remains
the next separate ownership change after this gate.

### Gallery hardware result

The user confirmed all four PASS results at both 100% and 200%, including the
intended blue/orange to green/yellow width-candidate switch. An initial fixture
installation error placed the 160×90 PNG under the 2x filename; the correct
320×180 file was restored and verified against the frozen bundle before these
accepted checks. No implementation change was needed for that incident.

Record 1859 is a clean, manually requested gallery record with two numbered
paint readings. Four catalog entries are shown, with no fetch failures or
retained-budget, submission, decode-allocation or decode-limit refusals. Its
latest memory snapshot has 288,000 B display and 288,000 B native pixels,
576,000 B retained peak, and 57,600 B of native pixels used only for metadata.
Two pixel acquisitions match the fixture's default draw and immediate crop.
The earlier corrected run, record 1849, has the same retained totals and zero
refusals. Prior readback of Yonder and all three changed libraries matches the
frozen `fa56bf22` cohort byte-for-byte; saved gallery HTML also matches it.

Both `d` presses append paint readings to one page record. Memory facts are
overwritten by the latest capture, and the zoom header retains the load-time
setting. Thus these are user-confirmed zoom checks with two paint readings,
not two independently preserved per-zoom memory/source snapshots. The saved
HTML is original markup and does not preserve the live PASS text.

The replacement button was reported to leave all results at PASS, but its
specific currentSrc transition was not independently confirmed. It changes a
detached Image, so no displayed rectangle changes.

### Original regressions and Microsoft hardware result

The user confirmed PASS on the original metadata and Canvas pages and that
Microsoft wallpapers still works. Records 1878/1879 are clean metadata visits:
57,600 B display, 8,294,400 B native pixels, all native bytes attributed to
metadata only, and no refusals. Record 1880 is the clean Canvas visit with
the same retained totals, one pixel acquisition and no metadata-only native
bytes. These preserve the M0 ownership baseline and immediate readback check.

The Microsoft comparison uses 2522×1257 content pixels, 100% zoom, Modern Web
and scripts enabled, seven ready sheets and no pictures still coming. Readback
of Yonder and the three changed libraries matches the frozen `fa56bf22` cohort.

| Metric | I1 return visit, 1753 | I2 first visit, 1882 | I2 return visit, 1884 |
|---|---:|---:|---:|
| Picture entries / shown | 76 / 52 | 124 / 122 | 76 / 74 |
| Retained-budget refusals | 24 | 2 | 2 |
| Network replies / fresh-cache replies | 22 / 76 | 94 / 32 | 14 / 64 |
| Network entity bytes | 148,747,732 | 48,399,832 | 27,287,019 |
| Display raster bytes | 428,715,748 | 518,184,996 | 517,148,196 |
| Native raster bytes, all metadata-only | 244,188,704 | 518,741,984 | 513,685,984 |
| Retained peak bytes | 1,070,431,488 | 1,069,764,288 | 1,063,671,488 |
| Decode/reduction time, aggregate worker µs | 26,522,019 | 6,613,220 | 6,355,938 |
| Resamples / reductions | 22 / 22 | 2 / 2 | 2 / 2 |

I2 has fewer observed transferred bytes, less aggregate decode/reduction work
and fewer refusals. It does **not** have lower settled retained memory: more
products fit, and display grids can remain larger under the existing reduction
threshold. The captured markup's 33 `sizes="100vw"` photos select 2880w
candidates at a 2522px CSS viewport. A 2880px source covering 2522px is below
the two-times-area reduction threshold; this is consistent with fewer
resamples, not evidence that the reduction policy was removed.

All three records have zero pixel acquisitions. Metadata attribution includes
host image-event polling, so acquisition counts are not counts of authored
JavaScript getter calls. The roughly 514 MB of native pixels in the I2 return
visit are still retained without a Canvas draw: I3 remains necessary.

The I2 visits have no submission, decode-allocation or decode-limit refusals,
and no validated/stale HTTP replies. The 96 MISSING diagnostics name existing
unsupported page features; the single FAILED entry is the Clarity script fetch,
also present in I1. The user reported two `d` presses on the first visit and
one after revisiting. Record 1882 is the final departure snapshot and retains
one numbered paint reading; 1884 is saved when asked. No intermediate memory
snapshot is reconstructed from the extra keypress.

These live visits retain their caches and have different request histories and
entry populations, especially during the first I2 load. They demonstrate
working responsive selection/cache reuse and improved observed coverage, but
do not isolate a causal percentage saving. No picture-budget increase was made.

The implementation follows the HTML algorithms for
[srcset parsing](https://html.spec.whatwg.org/multipage/images.html#parsing-a-srcset-attribute),
[source selection](https://html.spec.whatwg.org/multipage/images.html#selecting-an-image-source),
[sizes parsing](https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute)
and [Canvas image coordinates](https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-drawimage)
within the profile above.
