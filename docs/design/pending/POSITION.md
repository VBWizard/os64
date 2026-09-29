# POSITION.md — positioned layout: boxes that leave the flow

*Written 2026-09-29 by Opus, the day the stack that brought libgarb's first
pile to yonder was merged and Chris set yonder beside Chrome on the P5:
"most of my observations are about exactly that". This is the first slice
of GARB.md's pile 2. The library is libflow and its rules are
[LAYOUT.md](../completed/LAYOUT.md)'s; the properties come from libgarb
([GARB.md](../completed/GARB.md)); the face is yonder
([YONDER.md](../completed/YONDER.md)). Nothing here is built until Chris
has read it and the rulings at the end are his.*

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

## What CSS asks (CSS 2.1 § 9.3, § 9.6, § 9.9, § 10, Appendix E; CSS Position 3)

`position` is one of five values:

| Value | In the flow? | Placed against | Moves with the page? |
|---|---|---|---|
| `static` | yes | its place in the flow | yes |
| `relative` | yes: its space is kept where the flow put it | its own place in the flow, shifted by the offsets | yes |
| `absolute` | no | its CONTAINING BLOCK: the padding box of the nearest positioned ancestor, else the initial containing block | yes |
| `fixed` | no | the viewport | no: it stays where it is on the glass |
| `sticky` | yes | its place in the flow, until scrolling would carry it past an offset | stops at the offset |

The offsets are `top`, `right`, `bottom` and `left` (and `inset`, which
sets all four): lengths, percentages of the containing block, or `auto`.
An absolute box's width and height solve CSS 2.1 § 10.3.7 and § 10.6.4:
`left + margin + border + padding + width + ... + right` equals the
containing block, and whatever is `auto` takes the rest — which is how
`left: 0; right: 0` stretches a box, and how `width: auto` with one side
set SHRINKS TO FIT, as an inline-block does (§ 10.3.9 already has that
code). Where both offsets on an axis are `auto`, the box sits at its
STATIC POSITION: where it would have been in the flow.

`z-index` orders what overlaps. A positioned box with a `z-index` other
than `auto` makes a STACKING CONTEXT; within one, Appendix E paints the
context's own background, then its children with negative `z-index`, then
the in-flow blocks, then the inline content, then the positioned
descendants with `z-index: auto` or `0` in tree order, then the positive
ones, lowest first. The root is the first stacking context. `opacity`
below 1 and a `transform` make one too; neither is laid out yet (Booked).

## How it enters libflow

Each of LAYOUT.md's three passes takes a part, and the face takes one.
The shape follows the rule libflow already keeps: one door, geometry in
document coordinates, and the proof invariants stated before the code.

**libgarb** reads `position`, `top`, `right`, `bottom`, `left`, `inset`
and `z-index` (an integer, or `auto`) into its property table, the usual
way. A `position` value no slice lays out yet is READ — the cascade keeps
it, and libflow lays the box out as `static` — and is NOT SUPPORTED:
`@supports (position: sticky)` says no until P4, by the rule
`kDisplayApproximated` keeps for display (props.c: the list is every
approximation, of any property).

**Pass 1, style.** `flow_style_t` gains `position`, `inset[4]`
(`flow_length_t`, AUTO for `auto`) and `z_index` with a flag for `auto`.
CSS 2.1 § 9.7's rule is applied here, where display is: an absolute or
fixed box's `display` is BLOCKIFIED — an absolutely positioned `span` is
a block — and its `float` is `none`.

**Pass 2, boxes.** An absolute or fixed box is OUT OF FLOW: it keeps its
place in the box tree — under the box of its parent element, so the
tree's pre-order is still the document's and LAYOUT.md's partial-tree
relation (a) still holds — but it is marked, and the formatting context
it sits in skips it: no line holds it, no margin collapses through it, no
anonymous block is made round it. What the context leaves behind is its
STATIC POSITION, recorded where the box would have started: at the
current line's pen for one written among inline content, at the next
block's top for one among blocks. Each out-of-flow box is also listed
against its containing block — the nearest ancestor box whose style is
positioned, else the root — so pass 3 reaches it without a walk.

**Pass 3, layout.**
- A RELATIVE box is laid out exactly where the flow puts it, and then
  moved, with everything inside it, by its offsets (`left` wins over
  `right`, `top` over `bottom`; a percentage is of the containing block).
  Nothing else moves: its space stays where it was.
- An ABSOLUTE box is laid out when its containing block is FINISHED —
  its height is part of the equation — as a block container of its own
  (a new block formatting context), with § 10.3.7's and § 10.6.4's
  solutions for its position and size, against the containing block's
  padding box. Its content lays out as any block's does.
- A FIXED box is an absolute one whose containing block is the viewport:
  `(0, 0, width, viewport height)`. Its rects are in VIEWPORT coordinates,
  and it is flagged so, because the face draws it where it is on the
  glass, not where it is on the page.
- The page's extent (LAYOUT.md's "what comes out": its width and height)
  takes an absolute box's far edges as it takes any box's — what a page
  places below its end can be scrolled to — but never a fixed box's, and
  never anything left of or above the page's origin: content placed at a
  negative coordinate is drawn if it reaches the glass, and cannot be
  scrolled to, as in every browser. That is what makes `left: -9999px`
  hide text.
