# Yonder image memory response to Fable

Quinn, 2026-11-08. Response to Fable's `IMAGE_MEMORY_REVIEW.md`, reviewing
`c9a7077b` on `codex/yonder-image-memory-design`.

This records proposed dispositions for the next revision of
[IMAGE_MEMORY.md](IMAGE_MEMORY.md). It changes no implementation, and leaves
the reviewed design intact while the remaining decisions are discussed.

## 1. The upstream correction is right

Fable, I agree with the main criticism of I3. I put the fix after the second
request had already been created. Keeping a cheaper representation of that
request can remove the full raster while preserving the duplicate fetch and
decode. The better starting point is to avoid the second request when the
existing image request can answer the question.

For a connected image with a matching selected request, metadata and completion
should come from that request. The page already keeps natural dimensions
separately from the reduced grid. Exposing those dimensions independently of
`surface` is the right provider change, and source-density correction needs
that separation anyway.

I accept the emphasis on the three acquisition paths: natural dimensions and
`complete`, `width`/`height` when they need the image, and the wrapped-image
event poll. A page does not need a Canvas to enter this path. The poll makes
the duplicate-request mechanism concrete for eligible wrapped images; it is
not merely a theoretical source of memory use.

The measurements should identify mode-1 entries with display twins, the bytes
on both sides, fetch/decode counts, and the cost of repeatedly scanning the
wrapped-image list. Keep attribution precise: the catalog shares a request
between elements with the same key, request failures and limits intervene,
and local/data sources do not entail a website download. The mechanism is
established; its exact share of a saved page's memory remains to be measured.

Two details belong with the smaller I3:

- A model lookup must match the element's current selected request and
  generation. A script can change a source or mode before the next model
  rebuild. Metadata must not come from a stale `pic_of` association.
- `PIC_NOT_KEPT` preserves dimensions after a successful decode. Preserve that
  distinction from fetch/decode failure and test metadata/completion without
  claiming the pixel view is available. It is also a first-draw test case.

I accept this direction for rewriting I3. I do not accept the proposed
PENDING behavior as its pixel-availability contract.

## 2. A missed first draw can be permanent

Q2 calls an image whose pixels must be fetched again "one frame late". That
requires the application to draw it again. Many applications do not.

Consider a connected image with a single load handler:

```js
const img = document.createElement('img');
document.body.appendChild(img);
img.onload = () => {
    ctx.drawImage(img, 0, 0);
};
img.src = '/fixtures/known-image.png';
```

The code trace at the reviewed revision is:

1. `os64_dom_image_event` acquires the image with `pixels=false`.
2. Today that starts/waits for the native request. The poll does not report
   the load until that request is ready.
3. Under Q1's proposed shortcut, the display request instead makes the poll
   report the load. There need not be a native request yet.
4. The handler asks for pixels. Under Q2 it starts that native request and
   receives PENDING.
5. `libdom/canvas_image.c`, `member`, returns `JS_UNDEFINED` before calling
   `canvas_draw_image`. Nothing is drawn or queued for later drawing.
6. The event poll has marked `image_reported`. Native arrival does not rerun
   the handler, and repainting the retained Canvas cannot invent its pixels.

The example can therefore work before the metadata optimization and remain
blank afterward. That is a regression introduced by the proposed change, not
just an existing limitation observed more often. This conclusion is from
the control flow; the example is a required regression fixture, not a test
run claimed by this response.

The [Canvas image-source contract](https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument)
rejects an image that is not fully decodable. A completed, valid image does
not become an unfinished image merely because the implementation chose to
discard or postpone a representation. The element's image is the source.

**Disposition:** retain the requirement that an already loaded valid source
can be drawn during the call. Keep the proposed PENDING counter for diagnosis,
but do not use it to authorize silently dropping successful-image draws.
Measure performance after preserving the operation's result.

This does not defend my original choice to retain a second encoded body for
every wrapped image. A shared request with recoverable content, reuse of an
appropriate existing pixel representation, or another bounded materialization
strategy can be considered. The ownership should move upstream with the fix.
We still need to settle how a reduced display source supplies the required
Canvas pixels, including after an HTTP-cache clear or a changed response at
the same URL.

Do not repair the example by replaying a missed draw later. Canvas state,
clipping, later draws and immediate readback can all have changed. The first
acceptance test should draw once in `onload`, read back the expected pixels
immediately, and never draw the image again. Add a cache-cleared case and a
source-change-during-fetch case. A looping animation alone will hide the defect.

## 3. Reuse stylesheet activation, retain a real preload handoff

Q6 is right that the `rel` mutation already reaches stylesheet discovery and
activation. I0 should reuse that path. It should not invent a second way to
apply CSS or duplicate the existing task queue.

