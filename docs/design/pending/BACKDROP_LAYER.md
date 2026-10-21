# BACKDROP_LAYER.md — a moving background repaints only itself (design)

*2026-10-20. Opus writes. YONDER_PERF.md's "Then measure again" list named
it first ("A background frame that repaints only the background"), and
UI_DAMAGE.md's reading made it the next one: with the dirty list in place,
the whole views left on danlegt are its starfield. The goal, as Chris put it
once the first draft had been fitted too closely to danlegt: pages with
animated backgrounds get faster IN GENERAL, and **nothing is slower than
today**. danlegt's bottom at 5% of a CPU is one reading that has to come in
under it, not the target.*

## Why

When a box's background moves — a GIF's next frame, a CSS animation of
`background-position` — yonder marks the box's part of the view and paints
it: the background, and then everything in front of it, every box, glyph and
picture, to change what is behind them. When the box is the page, that is a
whole view at the background's frame rate.

At the bottom of danlegt (the Explorer window on the left, `body`'s
starfield over the rest):

| | |
|---|---|
| QEMU, userland 5fb5f7a6, 198 s | 3096 paints, 2113 whole views, 2065 of them "a moving part as big as the view": about 96% of the 923.5 Mpx painted |
| P5 2560 x 1440, 100%, after #316 | yonder about 25% of a CPU |

## What moves, on the pages Chris has visited

The census (`/home/tmp/yonder` on the P5: 1600 records, 629 pages) was
fetched again and every background picture and every `@keyframes` that
animates a background was looked at. Spinners and 16 px throbbers are many
and small, and the dirty list already handles them as parts. The ones that
repaint a large part of the view:

| page | the box | what moves | how much of it a frame changes |
|---|---|---|---|
| danlegt.com | `body`, a box (`html` has its own colour) | stars1.gif, 304 x 234, 6 x 90 ms, opaque | 0.7% avg, 1.0% most, scattered |
| warpbox.dev (retro theme) | `body` | stars1.gif | the same |
| theoldnet.com guestbook | `<body background>`, so THE CANVAS | chatbg2.gif, 912 x 702, 6 x 100 ms, opaque | 0.7% avg, 1.0% most |
| fauux.neocities.org | `body` | bg_main2.gif, 3 x 3, 3 x 40 ms, opaque | every pixel, every frame |
| fauux.neocities.org | `body` (another rule) | bg-dis-03.gif, 1024 x 1024, 23 frames, TRANSPARENT pixels | 8.7% avg, 100% most |
| fauux.neocities.org | `#enterWired` | 16c.gif, 838 x 650, 24 x 40 ms, transparent | 3.0% avg |
| toastytech.com/evil | a `td` | burnieani.gif, 126 x 157, 4 x 100 ms, opaque | 55% |
| ugbet88vip.com | a `div` | jackpot.gif, 688 x 188, 29 x 80 ms, transparent | 15% |
| cybertwee.net | `body` | a gradient, `background-position` animated over 15 s | every pixel, every frame |
| speakeasy.net | `.bkgd-home-hero` | the same, 25 s | every pixel |

So the general case has five shapes, and the design has to take all of
them: a sparse frame and a dense one; a box and the canvas; opaque pictures
and transparent ones; a GIF's frames and a CSS animation; a box as big as
the page and one that is a fair part of the view. danlegt is one row. (A
box smaller than that — toastytech's `td` — stays an ordinary part: § The
moving box says why.)

The twinkling rows also say why rectangles are no answer: stars1's changes
are 1% of the tile's pixels but 72% of its 16 x 16 blocks, and their
bounding box is the whole tile in four frames of six. What is small is the
number of pixels.

## The idea

For ordinary compositing, a pixel is linear in what is behind it. yonder's
`over` lays colour `s` at alpha `a` over `d` as `s·a + d·(255 − a)` (rounded),
so a stack of layers in front of a background pixel `B` gives

    out = C + T · B

