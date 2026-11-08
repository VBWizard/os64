# Yonder image memory and responsive sources

Quinn, 2026-11-08. Draft for Fable's review before implementation.
Source baseline: `c21b57b370f1068f79d8867a5588ef24b999e064` (`origin/userland`).
Branch: `codex/yonder-image-memory-design`.

## 1. Decision and scope

Keep the existing reduction of display pictures. Support stylesheet preloading
so image sizing rules actually arrive, fix native image HTTP-cache integration,
implement responsive image selection, and stop metadata-only
JavaScript access from retaining a separate full-resolution bitmap. Measure
each change before deciding whether Yonder needs a broader decoded-image
eviction policy, native lazy loading, or decoder work.

These are separate costs. An HTTP cache saves network transfers; selecting a
smaller source reduces transfer and decode work; releasing unnecessary pixels
reduces retained memory. Improving one does not establish that the others are
fixed. In particular, connecting the cache does not free decoded pixels.

This is a userland design spanning Yonder, libpage and libdom, with existing
libway, libgarb/libflow and libimage facilities reused where their contracts
fit. It proposes no kernel change or increase to the picture-memory limit.
Implementation starts after design review. The review should settle the
ownership and first-Canvas-draw questions in section 9 before code begins.

## 2. What the evidence establishes

### Saved hardware records

The investigation read the saved diagnostic records and HTML for these visits:

| Record | Picture entries | Entries shown | Entries refused by the retained-picture limit | Reduced rasters | Savings reported against retaining those rasters whole |
|---|---:|---:|---:|---:|---:|
| `unsplash.com-1436` | 653 | 353 | 300 | 77 | 1,757,139 KiB |
| `wallpapercat.com-0669` | 154 | 106 | 30 | 52 | 1,338,522 KiB |
| `reddit.com-1632` | 180 | 130 | 45 | 37 | 9,371 KiB |
| `reddit.com-1635` | 315 | 267 | 39 | 63 | 10,763 KiB |

The other entries failed to load. These are catalog entries, not a count of
distinct visible HTML images. The `shown` state means a retained product is
available; it does not mean the image is inside the viewport.

The records demonstrate retained-budget refusals alongside substantial
downsampling savings. They do not report retained bytes by consumer, duplicate
pixel bytes, or actual decoder peaks. A refusal means the next product did not
fit; it does not prove the retained account was exactly 1 GiB. Nor do these
historical visits identify the binary with the source baseline above. They
motivate a controlled baseline, not a numerical attribution to one code path.

The saved Unsplash HTML has 92 `img` elements, all with `srcset`; 71 also say
`loading="lazy"`. Its photo sources include `src` URLs requesting width 3000
alongside smaller responsive candidates. This is concrete input for selection
tests, not proof that all 653 runtime entries came from the initial markup.
Scripts can add images and change their sources during a visit.

### A layout prerequisite found on Microsoft Design

Record `microsoft.design-1684`, for `/wallpapers/`, reports a 2522 by 1257 page
viewport at 100% zoom. Its picture account reads `1021M`, with 76 entries,
52 retained and 24 refused; only one raster is reduced, saving 29 KiB.
This newer record supplies an aggregate picture-memory reading, but still no
split between display and native representations.

The saved markup loads `noncritical.min.css` using
`rel="preload" as="style"` with an inline handler that clears `onload` and
sets `rel='stylesheet'`. The stylesheet is absent from the saved resource
index. The alternative stylesheet link is inside `noscript`, and scripting is
enabled. Inspection of the referenced stylesheet finds the missing `w-full`,
`h-full`, `object-cover`, `max-w-full` and `overflow-hidden` utility rules used
by the gallery. Those rules are absent from the saved inline CSS.

The missing stylesheet strongly explains original image dimensions driving
layout and horizontal overflow. Confirm causality with a controlled stylesheet
comparison before calling the layout fixed. Large full-width images can still
be intentional: a 16:9 image at this viewport width is about 1419 pixels tall.
Source selection cannot substitute for the missing sizing rules.

