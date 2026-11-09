# I3: shared image requests and synchronous pixel acquisition

Quinn, 2026-11-08. Implementation on `codex/yonder-image-memory-design`;
P5 local validation and the 100% live-page comparison are recorded below.
Those captures validate the frozen `a131eece` cohort, before the PR review
corrections described in [the review response](IMAGE_MEMORY_PR513_REVIEW.md).

## Behavior

The selected display request now supplies image dimensions, completion and
load/error delivery. Metadata access does not create a second native request
or retain a full raster. Detached images use the same selection and catalog;
I2's pending/current generations and density corrections remain in force.

Modern Web display HTTP requests use the header-aware image fetch engine,
including cache policy, request credentials, CORS and redirect-chain taint.
Native image consumers retain MIME headers for decoding, while their recorded
response type still governs Canvas origin cleanliness. These products do not
enter script-response delivery. The legacy non-Modern display path is separate.

Canvas acquires pixels synchronously:

- A retained full display raster is lent directly, without another decode.
- Reduced or budget-refused rasters retain their original encoded response
  when it fits. Data URLs reconstruct from their retained address.
  The first draw decodes those bytes synchronously, charges the full raster,
  and drops the catalog's redundant encoded reference. Subsequent draws lend
  that raster. An in-flight resample can retain its own encoded reference.
- GIF display animation and Canvas's default frame remain independent. The
  admitted sequence keeps its own input/canvases, without a second retained
  body. Default-frame acquisition decodes that input without changing playback.
  A sequence refused by the retained budget can keep the encoded body alone.
- SVG keeps its secure-static document and synchronous rendering contract.
  A refused SVG document can still report successful dimensions; pixel
  acquisition explicitly refuses when no document was admitted.

An allocation or retained-budget refusal returns REFUSED, preserving completed
metadata and any reconstruction owner. It does not manufacture a pending
request, replay a draw, fetch a mutable URL, or fire another load event.
When even encoded backing does not fit at initial arrival, the picture retains
its metadata and subsequent pixel acquisition explicitly refuses.

## Ownership and bounds

Immutable reference-counted owners hold encoded bytes, full rasters and catalog
URLs. The catalog, resample workers and pixel loans have independent references;
page teardown can release its ownership while a worker or loan still holds its
input. Display rasters that borrow a full owner are not freed or counted twice.

The 1 GiB retained-picture account includes encoded backing, display grids,
full display/promoted grids, sequences and SVG documents. Catalog and
owner structures, URL strings (including data URLs), and per-element selection
state are separate census owners. The existing 4096-entry/4 MiB URL limits
remain. The worker pool's 8 GiB reservation cap withholds 512 MiB for a
serialized window-side image decode; the codec limits still bound actual work.
These reservations are conservative admission allowances, not measured memory
allocations or a unified process heap limit.

A whole raster keeps encoded input through its first known layout decision
when that input fits. Keeping the whole display drops the redundant input;
reducing it keeps the encoded response and releases the unborrowed full raster.
A full owner survives reduction after Canvas has acquired it. Growing back to
that retained full raster reattaches it directly without submitting a copy job.

A display resample uses retained encoded bytes or an immutable Canvas owner
when available. Without either, it uses the ordinary request/cache/file/data
route on a worker. A changed response with different natural dimensions is
refused, preserving the old representation; an accepted response supplies its
own taint. A thumbnail can be admitted even if its reconstruction does not fit;
a subsequent pixel request can explicitly refuse. Submit failures remember
the attempted size and do not repeat until the need changes. These display
retries do not introduce network fetches into synchronous Canvas acquisition.
The reconstruction subdivision remains available for diagnosing retained full
owners without Canvas use; it is not a policy to keep such owners after reduction.

The reduced-grid diagnostic subtracts the complete retained cost and clamps
savings at zero. Retaining reconstruction storage cannot wrap the savings
counter or be presented as released memory. First-draw promotions, elapsed
decode time and refusals have their own diagnostic line.

## Evidence

The original real-PNG fixtures retain their bytes and scripts. Their source is
1920 by 1080, displayed at 160 by 90 at 100% zoom:

