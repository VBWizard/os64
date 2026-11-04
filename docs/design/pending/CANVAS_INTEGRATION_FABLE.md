# Canvas integration — the whole-kit review (Fable, 2026-11-03)

Against `canvas-integration` at `2c2a7518`, read as one diff over the pinned
base `8faadcf5` (263 files; the production half is ~10,000 lines across
libcanvas, libdraw2d, libdom, Yonder, libflow, libgarb, libhtml, libos64,
libfreetype's port, libpng and libsvg). The brief is
[CANVAS_INTEGRATION_REVIEW_BRIEF.md](CANVAS_INTEGRATION_REVIEW_BRIEF.md); the
designs it names were read first, then the code at every seam in its table,
then the host harnesses' shape. `tools/test_canvas_core.sh` was run here:
240,341 assertions, ASan/UBSan/LeakSanitizer, exit 0.

**Verdict: merge it.** The composition is sound. Nothing found violates a
stated contract; every unsupported surface I checked is either refused by
name or sits in DEBTS. The findings below are two performance cliffs the
evidence does not cover, one small lifetime-adjacent cost, and text. None
is a merge gate; 1 and 3 want booking before the next slice starts.

## Findings

**1. Any page with `@font-face` never grafts geometry after a script
mutation.** `layout_font_dependencies` (yonder.c) advances the font epoch
whenever the cascade has a face rule and the DOM version moved. That epoch
is one of the inputs `reuse_source_prepare` compares, so on such a page every
incremental layout refuses with `FLOW_REUSE_INPUTS_CHANGED` and the 5c graft
(the 80% of the chain the 5a baseline pointed at) is off; 5b style copying
still works. 5c-browser.md records the choice as "conservative" and names the
reason (unicode-range selection reads the whole document's characters). It
is not in DEBTS, and the P5 gate pages (danlegt, Wikipedia, the captured
article) are not web-font pages, so `reused` was never measured where this
bites, which is GitHub, Google and anything on Google Fonts.
Fix: `webfonts_chars` already builds a bitmap of the document's characters
once per layout; hash it and advance the epoch only when the hash changes,
not when the version does. A text edit that adds no new character then keeps
the graft; a new character still invalidates, which is the actual
dependency. Ask for one `layout graft` line from a web-font page on the P5
showing `reused > 0` when it lands.

**2. `fillText`/`strokeText` allocate 229 KB per call.** `text_method`
(canvas_text.c) charges `OS64_GEOMETRY_COMMAND_MAX × sizeof(command)` =
4096 × 56 bytes of scratch through `d_canvas_allocate` for every draw, and
frees it after. A HUD that draws fifty strings a frame moves 11 MB a frame
through the first-fit free list, which is the HEAP_BINS symptom in a new
coat. Fix: size the scratch from the run (`view.glyph_count` times a
per-glyph bound, capped at the max) or keep one scratch per binding and
reuse it; `collect` already stops at the limit so the cap is unchanged.

**3. `fillStyle`/`strokeStyle` reads are O(registry) when a gradient is
set.** The getter in `member` (canvas.c) walks every `DValue` in
`dom->values` to find the wrapper whose `canvas_gradient` matches. A real
page retains thousands of wrappers; a script that reads the style back each
frame pays the whole walk. Fix: give `canvas_gradient_t` an opaque user slot
(libcanvas stays DOM-free) and store the held wrapper there at creation.

**4. `pointercancel` reports the last pressure.** `emit` (pointer.c) sets
`pressure` from `last.pressure`, which is 0.5 while a button was held; PE3
wants 0 on pointerup and pointercancel. One line.

**5. `os64_dom_canvas_dirty` has no production reader.** Only its
`yonder_scripts_canvas_dirty` wrapper exists, and nothing calls that. If it
is the host tests' door, say so in `dom/canvas.h`; otherwise drop it.

**6. The merged bytes have had no hardware pass.** The brief says so. The
Streams S2 merge-forward and the Canvas image fetch share `fetch_job.c`'s
`finish` (the `image` flag decides whether an opaque body is kept). The merge
was conflict-free and the combined suites are green, so this is not a gate:
one ordinary P5 browsing pass on the merged build, through `os64get -a`,
before the keyboard slice starts.

## The seams, verified

Each row of the brief's table, with what I checked and found true.

- **Shared drawing.** Canvas and SVG both go through the FreeType geometry
  backend and libdraw2d's premultiplied 16-bit compositor; the SVG fixtures
  stay byte-identical across the extractions (dash cutter, gradient samplers,
  blur). Canvas keeps its own transform timing (points mapped on append,
  stroke maps back through the inverse pen transform, caps built in user
  space with the device map only bounding error). `render` prepares
  geometry, coverage and the clip copy before a pixel is touched; every
  refusal returns through `done:` with pixels, clip and damage unchanged. The
  compact raster box is intersected with the retained clip's bounds and an
  empty clip still rejects everything. The round-tip fallback appends its
  disks to the same outline so overlap gets one coverage pass, charges
  13 points per dot against the point cap and frees its scratch on both
  exits. Shadows: source patch and shadow patch rasterized in the original
  grid before clipping, then composited shadow-then-source through the clip
  with the selected operator, which is the HTML drawing model's order.
- **Ownership and reset.** The element's `DValue` owns the bitmap; the
  context object carries that `DValue` as its opaque. Node wrappers live
  for the binding's life (the one early removal in node.c is a clone
  failure before any context can attach), so the opaque never dangles.
  A width/height mutation marks `canvas_reset_pending` from the shared
  mutation consumer, allocation-free, and every entry point converts its
  arguments before `d_canvas_sync`, so a getter that resizes cannot leave a
  stale pixel pointer. Self-draw syncs the source first, which is the same
  entry, so a pending reset cannot free the borrowed source under the draw.
  A failed reset destroys the bitmap and `isContextLost` says so; a later
  successful dimension assignment restores it. Teardown: `os64_dom_drain`
  stops the mutation listener, drains animation and pointer holds, then
  frees values; `os64_dom_free` runs `d_canvas_free` per entry after the
  engine is gone, and nothing there calls the engine.
- **Image acquisition and origin.** Canvas requests are separate catalog
  entries keyed by URL and mode, decode at natural resolution, keep the
  default GIF frame, and carry `origin_clean` from the fetch verdict
  (opaque → tainted; file pixels clean only for a file page; data: clean).
  A display-only entry can never answer a Canvas acquire, so a cross-origin
  picture already on screen is not proof of readability. Taint propagates
  through canvas-to-canvas draws; a singular transform still taints; read
  and export refuse tainted pixels; reset restores clean. Arrivals and
  replacement pixels advance the resource revision the snapshots compare.
- **Incremental layout.** Styles are copied by value into the new arena
  with lists deep-copied and memoized (`style_copy.inc`); the size tripwire
  on `flow_style_t` is what caught filter and backdrop-filter. Equality
  ignores addresses and compares list contents. The graft stages into
  `FGraftBox` and publishes only on success; fragments remap item and
  inline pointers through the match table and retain text runs by
  reference count, so no old-arena address survives the call. The old
  tree is freed before the old cascade in both the staged rebuild and the
  direct relayout, which is the order the borrowed keyframe records need.
  The cascade's in-place update removes slots for removed nodes before
  rotating its document pin, so an address cannot be reused while a slot
  names it; a failed update sets `update_failed` and every caller rebuilds.
  The shared cascade may run ahead of the displayed tree through a measured
  layout; the per-node `changed` stamp against the tree's recorded
  `cascade_updates` is what keeps the copy honest.
- **Damage and scheduling.** Pixel writes move no version. `script_rebuild`
  lays out first when the version moved, then maps each connected canvas's
  damage through the painter's own content and object-fit geometry into
  the paint queue, with one texel of fringe; a pending reset or any
  filtered, blended, transformed, animated or non-opaque ancestor falls
  back to the viewport. The painter samples the live bitmap and acks
  damage only after it consumed it, so a skipped or covered paint leaves
  damage pending. One loop: an ordinary task, then the due frame batch,
  then one rebuild and one paint; the 20 ms interval counts from batch
  start and clamps to completion, the host adds a 10 ms post-paint gap,
  covered windows keep requests and run nothing, and the idle grant stops
  at the nearest frame or CSS motion deadline.
- **Input lifetime.** One native sample is one packet in one task;
  capture is resolved at delivery against a held snapshot so listeners'
  requests wait for the next pass; navigation or runtime loss inside a
  listener ends the packet before any further default. The click target is
  the nearest common inclusive ancestor of down and up, or the capture
  target when captured, revalidated for connection. Removal of a boundary,
  capture, pending or down target is handled in the mutation consumer
  without script. Drain-until-neutral exists at both levels: the DOM
  controller after a cancel or failure, and the host after navigation and
  input loss, which is what consumes the compositor's grab tail. The frame
  stand-in regression from the PE1 review is closed by the held fallback in
  `asks_perform_with_fallback`.
- **Budgets and failure.** The 128 MiB page ledger covers bitmaps, resize
  peaks, gradients, Path2D, text scratch and export; each context keeps
  C1's caps. Every refusal I traced leaves the object usable: a refused
  stop leaves the gradient, a refused path leaves the prior path, a
  refused draw leaves pixels and clip, a refused export answers `data:,`,
  a refused pointer packet releases holds and drains.

## Not findings, noted

- Shadow composition scans the whole bitmap per shadowed draw. Booked in
  DEBTS; the wall's frames are fine, a shadowed brush would not be.
- `canvas_damage_mark` ignores ancestor clips and ancestor `visibility`.
  Over-invalidation only; the painter clips.
- `webfonts_public` resolves fonts for every distinct style after a copied
  layout with a 64-entry dedup; past 64 it is one resolver call per box.
  `webfonts_chars` is cached per layout so the cost is bounded, but it is
  worth a number from a big page once finding 1 is fixed and the graft
  actually runs there.
- Gradient and Path2D wrappers are uncollectable until teardown. Booked.
- The arc-angle normalization treats `start = 2π, end = 0` as a full
  circle. So does Chrome.
