# Canvas 2D design

Draft, 2026-10-29. Baseline: userland `ec337800`, after SVG Part 1 and the
0f event-interface work. Chris selected Danlegt's signature wall/editor and
scratch-off game as acceptance targets, with animation required and cursor
effects a desirable extension. Build genuine Canvas 2D behavior in bounded
increments, extracting reusable graphics operations from SVG as needed.

The next implementation work follows the dependency map below. This design
does not promise that drawing rectangles alone makes the live site work.
WebGL, OffscreenCanvas workers and video playback are separate features.

Acceptance update, 2026-11-01: use the
[HTML5 Canvas examples](https://curran.github.io/HTML5Examples/) for the next
real-page checks, including animation at different window sizes. Danlegt is
retained as diagnostic evidence rather than a release gate: its normal game
startup reaches missing observer and media APIs before installing the drawing
handlers. See [the captured-page checkpoint](../../canvas-evidence/c6-target-checkpoint.md).

## Acceptance targets and existing foundations

[Danlegt](https://danlegt.com/) uses Canvas 2D. The
[signature script](https://danlegt.com/static/js/signature_wall.js) creates
separate wall and drawing contexts. The editor stamps small rectangles,
erases pixels and saves/restores ImageData for undo/redo and draft storage.
The wall transforms and composites loaded images, draws frames and shadows,
and schedules chunks with requestAnimationFrame. Export uses toDataURL with
PNG. Pointer input and capture are needed for dragging.

The scratch modules use pixel reads to measure the revealed area and
destination-out compositing to erase the coating. Their shared
[pointer handler](https://danlegt.com/static/js/scratch2000/scratch-pointer.js)
uses pointer identity and capture. Ticket rendering also exercises text,
strokes, clipping, transforms and canvas-to-canvas copies. Rendering a ticket
and completing its interactive reveal are distinct acceptance checks. Live
API and script changes must be separated from renderer regressions using
captured local fixtures.
The [initial script inventory](../../canvas-evidence/danlegt-inventory.json)
records source hashes and named context members; it is a static scan, not an
execution trace or a complete site-support inventory.

| Existing component | Reuse and limits |
|---|---|
| HTMLCanvasElement in libdom | Prototype and width/height reflection exist in `elements.c`; context creation and bitmap reset behavior remain to be implemented. Older debt prose predates these reflections. |
| libflow replaced-canvas layout | Preserve bitmap dimensions versus CSS display size and scripting-off fallback content. |
| FreeType geometry backend | Reuse numeric outlines, coverage and stroke primitives; preserve owner accounting and bounds. |
| libsvg | Reuse numeric gradient sampling and extract dash cutting, image sampling and compositing operations. Geometry construction, stroking and rasterization already belong to the FreeType backend. Keep SVG scene/reference semantics in SVG. |
| Yonder image loading and drawing | Reuse decoded-image ownership, scaling, clipping and damage where their contracts fit; extend source access for detached image/canvas callers. |
| Yonder ticker and script turns | Host shared rendering opportunities and repaint scheduling. requestAnimationFrame is not implemented at this baseline. |
| DOM event dispatch | PE1 supplies mouse-backed PointerEvent dispatch and capture alongside compatibility mouse events; exposure follows the connected native route. |

## Dependency order

Chris's scheduling decision (2026-10-29): with one builder available, work
sequentially: C1–C5, then 5a–5d, then a short pointer-events design and its
implementation slice, then C6. Revisit parallel work if another builder
becomes available. Names on roadmap rows indicate the usual builder, not
exclusive ownership. Coordinate changes that are actively underway and
preserve reviewed contracts.

The slice 5 plan in [MODERN_WEB.md](MODERN_WEB.md) puts incremental relayout
before requestAnimationFrame. Retain that order: Canvas-only painting must
avoid layout, but arbitrary frame callbacks can mutate the DOM and still
trigger expensive whole-page work. A native bitmap demonstration cannot
establish that the browser is ready to expose the frame API generally.

Opus's custom-elements plan supplies `os64_html_observe`, the shared internal
mutation callback. The later 5a change log consumes that record. JavaScript
MutationObserver moves to the custom-elements slice; 5a keeps the change log
and timing instrument. These are three distinct contracts. Canvas needs
reliable dimension/lifetime mutation handling, including a same-value width
assignment; it does not inherently require the JavaScript observer API.

The custom-elements draft specifies one listener per document. Canvas must
consume the binding's C-level dispatch rather than replace that listener.
Its record includes borrowed old values and attribute namespace/local name;
a same-value assignment reports an event without moving the document version.
Reset handling must be observable before script next draws or reads the
canvas, not delayed until the mutation-observer microtask. The callback must
not reenter JavaScript or mutate the document. Install the shared dispatcher
at runtime creation when `modern` is enabled, with feature rows registering
C-level consumers; do not tie Canvas correctness to a custom-elements
registration having occurred.

| Dependency | Reason and next action |
|---|---|
| Internal mutation callback | Review the custom-elements contract and integrate the common hook for dimension changes, resets and lifetime bookkeeping. Do not create a competing document observer mechanism. |
| 5a timing instrument and change log | Establish rebuild costs and the shared record before optimizing layout. Coordinate with the custom-elements callback implementation. |
| 5b restyle and 5c layout reuse | Carry out the existing design and equivalence gates before enabling general frame callbacks. |
| 5d requestAnimationFrame | Implement on the browser rendering step and ticker; Canvas contributes paint damage to that same step. |
| Pointer events and capture | Add real mouse-backed pointer dispatch, capture/release and cancellation behavior, with compatibility mouse-event tests. Needed for site drawing and scratching. |
| Additional site APIs | Replay the captured target and classify missing APIs before expanding the dependency list. MutationObserver is a dependency if the executed target requires it, not merely because Canvas is present. |

C1–C5 do not depend on slice 5 or pointer events. Start with C1's native
graphics core. C2 must integrate the shared mutation hook for synchronous
dimension resets and implement the repaint-by-damage contract below; neither
requires the 5a change log. Coordinate that hook with custom-elements cut a
rather than adding a second listener. Start 5a after cut a has merged because
both edit the mutation core; keep 5a–5d in order. C6 begins once C5, 5d and
pointer events are complete. This is a dependency and staffing plan, not an
estimate that the independent work takes equal time.

## Shared graphics architecture

Introduce a small C `libdraw2d` for numeric paint operations, extracted from
existing code as consumers need them. Geometry construction, stroke expansion
and rasterization stay in the existing FreeType geometry backend. `libsvg`
keeps markup path parsing,
scene preparation, instances, paint-server lookup and SVG-specific bounds.
`libcanvas` owns persistent pixels and Canvas drawing state. Both call the
same geometry backend and shared paint implementation; neither fabricates
the other's DOM or scene to issue ordinary drawing commands.

Extraction gates preserve the existing SVG reference pixels, failure modes,
allocation accounting and cancellation tests. Move one reusable operation
with its tests and switch SVG to it before adding the Canvas caller. Prefer
that over duplicating the implementation or renaming the whole SVG library.
Keep SVG's public adapters while migrating their internals.

The initial shared operations are surface validation, clipped pixel blending
and coverage application. The paint boundary describes a surface (dimensions,
stride, capacity and pixel representation), a source (solid color in C1;
gradient/image samplers as their callers land), and a composite operation
(operator and opacity in C1; clip masks with C3). Keep dash cutting shared
above the geometry backend. Follow with gradient and transformed image
sampling. C6 extracts Yonder's shadow blur with its existing pixel tests;
SVG has no blur implementation to reuse. Endpoint-arc syntax and
SVG presentation attributes remain adapter concerns. Canvas arc construction
and transform timing require their own normalization before shared geometry.

Use explicit dimensions, stride, capacity, alpha convention and color format
in surface descriptors. Preserve SVG's tested higher-precision compositing
scratch and define conversions at boundaries. Canvas starts with an sRGB
unorm8-facing profile. C1 retains premultiplied `0xAARRGGBB`, rounds each
completed draw to eight bits, and tests repeated blends against independent
source-over equations. C2 converts straight RGBA at the ImageData boundary;
SVG retains its 16-bit intermediate layers and straight-alpha output.
Do not silently accept an unimplemented format or invent implicit conversion.

## Persistent state and browser integration

A canvas bitmap and its context belong to the element's runtime state, not
the derived layout tree. Relayout, scrolling and CSS resizing retain its
pixels. Detached canvases remain usable while reachable. Resizing the bitmap,
navigation, context teardown and garbage collection have distinct lifetime
rules; retained contexts must not point into freed page or layout storage.
Bindings expose one stable context identity and trace their references. C2
attaches native ownership to the libdom wrapper entry holding the node; the
painter obtains a borrowed bitmap through that binding lookup. Do not add a
second node-address registry in libcanvas.

Canvas drawing is immediately observable to subsequent script reads. Work may
be batched internally only if every read, copy and export flushes preceding
operations with identical ordering. Begin with synchronous native operations
and coalesced presentation; do not retain an unbounded command history.

### Repaint-by-damage contract (C2 requirement)

Pixel changes accumulate bounded bitmap-local damage without moving the
document's structural/style version. A native notification queues presentation
work without executing JavaScript or creating a frame clock. At the browser's
rendering step, map that damage through the canvas's current layout boxes into
page coordinates, respecting CSS scaling, clipping and visual effects. Union
it with other pending paint damage; use a conservative enclosing region when
precise bounds are unavailable. Detached or hidden canvases retain their
pixels and need no immediate visible repaint; becoming visible paints their
current contents.

The rendering step must support repainting the shown tree clipped to that
damage, including background and overlapping content, without running layout
when only pixels changed. If DOM, style, viewport or other layout invalidation
is also pending, perform the required update first, then map bitmap damage
using the resulting boxes and include the layout's damage. Bitmap dimension
changes also invalidate intrinsic sizing where necessary. Do not clear pending
damage on a failed paint or discard changes queued after the paint's snapshot.

Coalesce drawing operations into presentation work rather than scheduling a
wakeup per draw. Script reads still see synchronous bitmap changes before any
presentation. Slice 5d uses this same rendering step after its frame callbacks
and required microtask checkpoints; it does not add a separate Canvas loop.
A repaint must not execute the drawing script again. Dark-page presentation
must not rewrite the bitmap that getImageData reads. C2's exit tests must prove
pixel-only painting does not run layout, simultaneous layout and pixel changes
use fresh boxes, and repainting restores overlapping content correctly.

The state stack stores transform, styles, clipping and compositing state.
Keep the current default path and pixels separate from that stack. Handle
default-path transform timing separately from Path2D, so commands drawn under
different transforms are not transformed twice. Attribute resets and invalid
arguments follow the [HTML Canvas contracts](https://html.spec.whatwg.org/multipage/canvas.html),
with focused browser/WPT cases preceding implementation of each family.

## Images and video reuse

Define an image-source adapter that obtains a stable pixel view or an owned
snapshot for the duration of a draw. It reports dimensions, format, alpha,
color information and origin-clean state; it does not expose decoder-private
objects. Handle self-draw overlap with a snapshot or proven overlap-safe
operation. Image readiness and failure are checked before sampling.

Decoded images, another canvas, rasterized SVG and a future decoded video
frame can use this boundary. Video decoders may produce planar YUV and their
own retained frame storage: do not force their ownership or native format to
be a Canvas ARGB allocation. An adapter can convert/upload a view when a
consumer needs it. Initial implementation supports the formats actually
used by the browser, while preserving explicit format negotiation.

Canvas drawImage can consume the video frame at the current playback
position under the [HTML image-source rules](https://html.spec.whatwg.org/multipage/canvas.html#image-sources-for-2d-rendering-contexts).
That future caller shares scaling, clipping, blending, damage and
presentation. Media decoding, streaming, audio and playback synchronization
remain in media code. The media clock supplies frame deadlines; it is not
replaced by Canvas's animation timestamps. The browser scheduler coordinates
presentation without giving each consumer a separate repaint loop.

Origin-clean propagation is part of image integration. Pixel readback and
export must honor the [Canvas security rules](https://html.spec.whatwg.org/multipage/canvas.html#security-with-canvas-elements)
before external image sources are admitted. Local tests cover same-origin,
CORS-authorized and tainted sources, including propagation through another
canvas and reset behavior.

## Canvas implementation slices

| Slice | Delivered behavior | Exit evidence |
|---|---|---|
| C1 shared surfaces and context core | Extract shared blend/coverage primitives; bounded bitmap and context lifetime, resets, solid rectangles, clear and state foundation | Literal pixel tests, SVG equivalence, failure sweeps, bounds/stride guards and same-value reset cases |
| C2 browser connection and pixels | getContext, painting, ImageData construction/read/write, dirty repaint, detach/reattach and undo/redo fixture | Browser bindings, read-after-write, no layout for pixel-only changes; first visible gallery |
| C3 paths and compositing | Shared path/stroke/dash helpers, transforms, clipping and the composite modes needed for scratching | Mixed-transform paths, clip stack, destination-out reveal and independent blend oracles |
| C4 image sources and export | drawImage overloads, source snapshots, scaling/smoothing, origin-clean enforcement, PNG encoder and data-URL route | Self-copy, crop/scale, decoded image and SVG sources, taint and encode/decode round trips |
| C5 gradients and text | Shared samplers, live gradient objects; shared font selection/shaping and usable metrics/outlines for text | Gradient mutation across saved states, transformed text, metrics/fallback tests; reuse ready for later SVG text |
| C6 target fidelity and motion | Shadows and other required target behavior; connect the shared frame and pointer APIs; captured signature and scratch flows | Signature draw/erase/undo/redo, ticket render/reveal, scheduled frames, navigation teardown and P5 measurements |

This sequence is a dependency outline, not six fixed-size PRs. Split a slice
when ownership or proof requires it. Cursor effects join acceptance after
choosing the active effect and inventorying its calls; optional effects must
not silently become a promise of WebGL or complete Canvas support.

## Budgets and failure handling

Charge retained bitmaps, state stacks, paths, masks, snapshots and temporary
conversion surfaces to explicit owner ledgers. Bound dimensions, pixel count,
stack depth, path commands, scratch memory and operation work. Select initial
caps from the target's measured sizes and concurrent canvases before freezing
the API; include high-DPI pages and self-copy peaks in that measurement.

Prepare fallible allocations/geometry before modifying a persistent bitmap,
or render into bounded scratch and commit. A failed operation must not expose
a half-written result as a successful draw. Keep invalid JavaScript arguments,
unsupported features, context loss and resource exhaustion distinct; translate
native statuses through the web API's rules and record useful diagnostics.
ImageData objects and their ArrayBuffers use the script engine's memory
ledger; native conversion scratch uses the Canvas owner ledger. Export
allocations are charged to their owning subsystem too. Do not publish unsupported
methods as success-shaped no-ops to make feature detection pass.

## Verification and completion

Use the pinned WPT Canvas suites and independent pixel fixtures, plus browser
reference captures where antialiasing requires a measured tolerance. Maintain
separate exact-color, coverage-edge and text assertions. Include long-running
draw/clear, resize, navigation and context-retention tests under ASan, UBSan
and LeakSanitizer. SVG's existing regression gates accompany shared-code
extractions.

Replay the real target with deterministic local data and capture expected
dependencies. Verify the live page separately. A local fixture with matching
calls is not evidence that the site's initialization, loading and events work.
Exercise signature export locally; automated testing does not submit a real
signature to the public wall.

Completion requires the signature wall/editor and scratch-off game to work
on the P5 with scheduled drawing and pointer interaction. Record frame cost,
layout count, dirty pixel area and memory peaks. A bitmap-only animation must
not cause whole-document layout, and a covered/idle window must not accumulate
unbounded frame work. Preserve the existing small frozen-bundle handoff for
hardware tests. Video playback and a particular external game's broader API
requirements are outside this Canvas acceptance claim.

## C1 native foundation

`libdraw2d` now supplies SVG's shared 16-bit multiply, coverage, scaling and
source-over arithmetic, surface validation and bounded axis-aligned rectangle
painting. `libcanvas` owns a stable native handle, premultiplied bitmap,
fill/global-alpha state, save stack, synchronous fill/clear and reset. Same-size
resets clear pixels and saved state while reusing the allocation. Native reset
failure preserves the old bitmap and returns an explicit error; C2 must map
that outcome to browser resize/context behavior rather than silently report
success. Neither library exposes DOM interfaces or schedules frames.

The default policy admits dimensions through 16384, up to eight million
pixels per bitmap/draw, 256 saved states and 96 MiB of native allocations per
context including resize peaks. Callers may lower these literal limits; their
shared owner ledger also enforces aggregate page usage. These are bounded
native defaults, not measured promises about the live targets. Before browser
integration freezes policy, C2 must measure concurrent target bitmaps and
coordinate the page-wide budget. The unclipped source-over rectangle fast path
and pixel transfers allocate nothing after admission; path drawing uses scratch
charged to the same owner ledger. A reset prepares replacement storage before clearing or releasing retained
state. Read-only pixel views expire on successful reset or destruction.

`tools/test_canvas_core.sh` checks literal pixels, fractional/negative
rectangles, ignored nonfinite inputs, state/pixel separation, same-size resets,
zero-sized bitmaps, row padding/capacity, allocation refusals, reentrancy guards
and repeated blending under ASan/UBSan/LeakSanitizer. SVG's native painter
suite and before/after fixture comparison gate the arithmetic extraction.
C2 adds the JavaScript contracts, ImageData and the first visible gallery.

C1 adds `libdraw2d.so` and `libcanvas.so` to both filesystem image manifests.
The library placement allocator assigns `libdraw2d` the former `libpage` slot
and moves `libpage`; a partial installation must therefore include the rebuilt
`libpage.so` alongside the new paint library and updated SVG library. C2's
frozen delivery must check the complete dependency cohort and prelink ranges,
including libraries whose source code did not change.

## C2 pixel transfer

The native pixel interface reads straight RGBA8 into caller-owned storage and
writes source-coordinate dirty rectangles by replacement, ignoring drawing
state. Out-of-bitmap reads produce transparent black. Admission checks stride,
capacity, overlap with the retained bitmap and work bounds before modifying
output. Transfers allocate no native scratch; the caller owns readback storage.

The ImageData binding keeps a traced Uint8ClampedArray in engine memory,
retains a supplied array's identity and exposes read-only dimensions. Pixel
consumers reacquire its byte view after converting arguments so a script that
detaches the buffer during conversion cannot leave a stale pointer. The shared
structured-clone implementation serializes its array as a graph child, keeping
aliases with other references to that same array and buffer. This profile
supports sRGB RGBA8; requests for the other recognized color/pixel formats are
refused explicitly rather than labeled as supported.

## C2 browser and presentation implementation

C2 integrates custom-elements cut a (#403) through `libdom/mutation.c`.
The binding installs the document listener at creation with Modern Web on,
removes it at drain, and owns an embedded list of C consumers. Canvas's
consumer marks a retained element entry for reset on a width/height record
in no namespace. It allocates nothing and enters no script. Drawing, state
reads and host bitmap access synchronize that reset after argument conversion;
a script getter can therefore resize the canvas without leaving a stale
pixel pointer in the operation. No additional document listener is installed.
Custom-elements consumers can join this dispatcher through
`d_mutation_register` when their binding cut lands.

`DValue` owns the bitmap and the retained context identity. Detached canvases
remain drawable; moving or relaying out their boxes does not reset pixels.
A 128 MiB page bitmap ledger includes allocation headers and resize peaks;
each context retains C1's dimension, pixel, work and save-stack caps. The
allocator preserves max_align_t alignment. A failed resize destroys the old
bitmap, so old-size pixels cannot masquerade as a successful DOM resize.
The context identity remains, `isContextLost()` reports the loss, and a later
dimension assignment can restore a blank context. Event delivery for loss and
restoration is explicitly recorded in DEBTS.md.

The C2 profile comprises `getContext("2d")`, `canvas`, solid
`fillStyle`, `globalAlpha`, `save`, `restore`, `reset`, `fillRect`, `clearRect`,
`createImageData`, `getImageData`, `putImageData` and `isContextLost`.
ImageData retains its Uint8ClampedArray identity and participates in
structured clone, including shared cloned references and buffer transfer.
Pixel methods use EnforceRange long coordinates, normalize negative extents,
reacquire byte views after conversions, ignore drawing state and reject
detached storage. CSS colors use libgarb; currentColor uses the existing
computed-style provider. Source alpha remains unquantized in drawing state
for serialization and save/restore, while retained pixels remain ARGB8.
Creation profiles and later methods are tracked in DEBTS.md and the slice
schedule; a visible gallery is not a claim that arbitrary canvas games work.

`dom/canvas.h` lends an existing bitmap to the host without entering script.
Yonder's scaler accepts explicit source stride, capacity and alpha convention;
its existing straight-alpha image behavior remains under the painter suite,
and Canvas uses the premultiplied path. Object-fit, content clipping, CSS
transforms and opacity use the existing painter routes. Dark-page presentation
does not alter the retained bitmap.

After a script turn, the DOM damage visitor reports connected canvases needing
presentation without applying resets or acknowledging their damage. Yonder maps
plain-canvas pixel damage through the painter's shared content/object-fit
geometry, including scroll and a filtering fringe, into the existing paint
queue. Pending resets and ancestor transforms, opacity or animated groups use
a conservative viewport. The document version does not move for pixel writes,
so `script_rebuild` preserves the layout tree. When a DOM change also requires
layout, it publishes fresh boxes first. Detached and unlaid canvases do not
schedule visible damage; their pixels remain available when they become
visible. Successful synchronous canvas sampling acknowledges its damage without
an intervening script call; refused sampling leaves it pending.

Solid fills, strokes and clears whose operator preserves zero-coverage pixels
rasterize into conservative bounds from the expanded geometry, with the shared
SVG raster fringe. Their clipped destination sub-surface uses the original
bitmap stride; clip coverage is copied only for that region. Persistent clip
creation, gradient/image sources, operators affecting pixels outside the shape,
and backends without bounds retain the complete target. The whole-bitmap draw
admission limit remains unchanged. Allocation/backend failure publishes neither
pixels nor damage.

The gallery is `tools/pages/canvas-pixels.html`. Its four interactions exercise
palette repaint, ImageData undo/redo, same-value width reset, and moving one
retained canvas between containers. Host tests separately assert that
pixel-only updates keep the same layout tree, overlapping content remains
painted above the canvas, and simultaneous CSS/pixel changes use fresh boxes.
Pinned upstream expectations under `tools/wpt/expected/html/canvas/element/`
record both this profile's passes and the broader families awaiting C3–C6.

## Integration workflow

C1's reusable core is on `userland`. C2 connects Canvas to Yonder and targets
`canvas-integration`, initially based on `userland` after custom-elements
cut a (#403). Subsequent Canvas slices target that integration branch or stack
on an unmerged Canvas slice. Their review merges accumulate there; promotion
to `userland` is a separate reviewed integration step. Incorporate concurrent
Yonder changes explicitly and repeat affected checks before that promotion.


## C3 paths, clips and compositing

`libdraw2d/stroke.c` owns the curve-preserving dash cutter extracted from SVG.
Its callbacks charge the caller's work and allocation ledger and emit centerline
commands plus isolated cap contours. SVG retains its scene/style adapter;
Canvas supplies its own cap geometry and the current pen transform. The shared
cutter has no DOM, scene, FreeType allocator or script dependency. The existing
SVG fixture pixels remain byte-identical after extraction.

The Canvas current path stores points transformed when each command is appended.
It is separate from saved drawing state. Stroke maps those retained points back
through the current transform, constructs the pen and dashes in that user space,
then maps the resulting outline to device space. A singular pen transform draws
nothing. Arc/ellipse construction uses cubic Hermite segments with a device-space
error bound; resource limits produce an explicit refusal. Zero-length segments
are pruned before stroking; zero-width/height rectangles retain their closed-path
join semantics.

Drawing state contains the transform, solid stroke style, line width/cap/join,
miter limit, normalized dash list/offset, composite operator and a reference to
an immutable clip mask. Save/restore retains that mask; a new clip intersects it
without changing saved masks. Reset discards paths, clips and the save stack.
Geometry and coverage are prepared before a draw touches retained pixels. An
allocation or work refusal leaves the prior pixels and clip intact. Appends
reserve their command storage before changing the current path.

`draw2d_paint_mask` composites premultiplied ARGB8/16 through coverage and an
optional clip. The profile includes the Porter–Duff operators, copy, xor and
lighter; clearRect ignores paint, alpha and the selected operator. Operators
whose transparent source changes the destination process the uncovered portion
inside the clip as well. ImageData transfer still ignores drawing state. Canvas
continues using C2's damage/presentation path; C3 adds no frame loop.

Bindings add current-path construction, fill/stroke/clip, strokeRect, transform
operations, setTransform's numeric and dictionary forms, line styles and dashes,
and globalCompositeOperation. Numeric conversions and sequence iteration finish
before acquiring native state, so script callbacks can resize safely. Dash
iteration converts each yielded item before requesting the next and closes the
iterator when conversion throws. Defaults bound paths to 4096 commands, dash
lists to 256 entries after odd-list duplication, and geometry work to 16 Mi units
per operation, within the existing context and page memory ledgers.

This is a bounded profile, not complete Canvas path conformance. Path2D,
roundRect, hit testing, DOMMatrix/getTransform and advanced blend modes remain
outside it. Wide-stroke overlap artifacts and extreme-coordinate refusals are
recorded separately in DEBTS.md. The four-card gallery is
`tools/pages/canvas-paths.html`; evidence and the upstream pass/fail census are
in [C3 validation](../../canvas-evidence/c3-validation.md).


## C4 image sources and PNG export

`canvas_draw_image` accepts a stable read-only surface, its origin-clean bit,
and explicit source/destination rectangles. libcanvas clips source bounds
proportionally, normalizes negative extents, snapshots overlapping storage,
and prepares transformed coverage before painting. libdraw2d samples straight
or premultiplied sRGB pixels into the shared premultiplied compositing path.
Nearest-neighbor and bilinear sampling both clamp to the whole source image;
the saved smoothing-quality hint uses the same bilinear algorithm at each
quality level. The current path is unchanged by image drawing.

libdom exposes the three drawImage overloads for HTML canvas and image elements,
image smoothing state, natural image dimensions, complete/crossOrigin, and
PNG `toDataURL`. Numeric argument conversion completes before source acquisition
or painting, so reentrant script changes are observed. The provider loans
pixels synchronously, does not enter script, and receives a release for every
acquisition, including status-only queries and refusals. Blank canvas sources
and exports can create native pixels without creating a JavaScript context.
Canvas taint propagates through image draws and canvas-to-canvas copies; pixel
readback and export reject tainted pixels. Resetting dimensions or the context
restores a clean bitmap. save/restore does not restore origin cleanliness.

Yonder's bounded picture catalog is keyed by URL and request mode. A native
image fetch uses libway's request, redirect and CORS checks, retaining an opaque
body exclusively for decoding and marking its pixels unclean. The script-fetch
entry still discards opaque bodies. Display-only products cannot supply CORS
proof; a Canvas source may therefore fetch a displayed URL separately. Data
URLs and local fixtures use the same decode path; a network document cannot
request local file pixels. GIF sources use the default frame. SVG sources use
the isolated secure-static renderer, without a fetch resolver or script host.

Image requests begin when their document owns the displayed page's catalog.
Streaming and coming pages report pending instead of retaining catalog entries
outside that lifecycle. Wrapped image elements receive load/error tasks through
both host task dispatch paths. This is a bounded image-source lifecycle, not
full HTML image loading: unwrapped markup handlers, document-load dependencies,
srcset/picture selection and Image.decode are follow-up work. Other Canvas image
source interfaces, including ImageBitmap and video, are not exposed.

libpng's caller-owned writer emits RGBA8 PNG with sRGB and 96-dpi metadata.
Stored DEFLATE blocks give an exact admission size without an encoder workspace;
exports are valid but larger than compressed PNGs. Transparent premultiplied
pixels are unassociated before encoding. The DOM charges temporary PNG/base64
storage to its Canvas ledger, then returns an engine-owned string. Unsupported
MIME types fall back to PNG; quality is not coerced for PNG. Empty or refused
exports return `data:,`; tainted exports throw SecurityError.

Validation and measured limitations are in
[C4 validation](../../canvas-evidence/c4-validation.md). The gallery is
`tools/pages/canvas-images.html`: SVG stamps, nearest/bilinear crop comparison,
overlapping self-copy, and PNG export/decode/draw. Presentation continues through
the existing pixel-damage path. C4 introduces no animation clock.


## C5 implemented profile

Linear, radial and conic gradients are live native handles. Context state and
saved states retain them; adding a stop changes subsequent draws through those
references. Stops are inserted stably, with a literal 256-stop cap and a shared
owner allocation ledger. Gradient coordinates use the transform at painting,
not at creation. Paint preparation is synchronous and immutable while sampled.
No shader callback allocates or enters script.

`libdraw2d` owns the linear/radial numeric samplers, stop interpolation and
circle root solver extracted from SVG. SVG keeps its public function adapters,
its coincident-linear behavior and its tangent-cone extension. Canvas supplies
its own coincident-linear rule and leaves uncovered radial-cone pixels
transparent, including gradients containing only one color. Conic angles use
clockwise turns from the positive X axis. Colors interpolate straight sRGB and
alpha independently, then enter the existing premultiplied compositor.

The font backend's optional outline visitor exports unhinted curves and tight
bounds. `os64_text_run_outline` adds the shared run's glyph positions and fallback
contours. Canvas consumes that independent command stream through
`canvas_draw_contours`, preserving the current path. Placement/maxWidth fitting
changes glyph coordinates; stroke width and paint coordinates still use the
context transform. Font curves do not pass through SVG markup or private scenes.

The script profile includes `font`, `textAlign`, `textBaseline`, `direction`,
`fillText`, `strokeText` and the numeric TextMetrics attributes. The existing CSS
parser validates font shorthand. Absolute sizes, em/rem/ex/ch, percentages,
keyword sizes and supported arithmetic resolve before saving the font; viewport
units are outside this profile. Detached-canvas relative sizes start from 10px.
Font styles are copied with native save/restore/reset. Pixel font assignments
need no computed-style layout; relative inputs request the applicable style.
Yonder supplies font selection through its existing page/system family provider
and text-run layout. Font size is bounded at 1280px, input at 16 KiB, and emitted
contours at 4096 commands per operation. Resource refusal occurs before painting.

TextMetrics owns engine-managed storage and is collectible independently of the
page. Gradient wrappers share the binding's retained-object registry and remain
until binding teardown; native state references are released on restore, reset
and destruction. The DOM record cap and 128 MiB Canvas owner ledger bound that
retention. High-churn gradient construction is recorded in DEBTS.md.

This uses the shared Western text profile, with pair positioning and fallback;
it does not add general complex-script shaping or bidi layout. Direction affects
start/end alignment, with HTML dir inheritance. Non-alphabetic baselines use
metric-based synthesis. Additional text styling, CSS Font Loading, pattern paints,
shadow/filter effects and advanced blending remain outside C5. The shared run
and outline interfaces are usable independently of these browser omissions.

Validation and the test/gallery entry points are recorded in
[the C5 evidence](../../canvas-evidence/c5-validation.md).

## Relayout preparation after C5

Slice 5a provides the native change readers and opt-in phase profiling, with
its implementation contract in [RELAYOUT.md](RELAYOUT.md) and
[validation record](../../relayout-evidence/5a-validation.md). The P5 baseline
passed on 2026-10-29 (corpus 229). Slices 5b and 5c are implemented in
integration and have passed their hardware acceptance; the final 5c comparison
is recorded in [5c-p5-acceptance.md](../../relayout-evidence/5c-p5-acceptance.md).
Slice 5d implements `requestAnimationFrame` and `cancelAnimationFrame` on
Yonder's rendering loop. Canvas pixel damage and DOM edits share the frame's
presentation; SVG, CSS motion and Canvas do not own competing frame loops.
The animated gallery is `tools/pages/canvas-animation.html`, with the contract
and evidence in [5d-animation.md](../../relayout-evidence/5d-animation.md).
[PE1: pointer events and capture](POINTER_EVENTS.md) has passed P5 gallery
acceptance, with C6 target fidelity next. It connects mouse-backed
input, capture, compatibility events and cancellation through the browser
task path. [PE1 evidence](POINTER_EVENTS_PE1.md) records the host checks,
upstream test scope and interactive gallery; this does not yet claim the
captured signature and scratch applications work end to end.

The post-5d [userland reconciliation](../../canvas-evidence/userland-reconciliation.md)
combines the data-picture backport, observers, generated content, caches and
stack/budget changes with the retained Canvas and frame-loop interfaces.

## C6 fidelity work

C6a extracts Yonder's CSS-shadow coverage blur into `libdraw2d`. The shared
operation takes a strided uint16 coverage mask, an integer box radius, an
exterior value and caller-owned row/column scratch. It performs three
separable box blurs without allocation, preserving the painter's rounding and
edge treatment. Storage, scratch aliasing and pixel-budget checks precede
writes. Large radii use an analytical exterior contribution, so work stays
proportional to the mask area. Yonder retains mask geometry, allocation,
clipping, caching and emission. C6b supplies Canvas source alpha, offsets
and compositing through this numeric boundary.

C6b adds saved `shadowColor`, `shadowBlur`, `shadowOffsetX` and `shadowOffsetY`
state and uses the shared blur for fill, stroke, image and text operations.
Source patches retain premultiplied 16-bit alpha before clipping, including
geometry outside the bitmap that can cast a visible shadow. Global alpha is
applied to the source; shadow color alpha multiplies that coverage. Offsets
and blur use device coordinates, independently of the current transform.
The shadow and source are then composited separately through the final clip
with the selected Porter–Duff operator. `clearRect` and `putImageData` bypass
shadows. Scratch allocation and rasterization finish before bitmap writes;
resource refusal leaves pixels and damage unchanged.

The blur is a bounded Gaussian approximation: interpolate adjacent integer
three-box results to match the variance of sigma = shadowBlur / 2. Sigma is
capped at 64 device pixels without changing the saved property. Fractional
offsets use bilinear alpha sampling. Scratch patches are cropped and charged
to the Canvas owner; the final composition scans the bitmap, preserving the
full-clip semantics of destructive operators. It reports actual changed-pixel
bounds to the existing repaint path. This is not an exact Gaussian or a claim
of full Canvas/filter conformance. See [C6b validation](../../canvas-evidence/c6b-validation.md).

C6c implements the retained SVG-string Path2D profile used by the CD-ROM
ticket: empty/string/copy constructors and `fill(path, rule)`, `stroke(path)`
and `clip(path, rule)`. The binding calls libsvg's public parser and lowers
arcs with the destination context's transform and a 1/128-device-pixel budget.
It does not fork the SVG grammar or arc arithmetic. `libdom` acquires a
`libsvg` dependency; the native Canvas painter accepts a borrowed user-space
command stream through `canvas_draw_path`, also sharing its implementation
with text contours. Drawing never replaces the context's current path.

SVG syntax errors retain the complete prefix; memory/work refusal publishes
no path. Retained wrappers are charged to the page ledger and DOM registry,
and released at document teardown. Copies share immutable native geometry.
Path2D builder methods and `addPath` are outside this profile; adding mutation
requires independent geometry or copy-on-write, not mutation of shared copies.
Limits are 64 KiB of input, 4096 parsed/lowered commands and 16,777,216 shared SVG
work units per SVG operation, in addition to the page allocation ledger.
See [C6c validation](../../canvas-evidence/c6c-validation.md).

C6d adds collectible `DOMPointReadOnly` and `DOMPoint` values and SVG
`isPointInFill()`. Points support construction, copying from dictionaries,
coordinate access, JSON output and matrix-dictionary transformation. Native
storage follows engine collection rather than the document registry; the
scratch progress loop creates many temporary points. SVG graphics/geometry
prototype levels exist with either Modern Web setting; their interface names
and the query method are exposed through the Modern Web table.

`svg_path_contains` is the reusable native predicate. It evaluates the retained
SVG path directly, sharing the derivative-extrema solver with geometric bounds
and the endpoint-arc conversion with lowering/bounds. Monotone curve intervals
use bounded bisection for ray crossings, preserving nonzero/even-odd winding,
implicit closure and boundary membership without rasterization or allocation.
The binary64 boundary allowance is 64 epsilons of segment/query magnitude;
this is a numeric classification, not an exact-arithmetic predicate. Context
work/cancellation limits apply and failures clear the result.

The DOM converts the entire point dictionary before the provider resolves
geometry. Yonder obtains current layout/cascade state, uses the SVG shape and
viewport resolvers, and supplies only local geometry and inherited fill-rule.
Fill paint, visibility, opacity, transforms and clipping do not affect this
query. Connected hidden geometry remains testable. Detached elements are
refused because the current cascade has no resolved style for them; missing
style must not silently become initial geometry or fill-rule. Percentage shapes use their
SVG viewport's user dimensions, with the outer rendered content box supplying
its CSS viewport. Percentage geometry without an SVG viewport is refused
rather than assigned a guessed basis. Provider allocations use the page Canvas ledger and are
released before returning. No parsed-path cache retains a mutable DOM node.
The existing SVG limits apply (1 MiB path input, 32768 path commands,
4,000,000 work units per native operation); the script deadline is polled too.

The captured CD-ROM replay now exercises native pointer scratching, all four
sector reveals and the completion callback. Its fixture omits external page
styling and symbol images and performs no remote submission. See
[C6d validation](../../canvas-evidence/c6d-validation.md).

C6e optimizes shared rectangle painting: fully covered spans reuse their
source color, and opaque fills/clears avoid destination reads. Fractional-edge
coverage, blend rounding and work-limit admission retain their contracts.
Captured Bouncing Balls and 2D Wave Simulation frames match the baseline's
pixels and use less drawing time at both tested sizes. The first P5 browsing
pass found fullscreen Bouncing Balls still slow. Its follow-up tabulates large
translucent 8-bit fills, presents native-size images without per-pixel coordinate
division, and replaces equal-cost frame rest with a fixed 10 ms post-paint pause
under the existing DOM deadline. P5 acceptance of the follow-up remains pending;
see [C6e validation](../../canvas-evidence/c6e-validation.md).

Danlegt's wall and selected rainbow cursor render in the broader captured
replay. Its game startup reaches missing observer/media APIs, and its modal
layout still needs native investigation. The isolated signature-editor and
CD-ROM checks remain narrower evidence. The current release checks use
Curran's examples instead; the [target checkpoint](../../canvas-evidence/c6-target-checkpoint.md)
separates these outcomes from full-site support. Acceptance is not a fixed
count of remaining PRs.

See [C6a validation and target checkpoint](../../canvas-evidence/c6a-validation.md).