where `C` is what the stack puts on the pixel and `T` how much of `B` still
shows through, and neither depends on `B`. Paint the front once with the
moving background black (`B = 0`) and the pixel is `C`; once with it white
(`B = 255`) and it is `C + 255·T`. Call those `K` and `W`. A frame of the
background is then

    out = K + (W − K) · B / 255

per channel, at every pixel the frame changes and nowhere else.

**A background with transparent pixels** shows what is BEHIND the box's
moving layer there, and that is neither `K` nor `W`. A third paint covers
it: `P`, the view with the moving layer left out, which is `C + T·back`.
The moving layer lays its pixel `b` at alpha `α` over `back`, so

    out = C + T·(α·b + (1 − α)·back) = P + α·(K + (W − K)·b/255 − P)

which is linear too, and needs no split: a GIF's all-or-nothing alpha takes
`P` or the formula, and a soft edge — a PNG's, a WebP's, or a GIF's own
edge smoothed by the tiler at a zoom — takes the blend.

So the layer is three paints of the view — `K`, `W` and `P` — and a frame
costs its changed pixels in that arithmetic, plus the publish, instead of a
paint of everything in front of the background.

**The invariant this rests on: everything the painter draws in front of a
background is `over`.** Opacity groups keep it (GlassGroups' comment: a
group laid over its backdrop at an opacity is linear in the backdrop); so do
text masks, pictures, gradients, borders and shadows. A transform that
carries the moving box itself resamples background pixels across each other,
and is refused. yonder draws nothing else over a background; a verb that
does not keep the invariant (a blend mode, a backdrop filter) joins the
refusals in the commit that adds it. The invariant is written at the code.

## The moving box

The layer is held for one box at a time, `X`: of the backgrounds a frame
moved while none is held, the one covering the most of the view, if that
is at least a sixteenth of it. The paintings are the view's size whatever
the box's, so a smaller box would cost the view's memory for a part the
dirty list already paints apart, cheaply. The canvas counts as a box: its
owner (`yonder_canvas_owner`) paints the whole view, and the layer is the
view. `X` is held by its ELEMENT, layer and picture, never by its box: a
layout remakes every box, and one may come between a frame and its paint.

What makes `X`'s background move is one of two things, and the layer does
not care which:

1. **A picture's next frame** (`pictures_tick`): GIF, and the animated
   formats yonder decodes.
2. **A CSS animation or transition of a background property of `X` alone**
   (`background-position`, `-size`, `-color`, a gradient's stops): a tick
   whose only marks are those of `X`'s background (`tick_mark`).

Refusals, each named on the record, so a page that does not get the layer
says why: a layer of `X`'s drawn ABOVE the moving one (a picture or a
gradient: it is front painted as background); `X`'s layer drawn through a
sheet — a transform's or a clip's, which moves its pixels across each other
or masks them by more than `over`; drawn in two places in one painting (an
inline box's pieces); a copy of its picture past what is kept; more widgets
over it than are kept clear; no room beside the paint cache, or no memory;
and dearer than painting it, measured. A second moving layer of `X`'s
below the held one is not refused: its frames are ordinary marks of `X`,
which keep the view from ever going quiet, so the layer is never built.

The widgets libui paints after the view — the form controls, and anything
after the view or after a widget holding it — are seen by no painting of
the layer's. Their rectangles are HOLES: a frame never composes inside one,
and each frame marks each hole that meets `X`'s part of the view as an
ordinary part, so the widget repaints over it as it does today.

## The layer

| state | what it holds |
|---|---|
| `K`, `W`, `P` | the view's size, 4 bytes a pixel each |
| `B` | the moving layer as the view shows it now, with its alpha: for a tiled picture, one tile at the size the tiler lays it; for a CSS change, the moving layer painted alone across `X`'s part of the view |
| the invalid list | where `K`, `W`, `P` are stale, in view coordinates, the dirty list's rule (`os64_damage_add`) |
| quiet | frames running across which what was painted for anything else stayed at most an eighth of `X`'s part |
| the costs | measured, per pixel: an ordinary paint of `X`'s part, a compose, a rebuild |
| holes | the rectangles of the widgets painted after the view that meet `X`'s part of it |

**The surface always shows the current background.** Every frame either
composes every pixel it changes (rebuilding the invalid ones first), or
paints `X`'s part ordinarily, and an ordinary paint draws the current frame.
So a frame's change is always measured from the `B` the surface shows.

**Any ordinary paint of the view invalidates what it painted**: one line at
the choke point (`view_paint`), not an audit of every cause of a paint. A
scroll, a layout, a hover, a picture arriving, another animation's part, a
theme change all arrive as ordinary paints. Three paints do not: the
layer's own; a hole's; and a paint the held frame marked (its part painted
ordinarily, or its holes). That last one changes only what is behind, which
the paintings do not hold — a layer that invalidated itself whenever a
frame measured dearer would be lost and rebuilt over and over. What ELSE
was dirty as the frame marked it is noted then, rectangle by rectangle,
before libui's list merges it with the frame's own. A resize of the view
leaves the layer to be built again.

The same pixels count against the QUIET: the view is quiet across a frame
when what was painted for anything else since the last one is at most an
eighth of `X`'s part. A small animation in front (a GIF in front of the
stars) costs its own part a frame, which the policy weighs; a scroll or a
layout is not quiet.

