# Design brief: a change costs what it changes

*Fable, 2026-11-20, for the Opus on the layout work, in answer to
BRIEF_CASCADE_TRACKING_C3_RESULTS.md. This is the design I owed after
the measurement. Chris's rule is its whole purpose: nothing changes on
that page but the picture, the progress bar, the clock and now and then
a like count, so the page should cost what those cost. Every rule here
is CSS's and layout's; nothing is keyed to a site.*

## What the numbers decided

While playing, 92% of the layouts a script forced moved no box (418
paint only, 38 one node's boxes, of 456), each cost about 75 ms with its
cascade, and each paint was a whole view at about 400 ms. On the settled
feed, 75% of layouts changed nothing at all. Today every layout runs
everything, no read is answered from the tree yonder has, and nothing
repaints without a layout.

So the graft's `role` refusals are not next. **Not doing the work is
next.** Your lean is right and is slice N1; the design goes two steps
past it.

## The idea: three things can be stale, and each asker needs only one

A page's pending changes can leave three things out of date:

| | made current by | who needs it |
|---|---|---|
| **STYLE** | the cascade's update and the restyle | `getComputedStyle`, a geometry read that depends on a style (below), the paint |
| **GEOMETRY** | boxes, placement, the public tree | a geometry read, hit testing, scrolling, the paint |
| **PAINT** | the paint of what is damaged | the glass |

Today any asker makes all of it current. The design: **each asker makes
current only what it reads, and each change marks stale only what it
can change.**

## The classes of a change

A change to one element's computed style is one of:

- **NONE**: nothing came out different.
- **PAINT**: only fields placement never reads.
- **MOVE**: the element's own box moves and nothing else's does (N4).
- **GEOMETRY**: anything else.

The whole design leans on one table being true: **for every field of
the computed style, what reads it** (placement, boxes, the public
tree's geometry, the paint, hit testing). That table is the first thing
back to me (see "Before code").

Care, in the table's own terms; object where the code says otherwise:

- `transform`, `filter`, `perspective` and their kin are PAINT **only
  while the value stays on the same side of `none`**. Going to or from
  `none` makes or unmakes a containing block for fixed and absolute
  descendants: GEOMETRY.
- A `transform` moves a scroller's reach and what
  `getBoundingClientRect` answers. So PAINT does not mean "no reader
  sees it": it means placement need not run. Reach and transformed
  rectangles are recomputed from the kept boxes and the current style.
- `visibility` is PAINT except to or from `collapse`.
- `opacity`, `z-index`, colours, backgrounds, `outline`, shadows,
  `border-color`, `border-radius`, `clip-path`, `mask`,
  `text-decoration`, `object-position`: PAINT. `cursor` and
  `pointer-events`: hit testing only.
- A custom property is never classed at the write: `var()` can reach
  any field. It goes through the restyle, which says what came out
  different.
- An inherited PAINT field (`color`, `visibility`) is PAINT for the
  whole subtree it reaches.

## The slices, each its own PR, in this order

**N1. The restyle's verdict decides the rest** (your lean).
After the cascade's update and the restyle: if no element came out
GEOMETRY (or MOVE, until N4 treats it), keep the boxes, the placement
and the public tree; record the paint damage (N3 uses it; until then,
damage as today). A forced read is then answered from the kept tree.
Saves placement, boxes and public (about 40 ms of each such layout).

**N0, with N1: a write that changes nothing logs nothing.** An attribute
set to the value it has, a class list set to itself, a style property
set to its present value, text set to the same text: no entry in the
layout's change log, no DOM version for layout. (Mutation observers and
custom-element callbacks still hear what DOM says they hear; this is
the layout's log only.) The feed's 122 unforced layouts that changed
nothing: say what asked for them, since N0 should make most never
start.

**N2. A read asks for what it reads; a write says what it can change.**
- The change log's entry for an inline style write carries which
  properties differ. If all are PAINT by the table (same side of
  `none`), the entry is PAINT at the write: it stales STYLE and PAINT,
  never GEOMETRY.
- A geometry read (`client*`, `offset*`, `scroll*`) with only
  PAINT-class entries pending is answered from the kept tree with **no
  cascade update and no restyle**. That is the whole 75 ms, and it is
  the playing page's commonest case (`set transform`, then `get
  clientWidth`).
- `getBoundingClientRect` and any read that sees transforms needs STYLE
  current for the elements it walks (the element and its ancestors),
  not GEOMETRY: restyle, no placement.
- `getComputedStyle` needs STYLE, never GEOMETRY, except for the
  properties whose used value is a layout answer (`width`, `height`,
  `top` and the rest of that list): say which, from the code.
- Anything else pending (a class, an attribute, a custom property, a
  child added): N1's path.

**N3. The paint follows the classes.**
A PAINT change damages the element's painted bounds (old and new, with
its transform and its ink: shadows, outlines, filters), not the view.
Keeping the tree is what makes this possible: today a new tree repaints
the view. The record names, for each whole-view paint, why it was whole.
The target: a progress bar's tick repaints the bar.

**N4. A box that only moves.**
`left`, `top`, `right`, `bottom` on a positioned box whose size does
not depend on them, and `position: relative` offsets: translate the
box's subtree in the kept tree, recompute its scroller's reach, damage
old and new bounds. No placement. Smaller (38 of 456 here); after N3.

*As built (2026-11-24).* The restyle kept in the tree (restyle.inc
`keep_move`) takes a write whose only change past what the painter reads
is the insets of a positioned block-level box: its size not following them
(CSS 2.1 §§ 10.3.7, 10.6.4), the same side placing it before and after, no
percentage, and its own transform free to change in the same write. Nothing
in the box may be clipped, scrolled, sticky, fixed or transformed (named
declines). A relative inline and an atom on a line decline as `a relative
inline`: their pieces reach through their line into their block's overflow.
An element with no box, or a static one, takes the write and moves nothing.
The move is exact to the 26.6: the box's offset since the layout is summed
on the tree (`FMoved`), and its public boxes are made again by the public
build itself in a relocate mode (flow.c `public_box`), from the layout's
records plus that offset. So a rect read equals a whole layout's rounding
edge by edge, never a rounded delta. The records stay as the layout made
them, and so does the graft's snapshot reader (#722's rule), so the next
layout places the written element afresh. The page's extent and the
scrollers' ranges are reached again, the marks are the written element's
before and after (N3's), the moved boxes' drawn stamps move, and yonder
tells the observers something moved. The record counts kept moves (`layout
kept: ... a box that only moved N (M boxes)`).

## Proof: this is only safe with an oracle, as the cascade's was

- **The table's test.** For each field classed PAINT: on generated
  trees, change that field alone on a random element and require
  placement's result (every box's geometry, every scroll reach) to be
  identical to before; with the transform family, the reach and the
  transformed rectangles recomputed must equal a whole layout's. A
  field added to the style with no class fails the build.
- **`--geometry-check N`**, in yonder, diagnostics only, beside
  `--cascade-check`: every Nth time a read is answered from the kept
  tree, or a layout is skipped, run the whole layout beside it and
  compare the read's answer and the tree. A difference is logged with
  the element, the property, and the pending entries that were judged
  harmless. Chris browses with it on.
- **A random walk** on the host: random writes of every kind
  interleaved with random reads, each read's answer compared with a
  whole layout's, under ASan, a fixed seed.
- **Mutants** on the class table and each "is this stale" test.
- **The numbers:** the same two clean minutes (calm, playing) on the P5
  after each slice, by Chris, with `layout needed` in the record: forced
  layouts, their classes, reads answered with no work, whole-view
  paints and why. Plus the code-hosting page and the forum page, so a
  gain on one is not a loss on another.

## A page of our own that shows it (from Chris's run, 2026-11-20 night)

Chris, at a quarter of the screen, diagnostics off: one video played at
about 1 picture a second with the status line reading "12 forced
layouts in 800 to 1,850 ms" at every update; another played nearly
smoothly, reading "2 in 120 to 170 ms". Same build, same window, sound
perfect in both, CPU low. About 70 ms a forced layout in each. So the
picture's rate followed how many write-then-read pairs the page's script
makes per update (my guess: a progress bar in many parts against one in
one part), and not the video.

So the fixture for N1 to N3, `/tests/pages/layout-ticks.html`: a bar of
K parts, K chosen by buttons (1, 2, 12, 40); a timer four times a second
that, for each part, writes a paint-only style (`transform`, a
background) and reads `clientWidth`; a `<video>` playing beside it. The
proof is that the forced-layout line and the video's dropped pictures do
not depend on K. Variants: the write is `left` on a positioned part
(N4); the write is a class; the write is a custom property (must take
N1's path and still be right).

## First, and small: no ready task waits for ever (the loop's fairness)

Your finding on the controls that never hide is accepted, and it is a
defect of yonder's own, not only a symptom: a timer that is due never
runs because a media event is always ready ahead of it. Slow turns made
it visible; the order made it possible. N2 will make the turns fast, and
the order would still be wrong for the next slow page. It is also the
likeliest cause of pictures that arrive only when the video pauses.

The rule, for the loop that picks the next task:

- **A person's input first** (keys, pointer, the window), as now.
- **Among everything else that is ready, the one that has waited
  longest goes next,** whatever its source (timers, media events,
  arrivals from the network, pictures decoded, observers, messages). A
  source may be preferred when two have waited about the same; no
  source may be chosen over a task that has been ready longer than a
  bound. Lean: any task ready for 100 ms goes ahead of every newer task
  that is not input.
- **`timeupdate` coalesces:** while one is queued for an element and
  not yet delivered, another is not queued (HTML asks for it every 15
  to 250 ms "if the handler is keeping up"; a queue of them is never
  wanted). The same for any event the media host fires on a clock.
- A timer's order among timers stays HTML's (by due time, then by the
  order they were set).

Before code, with the three things below: the loop's order today as a
table (each source, when it is picked, what can pass it). Proof: a page
whose `timeupdate` handler holds the thread 300 ms and that sets a 1 s
timer on a key press; the timer must fire within a turn or two of its
time; pictures and network arrivals must land while it plays. This
slice goes FIRST: it is small, and it lets a page hide its own controls,
which is most of what Chris feels today.

## Before code: three things back to me, in one message

1. **The table:** every computed-style field against its readers, and
   the class you would give it. Where placement reads a field I called
   PAINT, say so; the code wins.
2. **The states table for N1 and N2:** rows are what is pending (nothing;
   PAINT entries only; restyle-needing entries; structural changes),
   columns are the askers (each kind of read, hit testing, a scroll, the
   paint, a frame's observers), and each cell says what is made current.
   Include resize observers, intersection observers and scroll
   anchoring, which read geometry on yonder's own behalf.
3. **What makes a paint whole today,** by cause, from the records, and
   which of them N3 removes.

## Beside this, not instead

- **Step 2 of C3 still stands** (`attributes` first: 55 fresh builds at
  815 ms; then `cap`; then the sheets' half kept). Run it beside N1.
- **Paint's own cost** (about 400 ms a whole view at full window, a
  third of it filters, then warps and backgrounds) is its own slice
  after N3 shows what is left. JS_SPEED_REPORT's fix #1 lives there.
- **The large play button over a playing video.** Chris had never seen
  it. Trace it before N3: if yonder shows a control the page meant to
  hide, that is a correctness defect, and it may be what carries the
  filter in every paint. Say what hides it in other browsers (a class
  set by an event we do not fire? a rule we do not match?).
  More from Chris (2026-11-21): on the slow video the progress bar split
  into chapters once it played (the 12 parts), and **the player's
  controls never hid** though the pointer had left long before; started
  by the headphones' Play the big play button stayed over the playing
  picture; paused by the space bar it showed a loading spinner until
  resumed. Three suspects, in the order I would look: (1) what the
  element tells the page about itself: `paused`, `readyState` and the
  `play`/`playing`/`waiting`/`pause` events when play starts from a
  media key and not from the page's own button, and `readyState` while
  paused (a spinner at pause is a page told it has no data); (2) the
  pointer leaving: are `mouseleave`/`mouseout`/`pointerleave` fired at
  the element the pointer left; (3) the page's own hide timer starved
  by the forced layouts (then it cures itself with N2). (1) and (2) are
  yonder's defects if true, and (1) belongs to the video arc's Opus:
  tell Chris, who carries it across.
  **Corrected by Chris's later runs: suspect (1) is the wrong way
  round.** Started by the headphones' Play, the controls do NOT show and
  the video plays well. Paused by the space bar they come up; resumed
  by the space bar they STAY up; paused by the headphones they go away
  after a while. And confirmed on several videos: controls hidden, the
  picture is smooth; controls shown, it crawls. So what keeps them up
  follows a KEY handled by the page, not the media key. Look at what a
  key press leaves behind that a media key does not: keyboard focus
  inside the player (other browsers' players keep controls up while a
  control has visible focus; `:focus-visible` and the focus events are
  ours to get right), a hide timer the page arms on a key and that never
  fires, or a pointer event the page waits for after a key. This is a
  look, not a slice: say which it is. N2 makes the cost of shown
  controls small either way.
- **Pictures that arrive only when the video pauses:** the main loop's
  fairness (arrivals behind a queue of events that never empties). Look
  after N2 has freed the loop; if it still happens, it is a scheduling
  defect to book with the loop's order.
- **The per-read table** you did not build: not needed now; the classes
  answered it. **The `role` breakdown:** wait for N1 to N3; then only
  the layouts that really move something are left to graft.

## Rules

As before: userland only; each slice its own PR, a cold read, then
Codex; nothing merges on my say; the P5 is Chris's; nothing shaped to a
site; no corpus host name, URL or kept content in a commit, a PR or a
tracked doc; edit with the editor tools; mutants once at the end through
`mutants_parallel.py`; the harnesses build in the real tree, so rebuild
and check before Chris installs; the comment is part of the code.
Questions in one message, each with your lean.