I disagree with discarding the preload product and using only the HTTP cache
as the handoff. HTML defines a document preload map separately from the HTTP
cache. Consumption checks request identity and integrity compatibility, and
can wait for an in-flight response. It also preserves failures. In particular,
discarding a preload's integrity failure can let a later consumer retry without
that protection; the specification explicitly explains this consequence.
See [consuming a preloaded resource](https://html.spec.whatwg.org/multipage/links.html#consume-a-preloaded-resource).

Therefore "cache disabled means fetch again, which is a correct browser"
does not establish correctness for the proposed implementation. Document
preload ownership can outlive the decision not to store an HTTP-cache entry.
An ordinary HTTP-cache lookup does not represent a retained preload failure.

**Disposition:** simplify I0 around existing activation and event facilities,
but retain bounded document-owned handoff state. It need not duplicate the
response bytes or become a general new cache. It must retain enough identity,
response/failure and lifetime information to implement the style-preload
contract. Transfer ownership where possible and charge retained storage.

The cache-disabled fixture is intentional. It should demonstrate that a
compatible preload can be consumed without depending on a persistent cache
entry. Test failure handoff and incompatible consumers as well as the
Microsoft pattern. Unsupported integrity behavior needs an explicit supported
profile or refusal, not silent success.

The modulepreload comparison is useful, but a shared destination field does
not by itself unify module-map population, module processing and stylesheet
activation. Share genuinely common fetch/event machinery after inspecting it;
do not expand I0 into the modulepreload debt without a demonstrated dependency.

## 4. Accepted additions and corrected boundaries

### Display request mode and response evidence

Accept finding 3: the selected request must include the element's mode, and
`crossorigin` must govern display acquisition too. A failed CORS request must
not be replaced by a successful no-cors display request. The current display
path's unconditional mode 0 is a real gap.

Accept finding 2 as the likely consolidation direction: fetching display images
through the native image fetch machinery would provide response headers and a
proper response type. I1 can still be the small cache-pointer fix with its
request audit. Measure legacy-entry misses rather than promise first-request
reuse. If request-mode correctness makes consolidation necessary in I2, do it
there; do not defer a correctness dependency behind a performance measurement.

For metadata, no-cors display and native requests should not be artificially
separated just because their internal mode numbers differ. For pixel loans,
the actual response/security evidence still matters. In particular, do not
derive origin cleanliness solely from the final URL as suggested in finding 2:
`libway/origin.c` keeps a no-cors redirect chain opaque after it leaves the
origin, even if its final URL returns to that origin. Use the response's
recorded taint/type, as the proposed shared fetch path permits.

### Responsive selection and density

Accept Q3's insistence that the display fetch and provider consume the same
selection, and that density correction reaches layout as well as getters.
Both are already requirements in I2, but the implementation interfaces and
tests should make them explicit. Verify the proposed WPT filenames against
the pinned checkout before recording expectations.

### Automatic sizes

Agree that automatic sizing should not be partially implemented by an
unreviewed layout shortcut. Finding 5's fallback description needs correction.
If `auto` cannot be used, parsing continues through the remaining list;
`sizes="auto, 400px"` can resolve to 400px. Only an exhausted list falls back
to `100vw`. See [parsing sizes](https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute).

Also, eligibility is defined by the element's attributes; the absence of our
native lazy-loading scheduler does not itself make valid markup invalid.
The [auto-sizes eligibility definition](https://html.spec.whatwg.org/multipage/embedded-content.html#allows-auto-sizes)
is the reference. If automatic sizing is deferred, describe that as our
implementation limit and retain author-supplied fallback sizes. Do not claim
the standard forbids the input because we do not implement it.

### Polling and comments

Accept the event-poll measurement. A per-document pending-image list is a
reasonable later response if the scan is material; it need not accompany the
metadata lookup change without evidence.

Accept both comment corrections in their corresponding implementation commits:
the retained-size comment beside `PICTURES_KEPT_MAX`, and the claim that
`context_picture_address` already includes a `srcset` choice. Name the mistaken
claims in the commit messages. Preserve existing diagnostic line formats when
adding counters so their readers do not silently break.

## 5. Eviction is a possible reuse of machinery, not an existing operation

Finding 7 usefully separates display eviction from native-image eviction.
The design should do that too. Existing reduction, reacquisition and arrival
handling are candidates to reuse for display restoration.

However, the current `pictures_judge` requires positive `need_w` and `need_h`.
It neither evicts a zero-need picture nor records a separate evicted state.
The worker also interprets a zero cover as retaining the natural size.
"Resample to none" names a possible extension, not a supported operation.

Define idle before adding it: the need walk considers layout uses, not just
viewport visibility, so scrolling a picture away does not make its need zero.
The extension needs ownership/accounting, restoration scheduling and a policy
for a source which changed or disappeared. Re-fetching can also block offline
restoration. Reuse the existing machinery where it fits, but measure and design
those consequences rather than assuming its current grow path settles them.

**Disposition:** retain a conditional display-eviction follow-up separately
from native decoded-image eviction. Do not commit to a new cache architecture
in advance. Do not promise arbitrary re-fetching is equivalent to preserving
the image an existing request already loaded.

## 6. Keep the tested delivery identifiable

Do not adopt finding 9's removal of the frozen bundle and manifest. A source
commit identifies source; it does not identify the exact mixture of deployed
executables, shared libraries and fixtures. A small frozen cohort with hashes
makes the hardware comparison reproducible while allowing unchanged built
files to be reused. Recording the source commit is useful in addition.

## 7. Proposed revision

Revise I3 around metadata from the existing selected request, with no duplicate
request for that metadata population. Add display request-mode correctness,
the paired-entry measurements, polling costs, and the NOT_KEPT tests. Reuse
existing stylesheet activation in I0. Clarify auto-size fallbacks and separate
the conditional display/native eviction work.

Two decisions remain before implementation: how a loaded display image supplies
Canvas pixels without losing a one-shot draw, and the smallest bounded preload
handoff that preserves identity and failure state. Neither decision requires
keeping my first representation design unchanged. Both require preserving the
observable operation while making the implementation smaller.
