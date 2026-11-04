# INTERSECTION_OBSERVER.md — `IntersectionObserver` in yonder

*Opus, 2026-11-03, at Chris's choice after the Streams arc. A design pass
before code: it adds a host verb to libdom and a step to yonder's rendering
loop. IO1 is built (§ 9).*

## 1. Why, measured

- **116 of the corpus's 415 records (28%) meet it, in 206 lines.**
- **The failures take three shapes:**
  - `ReferenceError: 'IntersectionObserver' is not defined` thrown at a
    bundle's or a module's top level, which takes everything that bundle was
    setting up with it. A code-hosting site's repository page dies in its
    app's entry module this way, so its React tree never mounts. Before
    Streams S1 the same page died earlier, on `ReadableStream`.
  - The same throw inside a custom element's constructor or callback (an
    `include-fragment` that loads its content when it scrolls into view).
  - `MISSING global IntersectionObserver`: a feature test that took the
    fallback path, or a polyfill that went looking for it.
- **Who uses it:**
  - wallpaper and picture galleries, for lazy pictures;
  - game portals, for thumbnails and ad slots below the fold;
  - search, retail and design sites;
  - the code-hosting site above;
  - infinite feeds. The page puts an empty sentinel element at the bottom
    and fetches more when the sentinel scrolls into view.
- **Missing, it costs more than lazy loading.** A page that constructs an
  observer throws where it would have armed it, and the work around the
  constructor dies with it.

## 2. The standard

The Intersection Observer specification (W3C), run from HTML's "update the
rendering".

**The interface:**
- `new IntersectionObserver(callback, {root, rootMargin, scrollMargin, threshold})`.
- `observe(target)`, `unobserve(target)`, `disconnect()`, `takeRecords()`.
- The attributes `root`, `rootMargin`, `scrollMargin` and `thresholds`, the
  last a frozen array, sorted.
- Each `IntersectionObserverEntry` carries `time`, `rootBounds`,
  `boundingClientRect`, `intersectionRect`, `isIntersecting`,
  `intersectionRatio` and `target`.

**The processing.** During "update the rendering", after the animation frame
callbacks and after style and layout, "run the update intersection
observations steps" for each document. For each observer, and each of its
targets:
- **The target's rectangle** is its bounding box: the union of its fragments
  (an inline element on two lines is one rectangle around both).
- **The intersection rectangle** is that rectangle clipped by each box
  between the target and the root that clips its overflow (a scroll
  container's scrollport, an `overflow: hidden` box's padding box, CSS
  `clip`, `clip-path`), then met with the root's rectangle grown by
  `rootMargin`. With no root, the root is the top-level viewport; with an
  element root, that element's padding box (its scrollport) when it clips
  its content, else its bounding box.
- **The threshold index** says how many thresholds the visible fraction
  (intersection area over target area) has reached.
- **`isIntersecting`** says whether the target meets the root at all. It is
  true even when the shared area is zero, because edges that touch count.
- **When to report.** The observer reports when either of those two answers
  differs from the one before. The first observation of a target always
  reports, because the previous index starts at -1. The entries are queued,
  and one task delivers each observer's queue to its callback.

**The cases with answers of their own:**
- A target with no box (`display: none`, detached, in another document) is
  not intersecting, with zero rectangles.
- A target outside the root's containing-block chain is not intersecting.
- A zero-area target that touches the root has a ratio of 1.
- `rootMargin` applies only to the implicit root, and to a root in the
  target's own origin.

## 3. What yonder has

- **libdom:**
  - MutationObserver (`observer.c`): an observer kept alive by what it
    observes, and records queued per observer and delivered together. Both
    shapes carry over.
  - The view's events (`view.c`, `os64_dom_view_step`): `resize`, `scroll`
    and media-query changes, one task per step in the rendering step.
  - The geometry provider (`os64_dom_set_geometry`): one element per call,
    each call able to force a layout. It knows nothing of the clips around
    a box.
