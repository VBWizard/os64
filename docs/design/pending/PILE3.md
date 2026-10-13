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
| **The blend** | Translucent colours, `opacity` between 0 and 1 painted as a group, and a non-positioned box below full opacity as a stacking context (POSITION.md § Booked). Every row below wants it (§ The blend). |
| **Borders** | `border-radius`, which also clips a background to the curve; dotted, dashed, double and ridge |
| **Backgrounds** | `background-size`, `-origin`, `-clip`, `-attachment`; every layer of a list; gradients as pictures (G2b already parses the grammar) |
| **Shadows** | `box-shadow`, `text-shadow` |
| **Transforms** | 2D `transform`, with hit-testing taken back through it |
| **Web fonts** | `@font-face`, `unicode-range`, `font-variant: small-caps` |
| **Animations** | `@keyframes`, `animation`, `transition`, on the Y5b ticker; and `prefers-reduced-motion` stops answering `reduce` (libgarb's media.c answers it because nothing moves) |
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
each into the innermost scroll frame it is in — past any sticky frames
between, since a sticky box is counted where the flow put it — and only
as far as can be SEEN of it: cut by its own clip and each sticky frame's
on the way out, but never by the container's own, which is what
scrolling reaches past (Quinn, #201: a child an `overflow: clip` wrapper
cuts away must not make a bar that scrolls through empty space). A sticky box whose
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
  new layout's range, and found again by `flow_scroller_for`: an
  inline-block's first box is the atom on its line, not the container.
- **A fragment link reveals its target in every box it is in**
  (`flow_scroll_reveal`, the door's because `scrollIntoView` will want it
  too): innermost first, each box asked to show the TARGET where the boxes
  inside have moved it — not the box inside it, which may be wider than
  its view (CSSOM View's walk; Quinn, #201) — its top at the box's top,
  and across as little as shows it, as Firefox and Chrome reveal a
  fragment; then the page by
  the same rule. A `hidden` box too, since a link may scroll what a
  person cannot.
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
until scrolled in, nested containers, a fixed one, the viewport's
overflow making none, an inline-block's container found by element,
positioned content reaching through a sticky box and stopped by a clip,
and a target revealed on both axes. Five existing dumps changed, each by the rule
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
| `filter`, `cursor` | not GARB.md's pile 3, and measured above (`image-rendering` was taken in § Zoom) | the slice that takes them, re-measured |

## The blend (slice S2)

### What CSS asks (CSS Color 4 § 3.2, § 4; CSS 2.1 Appendix E)

A colour may be TRANSLUCENT — `rgba()`, `hsla()`, `#rrggbbaa`, `/ alpha`
— and is then laid OVER whatever is already painted there, source-over:
each channel `alpha * colour + (1 - alpha) * under`. `opacity` below 1 is
different, and the difference is the whole of this slice: it does not
fade each thing the element paints, it paints the element and everything
inside it as ONE GROUP and fades the group, so a black card with a green
box inside it at `opacity: 0.5` shows the page through the green, not the
card's black. Such an element is a STACKING CONTEXT, positioned or not,
painted where a positioned box of `z-index: 0` would be.

### How it is built

**A colour keeps its alpha, in itself.** A flow colour is `0xTTRRGGBB`,
the top byte how TRANSPARENT it is (flow.h, `flow_alpha`). Transparency
rather than alpha so every plain `0xRRGGBB` anybody wrote — a face's ink
and paper, an attribute, the Rendering chapter — is still the opaque
colour it was; and in the colour so that whatever copies a colour copies
its alpha: inheritance, `currentColor`, a border or a decoration in its
element's colour. libflow used to lay alpha over the page's paper while
styling (`xrgb`), which made `rgba(0,0,0,0.5)` a grey sheet; it now keeps
it, and POSITION.md's stopgap — a translucent background on an
out-of-flow box not painted at all — is gone with it.

**A box below full opacity is STACKED.** `flow_box_t.stacked` is what the
layer list holds: every positioned box, and now a block-level box in the
flow below full opacity, which the tree's own walks skip and the stacking
order paints whole (POSITION.md's booked row). Unlike a positioned box it
has not left the flow, so it stays in its parent's overflow rect, clip
and frame. `flow_positioned` still lists only the positioned ones.

**A group is bracketed.** Every stacking context below full opacity — the
root's too — gets an OPEN layer before everything it paints and a CLOSE
after; `flow_visit_groups` hands the face both, the open with the
group's BOUNDS: every box of every layer inside, each overflow rect where
its frame puts it at this scroll, so an absolutely positioned member far
outside the box is inside. `flow_visit` is the same walk with no ears for
groups. `opacity: 0` stays unpainted and opens nothing.

**The face composites a group without a transparent canvas.** yonder
paints straight onto an opaque window, and libdraw cannot paint onto
transparency. It does not need to: at the open, yonder copies what is
under the group's bounds (cut to what is being painted); the group paints
as usual; at the close, the copy is laid back over the group at
`255 - alpha` with libdraw's own source-over. For a group G whose own
coverage is c, over a backdrop B, CSS asks `a*c*G + (1 - a*c)*B`; the
copy gives `a*(c*G + (1 - c)*B) + (1 - a)*B`, which is the same. Groups
nest on a small stack (`GlassGroups`).

**An element is framed once.** An inline-block (or inline-flex, inline-grid,
a button) is two boxes of one element — the atom on its line and its
content box inside — and both used to paint the element's background,
borders and picture: invisible while everything was opaque, twice as
opaque once it is not (Quinn, #202). The atom paints the frame; the
content box under an atom of its own element does not.

**libos64 gained two alpha verbs**: `os64_draw_fill_rect_alpha` and
`os64_text_draw_alpha`, with `os64_draw_blend`'s rounding — the text
one scales each glyph's coverage by the ink's alpha.

**`@supports (opacity: 0.5)` says yes** now: libgarb's approximation
list no longer holds it.

### Proof

`tools/test_libflow_blend.inc`: a block below full opacity stacked and
not positioned, painted as one group after the paragraph written after
it, its bounds, and hit over the flow it overlaps; a positioned group's
bounds holding an absolute member 50 below it; nested groups; an
inline-block left unstacked; `opacity: 0` opening nothing. One style dump
changed by rule: a translucent background keeps its alpha in or out of
the flow (`bg=#000000/128`). `tools/test_yonder_cases.inc`: translucent
fill and text reach the verbs with their alpha; a group recorded open,
painted, closed at 128; nested groups; a group out of view opening empty;
an inline-block, inline-flex and inline-grid's translucent background
laid once.
libgarb's `@supports` page now finds `opacity: 0.5` supported. In the
guest, a fixture of stripes under an `rgba` veil with half-black text, an
`opacity: 0.5` card holding a green box, an in-flow `opacity: 0.35` block
over both by a negative margin, and an `rgba` background on the paper
was drawn as headless Chrome draws it: the stripes through the veil,
through the card AND through its green, the in-flow block over the card.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| `opacity` on an inline box or an inline-block | an inline box is pieces on lines and an inline-block's content is painted where its atom is, so neither is one layer to group; both are painted opaque | a page whose faded link or badge matters |
| Form widgets and a box's scroll bars in a group | libui widgets are windows' children drawn after the page, and the bars are drawn after it too; both stay opaque | a faded form, or a faded scrolling panel |
| The root's own opacity over the canvas | the canvas is painted before the walk, so a translucent `html` fades the page over its own background, not over nothing | a page that fades `html` |
| Groups past 32 deep, or a group whose copy finds no memory | painted opaque | a page that nests them that deep |
| A group's cost | each visible group copies its bounds once a paint | a page whose many faded boxes make painting slow, measured |
| `mix-blend-mode`, `backdrop-filter`, `filter` | other compositing operators, and a filter is a picture of what is under it | the slice that takes them |

## The box's edge (slice S3)

Four small things that share one painter: the border styles libflow kept
as solid, rounded corners, and the shadows a box and its text cast.

### How it is built

**The border styles.** libflow now keeps `dotted`, `dashed`, `double`
and `ridge` (they were drawn solid, and a ridge as a groove). A side is
still drawn a row or a column at a time, meeting its neighbours on the
diagonals, through `stroke`: a double side leaves its middle third; a
dotted one is squares a width across with a width between, a dashed one
dashes three widths long with as much between, both counted from the
box's edge so the rows line up; only the part the view shows is walked.
A ridge is a groove turned over.

**Rounded corners.** libgarb reads the four corner longhands and the
`border-radius` shorthand with its slash (Backgrounds 3 § 5.1). libflow
carries `flow_style_t.radius` and resolves it on a box with
`flow_box_radii`: a percentage of the border box, and every radius
scaled down by one factor where the radii along a side would together be
longer than it — the radii as written, none capped first, so a corner
keeps its shape (§ 5.5; Quinn, #203). yonder draws a box with any radius row by row in 1/256 px — each
row's span through the corner ellipses at its middle (worked as a
fraction, so no product passes 64 bits at any size libflow takes), whole pixels in
runs, the pixel at each end laid over at the share it is covered — so a
curve is smooth across. Its border is the ring between the border box's
shape and the padding box's, each inner radius its outer one less the
border beside it (§ 5.2), each pixel in the colour of the side the square
corners' diagonal rule gives it. A square box takes the old path,
untouched.

**Shadows.** libgarb reads `box-shadow` and `text-shadow` (`none`, or a
comma list as long as a page writes — pixel art is drawn with hundreds),
each shadow normalised to `inset`, four lengths and a colour. libflow
carries them as `flow_shadow_t` arrays, `text-shadow` inherited, lengths
in pixels at the element's own font size and currentColor its colour. A
box's outer shadows are painted under its background, the last first so
the first is on top; its inset ones over its background and under its
borders. A shadow is its shape — the border box moved and grown by its
spread, or for an inset one the padding box moved and shrunk — as a
coverage mask, blurred by three box blurs (near enough CSS's Gaussian,
standard deviation half the blur) over the view and the three blurs'
reach around it, so a part repainted alone is shaded as the whole (Quinn,
#203), cut to where it may fall (outside the
border box, or inside the padding box), and handed to yonder's new `mask`
verb. An outer square shadow with no blur takes a fast path: its shape
less the border box, as rectangles. A text's shadow is its run again, in
the shadow's colour at its offset, under it.

### Proof

libgarb's declaration corpus gains eleven corner and twelve shadow cases.
`radius_cases`: a percentage, the overlap scale, a slash, a zero corner,
and the radii scaled as written (Firefox's 50 x 50 and 45 x 45).
`sweep_grown_lists`: every allocation of a shadow read failed alone, each
answer whole or marked incomplete.
yonder, each worked by hand: the four border styles on a top side; a
4px square with 2px corners to the pixel; an unblurred square shadow as
two rectangles; a text shadow under its text. A blurred shadow is checked
by what a blur must do — its mask the blur's reach past the shape, half
on at the box's edge, nothing on the box, an inset one only inside — and a
repaint split in two shading every pixel as one whole repaint does, for
blurs of 8, 20 and 40 and an inset of 20; a box 31,000,000 px across
paints its corner under UBSan. In
the guest, a fixture of every style, four kinds of corners, four kinds of
shadow and two text shadows drew as headless Chrome draws it, but for
round dots (yonder's are square) and ridge's exact tones, which CSS
leaves to the browser.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| A text shadow's blur | the text is drawn by its verb onto the glass, with no mask to blur; a blurred text shadow is drawn sharp | a page whose glowing text matters |
| Dotted, dashed, double, groove and ridge round a rounded box | the ring is drawn pixel by pixel in its side's tone, every style solid | a rounded box with a dashed border that matters |
| Round dots | a dot is a square a width across | a page where it shows |
| Clipping to the curve | a picture behind a rounded box, its `overflow`, its hit test and its children are all still cut square | a rounded box whose picture or content shows past the curve |
| A shadow past its box's overflow | the walk prunes on overflow rects, which leave shadows out, so a shadow reaching into the view from a box outside it is not painted, and a group's bounds leave it out | a page whose shadow is cut where a box leaves the view |

## Gradients (slice S4a)

### How it is built

**libgarb keeps the gradient it already checked.** A linear or radial
gradient (`repeating-` too) was read against its grammar and thrown away;
now the IMAGE value carries itself (values.h): its geometry — an angle in
degrees, or `to` one of eight sides and corners; a radial's shape, size and
centre — then its stops, a stop written with two positions becoming two
and a hint a position alone. It is still written out as `name(...)`, so the
declaration corpus does not churn; what was kept is proven through
libflow's style dump. Conic gradients and image-set are taken by name and
drawn as nothing.

**libflow carries `flow_gradient_t`** in a background layer's image, beside
the url() — one picture per layer, never both (S4c, below). The geometry stays
as written (a `to` corner, percentages of the box), because only the box's
size turns it into a line. A currentColor stop is marked, not resolved:
it is the colour of the box the gradient is drawn on, which is a child's
own when the child takes the gradient by `inherit` (CSS Color 4 § 4.4).
Shadows are marked the same way — a text shadow is inherited — and a
background or border colour written as currentColor is resolved when the
element's style is finished, its mark kept for `inherit` to resolve again.

**yonder paints it pixel by pixel** where a box's picture goes, between its
colour and its inset shadows. Its tile is the padding box (the positioning
area), drawn across the border box and repeated as `background-repeat`
says, and cut to the curve of a rounded box. A pixel's middle is projected
onto the line — toward a corner it is square to the box's diagonal (§
3.1.1), its length |w·dx| + |h·dy| — or measured out along the ray of an
ellipse or circle sized by its extent. Stops are fixed up by § 3.5.3 (the
ends 0 and 1 when left out, none earlier than the one before, a run left
of colour stops left out spread evenly between the colour stops round it,
a hint among them keeping its place); a hint bends the mix with the power
§ 3.5.3 gives; a repeating gradient wraps between its first stop and its
last, and one with nothing between them is one colour, the average of its
stops spread from 0 to 1 (§ 3.6); colours mix premultiplied. A radial
whose ending shape has no size still paints (§ 3.2.3): a circle of no
radius as a very small one, an ellipse of no width as a narrow tall one,
one of no height as its last colour. An inline element's gradient is
painted on each piece of its line, the piece its tile. The pixels reach the face through a new verb, `pixels`,
blended there. **A body's gradient is the canvas's**, its tile the root's
box — a page shorter than the view repeats it in bands, as the browsers
do.

### Proof

`gradient_style_cases`: five gradients read into flow's (the initial
direction, a `to` corner, a stop with two positions and a hint, a circle's
one radius, a turn, currentColor, an ellipse's initial size). yonder, each
pixel worked by hand: a 4px black-to-white line (t = 1/8 … 7/8), repeated
hard stops, a 4px radial circle (its inner, edge and corner pixels), and a
body gradient tiled by a 4px root; Codex's #208 round — a hint that does
not move the stops left out, a repeating gradient of no length, ellipses
of no width and no height, an inherited gradient's and an inherited text
shadow's currentColor, an inline element's gradient (each failing on the
code before it), an angle of 1e20 degrees, background and border colours
by `inherit`, and libgarb refusing a circle's percentage or two radii, an
ellipse's one, and a negative one. In the guest, `/tests/pages/
pile3-gradients.html` — twelve gradients of every kind, a rounded one, a
translucent one over stripes, and the body's own — drew as headless Chrome
draws it.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| `background-size`, `-origin`, `-clip`, and `background-position` for a gradient | — | done in S4b, below |
| An inline element's gradient across its pieces | each piece is its own tile, where the browsers lay the pieces end to end and draw one gradient along them; the public tree does not say which piece is first | a gradient on a span that wraps |
| Conic gradients and image-set | no picture is made for them | a page whose look depends on one |
| Interpolation in another colour space (`in oklab`) | Color 4's spaces are not converted | a page that writes one |
| A gradient's cost | every pixel of it the view shows is worked out on every paint | a page whose gradients make painting slow, measured |

## Background size, origin and clip (slice S4b)

### How it is built

**libgarb keeps three more longhands.** `background-size` is `cover`,
`contain`, or a width and a height — each a length-percentage or `auto`,
the height `auto` when only one is written; `background-origin` is one of
the three boxes; `background-clip` is one of them or `text`. Like the other
background longhands each takes a list, one per layer (S4c, below).
The `background` shorthand sets all three now: a size after the position's
`/`, and one box for both the origin and the clip, or two for the origin
then the clip. `text` only clips, so it places nothing; written first of
two, the declaration is invalid.

**libflow carries them** in a layer as `fit` and `size[2]`, `origin`
(initially the padding box) and `clip` (initially the border box).

**yonder sizes one copy, places it, and cuts it.** `yonder_box_edge` gives
any of a box's three boxes, and `yonder_background_tile` turns the style
and the ORIGIN box into the copy (§ 3.9): `cover` and `contain` scale a
picture by the larger or smaller of the two ratios, keeping its shape; a
written side is px or a percentage of the origin box, an `auto` side keeps
the picture's shape against the other side, or is its own size when both
are `auto`. A gradient has no size or shape of its own, so it takes the
origin box's wherever a picture would take its own. A side of 0 draws
nothing. The position is then worked out in the room that copy leaves,
as before. The picture tiler scales each copy nearest-neighbour to its
tile. The colour and the picture are both cut to the CLIP box, and on a
rounded box the colour and the gradient follow that box's own curve: the
outer radii less the inset, as the padding box's are.

### Proof

libgarb's corpus: the shorthand with a size and one box or two, `text`,
`text` first (invalid), a size with no position (invalid), three boxes
(invalid), and each longhand alone and as a list. libflow's
`background_box_cases`: the computed values, an em size against the box's
own font, and a later shorthand resetting an earlier size. yonder, worked
by hand: a 2x2 picture doubled into a 4x4 tile; cover (160x80 from 20x10
in 100x80), contain centred, a gradient's cover, each `auto` rule, 0, and
a scale past INT32_MAX held there; the three edge boxes; and painted, a
gradient in a 2px tile repeated, one placed in and cut to the content box,
and a colour cut to the content box. In the guest,
`/tests/pages/pile3-backgrounds.html` — sixteen boxes over a 40x20
`banner.png` and gradients, two of them rounded — drew as headless Chrome
draws it, but for the smoothing below.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| Every layer of a list | — | done in S4c, below |
| `background-clip: text` | — | done in S7c, § The text clip, below (the canvas's background is still drawn across the canvas whatever the clip says, as § 2.11.1 says and Chrome draws it) |
| An inline box's origin and clip boxes | a piece in the public tree is its content area — inline padding and borders are neither laid out nor drawn — so every box of a piece is the piece itself | a page whose padded inline background matters |
| A content box with percentage padding | the tree does not carry the width a percentage padding resolved against, so it counts as none, as for an atom's content | a page whose content-box background sits wrong |
| A scaled picture's smoothing | — | done in § Zoom, below |

## Layers (slice S4c)

### How it is built

**libgarb keeps every layer.** A background longhand written for more than
one layer is a LAYERS value, one item per layer, the top first (values.h);
written for one it is that one value, as before, so nothing that read a
single layer changes. The `background` shorthand gives each longhand its
list — a layer that leaves something out gives it the initial value — and
so does `background-position`, split into its two longhands' lists. The
lists grow outside the arena, so running out of memory partway is said by
hand: the read is the whole one or marked incomplete.

**libflow keeps a list per property**, as CSS computes them
(`flow_backgrounds_t`), each at least one long; `inherit` and the other
wide keywords take a whole list. `flow_background_layer` puts layer *i*
together: the images say how many layers there are, a shorter list is
repeated to fill them and a longer one cut (§ 3.1). An entry flow cannot
read is that property's initial value.

**yonder paints from the bottom up**: the colour first, cut by the BOTTOM
layer's clip, then each layer cut by its own clip and placed in its own
origin box. A gradient is painted as before; a picture is asked of the face
by its layer's index, and the face draws that layer's picture. With no
picture or gradient from any sheet, a `background` attribute's picture is
the one layer, as it was. The canvas paints its owner's layers the same way
across the whole view.

**A list is copied into every element its declaration reaches**, which is
the one cost of styling a page can multiply — a sheet's 400 layers on 500
elements (Codex, #210). Every such list — layers, gradients, shadows, font
families, grid tracks, areas and line names — comes out of a budget of its
own (`list_alloc`, `F_STYLE_LISTS_BUDGET`, 32 MB, far past any real page's);
past it, a declaration is dropped as one libflow cannot read is, so a
hostile page is styled plainly and still laid out, where an arena cap would
have blanked it.

### Proof

libgarb's corpus: the shorthand over two layers with the colour in the
last, an empty layer and a trailing comma (both invalid), the longhands as lists, and the
position shorthand split into lists. `sweep_grown_lists` now also fails
each allocation of a five-layer longhand, a five-layer shorthand and a
six-layer position, alone. libflow: three images with one repeat, two sizes
in turn and four clips cut to three; the shorthand's layers; `inherit`.
yonder, by hand: two gradients painted bottom first; the colour taking the
bottom layer's border-box clip while the top layer is cut to the content
box; a picture layer asked for by its index over the gradient under it. In
the guest, `/tests/pages/pile3-layers.html` — six boxes and a body of two
layers — drew as headless Chrome draws it.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| An attribute's picture under a sheet's `none` | a `background` attribute's picture shows whenever no sheet gives the box a picture or gradient, though a sheet's `background-image: none` should hide it | a page that hides a table's background attribute that way |

## Zoom (slice S5, with image-rendering)

Chris asked for zoom before pile 3 ends (9/27, Hacker News's tiny type):
not CSS's `zoom` property, but the browser's own — Ctrl and + or -.

### How it is built

**libflow lays the page out in device pixels**, as a browser does for a
screen denser than 96 dpi. `flow_env_t.zoom` says how many device pixels a
CSS pixel is (thousandths), and a CSS pixel becomes device pixels at the few
places one enters: the unit converter (`length_px` — px, pt, in and the
rest, and vw and vh), the Rendering chapter's own lengths and the HTML
attributes' (`px()`, `map_dimension`, the border widths), `medium`, a
replaced element's own size, the frame and broken-picture defaults, and the
viewport's height. An em, a rem and a percentage follow by themselves. So a
`16px` font at 150% is shaped at 24 px, and drawn as sharp as at 100%:
nothing is stretched.

**yonder** lays out at the window's width in device pixels and judges the
sheets at the window's size divided by the zoom — so a media query sees
the narrower CSS viewport and a page's narrow layout arrives as it does in
Chrome. The zoom is the WINDOW's (Chris, 10/3): kept across navigation,
started from the settings' default. Ctrl with = or + and Ctrl with - step
through Chrome's zooms (25% to 500%), Ctrl+0 goes back to the default, and
Ctrl and the wheel step too, wherever focus is in the window. A zoom
change keeps the element at the top of the view there, its offset scaled.
The status line says `zoomed 125%` when it is not 100%. **The Settings
window** gains a row, `Zoom every page to [ ] %`, saved as `zoom =` in
`yonder.conf`; the shared settings dialog grows taller for a body with
more rows. Presses in a row are laid out ONCE, after the events that came
with them (the same deferral a window drag gets).

**Zoom found the text engine's two ceilings** (Chris on the P5, 10/3: a
page zoomed to 500% stayed broken when zoomed back). Every size of a face
is a face of its own, and the text engine COPIED the whole font file for
each — 0.75 MB of DejaVuSans per size — and refused a 65th live face. A
page using thirty-odd sizes in four styles met both at 100%; a zoom asks
for every size again while the tree on screen still holds the old ones.
Both were booked in 04-web-faces.md, and both are paid: a context keeps
ONE copy of a file's bytes, shared by every face opened from the same
bytes and freed with the last (`text_cache.c`'s `text_file`, matched by
content, never by the caller's pointer), and `OS64_FONT_FACE_MAX` is 1024
— measured with FreeType on the host, a face costs 22 KB once its file is
shared (sixty sizes of DejaVuSans: 2.2 MB, where the copies made it 47).
A page of 32 sizes now lays out complete in 1.5 s where it stopped partway
in 8. And when a relayout still comes back incomplete while the old tree
exists, yonder frees both and lays the page out once more in the whole
budget, so a page is never stuck behind the tree it is replacing.

**`image-rendering`** (Images 3 § 5.3, inherited): `pixelated` and
`crisp-edges` — and the spellings older pixel-art pages wrote,
`-moz-crisp-edges`, `-webkit-optimize-contrast`, `optimizeSpeed` — keep a
scaled picture's pixels square; every other value SMOOTHS it: each pixel is
mixed from the four source pixels round where it falls, by how near each
is, their colours weighed by their alphas so that a transparent neighbour
lends no colour. A picture drawn at its own size is copied either way.

### Proof

libgarb: the property, an SVG spelling, Chrome's, and a word it is not.
libflow: `pixelated` inherited, `auto` back to smooth, Chrome's spelling
crisp; at zoom 2, a box whose `1em` margin, `50px` width, `1px` border and
`25vh` height are each doubled, and a picture's attribute size and its own
size doubled. yonder, by hand: black to white drawn twice as wide (0x40
and 0xbf between), red beside transparency (0xbf over black, where mixing
the transparent black in would give 0x8f), and a picture at its own size
copied. In the guest: Ctrl+=, Ctrl+-, Ctrl+0 and Ctrl+wheel on the
corpus's Wikipedia page and `/tests/pages/pile3-zoom.html`, which drew at
150% as headless Chrome does at a device scale of 1.5; the Settings row
applied at once and saved `zoom = 125` to /home's `yonder.conf`. The text
engine: `test_text_host`'s "one copy per file" (two sizes, one file, the
file outliving the first face; equal bytes from another buffer the same
file, and the same buffer with new bytes a new one, as "reload identity"
already held), the family cache pinning `OS64_FONT_FACE_MAX` faces with
runs and refused one more, and the whole font battery green, real FreeType
included. In the guest, a page of 32 sizes in four styles laid out complete
at 100%, at 500% and at 100% again — before, INCOMPLETE at all three, and
blank on the way back.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| A zoom per site | one zoom for the window (Chris, 10/3) | a person who wants one site bigger than the rest |
| Form controls at the zoom | — | done in § The Web face, below |
| A picture shrunk to less than half | four pixels are mixed, so a picture shrunk far shimmers where the browsers filter its whole area | a page whose thumbnails shimmer |
| Zoom's cost | more device pixels to paint, and smoothing costs more than copying | the deferred painting work |
| Heavy pages at a high zoom | on the P5 (Chris, 10/3) news.google.com breaks up from 250%, and news.yahoo.com goes wrong at 200% but recovers once zoomed back; the cause is not measured — the page's narrow-viewport sheets at a CSS width of 331 px and under, or a budget the layout still meets, are the suspects. A reading zoom of 200% or less is what yonder is for (Chris) | a page a person needs past 200%, or one that breaks at 200% or less |
| Text past 1280 device px | the font engine's largest face (OS64_FONT_PIXEL_MAX, raised from 256 for zoom — Chris, 10/4 — so any size a page draws at 100% is shaped true at 500%); a larger run is shaped at 1280 and laid out at its own size | a page writing a font over 256 px that a person zooms |

## The Web face (slice S6)

A page's form controls drew in the desktop's Interface font, at its size,
whatever the zoom — the debt § Zoom booked. Chris chose the cure (10/3): a
**Web** button in the Appearance Workshop's Fonts tab, so the choice is the
system's and any later browser shares it — and then its shape: the
desktop's applications keep the Interface font for their own controls, and a
control decides its font from a property the browser sets.

### How it is built

**`fonts.conf` gains the Web setting**: `web.face` (and `web.fallback.1`,
`.2`) is the face a browser draws a page's form controls in, and `web.size`
is the page's default font size, CSS's `medium` — what Chrome calls "Font
size". Unset, it is the shipped DejaVu Sans at 16. It is the shape of a role
and rides after the three in the config (`OS64_FONT_CONFIG_WEB`), but it is
**not a role**: no window's role set carries it, so a program that never
shows a page never loads it. A browser opens it itself,
`os64_font_config_web_prepare(context, config, px)`, at the size it needs.
The session carries it with the roles, so a Workshop draft read back keeps
it, and Save writes it; Apply publishes it, but yonder reads it when it
starts and does not follow it.

**libui gains the application's face.** `os64_ui_font_app(ui, set, role)`
lends a window a face beside the role set the settings choose;
`os64_ui_widget_app_face(ui, w, true)` puts a widget in it, and the role
`OS64_UI_FONT_APP` measures in it. A widget that asks draws in it instead
of its class's role, a textview's Document role included; one that has not
asked is untouched, so yonder's toolbar keeps the Interface font while its
page's controls change. An adoption — a new Interface or Document font —
leaves the lent face alone. A new lend lets the widgets wearing it go of
the runs laid out in the old face, keeps what their next paint reuses (a
list's row slots), and has them measure a scroll again in the new one — so
it is cheap enough to follow a zoom, and allocates nothing. Nothing lent, the application's role is the Interface role.

**yonder** reads `web.size` as the page's `medium`, and lends its window the
Web face at the size a browser gives a control — 13/16 of `medium` (Chrome's
small-control, 13 px at 16) — times the zoom, again at every zoom change.
A select's box is measured in that face, and handed to libflow in CSS
pixels, since libflow scales a replaced box by the zoom itself. Its frame,
row padding and label inset are the listbox's own, which libui draws in
device pixels, so they stay that size at any zoom (BROWSER_DEBTS.md). A Web face
that will not open leaves the controls in the Interface font, and yonder
says so once on its terminal.

**The Workshop** has a fourth button, Web, and a fourth specimen line drawn
in the Web face lent to the preview window. A Web face that will not open
refuses only a candidate that changes it: the shipped default is absent
without the fonts lot, and that must not stop anyone choosing the roles.

### Proof

Host: `test_ui_text_host --real`'s "application face" — a lent face worn by
the widgets that asked and no other, a list's rows and a button's height
re-derived at the lend, a new lend dropping only those widgets' runs, an
adoption leaving it, a widget leaving it, nothing lent being the Interface
role, and the window owing nothing at release. `test_font_config_host`'s Web
setting: the default, a round trip through encode and decode, refusals that
name the Web line (and not a role) — every refused allocation of the Web
door's set among them — the face opened at sizes past both ends of the
configurable 8..96 and clamped only outside the engine's 1..1280, a builtin
face at 16 whatever is asked, a broken face refused by the Web door and not
by the role door. `test_ui_text_host --real`'s "application face changed":
a field, a view and a list re-lent a smaller face end scrolled as fresh ones
would be, the list keeping its slots. `test_appearance_session_host`:
the Web setting survives the session. In the guest:
`/tests/pages/pile3-controls.html` with the controls in DejaVu Sans at 13 px
beside a bitmap toolbar; at 200% the controls' text doubled and the
select's box grew with its rows, and back at 100% as before; the Workshop's Web button
previewing DejaVu Sans Mono Bold, Save writing `web.face` and `web.size`
beside unchanged roles, and yonder started afterwards drawing the controls
in it; `web.size = 20` giving a 20 px paragraph and 16 px controls.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| yonder following a Web change live | it reads `fonts.conf` when it starts, as it reads the families | a person who changes the Web face with a page open and expects it to follow |
| A page's own `font` on a control | controls wear the Web face at the small-control size, whatever the page's CSS says | a page whose styled inputs look wrong |
| A checkbox at the zoom | its box grows, but the tick is the theme's size | a page of checkboxes at 150% or more |
| Textviews in the application's face | no browser control is one (a `<textarea>` is a field) | a `<textarea>` drawn as a textview |

## Transforms (slices S7a and S7b)

### What CSS asks (CSS Transforms 1 and 2)

`transform` moves what an element paints without moving it in the flow:
its layout box stays where it was laid out, and everything the element
paints — its own background, its children, its text — is drawn through a
matrix. The matrix is the element's functions (`translate`, `scale`,
`rotate`, `skew`, `matrix`, and their 3D forms) multiplied left to right,
about `transform-origin` (the box's centre unless it says otherwise), and
before them the individual properties `translate`, then `rotate`, then
`scale` (Transforms 2 § 6). Percentages in a translation are of the
element's own border box. A transform that is not `none` also:

- makes the element a STACKING CONTEXT, painted where a positioned box of
  `z-index: 0` would be;
- makes it the CONTAINING BLOCK of every absolutely positioned AND every
  FIXED descendant — a fixed box inside a transformed one scrolls with it;
- adds where its overflow is drawn to its ancestors' scrollable overflow;
- moves where it is HIT with where it is drawn.

It applies to a TRANSFORMABLE box: block-level or atomic inline-level. A
plain inline box's transform does nothing.

Without `perspective` a 3D transform is drawn FLAT: the point (x, y) goes
where the 3D matrix sends (x, y, 0), and z is thrown away. So any 3D
matrix with no perspective in it is a 2D matrix as far as the page shows,
and `translateZ(0)` — the "make it a layer" incantation on most of these
pages — is the identity.

### What the pages ask (measured 10/11)

The census records on the P5 (93 pages) report `transform` on **63** of
them and `transform-origin` on 51. Their records name properties, not
values, so the values were counted in the sheets of the census's
mainstream sites and the pages this pile already knows (Bing, Google,
Instagram, Radar Online, Wikipedia, GitHub, grokipedia, Hacker News,
YouTube, osgallery, danlegt): of about 945 STATIC transforms — outside
`@keyframes` and outside rules for `:hover` and its kind, which wait for
the animations slice to matter — 482 are translations (`translate(-50%, -50%)` centring,
drawers slid off-screen), 188 quarter turns and mirrors (`rotate(180deg)`
chevrons, `scaleX(-1)`), 223 scales (hover lifts, `scale(1.05)`), and
197 free rotations, skews and matrices. About 4% are 3D, nearly all
`translateZ(0)` or `translate3d(x, y, 0)`. The individual properties
appear on osgallery, YouTube, GitHub and Google.

**Exact is a property of the matrix, not of its spelling.** grokipedia's
drawer, Tailwind 3's `-translate-x-full`, is
`translate(var(--tw-translate-x), var(--tw-translate-y)) rotate(var(--tw-rotate))
skew(…) skewY(…) scaleX(…) scaleY(…)` with every variable but the first
at 0 or 1: a pure translation written as six functions. Counted by
matrix, the exact share is larger than the 71% above.

### The split

A matrix that only translates moves pixels: everything the painter does
today, drawn somewhere else. Anything else — even a quarter turn, since
text is rasterised upright — needs what yonder has never had: a box's
painting drawn into a surface of its own and then laid onto the page
through the matrix. So:

- **S7a, the model and the translations.** The properties are read, the
  matrix is built, and every consequence above is made: stacking, the
  containing block, overflow, hit-testing. A translation is drawn
  exactly. Any other matrix is drawn moved to where it puts the
  transform-origin — upright and unscaled, but where it belongs, so a
  scaled or rotated thing about its centre stays where it was, and
  `translate(-50%, -50%) scale(1.1)` is centred. `@supports` says no to
  such a value while S7a is all there is.
- **S7b, the painter.** A transformed stacking context is painted into a
  surface and laid onto the page through its matrix: exactly (an index
  map) for a quarter turn or a mirror at whole pixels, bilinear
  otherwise; clips become the matrix's quadrilaterals; hit-testing goes
  back through the inverse (§ Transforms, the painter).
- **S7c, `background-clip: text`**, on S7b's surfaces: a background
  drawn only where the text is IS a surface of its own laid through a
  mask (Chris, 10/10: done for real with the transforms work).

## Transforms, the model and the translations (slice S7a)

### How it is built

**libgarb reads five properties.** `transform`, `transform-origin`,
`translate`, `rotate` and `scale` (values.h says each one's shape). A
transform list is a `GARB_V_FUNCTIONS` of `GARB_V_FUNCTION`s, each named
in lowercase; transform-origin's keywords are kept as the percentages
they are. Angles are a value kind of their own (`deg`, `grad`, `rad`,
`turn`, and in a function a unitless `0`, which `rotate(var(--tw-rotate))`
relies on); an angle inside `calc()` drops the declaration, as a length
of the wrong kind does. Every 2D and 3D function is read. A value that
does more than move the box is APPROXIMATED until S7b, judged a function
at a time: a translation is not, nor a function that does nothing as
written (Tailwind's `rotate(0)` and `scaleX(1)`) or reaches only z
(`translateZ`, `scaleZ`); a rotation, a scale, a skew, a matrix that is
not a translation, and `perspective()` are — `@supports` answers what is
drawn. A `matrix3d()` that reads z into x, y or w is approximated too,
whatever else it is: no function passed then reads z, so a `translateZ`
cannot combine with another into more than a move. A `calc()` of numbers
or percentages is judged by what it comes to, worked as libflow works it,
so `scale(calc(100%))` is as exact as `scale(1)`.

**libflow keeps the list, each function its matrix.** `flow_style_t`
gains the transform (a list in the style-lists budget,
`F_STYLE_LISTS_BUDGET`, so a page of thousands cannot multiply it past
bounds), the three individual properties beside it, one function each,
and the origin. Every function is made its 4x4 matrix when the style is
computed, at the element's font; only a translation's percentages wait
for the box. libflow links libmath for the turns.

**A transform is a FRAME.** Frames already say "this box, and everything
inside it, is drawn somewhere other than where the flow put it" — a
sticky push, a scroll — and every door that asks where a box is goes
through them (`flow_box_doc_offset` and what is built on it: the doc rect,
the clip, hits, group bounds, the face's verbs). A transformed box gets a
frame of a third kind, `F_FRAME_TRANSFORM`, that holds its matrix, built
once its border box is known: `T(origin) · translate · rotate · scale ·
the list · T(-origin)`, in doubles. Like a sticky frame it moves its own box as well as what is
inside. A box both sticky and transformed has the transform frame inside
the sticky one: the matrix is about the box where it sticks. The matrix
is made in one function from the style and the border box, so the
animations slice can make it again on a tick without laying out again.

In S7a a transform frame's offset is what the matrix does to the
transform-origin, rounded by the painter's rule — the whole of the
matrix for a translation, the approximation above for the rest — so
every door that already follows frames follows transforms, and S7b turns
the offset into the matrix itself. That offset is the product of the
functions' own translation, whatever the origin is: in S7a the origin
moves nothing, and is the matrix's alone.

**A transformed box is stacked.** `makes_context` adds a transform, and
`stacked` takes in every transformed transformable box: block-level, and
— new — an ATOMIC inline box, whose layer is the atom WHOLE, the inline-
block's block box inside it included. The inline walks (paint and hit)
skip a stacked atom as the block walks skip a stacked block, and the
stack paints it in its context at z 0. The transform belongs to an
element's OUTERMOST box: the atom, and never again the block box inside it.

**A transformed box contains its positioned descendants.** Its element is
an anchor for absolute boxes, as a positioned one is, and a FIXED box
takes the nearest transformed ancestor's padding box as its containing
block instead of the viewport (`position_box`); it is then not `fixed` in
the public tree — no longer drawn in viewport coordinates, it scrolls
with the box that holds it. A plain inline's transform does nothing, so a
positioned one anchors absolute boxes and never a fixed one.

**Where it is drawn reaches, and where it was laid out does not.** A
transformed box's overflow, moved by its frame and cut by the clips on
the way out, is what its scroll container's range or the page's extent
sees, and so is a positioned box's inside one. Layout's own edges, and a
parent's overflow, leave a transformed box out, as they leave a
positioned one: a dialog centred by `translate(-50%, -50%)` does not
lengthen the page by the half it was moved back. A transformed root
is no stacked box, being the first context already, so its moved
overflow is reached apart; layout's unmoved count of it stays.

**yonder** needs nothing new to draw a translation: the verbs are in page
coordinates already, and a moved box's offset already moves them. A form
control in a transformed box is placed at its moved rect, so a slid
drawer's search field slides with it; the image-map point and the DOM's
`getBoundingClientRect` follow the offset as they already do.

### Proof

`tools/test_garb_*`: the five properties read and printed back, every
function and both angle spellings, a unitless 0 angle accepted and a
unitless 5 refused, a 3D list, `perspective()` and an S7b value
approximated in `@supports` and a translation not, a `matrix3d()` that
reads z approximated, a calc angle dropped, and a calc() holding a length
where only a number or percentage goes dropped (`scale(calc(0px))`).
`tools/test_libflow_transform.inc`, by hand from the standard: a
translation's percentages of the border box; the layout unmoved and the
drawn rect moved; the order of the individual properties against the
list; `translate(-50%, -50%)` centring an absolute box; a scale and a
rotation drawn where they were, and a translation after them scaled and
turned; `matrix()`'s translation in CSS pixels; a transformed block
stacked and painted after the flow it overlaps, and hit there; a
transformed box a stacking context; a transformed inline-block stacked as
one layer, painted once and hit moved; an absolute child placed against
the transformed box and a fixed one scrolling with it; what clips it from
outside staying put; the page's extent and a scroller's range reaching a
translated box, and not where it was laid out; a sticky box transformed;
grokipedia's Tailwind drawer off the page; a plain inline box's transform
doing nothing. `tools/test_libflow_transform_mutants.py` breaks each rule
in turn and the cases catch every one. They also hold a scale written
`calc(50%)`, a sticky transformed box sticking through its
scroll container, a transformed root reaching the page (written fixed or
inline, it is still the root: in the flow, and blockified), and a
positioned plain inline anchoring no fixed box. In the guest, a fixture beside
headless Chrome: a slid drawer, a centred dialog, an inline-block nudged,
a fixed bar inside a transformed panel — and grokipedia read from the P5.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| Every matrix but a translation drawn upright | the painter draws into the page, never into a surface of its own | S7b |
| `background-clip: text` | — | done in S7c, § The text clip, below |
| `perspective`, `perspective()`, `transform-style: preserve-3d`, `backface-visibility` | a flat drawing is the answer without them; with them it is a 3D renderer | a page whose 3D card flip matters |
| `transform-box` | the border box is the default for HTML, and SVG is not drawn | SVG |
| A transform on a table's rows, row groups and columns | they are no containing blocks here, as `position: relative` on them is not (POSITION.md § Booked) | a page whose transformed row matters |
| A table's transform moving its captions | the transform sits on the table's own box, and its captions are beside it | a transformed table with a caption |
| Calc angles | libgarb's calc has no angle type | a page that writes one |

## Transforms, the painter (slice S7b)

S7a built the model and drew a translation exactly; every other matrix
was drawn upright at its offset. S7b draws a FLAT matrix through itself:
its box's painting is made in a surface of its own, in the box's own
coordinates, and laid onto the page through the matrix.

### What is drawn through

A matrix is FLAT when its flat drawing (Transforms 2 § 4: x' = (m0 x +
m4 y + m12) / w, w = m3 x + m7 y + m15) has a w that does not depend on
x or y — m3 = m7 = 0 — which every 2D function, every 3D rotation,
scale and translation, and `translateZ(0)` give. A flat matrix is an
affine map of the plane, and that is what the surface is laid through.
What is NOT flat — `perspective()`, a `matrix3d()` whose w reads x, y
or z — stays upright at its offset, as in S7a, and `@supports` says no
to it; a function is judged so whatever comes after it, since w read
from z by one function and z written from y by another is a
perspective too. Everything else `@supports` now says yes to.

A translation stays an OFFSET, as in S7a: no surface, no cost. A frame
whose matrix is flat and more than a translation is DRAWN THROUGH
(`flow_frame_t.through`), and S7a's `upright` is left meaning "not
flat".

### Where a box is painted, and where it is

Until now one rule (`flow_box_doc_offset`) said both, and through a
turn they part:

- **Painted**: a box inside a drawn-through frame is painted in that
  frame's own coordinates — where the flow put it, every frame inside
  the drawn-through one applied, none outside. `flow_box_doc_offset`
  keeps this meaning, so the painter's walk, its verbs and its clips
  are unchanged inside a group: nested groups each paint in their own
  coordinates, and each is laid onto the one outside.
- **Is**: what a door that meets the world asks — a hit, a box's rect
  for `getBoundingClientRect` (but not `offsetTop` and its kin, which
  leave every transform out: `flow_box_layout_offset`), a control's place, `flow_box_covered`,
  revealing a fragment — goes through every drawn-through frame,
  innermost first. `flow_box_doc_rect` is the bounds of the box's four
  corners so mapped (CSSOM View's answer for a transformed box), and
  `flow_hit` takes the point back through each inverse as its walk
  enters a drawn-through frame; a point a frame cannot invert (a scale
  of 0) hits nothing inside it.

### The groups

A drawn-through box is a GROUP (`flow_visit_groups`), opened before its
first box and closed after its last, as an opacity group is — the same
group when it is both. Its bounds are in its OWN coordinates, and the
face asks `flow_box_through(box, scroll, m, &clip, &upright)` for the 2D
affine matrix `m` (a b c d e f) from them to the coordinates of the group
it sits in, what clips it from outside in those outer coordinates, and
where to draw it upright instead. The walk culls inside a group against
the viewport taken back through the matrices — the bounds of its corners,
one pixel more all round as the surface is (below) — joined with where an
upright drawing would put it, since the face may refuse the group after
the walk has passed. A matrix that collapses an
axis (`scaleX(0)`, `rotateX(90deg)`) draws nothing and is hit nowhere;
any other, however small or large, is undone — by one function,
`flow_affine_invert`, whose determinant and translation are worked
exactly, and which the face
lays sheets with too, so painting, laying and hits never disagree.
`flow_box_point` takes a page point into a box's own coordinates (a hit,
an image map's area), and `flow_affine_rect` / `flow_affine_rect_back`
send a rect through a matrix and back, for the painter's view.

### The face: two backdrops

yonder's surfaces have no alpha channel, and its text and pictures
blend as if what is under them were opaque. A group painted onto
nothing would need every primitive taught otherwise. It does not need
to be: SOURCE-OVER IS AFFINE IN THE BACKDROP. Whatever a group paints,
each channel of each pixel comes out B + T·D for the backdrop D, and
painting it twice — once on black (D = 0, giving B) and once on white
(D = 255, giving B + 255·T) — gives B and T for every channel exactly,
nested opacity groups, text edges and pictures included. So a
drawn-through group is painted into two surfaces, each verb drawn into
both, and laid onto the backdrop as B + T·D through the matrix — and
under its own opacity a, as a·B + (1 − a + a·T)·D.

Laid through the matrix, each pixel of the destination is taken back
through the inverse and sampled from the two surfaces: by INDEX when
the matrix is a quarter turn or a mirror and its translation whole
(an exact copy, so a `rotate(180deg)` chevron is the chevron), else
BILINEAR, B and T interpolated apart (B is premultiplied by its own
making, so they interpolate as they should). The destination is cut by
the group's outer clip and the view.

A surface is the group's bounds — each layer grown by its INK, at least
as far as the painter draws past it: its own shadows (a box shadow by
its spread, its offset and 3 (blur / 2) — its three box blurs — and not
at all when its spread collapses it; a text shadow, drawn sharp, by its
offset alone), its glyphs' overhang, its decorations and marker shape,
and what the boxes inside it hand up, each cut to its clip (a shadow is
in no box's overflow) — cut to the view taken back through the matrices,
one pixel more all round for the bilinear sample at the edge: what can
show.
At most 4096 pixels a side and 2,097,152 in all (8 MiB each), at most 16
deep, and within half of what a 32-bit coordinate can say (its offset is
added to every coordinate the glass draws); a group past that, or whose surfaces find no memory, is painted
straight onto the backdrop at its S7a offset — upright, inside what
clips it from outside, never missing. A group none of which can show is
not painted at all. An opacity group inside opens on both sheets or on
neither, so the two never disagree. The painter keeps 64 groups open;
past that an opacity group is painted opaque, and a group drawn through
its matrix not at all.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| A box scaled up softens | its surface is painted at its own size and stretched; Chrome paints at the scale | a page whose scaled-up text matters |
| A control in a drawn-through box is drawn turned but edited upright | a live widget is drawn on the glass, not through a matrix | a page with a turned form |
| A group the face refuses (past its sheet limits, out of memory) is drawn upright but hit through its matrix; inside a group that was laid, it is cut by that group's sheet, sized for it turned, and an enclosing opacity group saves no backdrop where the upright drawing falls, so it is opaque there | libflow answers hits and bounds for the page as written; which groups a face refuses is decided at paint time, and only it knows | a page whose turned group passes the sheet limits and is clicked, or is cut |
| Nested turned groups whose matrices only compose to something ordinary (`scale(1e308)` inside `scale(1e-308)`) are hit and measured where they are, but painted nowhere | each group's sheet is laid onto the one outside in whole pixels, and the inner one lands past what a pixel can say; drawing it would mean composing the matrices across groups, not a sheet per group | a page that nests reciprocal extremes |
| THE NUMERIC CONTRACT: transforms are as accurate as a browser's and strictly safe. Numbers a page actually writes are composed, undone and drawn right; past that — a composite that, w divided out, is past a double (`scale(1e200) scale(1e200)`), a cancellation finer than a long double's 64 bits (`matrix(1+2^-52, 2^104, …)` lists), a translation past what 26.6 holds (`matrix(1,0,0,1,1e307,0)`), a z entry so large that dividing the matrix down leaves its w 0 (`scaleZ(1e308)` twice) — a box may be drawn upright, or not at all where its matrix comes out singular, though `@supports` said yes; and a page scrolled past half a 32-bit coordinate (about 1.07 billion px; Chrome's layout saturates near 33.5 million) may lose a turned group nothing clips, its absent clip being the plane only that far. Any input, however hostile, is never a crash, a hang or undefined behaviour | exactness past a browser's has no end: each widening of the arithmetic (#258 rounds 13 to 17) met an input just past it, and Chrome composes in doubles; libgarb judges each function's numbers, not their product | a look at the whole contract, booked by Chris (2026-10-12); a page that needs more |
| THE BOUNDS CONTRACT: a group's bounds hold AT LEAST its ink, never less. Less is a bug, fixed (#258 rounds 15 to 19: shadow reach, glyph overhang, decorations). More is the stated behaviour: a fully transparent shadow (a `transparent` colour or `currentColor`), a positioned box under `opacity: 0` that is never painted, and the like are still counted, and a group they push past the sheet limits is drawn upright | counting only what is drawn means following every way a thing can paint nothing — each narrowing met the next (round 19) — and the cost of counting more is the degraded drawing, never a wrong one | a look with the numeric contract, booked by Chris (2026-10-12); a page whose invisible content refuses a turned group |
| A large `visibility: hidden` box inside a turned group still sizes its sheets | the bounds come from layout overflow, which keeps what is hidden; a page whose hidden content pushes a turned group past the sheet limits gets it upright | a page that does |
| A sticky box, a scroll container's bars, or a fragment revealed inside a drawn-through box | each is worked against the page's viewport, which the box's own coordinates are not | a page that scrolls inside a turned box |
| `perspective` and non-flat matrices | carried over from S7a | a page whose 3D card flip matters |
| `background-clip: text` | — | done in S7c, § The text clip, below |

### Proof

libflow, by hand: the matrices (turns about the centre, scales, a
skew, a 3D rotation flattened, a perspective drawn upright) and the
page reaching their corners; a turned box's doc rect the bounds of its
corners, its child's too; a hit inside a turned box taken back through
it, and where it was laid out no longer it; nested turns; what clips it
from outside staying put; the culling; a turned translucent box one
group. libgarb: `@supports` yes to a turn, a scale, a skew and a 3D
rotation, no to perspective and a w-reading matrix3d. yonder's painter:
a turned box asks to be laid through, its own bounds cut to what shows;
refused, it is drawn upright where its origin goes. The compositor is a
pure function (`warp.c`, `tools/test_yonder_warp_host.sh`) under ASan
beside references written apart from it: content of every alpha
painted over black and white and laid in place lands as the content
over any backdrop (opaque and empty paint exactly); a quarter turn and
a mirror copied by index, pixel for pixel; a scale and a turn bilinear
against the reference to within one level; the opacity rule; the clip;
a matrix that cannot be undone laying nothing.
`tools/test_libflow_transform_mutants.py` breaks each rule, painter's
included. In the guest, the fixture's last two rows beside headless
Chrome: turned, grown, a half-turned chevron, a mirror, a skew.

## The text clip (slice S7c, `background-clip: text`)

A background clipped to its text is the gradient headline: a gradient (or a
picture) painted only where the letters are, the letters themselves left
transparent so the gradient shows through them. Chris, 10/10: done for
real, on S7b's surfaces.

### What CSS asks

Backgrounds 4 § 2.8: "The background is painted within (clipped to) the
intersection of the border box and the geometry of the text, including any
decorations and emphasis marks in the element and its in-flow and floated
descendants. The text color has no effect on the painting of the
background." The background colour is cut by the bottom layer's clip, as
for any clip (§ 2.1), so `background-clip: text` with a colour clips the
colour to the text too. On the root it has no effect (§ 2.11.1, which S4b
already follows).

The page also has to stop the text covering what it is clipped to, and the
web does that with a prefixed property. The Compatibility Standard defines
both names pages write: **`-webkit-background-clip`** is a legacy name
alias of `background-clip` (§ 3.4.1), and **`-webkit-text-fill-color`**
(§ 3.4.6.1, `<color>`, initially `currentcolor`, inherited) is the colour a
glyph is FILLED with — a list marker's symbol too, a disc as much as a
number, as Chrome draws it (#262, Codex) — and `color` still decides
decorations, shadows' `currentcolor`, and everything else. The idiom is
`-webkit-background-clip: text; -webkit-text-fill-color: transparent`, and
a page that writes only the standard names writes `color: transparent`.

libgarb drops every prefixed name today, on the rule that one is one
engine's dialect. These two are not: the Compatibility Standard makes every
engine read them, so they are read as their standard twins are, and the
diagnostics rule (YONDER_DIAGNOSTICS.md: a prefixed name is not counted as
missing) is unchanged — it counts, it does not read.

### What Chrome draws

The spec leaves the edges open, so Chrome was asked (two probe pages,
headless, 2026-10-12; they become this slice's fixture):

| The case | Chrome |
|---|---|
| decorations: underline, wavy, line-through | in the clip: an underline of a transparent colour shows the gradient |
| a text shadow | not in the clip; drawn with the text, over the clipped background |
| a relatively positioned child, with or without `z-index` | in the clip |
| an absolutely positioned child | not in the clip (out of flow) |
| a float, a block child, a plain inline-block | in the clip |
| a TRANSFORMED child | not in the clip — its text disappears |
| a child below full opacity | in the clip, at FULL strength: the mask is the glyphs, not how they are drawn |
| a list marker's bullet | not in the clip |
| a child with a background of its own | painted over the clipped background, as painting order says |
| text past the border box | cut at the border box |
| a span that wraps | one gradient laid along the pieces end to end |

That table is the set this slice draws. The last row is the debt S4b
already booked (an inline box's layers are placed on each piece); it stays
booked, and a wrapped gradient span shows each piece its own gradient.

### How it is built

**libgarb.** `-webkit-background-clip` is read as `background-clip` (one
alias table, the Compatibility Standard's § 3.4.1 rows that are drawn —
this one). `-webkit-text-fill-color` is a new inherited colour property.
`background-clip: text` leaves `kApproximated`, so `@supports` says yes.

**libflow.** The style gains the fill colour (`currentcolor` until set,
resolved where `color` is). And libflow owns WHICH TEXT a box's clip holds,
because that is CSS and the table above, not drawing:
`flow_visit_text_clip(box, visit, ctx)` hands `visit`, in tree order,
every TEXT box the clip of `box` holds — the box's own and its
in-flow and floated descendants', skipping an out-of-flow descendant, a
transformed one and everything in them, and markers. A block's are its
subtree. A SPAN piece's are the text on its own line whose element is the
span's or inside it, since a piece is the text's sibling on the line, not
its parent.

**The painter.** A run of consecutive layers clipped to the text (and the
colour, when the bottom layer is one of them) is drawn as one TEXT CLIP:

1. `clip_open(bounds)` — the face makes a mask and a group;
2. every text box of the clip set is painted with a PEN: by `paint_box`
   itself, the code that paints it normally, at its own offset and inside
   its own clips, but its run and its decorations in white, with no
   shadows and nothing else, and no opacity group around anything;
3. `clip_fill()` — what follows is the group;
4. the layers, as they are drawn without the clip, cut to the border box;
5. `clip_close()` — the group is laid over what was under it, at each
   pixel's coverage in the mask.

Then the text is painted where it always is, in its fill colour: a
transparent fill draws nothing, and a solid one paints over the gradient,
as Chrome's row 7 does. A run the page wrote nothing for — no colour, no
picture, no gradient — makes no clip and rescues nothing: Chrome shows
nothing there either. An inline box's piece draws its pictures as well as
its colour and gradients (#262, Codex), each placed on the piece.

**The face.** A text clip is an opacity group whose opacity is PER PIXEL.
S7b already lays a group painted on two backdrops — B on black, W on white
— over the page as a·B + (1 − a + a·T)·D; here `a` is the mask's coverage
at each pixel instead of the group's opacity. The mask is a third surface,
white text on black, and white over black at coverage c is c — the text
verb's own blending is the coverage: (255·c + 127) / 255 is c, to the
level. The group nests inside a group drawn through its matrix (its
surfaces are in the coordinates being painted, as every group's are).
Nothing inside a text clip opens a group of its own: its mask is text, its
group is the layers.

### Contracts

Written before the code (the lesson of S7b's nineteen rounds):

- **Coverage is the text's own.** The mask is drawn by the code that draws
  the text and its decorations, in white. There is no second account of
  where a glyph or an underline is, so the two cannot disagree.
- **Membership is libflow's.** `flow_visit_text_clip` is the one answer to
  which text a clip holds, and it says Chrome's table; the painter asks it
  and does not decide.
- **Bounds are at least the ink** (§ Transforms, the painter, Booked: THE
  BOUNDS CONTRACT): the text clip's bounds hold the clip set's ink — each
  text box's rect grown by its own `ink` — cut to the border box and the
  view. Never less.
- **The composition is the warp's own**: each channel is the one
  function the warp lays a group with, B + (1 − a + a·T)·D, with the
  mask's coverage as `a` (`yonder_warp_masked`). No claim past that.
- **A refused clip is never nothing and never a box.** When the face
  cannot make the clip (past the sheet limits, out of memory, or a face
  with no such verbs), drawing the layers unclipped would
  paint a rectangle over transparent text, and drawing nothing would leave
  no text at all. So the text clip set is painted in ONE solid colour taken
  from the background: its colour if it has one, else its top gradient's
  middle, else `color` — each at full opacity, since a transparent one is
  the case being rescued. The headline loses its gradient and keeps its
  words. A clip the face made whose layers then find no memory (a
  gradient's pixels) is rescued the same way, over what it laid (#262,
  Codex): the layers report it, since only they know.
- **An all-covering mask is the opaque group**: a text clip whose mask is
  255 everywhere lays its group exactly as the warp lays an opaque one —
  the oracle the face's tests are checked against.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| A gradient span that wraps is a gradient per piece | an inline box's layers are placed on each piece (S4b, above) | a page whose wrapped gradient span matters |
| `-webkit-text-stroke` | an outline of a glyph is a rasteriser's verb the text engine does not have | a page whose outlined headline matters |
| Emphasis marks | yonder does not draw `text-emphasis` at all | the slice that draws them |
| `background-clip: border-area` (Backgrounds 4) | a newer value no page in the census writes; `@supports` says no | a page that writes it |
| A refused clip over a background that places nothing (a zero `background-size`) rescues the words, where the clip itself would show none | whether a run draws is asked of what the page wrote, not of where its tiles land — a picture's tile only the face can work out; the rescue is the degraded drawing, and drawing more there errs the way THE BOUNDS CONTRACT allows (#262, Codex) | a page that clips a zero-sized background to its text and is refused |

### Proof

libgarb's corpus: both spellings read, the alias's value list the same,
the fill colour inherited, `@supports` yes for `text` and for each
prefixed name. libflow: the clip set for every row of Chrome's table,
worked by hand (`flow_visit_text_clip` lists the text boxes it visits), a
span's from its line. warp's host harness: each pixel laid at its own
coverage against the content laid at that opacity, worked apart from the
warp; coverage 0 leaving a pixel as it is; the offset and the clip; and
the all-covering oracle. yonder's host harness: the verb sequence of a clip
(open, the text and its underline in white, fill, layers, close, the shadow
after), the refused clip's solid text, a clip whose gradient finds no
memory once it is open, and a clip with nothing to draw, each by hand. A mutant harness breaks each rule. In the guest,
`tools/pages/pile3-cliptext.html` — the two probe pages — beside headless
Chrome.

## Animations (slices S8a and S8b)

A page asks for motion in two ways: an ANIMATION runs a named list of
keyframes on its own clock once a style applies it, and a TRANSITION
eases a property from its old value to its new one when the style
changes. This section designs the first, and the machinery the two
share; § Transitions, below, says what the second needs and why it
waits.

### What CSS asks (CSS Animations 1 and 2, Web Animations 1, CSS Easing 1)

**Keyframes.** `@keyframes <name> { <selector># { <declarations> } … }`
names a list of KEYFRAMES, each an offset — `from` is 0%, `to` is 100%,
a percentage anything between, and a block with an offset outside them
is dropped — and the values some properties take there. The name is a
`<custom-ident>` other than `none`, or a `<string>`; two names are equal
only codepoint for codepoint, and of two rules with one name the last in
document order wins whole (Animations 1 § 3). Animations 2 § 3.2 builds
an element's keyframes from it: blocks with the same offset and the same
`animation-timing-function` are cascaded together, the last declaration
of a property winning; an `!important` declaration is invalid and
dropped; the `animation-*` properties are dropped, but for
`animation-timing-function`, which is that keyframe's own easing; and a
property that is not animatable is ignored. A property some keyframe
sets but none at 0% (or at 100%) gets a keyframe there holding its
computed value on the element — the IMPLICIT keyframes, which are how
`@keyframes in { from { opacity: 0 } }` fades to whatever opacity the
element has.

**The properties.** `animation-name` lists the animations; `-duration`,
`-timing-function`, `-iteration-count`, `-direction`, `-play-state`,
`-delay` and `-fill-mode` are lists coordinated with it — a short one is
repeated and a long one cut — and `animation` sets all eight, one comma
group per animation. In the shorthand the first time is the duration and
the second the delay, and a keyword another longhand can take is that
longhand's until it has one (Animations 1 § 4.9): `animation: ease ease
1s` is named `ease`. None of them is inherited.

**The clock (Web Animations 1 § 4).** An animation STARTS when the style
that applies it and its `@keyframes` are both resolved, and runs while
its name stays in the element's list: a change to the list keeps each
running animation by matching names from the last to the first
(Animations 1 § 2), and a change to its other properties applies as if
they had held from the start. `display: none` on it or an ancestor ends
it. The start time, the delay (a negative one starts it partway), the
duration and the iteration count (fractional, or `infinite`) give a
PHASE — before, active, after (§ 4.6.6) — and the fill mode says whether
the before and after phases show the value where the active interval
starts and where it ends, or nothing. In the active phase the ITERATION
PROGRESS is how far through the current iteration it is, read backwards
on an iteration its direction reverses (§ 4.7). A paused animation holds
its time.

**The value (Web Animations 1 § 5.3.4).** For each property: the two
keyframes around the iteration progress, among those that set it with
the implicit ones; the fraction of the way between them, passed through
the FIRST keyframe's easing — the element's `animation-timing-function`
unless that keyframe gave its own, so an easing applies per keyframe
interval, not per iteration (Animations 1 § 4.3); and the two values
interpolated at that fraction by the property's ANIMATION TYPE (§ 5.2
and Appendix A): by computed value, component by component; a repeatable
list; discrete, which flips at one half; or a rule of its own —
visibility, the shadows, transforms. An easing that overshoots gives a
fraction outside [0, 1], and the value goes past its keyframe. Of
several animations of one property, the last in `animation-name` wins.

**The easings (Easing 1 § 2).** `linear`; the cubic Béziers `ease`,
`ease-in`, `ease-out`, `ease-in-out` and `cubic-bezier(x1, y1, x2, y2)`,
x in [0, 1], extended outside [0, 1] along their end tangents; and
`steps(n, <position>)` with `step-start` and `step-end`, whose BEFORE
FLAG holds the bottom of a step at its own edge while the animation is
still before its active interval.

**The cascade.** An animated value is a declaration of the animation
origin, which beats every normal author declaration and loses to every
`!important` one (Cascade 5's order of origins).

**The side effects (Web Animations 1 § 5.6, Will Change 1).** While an
animation of a property is CURRENT (it has not ended) or IN EFFECT (it
shows a value, its forwards fill included), the element acts as if
`will-change` named the property: whatever a value other than the
initial one would make it — a stacking context, the containing block of
absolute boxes and of fixed ones — it is, even at a frame whose value is
the initial one. `will-change` itself does the same for what a page
names in it.

**The person.** `prefers-reduced-motion` says whether they asked for
less.

**Scripts.** The animation events (`animationstart`, `animationend` and
their kin) and the Web Animations API (`element.animate`,
`getAnimations`) are scripts' surface.

### What the pages ask (measured 10/13)

The census on the P5 — 381 records, 72 hosts — reports `@keyframes` on
41 hosts, `animation` on 38, `transition` on 39 and `will-change` on 15.
The values were counted in the sheets of 32 of them fetched again (the
census's mainstream pages, danlegt.com, and the transition demos Chris
visited): 1,104 `@keyframes` rules.

| Animated in `@keyframes` | Rules | Hosts |
|---|---|---|
| `transform` (263 of them as `-webkit-transform`), `translate`, `rotate`, `scale` | 779 | 24 |
| `opacity` | 571 | 20 |
| `background-position` | 40 | 10 |
| colours: `background-color`, `border-color`, `color`, `outline-color`, `fill` | 36 | 10 |
| `transform-origin` | 32 | 3 |
| `box-shadow` | 26 | 5 |
| `text-shadow` | 3 | 2 |
| lengths, nearly all of which move the layout: `width`, `height`, the insets, margins, paddings, `font-size`, borders and their radii | 56 | 11 |
| `visibility`, `pointer-events`, `display` | 26 | 9 |
| what yonder draws nothing of: `filter`, `clip-path`, masks, `stroke-dashoffset` | 77 | 11 |

Of the 1,074 rules that animate something yonder draws, 963 animate
nothing but what S8b moves in place (§ Animations in place): the
transform's properties, `opacity`, the background's colour and
position, the border colours, `box-shadow`. Of the 710 rules that give
`transform` two values or more, 598 pair their functions one for one at
every step, 27 pad a shorter list (13 of them from `none`), 50 pair a
`perspective()` or a `matrix()`, which mix only as matrices, and 35, on
seven hosts, interpolate a tail as matrices — danlegt's idle character
and its losing ticket among them, and protoweb.org's doors, a
perspective against a turn about y. `var()` inside keyframes is
common (Font Awesome's `fa-bounce`, danlegt's chips). Of 1,037 rules
that apply an animation, 90 apply it to `::before` or `::after`, which
yonder does not generate, and 29 under `:hover`, `:focus` or `:active`.
The easings written: `cubic-bezier()` 259, `linear` 182, `ease-out` 79,
`ease-in-out` 76, `ease` 56, `ease-in` 51, `steps()` 36, `step-end` 6;
and 104 keyframes give their own. 58 of the 971 cubic Béziers the sheets
write overshoot, a y outside [0, 1] — the bounce danlegt's overlays
enter with. 270 animations are `infinite`; of the fill modes, 202 are
`forwards` and 105 `both`. Of the 1,939 rules that declare a transition,
a third are on an element a `:hover`, `:focus` or `:active` rule
restyles — a floor, since an ancestor's hover and a script's class are
not counted — and those rules set what transitions name: `color`,
`background-color`, `box-shadow`, `border-color`, `opacity`,
`transform`.

danlegt.com runs 46: the visit counter's `rainbow-wave` slides a
`background-position`; the marquee is a linear `transform`; the scratch
ad flickers its `text-shadow` in `steps(2, end)`; and its overlays
enter on `cubic-bezier`s that overshoot.

What a layout costs is in the same records (`laid out at … in N ms`,
356 layouts on the P5): median 30 ms, 90th percentile 210 ms; danlegt 70
to 150 ms, GitHub 1.2 to 1.6 s, docs.ovh 6.7 to 7.5 s.

### What Chrome draws

Headless Chrome 155 was asked with animations PAUSED at a negative delay,
which holds each at a chosen time — the same trick the fixture uses — and
read back through `getComputedStyle` (10/13):

| The case, at the time asked | Chrome |
|---|---|
| `rotate(0deg)` to `rotate(720deg)`, 25% | `rotate(180deg)`: the angle is interpolated, not the matrix |
| `rotate(0)` to `rotateZ(400deg)`, 50% | 200°: `rotate` and `rotateZ` pair |
| `translateX(10px) rotate(30deg)` to `rotate(60deg) translateX(100px)`, 50% | a 45° turn moved (30, 43.30): the lists do not pair, so they are matrices |
| `scaleX(-1)` to `rotate(90deg)`, 50% | `matrix(0, 0, -0.707107, 0.707107, 0, 0)` — a 2D decomposition (Transforms 2 § 13's on the 3D form would give no matrix at all) |
| `skewX(0deg)` to `skewY(40deg)`, 50% | `matrix(1.08319, 0.394248, 0.0461182, 0.956478, 0, 0)` — not Transforms 1 § 11.2's answer (0.956, 0.348, -0.040, 1.050): Chrome's 2D decomposition is a shear and a turn, slerped |
| `matrix(2, 0.5, 0.3, 1, 10, 20)` to `matrix(1, 0, 0, -1, 0, 0)`, 50% | translation (5, 10): the translations mixed alone, where § 11.2 would carry them through the 2x2 left over |
| `skewX(20deg)` to `skew(40deg, 20deg)`, 50% | matrices: a skew pairs only with its own name |
| `none` to `rotate(400deg) scale(2)`, 50% | `rotate(200deg) scale(1.5)`: `none` padded with identities |
| `translate(50%)` to `translate(20px)` on a 100px box, 50% | 35px: a percentage and a length mix as `calc()` |
| `translateX(0)` to `translateX(100px)` under `cubic-bezier(0.2, 0.86, 0.32, 1.22)`, 50% | 103.628px: past the keyframe |
| `rgba(255, 0, 0, 0.2)` to `#0000ff`, 50% | `rgba(43, 0, 213, 0.6)`: premultiplied |
| `background-position: 0% 0%` to `100px 50%`, 50% | `calc(0% + 50px) 25%` |
| `box-shadow: 0 0 0 red` to `10px 10px 4px blue, inset 0 0 2px green`, 50% | the second shadow padded with a transparent inset one |
| `z-index` 1 to 4, 50% | 3: 2.5 rounds up |
| `width: auto` to `200px`, 49% | `auto`: discrete |
| `font-weight` 100 to 900, 30% | 340: a number |
| a gradient to another, 60% | the second: discrete |
| an opacity keyframe at 50% with its own `steps(2, start)`, 75% | the keyframe's easing, not the element's |
| `steps(4, start)` with a 1s delay and `backwards`, in the delay | the bottom step: the before flag |
| 2.5 iterations, `forwards`, long after | the value at 50% |
| `alternate`, three iterations, at 1.25s | the second iteration, backwards: 75% |
| `0s`, `both` | the last keyframe |
| `animation: ease ease 1s` | named `ease`, eased `ease` |
| a transform animation from `none`: paused at its start, in its delay, ended `forwards`; and `will-change: transform` | each the containing block of a fixed child |
| the same animation ended with no fill | no longer one |

### The split

- **S8a, the model.** Everything above is applied where the page is
  styled: the style pass gives each animated element its value at the
  time of the layout, so inheritance, `currentColor` and layout take it
  as they take any computed value; and the face lays the page out again
  when a frame is due, as often as a layout's cost allows. Every
  property moves correctly; a frame costs a layout.
- **S8b, in place.** The properties that change only what is painted —
  the transform's, `opacity`, the background's colour and position, the
  border colours, `box-shadow`: nine animations in ten — move between
  layouts: libflow rewrites the element's style, its frame and its ink,
  and the face repaints what moved.

Each is whole without the next: after S8a every animation runs, slow
where layout is slow; S8b makes the common ones cheap.

## The animation model (slice S8a)

### How it is built

**libgarb reads the properties and the rules.** The eight
`animation-*` longhands and `animation`, and `will-change`, join the
property table, and the Compatibility Standard's aliases join the alias
table: `-webkit-animation` and its seven (§ 3.4.1), and
`-webkit-transform` and `-webkit-transform-origin` (§ 3.4.1), which 263
keyframes write and which S7 owed. A TIME is a new value kind, in `s` or
`ms`. An easing is a KEYWORD or a FUNCTION — `cubic-bezier` with its
four numbers, x checked in [0, 1]; `steps` with its count, positive, and
its position, `jump-none` asking two steps at least — and an
`animation-name` keeps its names as written, since names are compared
exactly. `@supports` says yes to all of it.

`@keyframes` and `@-webkit-keyframes` (Compat § 3.1) are kept by the
cascade where `add_rules` meets them, inside the `@media` and `@supports`
that hold, the last of a name in cascade order replacing the rest; the
cascade no longer counts them as passed over. `garb_keyframes(cascade,
element, name, …)` answers an element's keyframes for a name, built by
Animations 2 § 3.2: grouped by offset and easing, the declarations of
each group read as winners are — into the longhands they set, `var()`
replaced against the element's own custom properties — and dropped where
Animations 2 drops them. The implicit keyframes are libflow's, which
alone knows the computed values. An element's winners now say which were
`!important` (`garb_style_t.important`), which an animated value may not
override. And `garb_env_t` gains `reduced_motion`, which
`prefers-reduced-motion` answers from, as `prefers-color-scheme` answers
from `dark`.

**libflow keeps the lists in the style.** Each `animation-*` longhand is
a list in `flow_style_t`, coordinated by `animation-name` where it is
used; an easing is a `flow_easing_t` — linear, a cubic's four numbers,
or a step count and position. `will-change` is two bits, TRANSFORM and
OPACITY — any of `transform`, `translate`, `rotate` and `scale` sets
the first — the two whose effects this slice gives; the rest of what it
names is read and does nothing (§ Booked).

**A transform keeps its functions.** `flow_transform_t` held each
function as its matrix and a translation's two lengths, which is all
drawing wants. Interpolation wants more: `rotate(0)` to
`rotate(720deg)` turns twice, and a matrix of either is the identity. So
each function keeps its PRIMITIVE and its arguments as computed — a
translation's lengths, a scale's factors, an angle in degrees and NOT
reduced by whole turns, a 3D rotation's axis, a skew's two angles, a
matrix's sixteen numbers — beside the matrix it makes, which style.c
builds from them as it builds it today. The primitives pair as
Transforms 1 § 7.2 and 2 § 12.3 say and Chrome agrees: the translations
with each other, the scales, the rotations (`rotate`, `rotateZ`,
`rotateX`, `rotateY` and `rotate3d`, about axes Transforms 2 § 14
compares); a skew with nothing but its own name; and `matrix()`,
`matrix3d()` and `perspective()` only as matrices (§ 14).

**The timeline.** Animations outlive the layouts that compute them: a
start time must survive every layout after the one that started it — a
picture arriving, a zoom, a script's change. So the face owns a
`flow_timeline_t` per page, made empty when the page is first laid out
to be shown and freed after its tree, which points into it, and before
its document, whose elements it holds; and hands it to each layout in
the environment with the time (`flow_env_t.timeline`, `.now_ms`). A
layout that measures a page not yet shown, for its scripts, has none. It keeps one record per
running animation: its element, its name and place in the element's
list, its start time, and while it is paused the time it holds. A layout
is a style change event, and updates it as Animations 1 § 2 says: an
element's new list is matched from the last name to the first against
its records, each match kept with its start time and given the new
properties of its place; an unmatched name with keyframes starts at
`now`; a name whose `@keyframes` do not exist yet starts nothing, and
starts when they arrive. A paused animation stores the time it has run
and a resumed one is restarted that much before `now`. A record no
COMPLETE layout styled is dropped — its element gone, its name gone, its
subtree `display: none`; an incomplete layout drops nothing it did not
reach. A record HOLDS its element (`os64_html_hold`), so a node a script
removes is not reused for another while a record names it, and the hold
goes with the record.

**The style pass gives each animated element its value at `now`.** Where
`style_element` has applied the author's winners and before `finish`
resolves them, an element with animations keeps that specified state as
its BASE and makes, for each keyframe, the state its declarations make
laid over the base — every property but those an `!important` winner
holds — each resolved by `finish` as the element's own style is: at its
font, its parent, its `currentColor`, its zoom. Each keyframe is then a
computed style, and the implicit keyframes are the base's. The value of
each animated property at `now` is worked from those (§ What CSS asks:
the clock, the value) and written into the element's style, and the derived
facts — a border or background or fill in `currentColor` follows the
animated `color` — are resolved from it. The element's children inherit
from that style. So an animated `color`, `visibility` or `text-shadow`
reaches the descendants that inherit it, an animated `width` is laid
out, and a layout at a time is the page at that time.

**Interpolation, by the field.**

| The field | How it is interpolated |
|---|---|
| a length (`flow_length_t`): PX, PERCENT, and PERCENT with an offset, which is how a `calc()` mix computes | each part separately — a length with a percentage mixes into the PERCENT-with-offset form (Values 4 § 5.6.1); AUTO and the other keywords discrete; each part rounded to its own unit, 1/64 px or 1/64 of a percent. A length its property keeps at 0 or more (widths and heights, padding, gaps, radii, `flex-basis`, `background-size`) is held at 0 through an overshoot, as Chrome holds it; a mix whose parts differ in sign is held where it is used, as a `calc()` is |
| a size in 26.6 (`font_size`, border widths, `border-spacing`, transform-origin's z) | a number in 26.6, rounded to 1/64 px |
| a colour (`0xTTRRGGBB`) | premultiplied by its alpha, each channel in doubles, then divided back and rounded half up to 8 bits (Color 4 § 13.4) |
| `opacity`, the flex factors (thousandths), `font-weight` | a real number, rounded to the field's grid |
| `z-index`, `order` | a real number, rounded half up (Values 4 § 5.2.1, as Chrome rounds 2.5) and held to the field's range, which an overshooting easing passes; a `z-index` between two numbers is a number, whatever the element's own, and `auto` at either end is discrete |
| `line-height` | a number with a number, a length with a length; else discrete |
| `visibility` | `visible` for every fraction strictly between 0 and 1 if either end is `visible`; else discrete (Web Animations Appendix A) |
| a shadow list | shadow by shadow, the shorter padded with transparent ones of no size that match the other's `inset`; a pair differing in `inset` makes the whole list discrete; colours as above (Appendix A) |
| `background-position`, `background-size` | repeatable lists: repeated to their least common multiple, then item by item as lengths (`cover` and `contain` discrete) |
| a transform | below |
| everything else — keywords, families, pictures and gradients, grid tracks | discrete |

`display` is not animated here (§ Booked). An interpolated value is
computed from the two keyframes every time, never from the last frame,
so nothing accumulates.

**Transforms interpolate as Transforms 1 § 9 says.** `none` is an empty
list. The shorter list is padded at its end with the identity of the
function in the longer one at each place (`translate(0)`, `scale(1)`,
`rotate(0)`). Then, from the start, each pair of the same primitive is
interpolated argument by argument — a 3D pair as well as a 2D one,
`rotateY` with `rotateY` — and at the first pair that does not pair (or
a pair that pairs only as matrices, § 14: two `rotate3d`s about
different axes both turned, two `matrix()`es), the rest of each list is
multiplied into a matrix and the two are interpolated as matrices — as
Chrome interpolates them, not as Transforms 1 § 11.2 writes it, since the
two part where a matrix holds a shear (§ Animations, What Chrome draws)
and a page is written against what browsers draw. Each matrix is decomposed on its
own: a 2D one into a translation, a scale (a flip on the axis whose
diagonal is the smaller), a shear and a turn about z; any other by
Transforms 2 § 13.1.1, into perspective, translation, scale, skew and a
quaternion. The parts are mixed, the turns slerped the shorter way, and
composed as perspective · translation · rotation · skew · scale —
Chromium's DecomposeTransform and ComposeTransform, whose answers the
proof checks to Chrome's six digits: `scaleX(-1)` against
`rotate(90deg)`, a skewed turn, `matrix()` against `matrix()`, and
protoweb.org's doors. A matrix that cannot be decomposed makes the pair
discrete (Transforms 1 § 11). The tail's lengths need the border box, so the tail is kept as
its two lists and the fraction, and multiplied out where the frame's
matrix is made (`f_transform_matrix`), with the box known.

**The side effects are fixed for an animation's life.** At each layout,
an element with an animation of a property that is current or in effect
at `now` takes `will-change`'s bit for it — and so the structure a
frame's value could change is decided once, not by the frame:

- TRANSFORM: a box a transform applies to has a transform frame even at
  a frame whose value is `none` (its matrix the identity, a move of
  nothing), is a stacking context, and is the containing block of
  absolute and fixed boxes — what S7a's transform does, from
  `f_transformed` and `f_contains_absolute`.
- OPACITY: a stacking context, as a box below full opacity is.
- An element whose `opacity` or transform is ANIMATED, not only named in
  `will-change`, is a GROUP (`flow_visit_groups`) for the life of the
  animation, whatever its value, so a frame that turns it or fades it is
  painted the way its neighbours are; and its own `opacity: 0` does not
  make it `unpainted` — the painter lays the group over at no opacity,
  which shows nothing, rather than the layout leaving it out.

When the animation ends without a fill, its side effects end with it,
and that is a change of structure: the face is due a layout at its end.

**The face lays out at the frames.** yonder hands each layout
`yonder_now_ms()`, and asks libflow when the page next changes:
`flow_motion_due(tree, view, scroll)` answers the earliest time an
animation that can show in `view` shows another value. One that only
repaints where its element draws — a colour, a fade, visibility, a
background's position, the corners' curves — can show there when its
element can be seen: its box; for one that makes none (`display:
contents`) the boxes inside it, which draw what it hands down; or any box
inside it drawn outside it, positioned, fixed or moved by a transform,
which no ancestor's overflow holds. Any other — a transform, a size, a
position, a shadow's offset — may carry what it draws into view from
anywhere, and is due wherever its element is. The time: the layout's own
for a value that moves continuously, which leaves when the next frame is
to the face; for one that changes at an instant — the next step's edge in
`steps()`, worked out from where the progress is and never walked to; each
crossing of a discrete value's level (one half, or for visibility 0 and
1) by its easing, every one an overshooting curve makes; the interval's
far end — the instant it shows, ahead of the progress the way it runs. A
value rising to a step changes there, one falling through it (running
backwards) just past it, and the clock's rounding may put either a hair
the other side of a whole millisecond: so the millisecond the instant
lies in is due, and failing that the one after — never late, and at
most one layout early. Then the start of an active phase; an end — and
never for one that is paused, finished and filling, or in place and out
of view. It joins the deadlines the
ticker keeps already (`pictures_schedule`). When it rings for an
animation the page is laid out again, quietly: the scroll kept as any
relayout keeps it, the controls placed, the cascade reused — the
viewport and the sheets have not changed — and the standing line left
saying what the page's own layout cost.

The cost is ruled by what a FRAME costs — the layout for an animation
and the paint that shows it, measured as they run (the paint is the
window's whole view, libui's one rectangle, and counting the layout
alone let a page whose paint costs twice its layout keep the window's
thread busy all the time):

- a page whose last frame — or, before there was one, its last layout —
  took more than MOTION_COST_MS (200) lays out for no animation: its
  animations show what they were at the last layout made for another
  reason, until S8b moves the ones it can. One whose layout fails is its
  last, as though it had cost too much;
- otherwise the next frame waits until twice the last one's cost has
  passed since its paint ended — at most a third of the window's time
  spent on motion — and never comes sooner than MOTION_FRAME_MS (16)
  after the last began, which the window's clock, in 10 ms ticks, makes
  20: fifty a second at most;
- a covered window lays out for no animation, as it shows no picture's
  frames (`g.covered`).

The constants are leans, to be moved by what the P5 measures.

**The person's settings.** yonder answers `prefers-reduced-motion` with
`no-preference`, as every browser on a machine nobody set answers, and
the Settings window gains Reduce motion — saved as `motion = reduce` in
yonder.conf — which makes it answer `reduce`. Pages decide what that
means: many turn their animations off under it, and every page that
does has been doing so here, since yonder said `reduce` to all of them.
media.c's comment, which says why, is rewritten to say what it answers
now.

Beside it, Run animations — on unless yonder.conf says `animations =
off` — is the switch for the day animations cause more trouble than
they are worth (Chris, 10/14: "at least temporarily"). Off, the face
hands no timeline to its layouts, and libflow runs no animation: every
element has the style its rules give it, as before S8, and nothing is
due. The page keeps its timeline, so turned on again an animation is as
far along as its start says. `will-change` is no animation and still
applies; what pages are told is Reduce motion's business, not this
switch's.

## Animations in place (slice S8b)

### How it is built

**What moves in place.** A property moves between layouts when nothing
but painting reads it, its effect on structure is fixed for the
animation's life (S8a's side effects), and nothing inherits it: the
transform's four properties and `transform-origin`, `opacity`,
`background-color`, `background-position`, the four border colours and
`box-shadow`. Every other animated property still moves by layouts, as
in S8a. A descendant that takes one of these by `inherit` makes it move
by layouts on that element, since a tick rewrites the animated element's
style and no other.

**A tick is the style pass's arithmetic, again.**
`flow_motion_tick(tree, now, view, damage, ctx)` works each in-place
animated value at `now` with the function the style pass works it with,
from the keyframe styles the layout made — the tree keeps them — and
writes it into the element's style. What the layout derived from those
fields is made again:

- the transform frame from its matrix: `f_transform_matrix` with the
  border box in 26.6, which the frame now keeps, and the frame's move,
  `through`, `upright` and affine as `transform_new` makes them;
- the box's ink for its shadows (`own_ink`'s shadow part), MERGED into
  `flow_box_t.ink` and handed up as `ink_up` hands it — so a box's ink
  only grows while its tree lives, which THE BOUNDS CONTRACT allows;
- nothing else: group bounds, hits, a box's place and its clip read the
  frames when they are asked.

**What it repaints.** For each element a tick changed, `damage` is handed
what the element painted before it and what it paints after, in the
page's coordinates: a group's bounds as `flow_visit_groups` works them
(everything in its stacking context, through every matrix); for any
other element, each of its boxes' overflow rect grown by its ink and cut
to its clip, through its frames; the whole view for the canvas's owner,
whose background is the canvas's. The face marks each, cut to the view
first — marks are joined into one rectangle (§ The one dirty rectangle,
below), and one outside the view would stretch it across the view. A
tick that moved a frame places the page's live controls again
(`forms_place`), so a field inside a sliding panel slides with it.

**When the next frame is.** `flow_motion_due` answers for the in-place
animations as it does for the rest, apart: the face ticks at the
in-place deadline and lays out at the other. A tick waits MOTION_FRAME_MS
after the last, and as long again as the last tick and its painting
took, so motion spends at most half the time; a value that only moves at
its steps ticks only at their edges, and a stretch where the keyframes
hold one value does not tick inside it. The face also ticks before
it paints the page for any other reason — a scroll brings an animation
into view — so what is painted is the value at the time it is painted.

## Animations, both slices

### Contracts

Written before the code (the lesson of S7b's nineteen rounds):

- **A value is a function of the time.** An animation's value at a time
  depends on that time, its start time and the page's styles, and on
  nothing else: no frame is built from the frame before. A layout at a
  time and a tick at that time give the same style fields and the same
  frame, to the bit — the tick calls the function the style pass calls.
  A frame the face skips is not drawn, and the next is right.
- **The timing is Web Animations' arithmetic**, in doubles of
  milliseconds from the start time. No promise past it.
- **An easing is a point on its curve.** On [0, 1] a cubic's output is
  the curve's y at a point whose x is within 1e-7 of the input — a
  promise about x, since near a vertical tangent a small step in x is a
  large one in y, and no solver can promise y there; outside [0, 1] the
  end tangents are exact; steps are exact, their before flag with them.
- **A value lands on its field's own grid once**: a length on 1/64 px
  (the layout's unit and Chrome's), a colour on 8 bits a channel from
  premultiplied doubles, `opacity` on a thousandth, an integer half up.
- **Structure is the layout's.** Whether a box is a stacking context, a
  containing block, a group, a frame, or painted at all is decided by
  the side effects at a layout and holds until the next; a tick changes
  what is drawn and where, never what is drawn as what.
- **What repaints holds what moved.** The damage a tick hands is at
  least what the element painted before it and after it — never less;
  more where a grown ink overcounts, which THE BOUNDS CONTRACT allows.
  Group bounds read the live frames, as they do now.
- **libflow owns what moves; the face owns when.** Which animations run,
  which keyframes, which value, which property in place — CSS, libflow's.
  The clock, the view, and what a frame may cost — the face's.
- **Cost is a policy, not a promise.** The frame interval, the shares of
  the time, and the frame's cost limit are constants to move by
  measurement.

### The one dirty rectangle

libui keeps ONE rectangle of what needs painting, the join of every mark
(`os64_ui_mark_dirty`). Two small things changing far apart repaint
everything between them. It is why danlegt's cockroaches slowed:
`img.roach` is turned (`rotate(-12deg)`, and `rotate(12deg) scaleX(-1)`
for the second), so each frame of each roach is drawn through its
matrix since S7b — and the two hang off opposite sides of `#main`
(`left: -64px`, `right: -64px`), so every frame of their GIF repaints
the band between them the width of the page, and a taller window joins
the other GIFs' frames to it, the penguin's among them. Animations
spread over a page meet the same wall. It is the perf slice's first
lever — a list of rectangles for the page view — and not S8's: S8 marks
honestly, and the cost is counted where it is spent.

### Transitions (designed here, built later)

A transition needs what S8a builds — the timeline, the easings,
interpolation by field — and three things more: the timeline keeps each
element's BEFORE-CHANGE values of the properties its `transition-property`
names, which every layout compares with the new ones (Transitions 1 § 3:
start, cancel, and the reversing of an interrupted one); a transitioned
value is of the TRANSITION origin, which beats even `!important`; and a
layout must say whether it is a style change a transition may start
from. That last is yonder's: a layout made as the page's own sheets
arrive is not one — Chrome renders nothing until a page's head sheets
are in, and yonder shows a page before them when they are slow (G4), so
a transition there would animate a page into its own style.

What starts transitions in yonder today is almost nothing. `:hover`
never matches (libgarb's `P_USER_ACTION`); scripts are off unless the
person turns them on; a resize or a zoom remains. The census's
transitions are on elements a hover restyles — a third of them at the
least — and name the properties a hover changes (§ What the pages ask);
and a hover is a new cascade and a new layout at the pointer's pace,
which the census's layout times put at 30 ms on half the pages and over
a second on GitHub. So transitions wait for `:hover`, and `:hover` for
the perf slice.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| Transitions, and `:hover` with them | § Transitions: their trigger is hover, and hover is a restyle and a layout per move | the perf slice: they are the slice after it (Chris, 10/14: "as long as we don't forget") |
| The animation events and the Web Animations API | scripts' surface, which the Modern Web arc gives (Platform waits for its arc) | a page with scripts on that waits on `animationend` |
| `animation-composition`, `animation-timeline` (scroll-driven), `animation-range`, view transitions | newer levels. GitHub writes `animation-timeline: scroll(self)` 15 times, outside `@supports`, on animations of no duration that set custom properties: run on the clock they end as they start and show nothing, as in a browser without scroll timelines | a page that runs on scroll |
| `display` in a keyframe | ignored: an animated display builds and unbuilds boxes, and browsers began animating it only lately | a page that animates it |
| Grid track lists interpolated | discrete here | a page that animates its grid |
| Custom properties in a keyframe, and registered ones (`@property`) | ignored: a keyframe's `--x` would have to reach the `var()`s that read it, which libgarb replaces at the cascade | a page whose animated variable matters |
| `will-change` beyond the transforms and `opacity` — `position`, `z-index`, and what yonder draws nothing of | read, and given no effect | a page whose stacking depends on one |
| The properties yonder draws nothing of — `filter`, `clip-path`, masks, `stroke-dashoffset` | animating them animates nothing | the slices that draw them |
| Animations on `::before` and `::after` | yonder generates no content for them at all | the slice that does |
| Inherited properties in place — `color`, `visibility`, `text-shadow`, the fill | they move by layouts, whose cost is ruled (S8a, the face); in place they would need every inheriting descendant's style rewritten | a page whose inherited animation costs too much in layouts |
| A page's scrollable extent following a transform's frames | the extent is the layout's | a page scrolled to where an animation carried something |
| A keyframe's `em` while `font-size` animates | its keyframes are computed at the layout's font | a page that animates both |
| The rest of Compat § 3.4.1's drawn rows (`-webkit-box-shadow`, `-webkit-border-radius` and the others) | not animation's | a page that writes only the prefixed name |
| `linear()` easing (Easing 2) | no page in the census writes it | a page that does |
| An animation that moves its element rests out of view | it may carry what it draws into view from anywhere, so S8a keeps it due wherever it is; a bound on where its frames can reach — a spinner turns inside its own circle — would let it rest | S8b, where such a tick is arithmetic and a repaint, or a page whose spinners out of view cost what the P5 notices |

### Proof

libgarb's corpus: `@keyframes` and its alias kept, named by ident or
string and compared exactly, `none` refused, offsets past 100% dropped,
the last of a name winning across sheets and inside `@media` that holds,
not inside one that does not; `!important` and the `animation-*` in a
keyframe dropped but its own easing; the shorthand's two times and its
keyword rule (`ease ease`); every longhand's list, the easings' checks;
`will-change`; `@supports`; `prefers-reduced-motion` both ways; the
winners' importance; `var()` in a keyframe replaced per element.

libflow's harness (`--motion`): the clock over a table of phases, fills,
iterations — fractional and infinite — directions, negative delays, a
zero duration, pause and resume; the easings against a reference worked
in long double, the steps against Easing 1's own examples with the
before flag; keyframes built — implicit frames, one offset twice, a
keyframe's own easing, groups by offset and easing; interpolation for
every row of the table above against Chrome 155's answers to the probe
pages, recorded with the page that asked; transforms' padding, pairing,
3D pairs, and tails by both decompositions against Chrome's (2D:
`scaleX(-1)`, a skew against a skew, a skewed turn, `matrix()` against
`matrix()`; 3D: a perspective door, two axes);
the side effects against Chrome's
containing-block answers; the timeline across layouts — a start kept,
`a` to `a, a`, a name gone, an element gone and its hold let go, a
pause and a resume, an incomplete layout dropping nothing; each due
time; an animated page's layout with each of its allocations failed in
turn, alone and with every one after it, leaking nothing, whole only
when it is the page, and its timeline coming through each failure with
its start times; and S8b's tick against a layout at the same time,
field by field and frame by frame to the bit, at many times; its damage
holding what was painted before and after; ink that only grows.

yonder's host harness, which hosts yonder.c with clocks it sets: S8a's
deadlines — the cost limit, a frame's paint counted in its cost, the
page's own cost before its first frame, the shares, the interval, a
covered window, an
animation out of view, a step's edge — the quiet layout, a failed one
being the last, an arriving page starting its animations and its costs
afresh, a script's change starting one, an element removed; the two
settings saved and applied, what the media answers, and with animations
off the rules' styles and nothing due. S8b's marks cut to the view, and
controls placed after a frame moved.

`tools/test_motion_mutants.py` breaks each rule. In the guest,
`tools/pages/pile3-motion.html`: FROZEN rows, paused at chosen times or
long finished, each box on a blue line where headless Chrome put it or
beside a neighbour drawn as Chrome computes it; and LIVE rows (a
spinner, a pulse, a hue cycle, a steps march, a delayed start, one
Reduce motion stops, one far below the fold) watched; then danlegt on
the P5.
