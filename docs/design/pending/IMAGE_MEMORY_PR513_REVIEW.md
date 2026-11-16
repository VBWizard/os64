# PR #513 review response

2026-11-09. Disposition of Fable's implementation review of `81c25438`.
The original P5 evidence remains attached to its frozen `a131eece` cohort.
The corrected `0abc8edb` cohort passed the image hardware gate recorded below.

## Findings

| Finding | Disposition | Change and regression evidence |
|---|---|---|
| 1, P1: resample without backing is refused and repeatedly retried | Fixed | Missing backing falls through to the worker's request/cache/file/data path. Submit refusal records the requested dimensions. A test refuses both the original raster and its encoded body, then displays a 57,600-byte thumbnail from the HTTP cache without a second network transfer. Its body still does not fit; that does not prevent display admission. Repeated judgement at a refused size submits nothing; changing the need permits another attempt. |
| 2, P1: early whole arrival retains full plus reduced grids | Fixed | A whole arrival keeps its encoded response through the first known layout decision when it fits. Reduction retires the unborrowed full owner; keeping the whole display drops redundant bytes. Canvas-acquired full owners remain available to their loans and subsequent draws. A late-layout test clears/disables the cache and changes the server response before reduction: the retained original bytes supply the thumbnail and immediate Canvas readback, with no refetch. |
| 3, P2: GIF input retained twice | Fixed | An admitted sequence owns the retained input; the worker preserves no separate sequence body. `os64_image_sequence_default` decodes that owned input without advancing or rewinding playback. Tests cover HTTP and data GIFs after advancing to a different displayed frame. See the admission detail below. |
| 4, P2: data body retained beside its address | Fixed | Data jobs do not preserve a separate encoded body. Resampling and synchronous pixel promotion decode the retained data URL. The delivered page scripts also run with data transport in the host harness and retain zero encoded-body bytes. |
| 5, P2: preload body limit changes successful load into error | Fixed | Fetch success survives independently of retained-body admission, including failure to allocate the response record. An activation whose body was dropped requests the original URL through the normal cache path. A regression fills the preload account, observes `load`, activates the stylesheet, and applies its CSS with one network request in total. |
| 6, P2: repeated linear image-source lookups | Fixed; one suggested call removal rejected | A growing node hash index replaces list scans in source lookup, display lookup and map refresh. The ownership list remains for iteration/cleanup. A 256-image test checks identity through index growth. The end-of-poll update is necessary, as demonstrated below. |
| 7, P2: unrelated mutations reselect every image | Fixed | A bounded independent HTML change-log reader invalidates selected attributes, picture structure/source attributes, moved image subtrees and base changes. Environment changes still reselect. Lost history or inspection-budget exhaustion conservatively invalidates all entries. Tests verify no selection allocation after unrelated class/alt mutations, and correct reselection for source, base, parent and lost-history changes. |
| 8, P3: grow-to-whole allocates a disposable copy | Fixed | Judgement reattaches an existing full owner directly. The outstanding-loan regression grows back to the same pixel pointer without a submitted worker job or duplicate grid. |
| 9, P3: local-file navigation omitted from summary | Documented | `docs/yonder.md` explicitly describes clicking a local-file link from an open local page. The PR description identifies this navigation change separately. |

## Two implementation details

**The final image-provider update must remain.** The first update publishes
completed requests before the poll reads their metadata. Those metadata probes
can themselves discover a detached `new Image()` that has never been queried
by script or encountered by layout. The final update submits that newly
discovered request before the host sleeps. It does not replay a Canvas draw.

`responsive_first_poll` runs the standalone image-creation script directly,
then makes one pending-image probe. The request must already be submitted when
that probe returns. With the final update present, the test passes. A temporary
build deleting that call fails specifically:

> responsive poll: the first pending probe submits the discovered image without another task or poll