- **yonder's layout (libflow)** answers everything the arithmetic needs:
  - a box's document rectangle (`flow_box_doc_rect`);
  - the clip it is painted under, met with the clip of every frame around
    it (`flow_box_doc_clip`, which paint already uses);
  - the scroll containers and how far each is scrolled (`flow_scroller`,
    `flow_scroll_at`);
  - transforms (`flow_affine_rect`).
- **The rendering loop.** `requestAnimationFrame` is slice 5d (PR #442),
  which reaches `userland` with `canvas-integration` before IO1 is built.
  A turn of yonder's window loop runs, in order:
  - `script_turn`: one task, then the animation frame batch when one is due
    (`os64_dom_animation_fire`, suppressed while the window is covered),
    then `script_rebuild`, which lays out what the task and the callbacks
    changed;
  - `motion_tick`: a due CSS animation's layout or tick;
  - the other turns (storage, cookies, downloads, pictures);
  - `before_paint`, then `glass_paint`.

  WPT's intersection-observer helper waits on `requestAnimationFrame`, so
  5d is also what lets the oracle judge this.
- **WPT.** `intersection-observer/` holds 119 files at our pin, and is not
  yet in `tools/wpt_fetch.sh`'s list.

## 4. The choice

**A. libdom keeps the observers and does the standard's arithmetic; the host
answers the geometry for all of them at once (my lean).**
- At a step, libdom hands the host every observed (target, root) pair.
- In one walk of the box tree, the host answers for each pair:
  - the target's bounding rectangle;
  - what is left of it after the clips between it and the root;
  - the root's rectangle;
  - whether the target has a box at all, and whether it is in the root's
    chain.

  Everything is in viewport CSS pixels, from the layout the rendering step
  has just made current.
- libdom does the rest: `rootMargin`, the ratio, the thresholds, the
  comparison with last time, the entries and the delivery.
- The split keeps the standard's rules in one place that the DOM host suite
  can drive with a fake geometry, and keeps the layout's knowledge in the
  layout.

**B. libdom asks the existing geometry provider once per target.** This
cannot see clips without a new provider anyway. It also costs a box-tree walk
per target per step, and each call may force a layout. A gallery that
observes 500 thumbnails would walk the tree 500 times a frame.

**C. Script it** (a polyfill: `getBoundingClientRect` on every `scroll`).
This is exactly the cost the standard exists to remove, and it cannot see
clips either.

**My lean is A.** It is a new host verb in libdom, of the same kind as the
geometry and view providers, and not a new mechanism.

## 5. Rules

1. **The arithmetic is libdom's; the geometry is the host's.**
   - One batch a step, answered from the layout the step has made current.
     A batch never forces a second layout.
   - Rectangles are in viewport CSS pixels, the same space
     `getBoundingClientRect` answers in.
2. **It runs right after the animation frame callbacks, on the layout they
   asked for**, the standard's order: animation frames, then style and
   layout, then intersections, then paint.
   - **In yonder's turn** that is after `script_turn` (the batch and the
     `script_rebuild` that follows it), after `motion_tick`'s layout and
     `before_paint`'s catch-up ticks, and right before `glass_paint`. The
     geometry is the layout about to be painted, so an entry describes the
     frame the person sees.
   - **A turn with no animation callbacks runs it too.** A scroll moves
     targets without any script asking for a frame.
   - **While the window is covered it does not run**, as the animation
     frames do not: a covered page has no rendering opportunity. The first
     step after it is uncovered reports whatever changed meanwhile.
   - **The step only queues.** The callbacks run in the delivery task (rule
     4), in a later turn among the other tasks, never inside the frame.
3. **It runs only when something it depends on may have moved.**
   - **What counts:** a new layout, a scroll of the page or of any container,
     a change of the view's size or zoom, an `observe()` since the last
     step, or a DOM change.
   - **Otherwise the step is skipped.** A page that observes a thousand
     thumbnails and is standing still costs nothing.
   - The host owns the "may have moved" test (it knows its layout and scroll
     generations), and libdom owns "an `observe()` is waiting for its first
     report".