One change outside the layer came with it. yonder marked every box whose
style shows a moving background picture, and a text run, a line box and a
list marker carry their element's style, background and all — but the
painter draws no background behind them. A frame of `body`'s picture
marked each run of text directly in `body` for nothing, and with the
layer, those marks kept the view from ever being quiet. They are not
marked now (`box_draws_background`).

## A frame

1. **What changed.** For a tiled picture, the new frame's tile is built and
   compared with the shown one: the changed pixels, as runs, laid at every
   copy of the tile that meets `X`'s part of the view. For a CSS change, all
   of `X`'s part (a moved gradient changes every pixel).
2. **Whether to compose.** The frame composes only when the layer is built
   and that is the cheaper way to draw it, by measurement: the changed
   pixels at the compose's cost per pixel, plus rebuilding what is invalid
   at three paints' cost, against one ordinary paint of `X`'s part at its
   cost. Otherwise `X`'s part is marked as a part and painted ordinarily, as
   today — the layer stays as it is, and the frame is counted with why.
3. **Rebuild**, when invalid and composing: the three paints of the invalid
   rectangles into `K`, `W`, `P`, and every pixel of them composed.
4. **Compose**: each changed pixel outside the holes and outside what was
   just rebuilt.
5. **Publish** what was composed: each changed copy's box, through the
   dirty list's rule.
6. **Holes** that meet `X`'s part are marked as parts.
7. The new `B` becomes the shown one.

### Building the layer the first time

The first build costs three paints of `X`'s part where today's frame costs
one: the one moment the layer is slower than today, and it has to buy that
back. So it is built only when it is likely to be:

- an ordinary paint of `X`'s part has been measured, and
- the view has been **quiet for two frames running**, and
- the last build was not **wasted** — invalidated whole before it had paid
  for itself. Each wasted build doubles the frames of quiet the next one
  needs; a build that pays for itself resets it.

What a frame composed costs is not known until the layer composes one, so
the build is an experiment that measures itself: from then on each frame
is composed only when that is measured cheaper, and a layer that measures
dearer on eight valid frames running is given up for good (named on the
record: "dearer than painting it").

A page that never goes quiet (a scroll held down, a page whose other
animations repaint much of the view) never builds the layer, and costs
exactly what it costs today.

## What "nothing slower" means, exactly

- A page with no moving background, or with one on a small box: one
  pointer test per paint. Nothing else runs.