This belongs in the immediate feature as I0: restoring layout is a prerequisite
for interpreting this page's image-memory measurements. It is a generic
stylesheet-preload slice, not a Microsoft-specific markup rewrite.

### Code paths at the baseline

| Path | Behavior | Source |
|---|---|---|
| Display raster decode | Decode at source resolution, then reduce when a cover size is available; free the full raster | `picture.c`, `decode` and `keep_smaller` |
| Display resampling | Reconsider the largest drawn use after layout; shrink at twice the needed area, grow when needed and budgeted | `yonder.c`, `picture_drawn_at`, `css_pictures`, `pictures_judge` |
| Native image source | Skip `keep_smaller` for a nonzero `canvas_request` | `picture.c`, `decode` |
| Script metadata and events | Dimensions, `complete`, and wrapped-image load/error polling acquire an image with `pixels=false` | `libdom/canvas_image.c`, `image_member`, `d_image_dimension`, `os64_dom_image_event` |
| Provider acquisition | The same native request is started for status-only and pixel acquisitions; `pixels` does not prevent a full raster decode | `yonder.c`, `canvas_source` |
| Catalog identity | Display uses mode 0; native requests use modes 1–3. The same URL can occupy both entries | `yonder.c`, `picture_slot`, `picture_for_mode` |
| Native HTTP caching | The temporary `yonder_fetch_job_t` omits `cache`, although the enclosing picture job has `hooks.cache` | `picture.c`, `yonder_picture_run` |
| Cache compatibility | `fetch_job.c` requires stored response headers and matching request variation; an old `way_fetch_whole` entry without headers is a miss | `fetch_job.c`, `cache_open`, `serve_kept` |
| Source selection | The page model resolves `src`; the native provider independently reads `src` | `libpage/core.c`, `add_image`; `yonder.c`, `canvas_source` |
| Lifetime | Catalog pixels are released at page teardown or when replaced by resampled pixels; there is no general eviction on scrolling away or node removal | `yonder.c`, `page_clear`, `picture_resampled` |

The full-resolution duplicate is a strong candidate for unnecessary retained
memory. Its contribution to the hardware totals remains unmeasured.

Other retained costs remain distinct: animated GIF sequences use their source
canvas size; an unused or hidden display picture with no measured need keeps
what it has; pixelated uses intentionally retain source pixels. The 512 MiB
`PICTURE_RESERVE` is a conservative worker admission allowance, not a measured
512 MiB allocation per image. The 1 GiB `PICTURES_KEPT_MAX` is a page account,
not a bound on the whole process or all concurrent working storage.

## 3. Contracts the changes must preserve

There are four separate facts: the selected request, its metadata, its encoded
content, and any decoded representations. A natural dimension is not the
width of a reduced grid. A successful request is not an assertion that a full
bitmap must remain allocated. A URL alone is not authority to reuse content
across request modes or to declare pixels origin-clean.

