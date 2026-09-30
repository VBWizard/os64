# POSITION.md — positioned layout: boxes that leave the flow

*Written 2026-09-29 by Opus, the day the stack that brought libgarb's first
pile to yonder was merged and Chris set yonder beside Chrome on the P5:
"most of my observations are about exactly that". Revised the same day on
Fable's review (§ Review record). This is the first slice of GARB.md's
pile 2. The library is libflow and its rules are
[LAYOUT.md](../completed/LAYOUT.md)'s; the properties come from libgarb
([GARB.md](../completed/GARB.md)); the face is yonder
([YONDER.md](../completed/YONDER.md)). Chris ruled on all five questions
the same day, each as recommended (§ Rulings).*

## Why this, and why now

libflow lays out every box in normal flow. libgarb does not read
`position`, the four offsets or `z-index` at all, so a page's positioned
boxes are laid out where the document wrote them, in its reading order,
pushing everything after them down. What that looks like on the web of
2026:

- a dropdown menu or a modal opens INTO the page instead of over it;
- a site header written as `position: fixed` scrolls away, and a cookie
  bar sits wherever the markup put it — often the top of the page, or the
  very end;
- a close button, a badge or an icon laid against a corner of its card
  (`position: absolute; top: 0; right: 0`) drops below the card;
- `left: -9999px`, the web's commonest way of hiding text from sighted
  readers and keeping it for screen readers ("skip to content"), shows it;
- whatever a page stacks with `z-index` draws in tree order.

Flexbox and grid are the rest of pile 2 and matter as much, but a page
without them degrades to blocks in order, which still reads; a page whose
positioned boxes land in the flow is often unreadable, and it is what
Chris sees first.

## What CSS asks (CSS 2.1 § 9.3, § 9.6, § 9.7, § 10.1, § 10.3.7, § 10.6.4, Appendix E; CSS Position 3)

`position` is one of five values:

| Value | In the flow? | Placed against | Moves with the page? |
|---|---|---|---|
| `static` | yes | its place in the flow | yes |
| `relative` | yes: its space is kept where the flow put it | its own place in the flow, shifted by the offsets | yes |
| `absolute` | no | its CONTAINING BLOCK: the padding box of the nearest positioned ancestor (for an inline ancestor, the bounding box of its first and last pieces' padding boxes), else the INITIAL CONTAINING BLOCK | yes |
| `fixed` | no | the viewport | no: it stays where it is on the glass |
| `sticky` | yes | its place in the flow, until scrolling would carry it past an offset | stops at the offset |

The INITIAL CONTAINING BLOCK is `(0, 0, width, viewport height)` (§ 10.1):
as tall as the window, not the page, so `position: absolute; bottom: 0`
with no positioned ancestor sits at the foot of the first screenful.

The offsets are `top`, `right`, `bottom` and `left` (and `inset`, which
sets all four): lengths, percentages of the containing block, or `auto`.
An absolute box's width and height solve § 10.3.7 and § 10.6.4: `left +
margin + border + padding + width + ... + right` equals the containing
block, and whatever is `auto` takes the rest — which is how `left: 0;
right: 0` stretches a box, how `margin: auto` with both sides set centres
it, and how `width: auto` with one side set SHRINKS TO FIT, as an
inline-block does (§ 10.3.9 has that code since G3). A percentage HEIGHT
on an absolute box resolves: its containing block's height is known when
it is laid out, and `top: 0; left: 0; width: 100%; height: 100%` is the
overlay idiom. Where both offsets on an axis are `auto`, the box sits at
its STATIC POSITION: where a hypothetical box with `position: static` and
the element's SPECIFIED display would have started.

`z-index` orders what overlaps. A positioned box with a `z-index` other
than `auto` makes a STACKING CONTEXT; within one, Appendix E paints the
context's own background, then its children with negative `z-index`, then
the in-flow blocks, then the inline content, then the positioned
descendants with `z-index: auto` or `0` in tree order, then the positive
ones, lowest first. The root is the first stacking context.

## How it enters libflow

Each of LAYOUT.md's three passes takes a part, the door takes the paint
order and the coordinates, and the face takes the rest. The rule that
shapes all of it: **a positioned box is REACHED FROM A LIST, never through
its tree ancestors.** Its box stays under its parent in the tree — the
pre-order is still the document's — but the tree's two walks skip it, its
tree ancestors' overflow rects leave it out, and the tree carries a list
of positioned boxes in paint order that the walks visit after the
ordinary tree. That one rule is what makes paint order right, what lets an
absolute box escape a clip its containing block does not have, and what
lets a fixed box, in another coordinate space, be reached at all.

### libgarb

`position`, `top`, `right`, `bottom`, `left`, `inset`, `z-index` (an
integer, or `auto`) and `opacity` are read into the property table, the
usual way. A value no slice lays out yet is READ — the cascade keeps it —
and is NOT SUPPORTED: `@supports (position: sticky)` says no until P4,
`@supports (z-index: 1)` until P3, and `@supports (opacity: 0.5)` until
the painter blends. The approximation list generalises for it: props.c's
`kDisplayApproximated` becomes a list of (property, value) pairs —
`kApproximated`, with `z-index` matching any integer — because the name
would be stale the day `position` joined it.

### Pass 1 — style

`flow_style_t` gains `position`, `inset[4]` (`flow_length_t`, AUTO for
`auto`), `z_index` with a flag for `auto`, and `opacity` in thousandths
(see the face below). CSS 2.1 § 9.7 is applied here, where display is: a
box that leaves the flow has its `display` BLOCKIFIED and its `float`
`none` — an absolute box from P1, a fixed one from P2, which until then
keeps the display it was given (a fixed span in a link must not split its
paragraph) — and beside the computed display one bit is kept, **`specified_inline`**,
that the specified display was inline-level, because the static position
needs it (below) and blockifying destroys it. Pass 1 also answers, parent
before child as it answers the link an element sits in, each element's
nearest positioned ancestor that makes a box (the containing block of an
absolute box inside it) and whether it or an ancestor has `opacity: 0`.

The first producer can position a box now: the Rendering chapter's
`dialog` rule is `position: absolute`, both inline insets 0, `margin:
auto`, `width: fit-content`, and it is paid in P1 (LAYOUT.md books
"`dialog` and `popover` positioning" with this document as its trigger).
So a page with no sheet can have a positioned box, and `fit-content` on
an absolute box with both sides set shrinks to fit where `auto` would
stretch: `flow_length_t` gains a FIT_CONTENT kind, whose producer is the
chapter alone (an author's `fit-content` is still read as `auto`, GARB.md
§ Booked).

Until their own slices, `fixed` (P1 alone) and `sticky` (until P4) are
laid out as `relative` with their offsets ignored — never as `static`:
a positioned box is its
absolute descendants' containing block, and a sticky nav bar with an
absolute dropdown inside it is ordinary.

### Pass 2 — boxes

- **Out of flow.** An absolute box is marked out of flow, and from P2 a
  fixed one. It
  hangs under its parent BLOCK CONTAINER in the box tree, and the
  formatting context it was written in skips it: no line holds it, no
  margin collapses through it, no anonymous block is made round it.
- **The holds-a-block bit** (`contributes_block`) counts IN-FLOW blocks
  only. An out-of-flow child contributes nothing, or an absolute badge in
  an `<a>` would split the link round it — LAYOUT.md's third review called
  the bit "contains-an-in-flow-block", and this is the day the qualifier
  matters.
- **Among inline content** an out-of-flow box leaves a PLACEHOLDER item in
  the item sequence, as every engine does: zero width, no break
  opportunity, nothing to a line's width or height or to the min- and
  max-content widths. It records the static position when its line is
  PLACED — after `text-align` has moved the line.
- **The list.** Every positioned element — relative, absolute, fixed —
  and every containing block is appended to the tree's positioned list in
  tree order, and each out-of-flow box is filed under its containing
  block's entry: the nearest positioned ancestor (a box, or an INLINE
  element, whose containing block pass 3 computes from its pieces), else
  the initial containing block. Whether a box is positioned and out of
  flow comes from its style, not from its entry, so a build that stops
  before the entry is made leaves an absolute box filed nowhere — never
  laid out — rather than one in a flow the whole build takes it out of.

### Pass 3 — layout

- **Relative.** A relative box moves, with everything inside it, by its
  offsets (`left` wins over `right`, `top` over `bottom`; a percentage is
  of the containing block, a vertical one only when that block's height
  is given in pixels, CSS 2.1 § 9.3.2 computing it to auto otherwise). It
  is LAID OUT where it moves to — the flow goes on as if it had not moved
  — rather than laid out and then moved: what finishes inside it is then
  where it stays, which relation (b) below asserts and the move-after
  version broke for every box inside a relative box a layout stopped in.
  A table cell is placed by its table, so it moves once placed. On an
  INLINE element that means its SPAN pieces and fragments on every line —
  `position: relative; top: -0.4em` on a `span` is how pages nudge text —
  moved as the line is placed, the offsets of every relative inline open
  round a fragment added up. Nothing else moves.
- **Static positions.** A placeholder whose element was specified
  inline-level (`specified_inline`) records the pen where it stands. One
  specified block-level would have broken the line where something in the
  flow — text, an atom, a marker — stood before it, so it records the
  START OF THE NEXT LINE, at the container's content edge; with nothing
  before it on its line, the START OF THIS ONE. Among blocks, a
  box records the top edge below the PREVIOUS sibling's margin alone — not
  the margin that sibling collapses to with the next one, which has not
  been met when the placeholder is passed (ruling 5, Chrome: 10 below a
  block whose 10px margin collapses to 40 with the next).
- **Absolute.** An absolute box is laid out when its containing block is
  FINISHED — its height is part of the equation — as a block formatting
  context of its own, with § 10.3.7's and § 10.6.4's solutions against the
  containing block's padding box: one solver for both axes, which are the
  same equation with the same cases, the width held to its limits and
  solved again (§ 10.4). An inline containing block's rect is the
  bounding box of the padding boxes of its first and last pieces
  (§ 10.1, 4.1), known once every piece is placed — when the block
  container they are all in is finished. A table cell's is known once its
  row has stretched it. An absolute TABLE is laid out as the flow would
  lay it out in the room its insets leave, and then moved to where they
  put it, its captions with it.
- **Fixed** (P2). An absolute box whose containing block is the viewport,
  `(0, 0, width, viewport height)`, whatever is round it, and whose rects
  are VIEWPORT coordinates, flagged so — with everything laid out inside
  it. The numbers are the initial containing block's, so it is filed and
  laid out with that block's absolute boxes. On an axis whose insets are
  both `auto`, its static position is read as a viewport coordinate: where
  it would stand with the page not scrolled, and there it stays.
- **All or nothing.** An out-of-flow box that does not finish is not
  placed, as a table cut short keeps its place with no rows: its
  containing block is marked unfinished and the tree is `incomplete`.
  When layout stops in the in-flow content, the out-of-flow boxes of
  containing blocks that did finish keep their layouts; those of the one
  that did not are not placed.
- **The page's extent** (LAYOUT.md's "what comes out") takes a relative
  or absolute box's far edges from the list, as it takes any box's — what
  a page places below its end can be scrolled to — never a fixed box's,
  and never anything left of or above the page's origin: content at a
  negative coordinate is drawn where it reaches the glass and cannot be
  scrolled to, as in every browser. That is what makes `left: -9999px`
  hide text.
- **Clips follow containing blocks.** A box's clip is what ITS containing
  blocks allow: for an in-flow box that is its tree parent's, so nothing
  changes for the pages laid out today; for a positioned box it is its
  containing block's, so a dropdown whose containing block is outside an
  `overflow: hidden` box escapes it, as CSS 2.1 § 11.1.1 and every browser
  have it.

### The door

- **Paint order, P1.** `flow_box_t` gains `positioned`, and the tree
  hands its list over in paint order (`flow_npositioned`,
  `flow_positioned`). `flow_visit` walks the ordinary tree as it does
  today — blocks, then inline content — SKIPPING positioned subtrees; then
  it walks the positioned list in order, each positioned box as its own
  small two-phase walk that skips the positioned boxes nested in it, which
  come later in the list. That is Appendix E's step 8 with every
  `z-index` read as `auto`. A relative INLINE (or atom) is the exception:
  it has no box of its own to list, only pieces on lines, so it moves and
  paints in the flow's order (booked). P3 then sorts the list by
  `z-index` and nests it by stacking context, and nothing else changes.
- **Hit testing** tries the positioned list backwards, then the ordinary
  tree, so a click lands on what is on top. `hit` tests a box's clip
  against the box's OWN rect and prunes children on overflow alone, since
  a positioned child may have a wider clip than its tree parent.
- **Overflow and containment.** A positioned subtree is left out of its
  tree ancestors' overflow rects, and each positioned box's own overflow
  rect covers its subtree less the positioned boxes nested in it. So the
  containment invariant stays literally true where it is asserted (every
  in-flow child inside its parent's overflow rect), and every walk that
  prunes on a rect still finds everything drawn inside the view.
- **Fixed coordinates** (P2). `flow_box_t` gains `fixed` (in a fixed
  box's subtree: its rects are viewport coordinates).
  `flow_visit` and `flow_hit` take the scroll offset — the rect yonder
  passes is the view's DIRTY PART, not the view, so the scroll cannot be
  derived from it — and the rule is written ONCE: `flow_box_doc_rect(box,
  scroll)` answers a box's rect in document coordinates (its rect, or its
  rect plus the scroll for a fixed one), and the same for its clip; both
  are `flow_box_doc_offset`, which is the rule. Every
  face site that reads a rect today — scrolling to a node or a fragment,
  pictures' dirty rects, widget placement — changes one expression and
  keeps working in document coordinates. This departs from LAYOUT.md's
  promise that no field changes its meaning and no signature at the door
  changes shape, and from flow.h's "whole document pixels"; the slice that
  does it amends both, out loud.
- **Covered.** `flow_box_covered(tree, box, scroll)` answers whether the
  pointer cannot reach `box`: `flow_hit` at its centre answers neither it
  nor anything inside it (ruling 9), so a face that draws live widgets
  over the page — yonder's form controls — hides one that is covered and
  draws its frame, as `forms_place` already does for one that is not
  wholly in view. A text field under a fixed header must not draw over the
  header and take its click. Paid in P2, where overlap becomes common;
  owed from P1.

### The face

- yonder hands ONE viewport height to both the cascade (`garb_env_t`,
  which `vh` and the media queries already read) and libflow
  (`flow_env_t.viewport_height`, which the initial containing block
  needs), in P1; `flow_layout` refuses a cascade judged at another height,
  so the two can never disagree. It cannot be the cascade's alone: the
  chapter positions `dialog` with no cascade present. yonder already lays
  a page out again when the height changes and it has sheets; with P1 it
  does so for any page that has a positioned box.
- It passes the scroll to `flow_visit`/`flow_hit` and reads rects through
  `flow_box_doc_rect` (P2). yonder's painter works in each box's own
  coordinates and adds `flow_box_doc_offset` where it hands a rectangle
  to a verb; its text verb is handed the run's origin rather than reading
  the box's, since that is the one verb that read a box's position itself.

### What positioning costs a browser with no JavaScript

Positioning meets two things yonder does not have, and the costs are
larger than a centred modal sitting off-centre:

- **Nothing can be dismissed.** A cookie wall, a newsletter modal, an
  off-canvas menu is closed by a click that runs script. In the flow they
  are ugly and can be scrolled past; after P2, `position: fixed; inset: 0`
  covers the glass at every scroll position, for good.
- **What script would reveal is shown.** The commonest ways to hide a
  positioned box until script shows it are `opacity: 0`, `visibility:
  hidden` (honoured today) and `transform: translateX(-100%)`.
- **Translucent colours are opaque**: `xrgb` lays alpha over the page's
  paper, because what is really underneath is not known, so `background:
  rgba(0, 0, 0, 0.5)` on a full-viewport overlay is a solid grey sheet.

Three answers, none of which needs a blend or a transform, all in P1:
**`opacity: 0` is not painted** — `flow_box_t.unpainted`, on the box and
everything inside it, which `flow_visit` does not hand over, while
`flow_hit` still finds it as a browser's pointer does unless it is
`pointer-events: none`, and a form control's widget is drawn whatever
its opacity unless the pointer cannot reach it (rulings 6, 7 and 9;
values between 0 and 1 stay booked); **a
background whose alpha is below 1, on an out-of-flow box, is not painted**
until the painter blends — seeing through an overlay is a better failure
than seeing only the overlay; and **yonder's `p` key lays pages out
with positioning off**, a mode until it is pressed again (ruling 8;
`flow_env_t`'s `static_only`: every box
`static` — today's layout, but for an open dialog, which the chapter still
sizes to fit and so centres in the flow) — the "kill sticky" bookmarklet
made a feature, and the instrument for the Chrome comparison: the same
page with and without, one keypress apart.

## Slices

Each slice is reviewed with Fable before the next starts, as the stack was.

| Slice | What | Proof |
|---|---|---|
| P1 | libgarb reads the properties; pass 1 carries them, blockifies with `specified_inline`, and pays `dialog`; `relative` on blocks and inlines; `absolute` against block, inline and initial containing blocks, with static positions, § 10.3.7/§ 10.6.4 sizing and percentage heights; the positioned list, the paint layer and the reversed hit test; clips along containing blocks; the page's extent; `viewport_height`; `fixed` and `sticky` as `relative` meanwhile; `opacity: 0`, translucent out-of-flow backgrounds, and the `static_only` key | hand-laid pages for every row of § 10.3.7's table and § 10.6.4's; the corpus dumps unchanged where no sheet positions anything; the partial-tree relations as restated below; the positioned fuzz |
| P2 | `fixed`: viewport coordinates, the `fixed` flag, `flow_visit`/`flow_hit` with the scroll, `flow_box_doc_rect`, `flow_box_covered` and yonder's widgets, LAYOUT.md's promise amended | hand-laid pages; a guest page scrolled with a fixed header held on the glass and a form field scrolled under it |
| P3 | `z-index` and stacking contexts: the list sorted and nested | hand-laid overlapping pages whose paint order is written out; a dropdown over the content it overlaps, clicked |
| P4 | `sticky` | a guest page whose header sticks at `top: 0` and lets go at its container's end |

## Proof, and the invariants that change

- **Sizes, not origins.** Every rect's SIZE is non-negative. Origins may
  be negative anywhere — negative margins made that so in G3, and
  `check_tree` already asserts sizes only; LAYOUT.md's prose is amended to
  match.
- **Containment,** as the door states it: every in-flow child's overflow
  rect inside its parent's; every positioned box's inside its own list
  entry's; a fixed subtree compared against the viewport, never the page.
- **The partial-tree relations**, restated so the assertion is not
  invented at the keyboard. Out-of-flow boxes are all or nothing (pass 3
  above). Relations (a) — the pre-order is a prefix — and (c) — every
  unfinished box is an ancestor of the last — are asserted over the
  IN-FLOW tree; placed out-of-flow boxes are compared under (b) only, and
  (d) — nothing unplaced reaches the page's edges — holds for both, fixed
  boxes excepted as always.
- **The positioned fuzz.** Today's fuzz lays out with no cascade and a
  soup with no `style` attribute, so it can prove nothing about positioned
  boxes. The soup gains `style=` attributes drawn from `position`, the
  four offsets (negative, huge, percent, `auto`), `overflow`, `z-index`,
  `width`, `height` and `opacity`, and the fuzz lays out with a cascade —
  of those and of each corpus page's own sheets. That is the instrument
  that would have caught the containment and reachability problems of the
  first draft mechanically. Containment is of what a box DRAWS, its
  overflow rect met with its clip: a box that clips reaches no further
  than its border box, whatever its content says, which the fuzz met the
  first time it read wikipedia's sheets.
- **Fixtures**, hand-computed (F2's rule): an absolute box in each corner
  of a relative parent; the corner badge inside a relative `<a>` (an
  inline containing block); `left: 0; right: 0` stretching; `width: auto`
  with one side set shrinking to fit; every `auto` combination in § 10.3.7
  and § 10.6.4, margins `auto` included; percentages, heights among them;
  an absolute `span` and an absolute `div` written mid-line (the static
  position each way); a static position among blocks; one against the
  initial containing block, `bottom: 0`; one inside an `overflow: hidden`
  box that is and is not on its containing-block chain; `left: -9999px`;
  relative boxes nested three deep, and a relative `span` across a line
  break; a table cell, a list item and an inline-block as containing
  blocks; an open `dialog` with no sheet; a modal written last whose
  background is painted over the page's text and whose text is on top of
  its own background.

## Rulings

Chris, 2026-09-29, each as recommended: "I really tried to find something
to disagree with. No luck."

1. **The slice order**: P1 → P2 → P3 → P4, with the paint layer in P1 so
   no slice ships a page whose text shows through its own dialog.
   Recommended; with the layer in P1, overlap is right for every page that
   writes no `z-index`, and the case for P3 before P2 mostly goes away.
2. **Negative coordinates are allowed and cannot be scrolled to**, as in
   every browser. Recommended; clamping would show what `left: -9999px`
   hides.
3. **`flow_visit` and `flow_hit` take the scroll, and rects are read
   through `flow_box_doc_rect`**, amending LAYOUT.md's promise in P2.
   Recommended: the scroll cannot be derived from the dirty part the face
   hands in, and the helper keeps the fixed rule in one place.
4. **`transform` stays booked, and three things arrive with P1 instead**:
   `opacity: 0` not painted, translucent backgrounds on out-of-flow boxes
   not painted, and the `static_only` key. Recommended: after P2 a fixed
   overlay nothing can dismiss would otherwise make pages unreadable that
   today are merely awkward.
5. **A test page viewed in Chrome** before P1's static-position fixtures
   are written: an absolute box between two blocks whose margins collapse,
   to settle which margin its static position sits below
   (`tools/position_probe/static-position.html`, which prints what Chrome
   measured). Chrome 154 answered: 10, the previous sibling's own margin;
   and the mid-line cases as designed — an absolute `span` at the pen, an
   absolute `div` at the start of the next line. And, asked on Fable's
   review of the code (the probe's case 3, answered by Chrome 154 the same
   day): an absolute `div` that LEADS its line, with nothing before it,
   stands at that line's top.

Three more, on Fable's review of P1's code, Chris 2026-09-29, each as
recommended ("I trust your judgement"):

6. **A form control is drawn whatever its opacity.** An invisible control
   is nearly always a custom checkbox's real input, hidden so a styled
   stand-in can follow its state, which it cannot here (`:checked` does not
   follow a click); hiding the widget too would leave a form nobody can
   use. `unpainted` keeps the rest of the box unpainted. (Ruling 9 makes
   the one exception.)
7. **`pointer-events: none` is read**, and `flow_hit` passes through such
   a box — the property that says a click goes to what is under it. An
   invisible overlay otherwise eats every click over the links a person
   can see, forever, with no script to show it.
8. **Positioning off is a MODE**: `p` holds across navigation until it is
   pressed again, and the status line of every page says so while it is
   in force.

And one more, on the third round, which Chris left to his reviewer —
"I'm deferring to you on this question"; an OS developer and a web user,
he said, not a CSS one — and Fable ruled as he had recommended:

9. **A control the pointer cannot reach is not drawn**: its box unpainted
   AND `pointer-events: none`. Rulings 6 and 7 meet there: a custom
   checkbox's real input takes the pointer and is drawn; a hidden dialog's
   or search overlay's controls do not, and a text field drawn from one
   would float over the page, unlabelled, taking typing. Fable made it
   exact on P2's review, where fixed boxes made covering common: a
   control's widget is drawn exactly when `flow_hit` at the control's
   CENTRE answers the control or something inside it. That holds ruling
   9's case, a field under a fixed header, one `visibility: hidden`, and
   leaves usable a field a floating label or a corner icon only touches;
   it gives up a header crossing a field short of its centre.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| `transform` (and `translate()` centring) | a transform is a new kind of geometry: a box's rect is no longer where it is drawn | the first centred modal that matters after P1, which will be the first one |
| `opacity` between 0 and 1, translucent colours | a blend the painter does not have | pile 3's painter work |
| `clip`, `clip-path` | the first is deprecated; the second is a path (Bootstrap's `.visually-hidden` hides without `clip`, from P1) | a page that needs one to be readable |
| Floats beside positioned boxes | libflow has no floats; a static position among floats is not computed | floats |
| `position: relative` on table rows, row groups and columns | undefined in CSS 2.1, defined in Position 3 | a page that needs it |
| The logical insets (`inset-inline-start` and the rest) | the physical four first; the chapter's `dialog` rule is applied physically | a page that writes them |
| Percentage heights of an absolute box's descendants | GARB.md's percentage-height row, unchanged; the absolute box's own resolves | that row |
| Scrolling a positioned box's own overflow | nothing scrolls a box yet | box scrolling |
| Form widgets under positioned boxes, between P1 and P2 | `flow_box_covered` is P2's | P2 |
| A relative inline, or atom, painted in the positioned layer | an inline has no box to list, only pieces spread over lines: they move and paint in the flow's order | a page where a nudged inline must paint over its neighbours |
| A block inside a relative inline, moved with it (CSS 2.1 § 9.2.1.1) | the offset lives on the inline's pieces and fragments; the block the inline is split round is laid out by the block flow, which knows no inline open round it | a page that nudges an inline holding a block by a visible amount |
| `position` on the root element | it is laid out in the flow — absolute or not — and a relative root is a containing block that does not move | a page that positions its `html` |

## Review record

Fable read the first draft at Chris's request the day it was written, a
review given privately, as design reviews are here. Twelve findings, each
answered above:

1. Paint order (HIGH): `flow_visit` paints every block page-wide before
   any text, so "tree order until P3" would have put a modal's background
   under the page's text. The positioned layer moved into P1.
2. Clips and containment (HIGH): an escaped box broke the overflow rect
   its tree ancestor was trimmed to. Positioned boxes are reached from the
   list and left out of their tree ancestors' overflow rects; clips follow
   containing blocks.
3. Fixed reachability (HIGH): the walks prune on overflow rects that could
   not hold a fixed box's viewport coordinates. The same list reaches it.
4. The initial containing block is the viewport, not the root box; one
   height, handed to both the cascade and libflow, in P1.
5. Inline parents, inline containing blocks, relative inlines, and the
   holds-a-block bit.
6. The static position needs the specified display: `specified_inline`.
7. The restated invariant was false (negative margins) and unexercised
   (no positioned fuzz): sizes only, and the positioned fuzz.
8. The partial-tree relations for deferred layout: out-of-flow boxes all
   or nothing, relations over the in-flow tree.
9. Form widgets over positioned boxes: `flow_box_covered`, in P2.
10. The fixed rule in every face site: `flow_box_doc_rect`, and
    LAYOUT.md's promise amended out loud.
11. `sticky` and `fixed` degrade to `relative`, not `static`; `z-index`'s
    `@supports`; the approximation list generalised.
12. `dialog`'s trigger fired; booked rows added; an absolute box's own
    percentage height resolves.

And on the fourth ruling, the three P1 answers to positioning without
script, which were Fable's.