- A page with one held: once its costs are measured, each frame is drawn
  whichever way measures cheaper. What can cost more than today is
  bounded: the three-paint build, attempted only after quiet and backing
  off when wasted; and the frames that measure a layer which turns out
  dearer, at most eight running before it is given up for good.
- Memory: refused memory is an ordinary frame, never a stall.

The record says every one of these (below), so a page where the layer
costs more than it saves is visible, not a feeling.

## Numbers, and what is promised

`over` rounds: `(s·a + d·(255 − a) + 127) / 255`. A composed pixel is

    F = K + ((W − K) · b + 127) / 255
    out = P + ((F − P) · α + 127) / 255

per channel, which is:

- **exact** where the front is opaque (`W = K`, and then `P = K` too), where
  nothing is in front and the moving pixel is opaque (`K = 0`, `W = 255`,
  `α = 255`), and where the moving pixel is transparent (`P`);
- elsewhere — an antialiased glyph edge, a translucent box, a soft edge of
  the moving layer — **within one step per channel for each translucent
  layer involved** of what an ordinary paint draws, and never outside the
  range of `K`, `W` and `P`.

The next ordinary paint of the pixel draws it exactly. The host harness
holds the bound with the painter as the oracle.

## Memory

`K`, `W`, `P`: 12 bytes a pixel of the view, 44 MB at 2560 x 1440. Taken at
the first build, and given back when the page leaves, when `X`'s picture
stops (its loops done), and when the layer is refused. A resized view
takes them again at its new size at the next build.

They live under the paint cache's ceiling, and never at the cache's
expense (Chris, 2026-10-20: "yes, as long as it's still big enough"). The
cache keeps its own budget, eight screens at 4 bytes a pixel; the layer's
three views come on top of it; and the two together answer to the share of
installed RAM the cache already answers to (RAM / 16, 1.4 GB on the P5).
On a machine where both do not fit, the LAYER is refused — counted, the
frame drawn ordinarily — and the cache is not made smaller to make room.

## What the record says

One line, beside `whole views:`:

    backdrop: a box, 1294 frames: 1201 composed (44.3 Mpx), 93 painted
    (90 before it was built, 3 dearer so), 3.1 Mpx painted again for it,
    2 built (1 lost before they paid), 1201 holes; refused: <reason>

("the canvas" for the canvas's owner, "none held" when no background was
big enough.) A `d` reading carries it as well. The `whole views:` line keeps
its causes; a frame painted ordinarily is still "a moving part as big as the
view". Composed frames are not paints: the `paint:` line counts what libui
painted.

## The lifecycle

| The layer is | When | It becomes |
|---|---|---|
| none | a frame of `X`, an ordinary paint measured, quiet two frames (or the backed-off count) | built: three paints of `X`'s part, composed; the cost of the build recorded against what it saves |
| none | a frame otherwise | none; the frame painted ordinarily |
| none | memory refused | none; counted; the frame painted ordinarily |
| built | an ordinary paint of rect R | R joins the invalid list unless inside a hole; quiet = 0 |
| built | a frame, composing cheaper | invalid rebuilt and composed; valid |
| built | a frame, painting cheaper | unchanged; the frame painted ordinarily |
| built | wholly invalid | not built, memory kept; when it had not paid for itself, counted as lost and the next build needs twice the quiet |
| built | a frame of `X`'s painted ordinarily with nothing else dirty | unchanged |
| any | `X`'s picture stops | not built, memory given back |
| any | refused (named) | not built, memory given back; `X`'s frames ordinary marks until the page leaves |
| any | the view is resized | not built; taken again at the new size at the next build |
| any | the page leaves | none held |
| any | the window is covered | unchanged (pictures do not tick while covered) |

## Steps

1. **The pure half** (`apps/yonder/backdrop.c`, no yonder state): the
   compose, the tile compare as runs, the cost rule, the build rule and its
   back-off, the holes. Host-tested against `over` as the oracle; mutants.
2. **Picture frames**: the layer for a GIF behind a box and behind the
   canvas, opaque and transparent; the record line. Readings: danlegt,
   theoldnet's guestbook, fauux.