The normal focused suite passed 4,092 checks; the temporary removal produced
one failure. Subsequent full-suite validation, including additional GIF and
late-layout fixture cases, passed 24,560 checks. The call is retained and its
purpose is documented beside it. The node index and selective invalidation
remove the quadratic lookup and redundant selection costs motivating finding 6.

**GIF input belongs to an admitted sequence.** The first correction retained
the worker response through admission so a NOT_KEPT sequence could keep its
encoded body alone for a later Canvas default-frame draw. Round two reconsidered
that policy: no path restores a refused sequence to animated display, so Chris
chose to leave the remaining picture budget available for displayable images.
The worker now preserves separate encoded backing for non-data rasters, not
sequences. An admitted sequence supplies Canvas's default frame from its owned
input; a refused HTTP/file sequence has no backing and refuses pixel acquisition.
Data URLs still carry their reconstruction in their address.

## Validation and limits

- Complete Yonder host suite: **24,560 checks, zero failures**, with ASan,
  UBSan and LeakSanitizer enabled outside the sandbox.
- GIF sequence oracle: passed, including default-frame output, unchanged
  animated frames, allocation failures, loops, disposal and budgets; ASan,
  UBSan and LeakSanitizer enabled.
- Target Yonder and matching shared libraries build successfully.
- The callback-removal experiment is a negative test, not a production change.
- The existing retained-picture budget is unchanged. A request/cache retry is
  not guaranteed to be a cache hit. No synchronous network fetch was added to
  Canvas. Pixel acquisition may refuse because backing, admission or allocation
  is unavailable; it does not turn a completed image into PENDING.
- General inactive-owner eviction, broader document-load lifecycle and SRI
  remain outside these corrections. The existing WPT qualification stands.

## Hardware gate for the corrected cohort

With Modern Web and scripts enabled, use the new frozen cohort:

1. At 100%, open `/tests/pages/image-memory-metadata.html` and
   `/tests/pages/image-memory-canvas.html`. Confirm PASS and press `d` once each
   after settling. Metadata should retain no native raster; Canvas must still
   supply the immediate one-shot readback.
2. Open `/tests/pages/image-memory-late-layout.html` at 100%, confirm PASS,
   settle and press `d`. Its image has neither width nor height attributes.
   Expected settled picture ownership: 57,600 display bytes, 9,780 encoded
   bytes, zero native/reconstruction-raster bytes. The retained peak includes
   the early full-size arrival and is not expected to equal the settled total.
3. Check `/tests/pages/image-responsive.html` at 100% and 200%, including the
   replacement button; capture `d` after settling at each zoom.
4. Return to 100%, visit `https://microsoft.design/wallpapers/`, settle and
   press `d`; leave, return, settle and press `d`. Check horizontal fit and
   compare native/reconstruction, retained total/peak, source counts and
   refusals with the original I3 captures. Account for live-page/cache changes.

## Corrected-cohort P5 results

Chris completed the checklist and reported PASS for the local galleries,
including responsive selection at both zooms and the replacement button, and
successful live-page layout. Readback of all 22 installed runtime files and the
seven relevant installed fixtures matched the frozen `0abc8edb` bundle
byte-for-byte. Records 1998–2004 use Modern Web/scripts and a 2522×1257 content
viewport; their initial zoom is 100%. The original saved HTML does not preserve
on-screen PASS text, so those visual results are Chris's observations.

| Record | Fixture | Display bytes | Native bytes | Encoded bytes | Retained peak bytes |
|---|---|---:|---:|---:|---:|
| 1998 | Metadata | 57,600 | 0 | 9,780 | 67,380 |
| 1999 | One-shot Canvas | 57,600 | 8,294,400 | 0 | 8,352,000 |
| 2000 | Late layout | 57,600 | 0 | 9,780 | 8,304,180 |
| 2001 | Responsive, final account | 148,452 | 230,400 | 0 | 465,592 |

