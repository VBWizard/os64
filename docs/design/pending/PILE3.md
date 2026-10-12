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
| `background-clip: text` | the glyphs are drawn by the text verb, with no mask to fill with a picture; a background clipped to its text is not drawn on any box (the canvas's is drawn across the canvas whatever the clip says, as § 2.11.1 says and Chrome draws it), and `@supports (background-clip: text)` says no, so a page takes the fallback it wrote | a page whose gradient headline matters |
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
| `background-clip: text` | a background drawn through a mask of the text is a surface of its own | S7c |
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
  for `getBoundingClientRect`, a control's place, `flow_box_covered`,
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
the viewport taken back through the matrices — the bounds of its corners
— joined with where an upright drawing would put it, since the face may
refuse the group after the walk has passed. A matrix that collapses an
axis (`scaleX(0)`, `rotateX(90deg)`) draws nothing and is hit nowhere.
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

A surface is the group's bounds cut to the view taken back through
the matrices — what can show — at most 4096 pixels a side and 2,097,152
in all (8 MiB each), at most 16 deep; a group past that, or whose
surfaces find no memory, is painted straight onto the backdrop at its
S7a offset — upright, never missing. The painter keeps 64 groups open;
one deeper is painted where it was laid out.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| A box scaled up softens | its surface is painted at its own size and stretched; Chrome paints at the scale | a page whose scaled-up text matters |
| A control in a drawn-through box is drawn turned but edited upright | a live widget is drawn on the glass, not through a matrix | a page with a turned form |
| A sticky box, a scroll container's bars, or a fragment revealed inside a drawn-through box | each is worked against the page's viewport, which the box's own coordinates are not | a page that scrolls inside a turned box |
| `perspective` and non-flat matrices | carried over from S7a | a page whose 3D card flip matters |
| `background-clip: text` | its own slice, S7c, on these surfaces | S7c |

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