3. **CSS motion of a background**: the tick that marks only `X`'s
   background routes to the layer. Reading: cybertwee.

Each step ships when its readings say nothing is slower.

## Proof

- **Host, pure**: random stacks of up to three layers over random
  backgrounds, every pixel exact where promised and within the bound
  elsewhere; transparent frames take `P`; the runs; the cost and build rules
  including the back-off. Mutants for each rule.
- **yonder, host**: a page with a moving background behind text, painted
  ordinarily, then advanced frame by frame through the layer and compared
  at every frame with an ordinary paint of that frame (within the bound,
  exact where promised); an ordinary paint between frames invalidates what
  it painted; each refusal says its name; the canvas and a box both.
- **QEMU and the P5**: the census rows above, the same scroll position,
  before and after: the paint line, the backdrop line, yonder's busy share;
  and the pages with no moving background, to show they did not move.

## The reading that has to come in under 5% (Chris, 2026-10-20)

yonder's busy share on the P5, fullscreen 2560 x 1440 at **150%**, the
bottom of danlegt: the Explorer window and what of the donations box is not
JavaScript, over the stars. Chris's careful before-reading put it at 8-14%
(§ Measured; an earlier quick look had said 27-28%). At 100% the QR code's
area is in view as well, and what moves there is not the background, so it
is a reading for this slice and not its target.

At 150% the tiler scales the tile with smoothing, so a twinkle's change
spreads to its scaled neighbours: the changed pixels are counted at the
size the view shows them, which is why step 1 compares tiles as laid, not
the picture's own frames.
## Measured (2026-10-20/21): shelved

Built through step 2 and measured on the P5 by Chris, 2560 x 1440, yonder's
share of a CPU before and after, each read twice (`/home/yonder-timings` on
the P5):

| danlegt | 100% before | 100% after | 150% before | 150% after |
|---|---|---|---|---|
| top | 38-45 | 48-56 | 35-50 | 49-55 |
| middle | 52-60 | 68-73 | 51-85 | 84-89 |
| bottom | 27-37 | 39-46 | 8-14 | 27-29 |

**Slower everywhere.** yonder's own counters (`/proc/<pid>/status`) at the
bottom at 150%: about 92% of its time in user code, publishing about 1%,
some 24 ms of user time a starfield frame. QEMU had shown the opposite —
whole views gone, the loop busy 25% against 72% — because TCG counted
pixels faithfully and timed nothing faithfully.

Why. This design priced a paint per PIXEL (§ The layer, the costs). A paint
also costs a walk of the whole box tree whatever its size, and the layer
traded one whole paint a frame for several small ones: the view under each
widget over the stars (four at the bottom, each a full walk, though every
one of them sits on the opaque Explorer window where no star can show),
and at the top and middle, where the P5 does build the layer, the three
paintings of each rectangle another animation changed. At 150% it also
re-scaled the whole tile with the smoothing arithmetic every frame. The
per-pixel policy saw none of it, so "nothing slower" was argued and not
measured — and it was not true.

Why it stopped rather than being fixed. The before-numbers were already
good (150% bottom 8-14%, near the 5% the slice was for), the fix is a
redesign (below), and the pages with large moving backgrounds are old-web
pages, not the ones the Modern Web arc is buying JavaScript time for.
Chris and Opus agreed: not now.

What reviving it takes, each step measured on the P5 before the next:
1. Compose-or-paint decided by measured whole-frame time, the other way
   retried now and then.
2. A hole repainted only where the background shows under it; pixels the
   background cannot reach not written at all.
3. Each scaled frame of the tile kept, not re-scaled.
4. What is stale rebuilt as one rectangle: one walk a painting.

Carried forward on its own branch: the text runs, lines and markers that
yonder marked as if they showed their element's moving background
(`box_draws_background`, § The layer). That is a bug in code that ships,
found by this slice's tests, and it does not need the layer.
