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
| `filter`, `cursor`, `image-rendering` | not GARB.md's pile 3, and measured above | the slice that takes them, re-measured |

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
size turns it into a line; currentColor in a stop is the element's colour.

**yonder paints it pixel by pixel** where a box's picture goes, between its
colour and its inset shadows. Its tile is the padding box (the positioning
area), drawn across the border box and repeated as `background-repeat`
says, and cut to the curve of a rounded box. A pixel's middle is projected
onto the line — toward a corner it is square to the box's diagonal (§
3.1.1), its length |w·dx| + |h·dy| — or measured out along the ray of an
ellipse or circle sized by its extent. Stops are fixed up by § 3.5.3 (the
ends 0 and 1 when left out, none earlier than the one before, a run left
out spread evenly); a hint bends the mix with the power § 3.5.3 gives; a
repeating gradient wraps between its first stop and its last; colours mix
premultiplied. The pixels reach the face through a new verb, `pixels`,
blended there. **A body's gradient is the canvas's**, its tile the root's
box — a page shorter than the view repeats it in bands, as the browsers
do.

### Proof

`gradient_style_cases`: five gradients read into flow's (the initial
direction, a `to` corner, a stop with two positions and a hint, a circle's
one radius, a turn, currentColor, an ellipse's initial size). yonder, each
pixel worked by hand: a 4px black-to-white line (t = 1/8 … 7/8), repeated
hard stops, a 4px radial circle (its inner, edge and corner pixels), and a
body gradient tiled by a 4px root. In the guest, `/tests/pages/
pile3-gradients.html` — twelve gradients of every kind, a rounded one, a
translucent one over stripes, and the body's own — drew as headless Chrome
draws it.

### Booked

| Debt | Why it waits | Trigger |
|---|---|---|
| `background-size`, `-origin`, `-clip` | — | done in S4b, below |
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
| `background-clip: text` | the glyphs are drawn by the text verb, with no mask to fill with a picture; a background clipped to its text is not drawn at all | a page whose gradient headline matters |
| A content box with percentage padding | the tree does not carry the width a percentage padding resolved against, so it counts as none, as for an atom's content | a page whose content-box background sits wrong |
| A scaled picture's smoothing | a copy is scaled nearest-neighbour, where the browsers smooth it unless `image-rendering` says otherwise | the image-rendering slice |

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