| Fixture | Display bytes | Native bytes | Encoded backing bytes | Native solely for metadata |
|---|---:|---:|---:|---:|
| Untouched | 57,600 | 0 | 9,780 | 0 |
| Metadata | 57,600 | 0 | 9,780 | 0 |
| One draw in onload | 57,600 | 8,294,400 | 0 | 0 |

The metadata fixture previously retained 8,294,400 native bytes in addition to
its display grid. The new retained pixel/backing total is 67,380 bytes; catalog,
DOM and script storage are additional. These are controlled host results, not
claims about Microsoft or whole-process memory savings.

`tools/test_yonder_image_ownership.inc` exercises the production window
provider, fetch/cache engine and PNG/GIF codecs. It covers cache absent and
cleared/disabled, changed server bytes, one-shot onload drawing with immediate
readback, NOT_KEPT metadata and promotion refusal/retry, allocation cuts,
repeat draws, shared full-raster loans across resampling and teardown,
currentSrc lifetime, default GIF frame after display advancement, and no-CORS,
anonymous, credentialed and cross-origin-detour taint. I2's mutation/obsolete
completion and density/default-draw/crop assertions continue to run.

The host uses synthetic raster dimensions for some sizing tests, real PNG/GIF
codecs for ownership fixtures, a fake network, and real cache code on its fake
filesystem. It does not measure P5 first-draw latency or total process memory.

- Full Yonder host: **24,145 checks, zero failures**, including the final raster,
  URL and SVG loan lifetime tests.
- Shared fetch/cache host: **823 checks, zero failures**.
- Pinned density WPT (`a699fb16767be39ba3e61a8e8b6bef60383f08ee`): the
  original `current-pixel-density/basic.html` remains **0/14**; changing only
  PNG transport to data URLs also remains **0/14**. The documented I2
  adaptation, which runs the unchanged assertions after wrapped image
  completion, remains **14/14**. This is a density-profile result, not an
  unmodified WPT conformance pass. Document-load dependencies remain a debt.
- Host and WPT runs used ASan, UBSan and LeakSanitizer. The target Yonder build
  succeeds with warnings treated as errors.

The WPT run preceded the final SVG-loan teardown guard; the complete host run
covers that guard and its regression test. No DOM/density code changed between
those runs. Broader image-lifecycle WPTs listed in the design have not been
claimed as passing here.

## P5 gate

Use a new frozen bundle with matching dependencies and the unchanged metadata,
Canvas and responsive galleries. Check metadata and one-shot Canvas PASS, then
responsive selection/cropping at 100% and 200%. Capture settled Microsoft
wallpapers on first visit and return with Modern Web/scripts enabled. Compare
retained subdivisions, promotion cost, source counts and refusals with the
accepted I2 captures; account for cache and live-page variation. Do not change
the retained budget or reduction threshold to improve the comparison.

## P5 local validation and supplemental live capture

Chris reported completing the requested hardware checks. All 22 installed
runtime files and the six installed gallery assets read back byte-for-byte
against the frozen I3 cohort. The records have Modern Web and scripts enabled
and a 2522×1257 content viewport.

| Record | Result | Display bytes | Native bytes | Encoded bytes | Synchronous promotions |
|---|---|---:|---:|---:|---:|
| 1915, metadata | clean | 57,600 | 0 | 9,780 | 0 |
| 1916, one-shot Canvas | clean | 57,600 | 8,294,400 | 0 | 1, taking 58,208 µs |
| 1917, responsive | clean | 288,000 | 0 | 0 | 0 |

All three have zero image refusals. The responsive record contains three
numbered paint readings and two pixel acquisitions; its two image catalog
entries are shared across display and Canvas. The final retained peak was
465,592 bytes. The capture header gives initial zoom, not each reading's zoom,
and repeated diagnostic requests overwrite memory facts. These are not
independent per-zoom memory snapshots. Original saved markup does not itself
record the live PASS text.

Microsoft record 1918 is the singular `/wallpaper` URL and returns HTTP 404;
it is excluded from the wallpapers comparison. Record 1919 is the correct
`/wallpapers/` page but began at 200% zoom. It is supplemental evidence:
42 of 42 catalog entries shown, no image refusals, 520,023,992 display bytes,
57,768,960 native bytes entirely attributed to reconstruction, and **zero
metadata-only native bytes**. All 42 image responses came from fresh cache;
there were no image network transfers or Canvas promotions. Its 96 MISSING
features and Clarity script fetch failure match the existing limitations.