All four records are clean and have zero image refusals. Late layout records
one resample: its early whole raster contributes to the peak, but no native or
reconstruction raster remains after reduction. Canvas records one synchronous
promotion taking **46,590 µs**. Responsive records two pixel acquisitions and
one 1,393 µs promotion; its retained native raster has Canvas use, with zero
reconstruction-only bytes. Its two numbered paint readings do not constitute
independent per-zoom memory snapshots: repeated `d` captures replace the memory
facts. The visual checks at both zooms passed as reported above.

Both live records name `https://microsoft.design/wallpapers/` at 100%, with seven
ready stylesheets and no outstanding picture jobs:

| Metric | Original I3 return, 1923 | Corrected first, 2002 | Corrected return, 2004 |
|---|---:|---:|---:|
| Catalog entries / shown | 42 / 42 | 82 / 82 | 42 / 42 |
| Display bytes | 518,184,996 | 523,240,996 | 518,184,996 |
| Native / reconstruction bytes | 57,768,960 / 57,768,960 | 0 / 0 | 0 / 0 |
| Encoded backing bytes | 0 | 5,857,424 | 5,523,911 |
| Settled retained picture bytes | 575,953,956 | 529,098,420 | 523,708,907 |
| Retained picture peak bytes | 575,953,956 | 561,935,728 | 557,155,809 |
| Fresh-cache image replies | 42 | 82 | 42 |
| Worker decode/reduction total, µs | 2,924,950 | 3,560,630 | 3,485,185 |

Both corrected visits have zero image network transfers, pixel acquisitions,
promotions, metadata-only native bytes and image refusals. The return visit
retains **52,245,049 fewer picture bytes** than the original I3 return: the
57,768,960-byte reconstruction subdivision is replaced by 5,523,911 encoded
bytes, with the same display-byte total and catalog count. This is an observed
live-page comparison, not a whole-process saving or a controlled timing/network
benchmark. Decode work increased in these samples; no speedup is claimed.

The live diagnostics retain 96 MISSING features and the Clarity script fetch
failure. They also report **one unsupported inline-SVG failure absent from the
original I3 captures**. The saved list of 50 inline SVGs is identical, and the
SVG adapter/rendering call site and installed libsvg are unchanged from that
cohort. Layout/cascade changes arrived in the intervening base integration;
these facts do not establish the warning's cause. Keep it as an open diagnostic
follow-up, not an image-fetch failure or a claim of a wholly clean live page.
The image hardware gate is complete for `0abc8edb`; no new implementation or
broader SVG acceptance is implied by this evidence update.

## Round two

- **Raster reattachment applied to sequences:** fixed by restricting the
  grow-to-whole shortcut to `YONDER_PICTURE_RASTER`. The regression promotes a
  GIF's red default frame after animation advances to green, then judges it at
  natural size. Both HTTP and data-URL cases verify that the moving display,
  ownership account and layout revision stay unchanged, with no resample owed.
  Before the guard, both cases failed this check.
- **Encoded retention for a refused sequence:** Chris chose to discard the
  separate body, prioritizing remaining room for displayable images. The worker
  preserves separate input for non-data rasters; the refused-arrival path also
  restricts body adoption to rasters. A refused HTTP GIF retains dimensions and
  no picture bytes. Its pixel request returns REFUSED and is counted without a
  new fetch, even after the simulated competing memory is released. Admitted
  GIFs retain their sequence input and independent Canvas default-frame behavior;
  data URLs retain their address as before.

The frozen P5 cohort and its acceptance above remain `0abc8edb`. These narrow
GIF corrections are covered by host regression tests; they do not relabel the
earlier hardware captures as a run of the subsequent source.

Round-two validation: the complete Yonder host suite passes **24,564 checks,
zero failures**, with ASan, UBSan and LeakSanitizer enabled outside the sandbox.
The target Yonder build passes. The pre-fix focused run fails precisely the
two new HTTP/data GIF judgement checks (4,119 checks, two failures). No library
implementation changed in this round.