4. **One task delivers everything a step queued**: each observer that has
   entries, in creation order, its callback handed its entries and itself,
   as the standard's "notify intersection observers" does. An exception in
   one callback is reported, and the next callback still runs.
5. **Lifetime is MutationObserver's.**
   - An observer is held alive while it observes anything, and a target
     holds no observer once it is unobserved.
   - A target that leaves the document stays observed, and reports "not
     intersecting" once.
   - The page leaving frees everything.
6. **The cost is bounded.**
   - The batch walks the box tree once, against a set of the asked nodes, so
     a step costs O(boxes + targets) and not O(boxes × targets).
   - The number of targets an observer can hold is the binding's registry
     quota, as any page-held object's is.
7. **The switch.** `IntersectionObserver` and `IntersectionObserverEntry` are
   a row in libdom's Modern Web table (`modern.c`), so with `modern` off
   they do not exist.

## 6. Cuts

**IO1, one slice:**
- The whole interface (`observe`, `unobserve`, `disconnect`, `takeRecords`,
  the attributes and their WebIDL conversions), with the frozen thresholds
  array and `rootMargin` in pixels and percentages.
- **Roots:** the implicit root (the viewport), `document`, and an element
  root, including a scroll container.
- **The arithmetic:** the first observation, thresholds, edge-adjacent and
  zero-area targets, `display: none`, and targets out of the root's chain.
- WPT `intersection-observer/` added to the fetch list, with an expectation
  file.
- A test page in `tools/pages/`.

**Booked, not cut:**
- `scrollMargin`'s clipping (a newer option, its own WPT pages; the
  option itself is read and reported, § 9).
- Intersection Observer v2's visibility: `isVisible` stays false, which the
  standard allows (§ 9).
- Roots and targets across frames (yonder's frames are their own
  documents; the implicit root of a frame's document is its own viewport
  until frames share a rendering step).
- `<img loading="lazy">` built on the same step. yonder loads every
  picture now, which is correct, only eager.
- **ResizeObserver.** The same batch, asking for a box's size instead of
  its intersection, and a step of its own that runs script and lays out
  between rounds: built as RO1, § 11.

## 7. Lifecycle

| Event | libdom | yonder |
|---|---|---|
| `new IntersectionObserver(cb, opts)` | options converted (rootMargin parsed, thresholds sorted and checked); the observer listed | — |
| `observe(t)` | t registered; the observer held; a first report owed | — |
| a rendering step, after the animation frame callbacks and the layout they asked for, with something moved or a report owed | the pairs handed over; ratios, thresholds and changes worked out; entries queued | one walk of the box tree answers each pair, then the frame is painted |
| entries queued | one task: each observer's callback, in creation order | — |
| `unobserve(t)` / `disconnect()` | t dropped, or all; the observer let go when it observes nothing | — |
| t removed from the document | still observed; reports not intersecting once | the batch answers "no box" |
| the page leaves | everything freed with the binding | — |

## 8. Proof

- **WPT `intersection-observer/`**, the oracle. Its helper waits on
  `requestAnimationFrame`, which this builds on.
- **The DOM host suite**, against a fake batch provider:
  - every interface member and its conversions;
  - first reports, threshold crossings both ways, `rootMargin` in pixels and
    percentages, zero-area and edge-adjacent targets;
  - the order: an observation made by an animation frame callback (a
    target moved in it) is reported from that frame's layout, and its
    callback runs after the frame, in the delivery task;
  - lifetime: an observer with no other reference still reports, and a
    disconnected one is collected;
  - one task for many observers, and a throwing callback;
  - every allocation failed in turn.
- **The scripted-page host**, against real layout:
  - a tall page whose target below the fold reports when the view scrolls
    to it;
  - an `overflow: auto` container as the root, scrolled;
  - `display: none` toggled;
  - a sentinel at the end of a list that grows the list when it shows.
- **QEMU and the P5**, with a page in `tools/pages/`:
  - a gallery that loads its pictures as they scroll in;
  - an infinite list.