The HTML image algorithms define source-set parsing, selection and updates;
the element exposes `currentSrc` and density-corrected natural dimensions.
Use those algorithms as the compatibility contract, including malformed-input
handling and request transitions, rather than inventing a comma splitter.
See [HTML images](https://html.spec.whatwg.org/multipage/images.html) and
[the image element](https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element).

Yonder's existing provider lends a stable view until release. Metadata reads
must remain cheap after completion. Canvas drawing from an already available
image must have the required pixels during the call; an internal representation
miss must not turn a successful draw into a silent no-op or manufacture another
load event. Source changes and navigation must reject obsolete worker products.

For example, a 3840 by 2160 ARGB raster occupies 31.64 MiB; a 400 by 225 display
copy occupies 0.34 MiB. Keeping both costs about 31.98 MiB. The encoded file's
size depends on its content and codec. Keeping recoverable encoded content can
save memory, but needs its own account and a measured decode-latency tradeoff.

## 4. Immediate implementation

### I0 Support stylesheet preload and its activation event

Implement connected `link rel="preload" as="style"` requests, including links
inserted or changed by script. Follow the
[HTML preload contract](https://html.spec.whatwg.org/multipage/links.html#link-type-preload):
preloading must not apply CSS or become a render-blocking stylesheet, and does
not itself delay document load. Deliver the preload completion event through
the existing task machinery. A subsequent change to `rel="stylesheet"` activates
the normal stylesheet lifecycle, consuming a compatible preload response.

Keep document-owned preload state separate from applied `Sheet` entries.
Bound its entries and retained bytes; account for ownership transfer when the
stylesheet consumes it. Reuse fetch/cache and owed-event facilities, without
equating preload completion with stylesheet parsing or import completion.
Preserve request identity, policy and failure information through handoff;
reuse must respect URL, destination, mode, credentials and integrity compatibility.
Keep obsolete completions from activating a newer link request.

The style-only scope includes request attributes, applicable media/type checks,
link mutation reporting and the bindings necessary to support that lifecycle.
Audit existing support before adding another observer or event queue. Review
unsupported policy/integrity behavior explicitly rather than silently treating
an unverified response as usable. Other preload destinations, HTTP early hints
and the existing modulepreload debts remain outside this slice.

The decisive fixture uses a large image and a preloaded stylesheet that sets its
display width. Assert that CSS has no effect before activation; the inline
`onload` handler changes `rel`; then the image fits its container. Also exercise
ordinary stylesheet links, errors, a retained handler across activation, dynamic
insertion, replacement during fetch, navigation, incompatible request identities
and cache-disabled operation. Compatible handoff must not depend solely on the
HTTP cache or download the body again. Assert preload and stylesheet event
lifecycles separately, rather than suppressing a legitimate second event or
allowing an activation loop.

### I1 Connect native image requests to the HTTP cache

Pass the picture job's cache into its native fetch job. Audit the request
construction alongside it: destination, Accept negotiation, referrer, origin,
credentials, redirects and cache policy must describe an image request. Reuse
the existing fetch/cache machinery and CORS checks, including on cached replies.

Do not accept a display-only cache entry as evidence of readable Canvas pixels.
`cache_open` explicitly rejects entries without response headers. The first
native request may therefore fetch again even after a display request has
cached the URL. Subsequent eligible native requests should reuse their complete
cache entries. Header-aware sharing with display fetches is a separate decision
if measurement shows this first duplicate remains material; it is not silently
promised by passing the pointer.

Tests must distinguish a fresh cache hit, conditional validation, and a full
transfer. Include cache disabled, missing/evicted entries, legacy headerless
entries, `Vary`, credential separation, redirects and rejected CORS. A cache
miss is allowed. A taint or credential bypass is not. Exercise alternating
display/native requests too: a later headerless cache write must not make the
measurement mistake repeated misses for successful shared reuse.

### I2 Implement one responsive selection result for each image element

Build a shared selector, consumed by both page display and native image
acquisition. Do not add a second independent `srcset` parser to the Canvas
provider. Pure candidate parsing belongs beside the page model; environmental
evaluation uses the existing media/length facilities. Yonder supplies the
environment and owns asynchronous requests; libdom exposes the element state.
Choose exact module/API names during implementation after inspecting those seams.

The immediate scope includes `img srcset`, `sizes`, and preceding `source`
elements in `picture`, with their media/type choices and fallback to `img`.
Expose the corresponding reflected attributes and `currentSrc` through the
existing bindings. CSS `image-set`, responsive preload and new codecs are
outside this slice.

Use a bounded parser with explicit refusal diagnostics for resource limits.
Cover density and width descriptors, default-source participation, invalid
tokens, whitespace, duplicate descriptors and URLs containing commas. Resolve
relative candidates against the document base. Format eligibility must reflect
the actual image decoder/renderer profile, not a list of MIME types it cannot
render. Candidate failure must follow the request contract, not an improvised
loop that downloads every alternative.

Selection uses the same CSS viewport and effective pixel ratio that Yonder
reports to style and scripts. `css_view` and the devicePixelRatio provider are
the existing sources of those facts; do not multiply page zoom twice. Evaluate
`sizes` with the existing media and CSS-length machinery. Document unsupported
expressions and apply the standard fallback behavior. Treat `auto` with its
eligibility rules; general native lazy loading remains section 7 work, so this
slice must not pretend every image is eligible for automatic sizing.

Keep the selected URL, source density and selection generation with the
element's request state. Consumers must not return to the literal `src` after
selection. Handle `srcset`-only images, including those which `add_image` now
omits because `src` is empty. Reevaluate on relevant source/attribute/base or
environment changes, and invalidate the request when its mode changes. Coalesce
related mutations before starting work. A pending replacement must not let a
late completion overwrite a newer selection. Preserve the current image while
a replacement is pending where the request algorithm requires it.

Separate source raster dimensions, density-corrected natural dimensions and
display-grid dimensions in the host view. This affects layout, DOM getters and
Canvas source-coordinate mapping. A 2x source is a required regression case:
selecting it must not double the element's natural CSS size or crop the wrong
part of a Canvas source. Fixing rendered `img.width`/`height` to use resolved
CSS content boxes remains the separate DEBTS item; density correction cannot
wait for that layout work.

This needs a shared selected-request state, but does not claim to finish every
HTML image lifecycle feature. Document-load dependencies, unwrapped markup
handlers and `Image.decode()` remain separately tracked unless a specific
dependency is demonstrated and reviewed. Selection updates must work within
the event/task paths already implemented.

### I3 Avoid retaining full rasters for metadata-only access

Give native image acquisition two representations of a completed source:

| Representation | Owned content | Response to metadata acquisition | Response to pixel acquisition |
|---|---|---|---|
| Metadata ready | Validated dimensions/status/security result plus bounded recoverable source content | Return metadata without allocating a full grid | Materialize a stable pixel view |
| Pixels ready | Full default-frame pixels, or the existing SVG document/render machinery | Return the same metadata | Lend pixels until release |

For raster sources, the initial bounded worker may still perform a full decode
to establish successful decoding, then release its full grid for a metadata-only
consumer. Header-only format probing is not required by this slice, and a
plausible header must not convert an invalid image into a successful load.

Retain the exact encoded response for a metadata-ready raster, owned by the
selected request and charged to the page's image account. Keep the response's
origin/CORS verdict with it. Use the existing encoded-body limit and charge
retained backing against `PICTURES_KEPT_MAX`; do not create an additional
unbudgeted pool. This backing is active document state, distinct
from the HTTP cache; it must remain reconstructible if the HTTP cache is
cleared, evicted, stale or disabled. It is not permission to persist a forbidden
HTTP cache entry or expose an opaque body to script. Data URLs already have
retained source text: account for text and decoded bytes without hiding a second
copy. A mutable file or network URL is not an immutable reconstruction source.

The proposed first implementation promotes a raster on its first pixel
acquisition by decoding its retained bytes synchronously, without network I/O
or script reentry. It then keeps those pixels for later draws, as the native
path does today. This is deliberately not a per-draw decode or an eviction
cache. It bounds the architectural change while making first-use decode
latency an explicit measurement. If that latency is unacceptable, revise the
promotion design before broad rollout rather than return pending and omit a
draw. Allocation/refusal follows the provider's explicit error contract and
must leave metadata and source ownership intact.

Admission must cover old retained bytes, temporary decoding and the new grid
during promotion. Serialize and reserve synchronous work separately from worker
admission so it cannot exceed the intended memory envelope unnoticed. After a
successful promotion, a raster's encoded backing can be released if no other
representation depends on it: this slice does not evict the full grid later.
On failure, retain the prior valid state. Pins protect borrowed views until
release; page teardown and source replacement release each owner once.

Display reduction continues independently. Reuse an existing resource's
metadata/content only when its request identity and security evidence are
compatible; do not erase the current mode separation just to reduce a counter.
Generic cross-mode content deduplication is not required here. If a native
metadata request still obtains its own compressed copy, measure and account for
that copy honestly.

Keep GIF default-frame Canvas behavior distinct from display animation, and
preserve SVG's secure-static document ownership. Neither a GIF sequence nor an
SVG tree should be forced into a raster-only metadata abstraction. This slice
does not add GIF sequence shrinking or broader SVG support.

## 5. Measurement before and after the immediate changes

### M0 Establish the baseline before changing behavior

Add observational counters first, then run the original behavior at the same
source revision. Preserve that instrumented baseline. Run I0, I1, I2 and I3 with
the same inputs so improvements can be attributed separately. Include a final
combined run; do not compare only the start and finish of four simultaneous
changes.

For Microsoft Design, record stylesheet activation and geometry before the
memory comparison. Separate savings caused by restored CSS from savings caused
by source selection or representation changes.

Keep counters in the existing diagnostics. Record current values and peaks
separately, and explain the boundary of each account:

| Measurement | Question it answers |
|---|---|
| Retained bytes by display raster, native raster, encoded backing, GIF sequence and SVG document | What is still owned after the page settles? |
| Number of requests, resource owners and representations; URLs shared across display/native modes | How much duplication exists, without implying that cross-mode reuse is safe? |
| Metadata-only acquisitions and promotions, with full-raster bytes retained because of each | Does I3 remove the suspected cost? |
| Selected source, descriptor/density, natural dimensions, raster dimensions and drawn need on selected fixtures | Is I2 choosing and sizing the intended resource? |
| Network body bytes, full transfers, cache hits, validations and misses | Does I1 save transfers, and does I2 reduce the requested data? |
| Decode/reduction/promotion count and elapsed time | Is a memory improvement buying excessive decode work? |
| Actual image allocation peaks and concurrent decoding, with known coverage limits | Are temporary peaks still material? |
| Refusal counts by retained budget, working admission, decoder limit and allocation failure | Why did a picture fail to appear? |
| Process memory, page load time and Canvas frame/first-draw latency | Does the change help the application as a whole? |

Report actual allocations where hooks exist. If a codec lacks allocation
coverage, label its estimate or missing evidence rather than substituting
`PICTURE_RESERVE` as a measured peak. Shared allocations are charged once to
their owner, with borrower counts separate. The native image account is also
separate from the JavaScript heap and Canvas destination bitmaps.

Instrument cache outcomes per request without logging cookies, credentials or
private query strings. Detailed per-image rows can be opt-in for fixtures;
normal browsing gets bounded aggregates. Do not turn the diagnostic facility
into a second unbounded catalog.

### M1 Use repeatable fixtures before live-site comparisons

Prepare a small local fixture server with known image bodies and transfer
counters. The core matrix is:

1. Large still images displayed as thumbnails, with no script touching them.
2. The same page with dimension/complete reads and load listeners, but no Canvas.
3. The same images drawn once to Canvas, then repeatedly in an animation.
4. Density and width candidate sets, media-selected `picture` art, source-only
   markup, and malformed candidate inputs.
5. Resize/zoom and source/mode changes during fetch, followed by navigation away.
6. Fresh cache, stale validation, cleared/disabled cache and non-cacheable replies;
   include changed server bytes at the same URL while an old loaded image lives.
7. GIF, SVG, data and local-file sources; hidden and removed image elements.
8. Stylesheet preload with event-driven activation, compared with an ordinary
   stylesheet link supplying the same bytes and an intentionally inactive preload.

Use fixed viewport, zoom, settings and scripted interactions. Record source
commit, fixture hashes, browser binary identity and cache condition. Separate
first-visit and warm-cache runs; use three runs of each measured case and retain
the individual readings, median and worst value. Report first promotion apart
from steady animation so a large first-draw stall cannot disappear in an average.

### M2 Revisit a small hardware cohort

Use Microsoft Design wallpapers as the I0 layout gate, then Unsplash desktop
wallpapers, Wallpapercat desktop and a Reddit image feed, plus the local
metadata-only and Canvas fixtures. Agree the live actions and
viewport once, and keep them fixed for the comparison. Live-site content drift
makes these corroborating observations; fixture results supply causal evidence.

The saved corpus is useful for markup and failure examples, but the existing
replay does not restore picture fetch/decode outcomes. Do not claim a normal
corpus replay measured image memory. Build only the affected binaries, matching
libraries and fixtures into a small frozen delivery with a SHA-256 manifest;
keep it stable across the hardware readings.

### Exit criteria for the immediate work

- The stylesheet-preload fixture activates through its real event handler and
  matches the ordinary-stylesheet control's image geometry. The Microsoft page
  records its main stylesheet as applied; compare horizontal extent and image
  boxes before attributing its remaining size or memory behavior to images.
- Eligible warm native requests avoid a body download; a required validation
  is recorded separately. Security and cache-policy tests remain correct.
- Responsive fixtures request the expected candidate and retain correct natural
  dimensions through resize, zoom and mutation. Display and Canvas agree on the
  selected request.
- A metadata-only fixture retains no separate full-resolution native raster
  after settlement. Encoded backing is bounded, counted, and survives HTTP-cache
  removal while the loaded image still needs it.
- Drawing an already loaded source succeeds during the call. Repeated draws do
  not repeatedly decode. Readback, crop mapping, alpha, taint and export remain
  correct. First-use latency is reported for the review decision.
- Accounting reconciles after source replacement, cancellation and teardown;
  the targeted host suites pass with ASan/UBSan and leak detection enabled.
- The report gives retained and peak bytes before/after, transfer and decode
  counts, latency, and remaining refusals. No arbitrary percentage improvement
  or zero-refusal promise substitutes for that evidence.

At this gate, Chris and the reviewers decide whether the remaining memory and
first-use latency are acceptable. If the scoped fixes suffice, stop. A proposed
larger mechanism needs evidence from the next section.

## 6. Validation and implementation order

Order: M0 counters and baseline; I0 stylesheet preload; I1 cache integration;
I2 selection; I3 metadata
representation; M1 comparisons after each; M2 combined hardware comparison;
review the result. I2 and I3 share selected-request state, so design that
interface before implementing either rather than merging two incompatible
lifetimes afterward.

Extend the existing suites: `tools/test_yonder_fetch_job_host.sh`,
`tools/test_way_fetch_host.sh`, `tools/test_libpage_host.sh`,
`tools/test_yonder_scripts_host.sh`, and the picture-size/Canvas fixtures in
`tools/test_yonder_picture_sizes.inc` and `tools/test_yonder_canvas.inc`.
Pin a focused upstream WPT image/source-selection set with explicit expectations;
do not mark an entire directory passing based on custom fixtures. Test bounded
parser failures, allocation failures and old-generation arrivals as well as the
successful paths. Reuse the existing source-size and reduction tests.

Keep leak detection enabled. Diagnose attachment failure and run the same
binaries outside a restrictive sandbox when that is the supported route;
record missing leak evidence if no supported run exists.

Update comments with the implementation. In particular, the comment beside
`PICTURES_KEPT_MAX` still describes pictures as retained at their own size;
that statement does not describe the display reduction already implemented.
Correct it when editing this ownership path, and record the mistaken claim in
the commit message. Keep old design documents readable as historical designs;
add precise cross-references instead of silently rewriting their evidence.

## 7. Work considered only after measurement

| Evidence after I0–I3 | Possible follow-up | Contract that needs a separate design |
|---|---|---|
| Large idle Canvas/native grids dominate retained memory | Bounded decoded-image cache with eviction and pinning | Immutable recoverable backing, synchronous draw, first-use latency, and encoded-byte budget |
| Removed/replaced sources accumulate through a long visit | Release unreferenced resource owners or reclaim inactive entries | Element and pending-request references, Canvas loans, animations and document teardown |
| Images far from the viewport dominate loading and memory | Native `loading="lazy"` support | Scheduling, observation margins, load dependencies, auto-sizing and interaction with scripts |
| Full-resolution JPEG decoding dominates peak bytes/time after source selection | Scaled JPEG decoding at supported reduction factors | Quality, dimensions, decoder limits, and display versus Canvas resolution requirements |
| Animated GIF sequences dominate retained memory | Reduced animation sequences | Disposal/restore semantics, frame timing and zoom growth |
| Separate native/display compressed bodies or fetches remain significant | Shared header-aware resource ownership | Request identity, credentials, origin/CORS proof, variation and cache compatibility |
| SVG preparation or storage dominates | Existing SVG retention/cache follow-ups | SVG document validity, render budgets and secure-static behavior |

An HTTP cache is not an ownership guarantee for an already loaded image. It can
be cleared, become stale, or decline to keep a response. The system's disk cache
can accelerate reading an existing backing file; it does not recover a missing
HTTP response. Any later pixel eviction must retain a reliable reconstruction
source and its identity. Re-fetching a changed URL must not silently replace the
content of a still-loaded source during Canvas drawing.

Raising `PICTURES_KEPT_MAX`, reducing quality globally, and decoding every Canvas
source on every frame are not proposed remedies.

## 8. Existing debts and related designs

- [PICTURE_SIZES.md](PICTURE_SIZES.md): retain the display reduction and its
  distinction between natural and kept sizes. Its JPEG and GIF residuals remain
  measurement-dependent follow-ups here.
- [CANVAS.md](CANVAS.md): image provider loans, source modes, taint, default GIF
  frame and secure-static SVG. This proposal changes source representation,
  not the destination Canvas bitmap model.
- [SCRIPT_CACHE.md](SCRIPT_CACHE.md): fetch cache keys, stored headers,
  validation and live CORS checks. I1 must use these contracts.
- [LOAD_EVENTS.md](LOAD_EVENTS.md): task delivery conventions to reuse for I0, without
  treating stylesheet/script completion as a complete image lifecycle design.
- [DEBTS.md](../../../DEBTS.md): the Canvas image-source lifecycle row is the
  closest existing entry. Close only the portions actually implemented; retain
  outstanding image interfaces, document-load behavior and export limitations.
  The rendered-image-dimension and image-button-dimension rows remain separate.

## 9. Questions for Fable's review

1. Does the proposed split between selected request, encoded backing and pixel
   representation fit the current page/provider ownership, or should a shared
   resource owner be introduced before I2/I3? Identify the smallest necessary
   interface change rather than assume complete image-lifecycle replacement.
2. Is synchronous first-pixel promotion from bounded immutable bytes an acceptable
   initial tradeoff? If not, specify how an already loaded image remains drawable
   synchronously without retaining full pixels for every metadata-only consumer.
3. Are I2's binding, density, environment and mutation boundaries sufficient to
   prevent display/native disagreement? Which focused WPT cases should gate it?
4. Does I1 correctly preserve header/CORS requirements while accepting that
   legacy display-cache entries can miss? Is any additional shared fetch work a
   prerequisite, rather than a measurable later improvement?
5. Are the retained/working accounts and baseline measurements sufficient to
   decide whether eviction is needed, without hiding compressed backing or
   synchronous decoder scratch outside the totals?
6. Is I0's separate preload state and stylesheet handoff the right bounded
   addition to the existing link/event machinery? Confirm its policy checks,
   completion events and memory ownership before implementation.

The review outcome should approve or revise the immediate slices and their
measurement gate. Section 7 is a set of conditional options, not an instruction
to implement a larger cache system before those results exist.
