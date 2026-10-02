# PILE3.md — pile 3: what the page looks like, and what moves

*Written 2026-10-02 by Opus, the day Chris remembered that pile 3 was still
owed before QuickJS reaches yonder, and the same morning he put
danlegt.com's "Tune In" box beside Chrome's: a box too skinny for its
twelve icons spilled them past its bottom edge in yonder, while Chrome
scrolled them inside it. GARB.md's module table names pile 3; this
document orders it and designs each slice as it is built, the first of
them below. The library is libflow and its rules are
[LAYOUT.md](../completed/LAYOUT.md)'s; the properties come from libgarb
([GARB.md](../completed/GARB.md)); the face is yonder
([YONDER.md](../completed/YONDER.md)).*

## What pile 3 is

GARB.md's table gives pile 3 *Backgrounds and Borders, Images,
Transforms, Fonts (`@font-face`) and Animations*, and everything earlier
slices booked against it:

| Area | What it is |
|---|---|
| **The blend** | Translucent colours, `opacity` between 0 and 1 painted as a group, and a non-positioned box below full opacity as a stacking context (POSITION.md § Booked). The painter today has no blend at all, and every row below wants one. |
| **Borders** | `border-radius`, which also clips a background to the curve; dotted, dashed, double and ridge |
| **Backgrounds** | `background-size`, `-origin`, `-clip`, `-attachment`; every layer of a list; gradients as pictures (G2b already parses the grammar) |
| **Shadows** | `box-shadow`, `text-shadow` |
| **Transforms** | 2D `transform`, with hit-testing taken back through it |
| **Web fonts** | `@font-face`, `unicode-range`, `font-variant: small-caps` |
| **Animations** | `@keyframes`, `animation`, `transition`, on the Y5b ticker |
| **Zoom** | not CSS: the face scales its viewport. Chris asked for it before pile 3 ends (9/27, Hacker News's tiny type) |

**Scrolling boxes** joins it: GARB.md booked `overflow: auto` and `scroll`
for the day "yonder scrolls a box", and that day is the first slice
(§ Scrolling boxes).

## The order, measured

GARB.md said danlegt.com would be measured again to order this pile.
Counted on 2026-10-02 over its 28 linked sheets and its inline `<style>`
(172 KB): the lines that name each property.

| Lines | Property |
|---|---|
| 207 | `transform` (most inside `@keyframes`) |
| 206 | a translucent colour (`rgba()`, `hsla()`, `/ alpha`) |
| 160 | a gradient |
| 112 | `animation` and `@keyframes` |
| 89 | `box-shadow` |
| 86 | `opacity` |
| 75 | `filter` (Filter Effects 1: not in GARB.md's pile 3 — noted here, and booked below) |
| 51 | `cursor` |
| 50 | `image-rendering` (a pixel-art site's `pixelated`) |
| 44 | `text-shadow` |
| 29 | `transition` |
| 18 | `border-radius` |
| 18 | dotted, dashed, double or ridge |
| 16 | `overflow` `auto` or `scroll` |
| 13 | `background-size`, `-clip`, `-origin` |
| 6 | `@font-face` — every one a `.ttf`, so the site needs no WOFF2 |

The order: **scrolling boxes** first, because it is a page that reads
wrong today rather than one that looks plainer; then **the blend**, which
every later row stands on; then gradients and backgrounds, transforms,
shadows and borders, animations, and web fonts — each slice re-measured
against the corpus when it starts, since a count of lines is a guide and
not a ruling.

## Scrolling boxes (slice S1)

### What CSS asks (CSS Overflow 3 § 2–3)

A box whose `overflow` is anything but `visible` CUTS its content to its
padding box. Every value but `visible` and `clip` also makes it a SCROLL
CONTAINER: it has a SCROLL POSITION, and its content is drawn moved back
by it. `auto` and `scroll` let a person scroll it; `hidden` does not, but
a script and a fragment link may, so it is a scroll container all the
same. How far it scrolls is its SCROLLABLE OVERFLOW: everything of its
content, its positioned descendants included, and its own end padding
past what is in its flow. What is left of or above the padding box cannot
be scrolled to. The root's `overflow` — or the body's, while the root's is
`visible` — is the VIEWPORT's, and the box itself neither clips nor
scrolls: that is the page scrolling, which yonder has always done.

A visible or clip axis beside one that scrolls computes to `auto` (§ 3),
so `overflow-x: hidden` alone makes the y axis `auto`, which clips too.
Chrome draws it that way; libflow did not, while `auto` drew unclipped.

### How it is built

**A scroll container's content is a FRAME.** POSITION.md's P4 built the
machinery a sticky box needed: a record that moves a box and everything
inside it, worked out at each scroll, with what clips it from outside
held where it stands while it moves. A scroll container needs exactly
that, the other way round: its content moves back by the scroll
position, and the container's own padding box is what clips the content
from outside. So the record is generalised (`flow_frame_t`, with a kind,
STICKY or SCROLL), and every box carries the innermost frame that moves
it (`flow_box_t.frame`, which was `sticky`). The rules that already hold
for sticky boxes hold for this one without a second copy:

- **Frames follow containing blocks.** A box in the flow is in its
  parent's content frame; an absolute box is in its containing block's.
  So an absolute box whose containing block is outside a scroll
  container neither scrolls with it nor is cut by it (§ 11.1.1), and a
  relative scroll container's absolute content does both.
- **A frame's clip from outside does not move with it.** A box's own
  `clip` holds only what clips inside its innermost frame;
  `flow_box_doc_clip` meets it with each frame's, where each frame stands.
- **One rule turns a box's coordinates into the page's**
  (`flow_box_doc_offset`): a fixed box's page scroll, each sticky push,
  and now each scroll position taken back, outermost first.
- **A sticky box inside a scroll container sticks to it** — its
  scrollport was already "the nearest scroll container", and now that
  container moves.

**The scroll position is the one thing a face may change in a laid-out
tree.** `flow_scroll_set(tree, i, at)` holds it to `[0, range]`; every
rect, clip, `flow_visit` and `flow_hit` answers by it; a new layout
starts every box at 0,0. The page's own scroll stays a parameter, as it
was: it is the viewport's, not the document's. A box's is the box's, and
belongs in the tree the box is in — until the DOM work, where
`element.scrollTop` lives on the element (DOM.md), and a face keeps it
across layouts by node in the meantime.

**The walks enter a scroll container's content by its scroll.**
`flow_visit` moves the view by the scroll position at the container and
prunes the content against it; `flow_hit` asks the container's clip
where the container stands, then takes the point to where its content
stands. Positioned content is a layer of its own, and
`flow_box_doc_offset` already moves it.

**The range is built with the content.** Each piece of a container's
content — its lines, its in-flow boxes, its marker — reaches into the
range with the container's end padding past it, as the public tree is
built. Positioned boxes are reached afterwards from the positioned list,
each into the innermost scroll frame it is in. A sticky box whose
containing block IS a scroll container may then move as far down as
that container's content reaches, not one screenful of it: its room is
its distance to the padding box's foot plus the container's range.

**The page's extent no longer holds what a box scrolls.** A clipping box
widened nothing and lengthened nothing; `auto` and `scroll` did, because
they drew everything. They clip now, so they do not — except the
viewport's own, which is the page.

**Counting frames.** A sticky box's push is worked from every frame
outside it, and a lookup is remembered for at most `F_FRAME_DEPS` of them
(was `F_STICKY_DEPS`). Scroll frames count toward it: a sticky box past
32 frames is laid out where the flow put it, as one past 32 sticky boxes
was. A chain of scroll containers alone costs nothing extra, since each
is worked from the one frame outside it.

### The face (yonder)

- **The wheel goes to the innermost box under the pointer that can still
  move that way**, then to the box outside it, then to the page — scroll
  chaining, as every browser does it. A box asks `auto` or `scroll` of
  the axis before it takes the wheel; a `hidden` one passes it on.
- **A thin bar over the inside of the padding box's far edge** says where
  a box is scrolled to. It OVERLAYS the content, as touch screens and
  macOS draw theirs, so showing it changes no layout; an `auto` axis
  shows one only when there is something to scroll to, a `scroll` axis
  shows its track always. A bar is drawn only where the pointer would
  reach the box (POSITION.md, ruling 9), so whatever covers the box
  covers its bars.
- **A box keeps where it was scrolled to across layouts** — a resize, a
  late sheet, `p` — by its element (`page_keep_box_scroll`), held to the
  new layout's range.
- **A fragment link scrolls every box its target is in**, innermost
  first, each to put the next one in at its top, then the page; a
  `hidden` box too, since a link may scroll what a person cannot.
- **A control scrolled out of its box has no widget**, by ruling 9's own
  test, and its painted stand-in is cut to the clip like a run or a
  picture. (It was not: `glass_control` threw its clip away, so a hidden
  widget's stand-in drew over an `overflow: hidden` edge too.)

### Proof

`tools/test_libflow_scroll.inc`, in `tools/test_libflow_host.sh` under
ASan and UBSan: the range with border and padding, a position held to it,
the content drawn moved and the clip still, a hit inside the padding box
reaching the scrolled content and one on the border reaching the box,
the walk pruning what is scrolled away, an absolute box in and out of a
container's frame, a sticky header riding a container's top (in a
wrapper, and straight inside it the whole way down), a control covered
until scrolled in, nested containers, a fixed one, and the viewport's
overflow making none. Five existing dumps changed, each by the rule
above: a scroll container prints `scroller N range X Y`, `auto` no
longer widens the page, and `overflow-x: hidden` clips both axes. The
guest: danlegt's shape (a grid row of fixed height, a `ridge`-bordered
panel with `overflow-y: auto`, twelve icons) cut at its edge with a bar,
wheeled to its end and chaining on to the page, its field live once
scrolled into view and absent before; a list with a sticky heading
riding its own top while the page scrolls; a fragment link scrolling the
list and the page; both positions surviving a relayout.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| Dragging a box's bar, clicking its track | the wheel is how a person scrolls a box today; a bar that takes a press needs libui's grab for a region that is not a widget | a person without a wheel, or a box too long to wheel through |
| The keyboard scrolls only the page | nothing in yonder focuses a box | a box focused by a click, as browsers do |
| Back and Forward keep the page's position, not its boxes' | libway's crumb holds one position (`way_position_t`) | a page whose boxes a person comes back to |
| `scroll-padding`, `scroll-margin`: a fragment target under a sticky heading | a target is brought to the box's top, where a heading painted over it hides it — as Chrome does without them | a page that sets them |
| Smooth scrolling, `scroll-snap`, `overscroll-behavior` | a wheel moves a box at once, three lines a notch | a page whose carousel snaps |
| Classic bars that take room from the content (`scrollbar-gutter`, `scrollbar-width`) | the overlay bar changes no layout, which is the point of it | a page whose layout counts on the gutter |
| `filter`, `cursor`, `image-rendering` | not GARB.md's pile 3, and measured above | the slice that takes them, re-measured |