- **The corpus.** The repository page's entry module no longer dies on the
  constructor. Whatever stops it next is named in its record.

## 9. As built (IO1)

**libdom.**
- `intersection.c` holds the observers, their targets (each on its
  observer's list in `observe()` order, and in a bucket by node), and the
  entries, which stay C records until `takeRecords()` or the delivery task
  makes objects of them.
- `os64_dom_intersections_update` is the step. It hands the provider one
  item per (target, root) pair, works the arithmetic, and queues entries.
  `os64_dom_intersections_fire` is the delivery task.
- An observer is held while it has targets or entries waiting. The GC's
  finalizer only marks one dead, and the step and the task sweep the dead
  between walks.
- `rect.c` adds `DOMRectReadOnly` and `DOMRect` (Geometry Interfaces § 4).
  An entry's rectangles are `DOMRectReadOnly`, and `getBoundingClientRect`
  answers a `DOMRect` with the switch on, the old plain object with it off.

**What the browsers answer, where the standard's text differs.** WPT is
the judge, and each of these follows it:
- `isIntersecting` is "the threshold index is above 0" (the standard's
  issue 432). A ratio short of the smallest threshold is not intersecting,
  though `intersectionRect` and the ratio still say what meets.
- A target with no box reports every rectangle zero, `rootBounds` too.
- A clip anywhere on the chain counts toward `isIntersecting`, not only
  the root.
- `rootMargin` percentages are of the root's height on the top and bottom
  and of its width on the sides. They are held to 1/64 px, layout's unit,
  so margins that sum to the whole leave an empty rectangle and not an
  inverted one.
- Absolute lengths are taken to pixels and floored (`10cm` reports
  `377px`).

**The surface goes slightly past § 6's cut:**
- `delay` and `trackVisibility` exist.
- `isVisible` is always false. The standard allows that answer from an
  implementation that cannot guarantee a target is unoccluded.
- `scrollMargin` is parsed, kept and reported, and clips nothing (DEBTS).

**libflow.** Two new doors:
- `flow_clip_parent`: the element whose box clips an element's box (its
  parent in the flow; its containing block out of it, from the anchors the
  style pass already keeps).
- `flow_box_clips`: whether a box cuts its content on an axis, the
  viewport's overflow excepted.

**yonder.**
- **The batch.** `geometry.c`'s `yonder_intersections` answers it:
  - one walk of the boxes, for every asked element's bounds, against a hash
    of the asked elements;
  - then each target's clip chain to its root.
- **The step.** `intersections_turn` runs in the window loop after
  `script_turn`, `motion_tick` and `before_paint`, right before
  `glass_paint`:
  - it runs only when something moved since the last answered batch: a tree
    given to a Page, an animation tick that moved boxes, the page's or a
    box's scroll, the zoom, or the view's size;
  - or when a target waits for its first observation;
  - not while the window is covered.
- **The delivery task** is a task source in `script_turn`.

**Proof.**
- **The DOM host** (`tools/test_dom_intersection.inc`) covers:
  - the interface and its conversions;
  - the arithmetic against a fake batch: thresholds both ways, edges,
    zero area, margins, clips, element roots, foreign roots, refusal,
    `delay`;
  - lifetime;
  - delivery order;
  - every allocation failed in turn.
- **The scripted-page host** (`tools/test_yonder_intersections.inc`) covers
  real layout:
  - scrolling into view;
  - a scroll container as the root;
  - containing blocks;
  - the step after an animation frame;
  - `display: none`;
  - an infinite list;
  - a covered window.
- **WPT `intersection-observer/`:** 171 of 358 pass. Most of the rest are
  other features, each a DEBTS row:
  - element scrolling (every test that scrolls a root by script);
  - SVG geometry;
  - window named access;
  - frames;
  - IntersectionObserver v2's occlusion;
  - the binding-wide WebIDL getter names and enumerable operations.

## 10. Ruled

- **What this builds on** (Chris, 2026-11-03). `canvas-integration`, with
  5d's `requestAnimationFrame`, merges into `userland` first, and IO1 is
  built on `userland` after it. The step runs right after the animation
  frame callbacks (rule 2).

