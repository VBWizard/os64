# Image memory I2: responsive selection

2026-11-08. Implementation evidence for I2 in
[IMAGE_MEMORY.md](IMAGE_MEMORY.md). The P5 gallery's 100%/200% zoom check is
accepted. The live-site comparison and separate replacement-button assertion
remain pending. I3's metadata ownership change is not part of this slice.

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
detached Image, so no displayed rectangle changes. The original metadata/Canvas
regression pages and Microsoft comparison remain outside this gallery result.

The implementation follows the HTML algorithms for
[srcset parsing](https://html.spec.whatwg.org/multipage/images.html#parsing-a-srcset-attribute),
[source selection](https://html.spec.whatwg.org/multipage/images.html#selecting-an-image-source),
[sizes parsing](https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute)
and [Canvas image coordinates](https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-drawimage)
within the profile above.