- `overflow` that clips cuts an absolute descendant only when the
  clipping box is on that descendant's containing-block chain: a dropdown
  whose containing block is outside an `overflow: hidden` box escapes it,
  as CSS 2.1 § 11.1.1 and every browser have it.

**The door.** The public tree says which boxes are positioned, which are
fixed, and the paint order, so no face re-derives Appendix E:
- `flow_box_t` gains `positioned` (not static) and `fixed` (in a fixed
  box's subtree: its rects are viewport coordinates).
- The door builds, per stacking context, its children in PAINT ORDER, once,
  into the tree's arena; `flow_visit` walks that order instead of tree
  order, and `flow_hit` walks it backwards, the last painted winning, so
  a click lands on what is on top.
- `flow_visit` and `flow_hit` take the scroll offset, so they can test a
  fixed box against the glass rather than the page. A face that places
  things itself (yonder's form widgets and pictures) adds the scroll to a
  box whose `fixed` is false and not to one whose `fixed` is true.

**The face.** yonder repaints the whole view on every scroll already, so a
fixed box costs nothing new to draw where it belongs; it passes the scroll
to `flow_visit`/`flow_hit`, places a fixed control's widget and a fixed
picture by viewport coordinates, and tells libflow the viewport's height
(`flow_env_t.viewport_height`), which the initial containing block and
every fixed box need and which layout does not have today.

## Slices

Each slice is reviewed with Fable before the next starts, as the stack was.

| Slice | What | Proof |
|---|---|---|
| P1 | libgarb reads the properties; pass 1 carries them and blockifies; `relative` offsets; `absolute` against its containing block with static positions, § 10.3.7/§ 10.6.4 sizing, the page's extent, and the overflow-clip chain; yonder draws it | hand-laid pages for every row of § 10.3.7's table and § 10.6.4's; the corpus dumps unchanged where no sheet positions anything; the allocation sweep with positioned boxes in it |
| P2 | `fixed`: the viewport as containing block, `viewport_height`, the `fixed` flag, `flow_visit`/`flow_hit` with the scroll, yonder's widgets and pictures | hand-laid pages; a guest page scrolled with a fixed header held on the glass |
| P3 | `z-index` and stacking contexts: the door's paint order, the hit test reversed | hand-laid overlapping pages whose paint order is written out; a dropdown over the content it overlaps, clicked |
| P4 | `sticky` | a guest page whose header sticks at `top: 0` and lets go at its container's end |

P1 on its own fixes most of what shows: the dropdowns and modals leave the
flow (they paint in tree order until P3, which is usually on top anyway —
they are written late in the document), the corner badges land on their
corners, and `-9999px` hides what it hides.

## Proof, and the invariants that change

LAYOUT.md's harness asserts, on every fuzzed tree, that every rect is
non-negative and every child's overflow rect lies inside its parent's.
The second still holds: an out-of-flow box stays under its parent in the
tree, and an overflow rect is the union of the subtree's, wherever its
boxes were placed. The first does not, and is RESTATED rather than
dropped: every rect's SIZE is non-negative, and a negative ORIGIN is
allowed only in a positioned box's subtree. A fixed box's subtree is
checked against the viewport instead of the page.

Fixtures are hand-computed, F2's rule: fixed expected geometry, never a
self-consistency test. The families: an absolute box in each corner of a
relative parent; `left: 0; right: 0` stretching; `width: auto` with one
side set shrinking to fit; every `auto` combination in § 10.3.7 and
§ 10.6.4, margins `auto` included (which centre a box with both sides
set); percentages of the containing block; a static position among
inline content and among blocks; an absolute box whose containing block
is the root; one inside an `overflow: hidden` box that is and is not on
its containing-block chain; `left: -9999px`; relative boxes nested three
deep; and a table cell, a list item and an inline-block as containing
blocks.

## Rulings Chris is asked for

1. **The slice order**: P1 (relative and absolute) → P2 (fixed) → P3
   (z-index and stacking) → P4 (sticky). The recommendation is this order;
   P3 before P2 is the alternative if overlap order matters more on the
   pages Chris has than headers that stay put.
2. **Negative coordinates are allowed and cannot be scrolled to**, as in
   every browser; the invariant is restated as above. The alternative,
   clamping to 0, would show `left: -9999px` text at the left edge.
3. **`flow_visit` and `flow_hit` take the scroll offset** (the face passes
   it; a fixed box is tested against the glass). The alternative — the
   face translates fixed subtrees itself — would put Appendix E's order
   and the fixed rule in every face.
4. **`transform` and `opacity` stay booked** (below), knowing what that
   costs: the web's commonest way to centre a box is `top: 50%; left: 50%;
   transform: translate(-50%, -50%)`, so a centred modal will sit with its
   top-left corner at the centre until transforms are laid out.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| `transform` (and `translate()` centring) | a transform is a new kind of geometry: a box's rect is no longer where it is drawn | the first centred modal that matters after P1, which will be the first one |
| `opacity` below 1 | a stacking context and a blend the painter does not have | pile 3's painter work |
| `clip`, `clip-path` | the first is deprecated; the second is a path | a page that needs one to be readable |
| Floats beside positioned boxes | libflow has no floats; a static position among floats is not computed | floats |
| Scrolling a positioned box's own overflow | nothing scrolls a box yet (LAYOUT.md's overflow rows) | box scrolling |