## 11. ResizeObserver (RO1)

*Opus, 2026-11-04, the small slice § 6 booked after IO1.*

**The standard** (Resize Observer 1, run from HTML's "update the
rendering" after the layout and before the intersection steps):
- `new ResizeObserver(callback)`; `observe(target, {box})` with `box` one
  of `content-box` (the default), `border-box`, `device-pixel-content-box`;
  `unobserve`; `disconnect`.
- An observation starts with a last reported size of (-1, -1), so every
  target reports once, a 0 by 0 one included.
- Each rendering step: gather the observations whose observed box size
  differs from the last reported one, deeper than a depth (0 at first);
  broadcast them, each observer's callback handed its entries, in the
  order the page made the observers; lay out again; gather again, only
  deeper than the shallowest target just broadcast; until none is active.
  An observation left over is reported as an ErrorEvent, "ResizeObserver
  loop completed with undelivered notifications."
- An entry carries `target`, `contentRect` (the content box at the padding
  edge's offset), and `borderBoxSize`, `contentBoxSize` and
  `devicePixelContentBoxSize`, each a frozen array of one
  `ResizeObserverSize {inlineSize, blockSize}`.

**How it departs from IO1's step: it runs script inside the rendering
step, and lays out between rounds.** Intersection callbacks are a task
queued after the step; resize callbacks are invoked in the step, so a
callback's change to the page is laid out and measured again before the
paint (that is what the depth rule bounds). So:
- **The provider may lay the page out** (as `getBoundingClientRect`'s
  geometry provider may), where IO1's rule 1 forbids it: the second
  round's sizes must be the layout after the first round's callbacks. The
  work is charged to the step's task.
- **The step is one task** in libdom (`os64_dom_resizes_step`): gather,
  broadcast, gather deeper, as many rounds as the depth rule allows, then
  the loop error if any observation was skipped. yonder runs it in the
  window loop after `script_turn` and `motion_tick` and before the
  intersection step and the paint, when the page's trees, the view's size
  or the zoom moved since the last step, or a report is owed: a target's
  first, one the last step's loop passed over, or what a step left undone
  (a refused gather, a callback whose entries could not be made: the host
  records the layout as measured either way, so the binding keeps it).
- **One step a frame.** A step's own callbacks change the page, and the
  rebuild that follows reads as "the trees moved", so a step allowed on
  every turn would run again on the next one: a callback that resizes its
  own target (the loop error other browsers print once a frame) would
  spin the window loop. After a step, the frame it ran in ends, then the
  gap script frames keep (10 ms), and while the page waits for an
  animation frame the step waits for that frame's callbacks too, since
  the standard runs both in one rendering update, the callbacks first.
  The ticker is offered the moment (`resizes_next`), so an owed report
  arrives without anything else waking the loop.
- **The batch** is the intersection batch's shape: one call, every
  observed target, answered in one walk of the boxes: whether it has a
  box, its border box and content box sizes in CSS pixels, its content
  box in device pixels, and its padding's left and top.

**Lifetime** is IntersectionObserver's: held while it observes something,
marked dead by its finalizer and swept between walks.

**Cut, booked** (DEBTS.md's row, with the WPT pages each costs):
- vertical writing modes (inline and block sizes are width and height);
- SVG elements without a CSS box (their bounding box);
- fragments (multi-column): one size per array;
- sizes in whole device pixels, CSS `zoom`, and scrollbars that take no
  room from the content box: layout's, not the observer's.

**Proof.** WPT `resize-observer/` (added to the fetch list): 74 of 104,
every failure one of the cuts above, shadow DOM, or window named access.
The DOM host against a fake batch (first reports, a 0 by 0 target, each
box option, the depth loop and its error, lifetime, every allocation
failed in turn), and the scripted-page host on real layout (a box resized
by a script and by the window, a callback that resizes a deeper element,
and a callback that resizes its own target into the loop error, one step
a frame).
