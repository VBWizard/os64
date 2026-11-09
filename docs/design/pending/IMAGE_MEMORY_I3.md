# I3: shared image requests and synchronous pixel acquisition

Quinn, 2026-11-08. Implementation on `codex/yonder-image-memory-design`;
P5 acceptance and the live-page comparison remain pending.

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
- Reduced or budget-refused rasters retain their original encoded response.
  The first draw decodes those bytes synchronously, charges the full raster,
  and drops the catalog's redundant encoded reference. Subsequent draws lend
  that raster. An in-flight resample can retain its own encoded reference.
- GIF display animation and Canvas's default frame remain independent. The
  sequence keeps its own input/canvases; the reconstruction copy and any
  promoted default-frame raster are separately charged.
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
full reconstruction/promoted grids, sequences and SVG documents. Catalog and
owner structures, URL strings (including data URLs), and per-element selection
state are separate census owners. The existing 4096-entry/4 MiB URL limits
remain. The worker pool's 8 GiB reservation cap withholds 512 MiB for a
serialized window-side image decode; the codec limits still bound actual work.
These reservations are conservative admission allowances, not measured memory
allocations or a unified process heap limit.

Resampling a completed Modern image uses its immutable source, rather than
re-fetching its URL. This exposes an important conservative case: if an image
arrived whole, its encoded body was released because that raster could fulfill
the pixel contract. If layout or zoom later reduces it, the full raster remains
its reconstruction source beside the reduced display grid. Growing back to the
full size shares the existing raster. This slice does not sacrifice the only
reconstruction source to reclaim memory. The diagnostic labels full grids
retained without Canvas use as reconstruction bytes; they are not attributed
to metadata-created native copies. Whether to retain compressed input longer,
create another recoverable representation, or reclaim inactive owners is
measurement-led follow-up work.

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
