# Canvas 2D design

Draft, 2026-10-29. Baseline: userland `ec337800`, after SVG Part 1 and the
0f event-interface work. Chris selected Danlegt's signature wall/editor and
scratch-off game as acceptance targets, with animation required and cursor
effects a desirable extension. Build genuine Canvas 2D behavior in bounded
increments, extracting reusable graphics operations from SVG as needed.

The next implementation work follows the dependency map below. This design
does not promise that drawing rectangles alone makes the live site work.
WebGL, OffscreenCanvas workers and video playback are separate features.

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
| DOM event dispatch | Mouse events exist. PointerEvent, pointer dispatch and capture are missing; an interface constructor alone would mislead feature detection. |

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
| C4 image sources and export | drawImage overloads, source snapshots, scaling/smoothing, origin-clean enforcement, PNG encoder and data-URL route (the existing libpng is a decoder) | Self-copy, crop/scale, decoded image and SVG sources, taint and encode/decode round trips |
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
coordinate the page-wide budget. Pixel draws allocate nothing after admission;
reset prepares replacement storage before clearing or releasing retained
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