The subsequent 100% pair is recorded below. The 200%-initial capture is
supplemental and is not used for the before/after percentage.

## Matched 100% Microsoft visits

Chris repeated the two visits at 100%. Records 1921 and 1923 both have a
2522×1257 content viewport, Modern Web/scripts enabled, seven ready stylesheets,
one settled diagnostic reading and no outstanding image jobs. The first visit
was entered as `http://microsoft.design/wallpapers/`; the return record names
`https://microsoft.design/wallpapers/`, matching the I2 return URL. Yonder's
installed hash was rechecked after these captures and still matches I3.

| Metric | I2 return, 1884 | I3 first, 1921 | I3 return, 1923 |
|---|---:|---:|---:|
| Catalog entries / shown | 76 / 74 | 82 / 82 | 42 / 42 |
| Display raster bytes | 517,148,196 | 523,240,996 | 518,184,996 |
| Native raster bytes | 513,685,984 | 57,768,960 | 57,768,960 |
| Native solely for metadata | 513,685,984 | 0 | 0 |
| Native reconstruction bytes without Canvas use | not separately reported | 57,768,960 | 57,768,960 |
| Encoded backing bytes | not retained by I2 | 0 | 0 |
| Settled retained picture bytes | 1,030,834,180 | 581,009,956 | 575,953,956 |
| Retained picture peak bytes | 1,063,671,488 | 581,009,956 | 575,953,956 |
| Retained-budget refusals | 2 | 0 | 0 |
| Network / fresh-cache image replies | 14 / 64 | 0 / 82 | 0 / 42 |
| Network image entity bytes | 27,287,019 | 0 | 0 |
| Worker decode/reduction total, µs | 6,355,938 | 3,217,332 | 2,924,950 |
| Canvas pixel acquisitions / promotions | 0 / not applicable | 0 / 0 | 0 / 0 |

The recorded return-visit picture account is **454,880,224 bytes lower
(44.1%)**. This is an observed comparison, not a controlled claim about total
process memory or network savings: the live page, cache history and catalog
populations differ. The per-request metadata-only raster cost is gone, consistent
with the controlled local fixture. The remaining 57.8 MB native subdivision is
two full reconstruction owners retained after display reduction, not a second
raster created by metadata polling. Display rasters account for about 518 MB.

Both I3 visits have zero image submission, allocation, decode-limit and
retained-budget refusals. Their 96 MISSING features and single Clarity script
fetch failure match I2; no new image-fetch failure appears. All image responses
in these visits were fresh cache hits. This does not establish cold-cache
transfer savings.

The original local and live I3 hardware gates are complete for `a131eece`.
Retain that frozen cohort. The evidence supports removing metadata-created
copies; PR review additionally identified the unnecessary post-reduction full
owners above, now corrected in the review branch. The corrected build needs
the separate hardware gate in the review response. Inactive-owner eviction and
decoder policy changes remain measured follow-ups. The local first-draw
promotion's 58.2 ms cost is explicit evidence
of the synchronous latency tradeoff, rather than a claim that promotion is free.

## PR integration

The review branch integrates `userland` through `1f42cb36`. Its DOM wrapper
split keeps the image completion generation in the compact wrapper, sharing
the plain engine-held target's listener-hook slot. Listener-hook dispatch is
guarded by the target kind, preserving the existing 128-byte charged-size gate.
The separate DOM Canvas fixture uses designated initializers so the extended
image-provider API retains its documented zero-initialized defaults. The
responsive-image regression also adds, removes and assigns listeners after
completion to exercise the distinction between the shared slot's uses.

The integrated Yonder host suite passes **24,215 checks, zero failures**, with
ASan, UBSan and LeakSanitizer enabled outside the sandbox. The DOM target-engine
profile passes **42,982 checks, zero failures** (3,849 binding allocation
sweeps). The separate fully instrumented engine profile passes **304,728
checks, zero failures** (36,142 binding allocation sweeps). Both DOM profiles
complete with leak detection enabled outside the sandbox. The target Yonder
build passes.

The integration checks cover the merged source; the P5 results
above belong to the frozen `a131eece` implementation cohort, which remains
unchanged and has not been replaced with the integrated build.
