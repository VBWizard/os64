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
| libsvg | Reuse numeric gradient sampling and extract useful private stroke, geometry and compositing operations. Keep SVG scene/reference semantics in SVG. |
| Yonder image loading and drawing | Reuse decoded-image ownership, scaling, clipping and damage where their contracts fit; extend source access for detached image/canvas callers. |
| Yonder ticker and script turns | Host shared rendering opportunities and repaint scheduling. requestAnimationFrame is not implemented at this baseline. |
| DOM event dispatch | Mouse events exist. PointerEvent, pointer dispatch and capture are missing; an interface constructor alone would mislead feature detection. |

## Dependency order

Chris's ruling: write the design, then undertake the prerequisites the target
actually needs, including work elsewhere in the browser. Names on roadmap
rows indicate the usual builder, not exclusive ownership. Coordinate changes
that are actively underway and preserve reviewed contracts.

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
when a consumer needs it; do not tie Canvas correctness to a custom-elements
registration having occurred.

| Dependency | Reason and next action |
|---|---|
| Internal mutation callback | Review the custom-elements contract and integrate the common hook for dimension changes, resets and lifetime bookkeeping. Do not create a competing document observer mechanism. |
| 5a timing instrument and change log | Establish rebuild costs and the shared record before optimizing layout. Coordinate with the custom-elements callback implementation. |
| 5b restyle and 5c layout reuse | Carry out the existing design and equivalence gates before enabling general frame callbacks. |
| 5d requestAnimationFrame | Implement on the browser rendering step and ticker; Canvas contributes paint damage to that same step. |
| Pointer events and capture | Add real mouse-backed pointer dispatch, capture/release and cancellation behavior, with compatibility mouse-event tests. Needed for site drawing and scratching. |
| Additional site APIs | Replay the captured target and classify missing APIs before expanding the dependency list. MutationObserver is a dependency if the executed target requires it, not merely because Canvas is present. |

The first prerequisite task is the shared callback foundation, using the
custom-elements record refinements above, followed by the 5a timing instrument
and change log. Foundation
graphics work can be developed independently, but final animated-site
acceptance includes the frame and input dependencies.

## Shared graphics architecture

Introduce a small C `libdraw2d` for numeric drawing operations, extracted from
existing code as consumers need them. `libsvg` keeps markup path parsing,
scene preparation, instances, paint-server lookup and SVG-specific bounds.
`libcanvas` owns persistent pixels and Canvas drawing state. Both call the
same shared geometry/paint implementation; neither fabricates the other's
DOM or scene to issue ordinary drawing commands.

Extraction gates preserve the existing SVG reference pixels, failure modes,
allocation accounting and cancellation tests. Move one reusable operation
with its tests and switch SVG to it before adding the Canvas caller. Prefer
that over duplicating the implementation or renaming the whole SVG library.
Keep SVG's public adapters while migrating their internals.

The initial shared operations are surface validation, clipped pixel blending
and coverage application. Follow with numeric paths, stroke/dash expansion,
gradient sampling and transformed image sampling. Endpoint-arc syntax and
SVG presentation attributes remain adapter concerns. Canvas arc construction
and transform timing require their own normalization before shared geometry.

Use explicit dimensions, stride, capacity, alpha convention and color format
in surface descriptors. Preserve SVG's tested higher-precision compositing
scratch and define conversions at boundaries. Canvas starts with an sRGB
unorm8-facing profile; choose and lock its retained pixel representation in
the first core slice using readback and repeated-blend reference tests.
Do not silently accept an unimplemented format or invent implicit conversion.

## Persistent state and browser integration

A canvas bitmap and its context belong to the element's runtime state, not
the derived layout tree. Relayout, scrolling and CSS resizing retain its
pixels. Detached canvases remain usable while reachable. Resizing the bitmap,
navigation, context teardown and garbage collection have distinct lifetime
rules; retained contexts must not point into freed page or layout storage.
Bindings expose one stable context identity and trace their references.

Canvas drawing is immediately observable to subsequent script reads. Work may
be batched internally only if every read, copy and export flushes preceding
operations with identical ordering. Begin with synchronous native operations
and coalesced presentation; do not retain an unbounded command history.

Pixel changes accumulate bounded paint damage without moving the document's
structural/style version. Yonder paints the surface at the current layout
box, respecting clipping, opacity and CSS scaling. Bitmap dimension changes
also invalidate intrinsic sizing where necessary. A repaint must not execute
the drawing script again. Dark-page presentation must not rewrite the bitmap
that getImageData reads.

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

## Implementation slices after prerequisites

| Slice | Delivered behavior | Exit evidence |
|---|---|---|
| C1 shared surfaces and context core | Extract shared blend/coverage primitives; bounded bitmap and context lifetime, resets, solid rectangles, clear and state foundation | Literal pixel tests, SVG equivalence, failure sweeps, bounds/stride guards and same-value reset cases |
| C2 browser connection and pixels | getContext, painting, ImageData construction/read/write, dirty repaint, detach/reattach and undo/redo fixture | Browser bindings, read-after-write, no layout for pixel-only changes; first visible gallery |
| C3 paths and compositing | Shared path/stroke/dash helpers, transforms, clipping and the composite modes needed for scratching | Mixed-transform paths, clip stack, destination-out reveal and independent blend oracles |
| C4 image sources and export | drawImage overloads, source snapshots, scaling/smoothing, origin-clean enforcement, PNG/data-URL route | Self-copy, crop/scale, decoded image and SVG sources, taint and encode/decode round trips |
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
Readback/export allocations are charged too. Do not publish unsupported
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
