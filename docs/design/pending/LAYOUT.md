# LAYOUT.md — a tree in, boxes out: yonder's layout library

*Written 2026-09-25 by Fable, after the six work packets in `docs/yonder/`
and the rulings of 2026-09-24. This is LIBPAGE.md's twin: the constitution
for the one boss BROWSER.md names and the campaign never designed, because
the ladder was built to reach it and not to guess at it. The library's
placeholder name is `libflow` — normal flow is CSS's own word for what it
does — and nothing below depends on the name; Chris settles it.
Reviewed twice by Opus, then handed to Opus the same day (Chris: "Doc's
all yours now") — Opus owns the document and builds the library.*

## The ruling this rests on

Two things were decided 2026-09-24 and every section below is one of them
applied:

1. **The visual formatting model is built to CSS 2.1 WHOLE; the cascade
   is not built yet.** The box model, margins, line boxes, white-space,
   the table algorithm: these are the standard's whatever produces the
   styles, and LIBPAGE.md's lesson was that a half-rule in a durable layer
   costs review rounds until it is finished. What produces the styles in
   the first cut is the WHATWG standard's Rendering chapter — which is
   written AS a stylesheet — hand-compiled into a computed-style struct,
   presentational attributes included. The cascade arrives later as a
   SECOND PRODUCER of the same struct: selectors, specificity and author
   sheets are a feature, and the campaign's stance ("build it when
   something asks") governs features. **The design test for every field
   in the struct: it is a field the first producer can SET, held in the
   SHAPE the cascade will want** — the property's own computed value, its
   own units, its own initial value. The struct will GROW when the cascade
   brings properties nothing here lays out yet (`line-height`,
   `text-indent`, border style and colour, `min-`/`max-width`,
   `background-image`, `pre-line`); what the promise of "an unchanged
   engine" means is that no field present today changes its meaning, its
   units or its name, and no signature at the door changes shape. The one
   place that promise needed a decision NOW is the font family: a cascade
   writes a NAME LIST (`font-family: Georgia, "Times New Roman", serif`),
   so the struct holds a list ending in a generic from the first line, and
   the resolver's signature takes that list — settled before packet 04
   freezes its API, because a signature is the thing a later change cannot
   hide.
2. **JavaScript after the first daily-driver browser, and nothing here may
   make that day worse** (LIBPAGE.md ruling 2). So the box tree is DERIVED
   from the tree and the model, is rebuilt whole from them, and holds
   nothing a rebuild cannot recreate. Incremental relayout is booked, not
   designed around.

And the house rule that made `wend`'s renderer and libpage reviewable:
**pure computation, host harness, checked-in dumps.** A tree, a model, a
width and a font resolver in; a box tree out; no I/O, no syscalls, no
window. The harness renders the corpus and diffs box coordinates.

## Where it sits

```
  yonder (the face)      the window, the page-view widget, scrolling, painting,
                         clicks, the image fetch loop, form widgets, history
  ──────────────────────── the seam ────────────────────────────────────────
  libflow                 style: tree → computed style per element
                          boxes: tree + style → box tree (anonymous boxes made)
                          layout: box tree + width → positioned boxes with text runs
  libpage                 what a link, control, image or form IS (never re-decided here)
  libhtml                 the tree            libos64 text engine   runs, fit, carets
```

- **Above the seam** is everything that touches a pixel or a person: the
  face paints the boxes, hit-tests a click into a box and asks libpage
  what the node behind it means, fetches images and tells the engine
  their sizes, instantiates a libui widget for each control and places it
  where the engine put its box. The face decides nothing about geometry.
- **Below the seam** libflow decides ALL the geometry and none of the
  meaning. It never resolves a URL (libpage did), never asks what a
  control holds (libpage knows), never opens a font file (the resolver it
  is handed does), never decodes an image (it is told the size). It reads
  the tree and the model, and it writes boxes.
- The tree is read-only and document-owned (libhtml); the model is
  libpage's; libflow points at both and copies no text it can point at.
  Text runs are the one thing it OWNS: the engine's runs are immutable and
  retained, so the box tree holds the run handles and releases them when
  it is freed.

## What goes in

- **The document** (`os64_html_document_t`), complete or refused; a
  refusal yields a tree, libflow lays out what there is and reports
  `incomplete`, as libpage and `wend` do.
- **The model** (`os64_page_t`), for the questions that are meaning: is
  this `a` a link (`os64_page_link_for`), which control is this node
  (`os64_page_control_for`), what does this control HOLD right now (its
  value, checkedness, options — the person's edits included, since libpage
  keeps them), is this subtree hidden. And the images, each with its
  resolved `src` (`os64_page_image_for`) — see *What the consumers owe*.
- **The available width**, in pixels: the page view's content width. The
  height is an OUTPUT; a page is as tall as it is.
- **A font resolver**: a callback table the face supplies, answering
  `(family list, bold, italic, pixel size) → the ordered font list and
  its metrics`, where the family list is names as the page wrote them
  followed by a generic (serif, sans, mono) — packet 04's family API in
  the guest (which matches no name in its first cut and takes the generic
  tail; that is the resolver's business, and the day it matches names
  nothing here changes), `tools/fonts/`'s fake backend on the host, which
  is what makes the dumps deterministic across machines and FreeType
  versions.
- **A replaced-size oracle**: a callback answering `node → (w, h, known)`
  for images and form controls, because an image's intrinsic size arrives
  from a fetch the engine never sees, and a control's natural size is the
  face's theme and fonts (a libui textfield's row height is libui's to
  say). Unknown is an honest answer; the engine has a rule for it.
- **The viewport's font size** (16 px unless the face says otherwise) and
  the three colours a face owns — the page's default ink, the link colour,
  the paper — so the dumps carry no theme.

## What comes out

**A box tree**, every box carrying its node (or NULL for an anonymous
box), its computed style, its kind, and after layout TWO rectangles in
DOCUMENT coordinates — absolute, integer pixels, x from the content edge,
y from the top of the page — because the face paints and hit-tests by
rectangle and a rectangle relative to a parent would make every click a
walk. The first is the box's own border rect. The second is its
**overflow rect**: its own rect joined with every descendant's, because
content is ALLOWED to overflow here in three named places (a set height
smaller than its content, a table that will not squash a word below its
min-content width, a word wider than its line) and a walk that pruned on
the own rect would skip visible children in all three. Pruning, hit-
testing and the harness's containment invariant all use the overflow
rect. Boxes are:

- **Block containers** (`p`, `div`, `li`, a cell's content, the body…),
  holding either block children or line boxes, never both — CSS 2.1
  §9.2.1.1's anonymous block boxes are made where a page mixes them.
- **Line boxes**, each holding the inline fragments laid on one line.
- **Text fragments**: one run of the text engine each, with the byte range
  of the tree text it came from (after white-space processing, so the
  range is into a per-node collapsed copy the box tree owns) — the range
  is what lets a future selection map a pixel to a source byte.
- **Inline boxes** (`b`, `a`, `span`…) as ranges over the fragments they
  contain on each line: what a link underline or a background is painted
  behind. An inline box that spans lines has one fragment range per line.
- **Replaced boxes**: an image, a form control, a frame's placeholder, an
  `hr` (a replaced box the engine paints itself, described in the dump).
- **Table boxes**: table, caption, row group, row, cell, with the column
  widths the algorithm settled on carried on the table.
- **List markers**: outside the `li`'s content, positioned; their text
  (`•`, `3.`, `iv.`) generated here, since the marker is geometry and
  the counter is not the page's data.

Plus four facts about the whole: the page height, the page WIDTH (the
root's overflow width, which exceeds the width passed in exactly when a
table or an unbreakable word overflowed — the number a horizontal
scrollbar is sized from), `incomplete` (memory or a run limit stopped it
partway — what is there is real), and the image and control boxes as
lists so the face can place widgets and schedule fetches without walking.

## The three passes

Like libpage's build, three dependency-ordered passes over the tree, and
the reason is the same: each needs what the last one found.

### Pass 1 — style: tree → computed style

**The struct** (`flow_style_t`), one per element box. An ANONYMOUS box
gets the INHERITED properties of its parent and the INITIAL value of
everything else (CSS 2.1 §9.2.1.1's own sentence) — never a copy of the
parent's struct, which would hand an anonymous block its parent's
margins, border and background and apply each twice. The `Inherited`
column below is that rule's table. Every field is one CSS 2.1 property the
Rendering chapter or a presentational attribute can set, in the
property's own units after inheritance and `em` resolution — the
standard's "computed value", which is precisely what a cascade produces,
so a cascade slots in above this struct with no field renamed:

| Field | Values | Inherited |
|---|---|---|
| `display` | none, block, inline, list-item, table, table-caption, table-row-group, table-header-group, table-footer-group, table-row, table-cell, inline-block (the last for form controls and `img`, which are inline replaced) | no |
| `font` | a family list — names as written, ending in a generic (serif, sans, mono) — bold, italic, size px | yes |
| `color`, `background` | XRGB; background may be "none" | color yes, background no |
| `margin[4]`, `padding[4]`, `border[4]` | px; margins may be `auto` (horizontal only) | no |
| `width`, `height` | auto, px, or percent (kept as percent until layout knows the containing block) | no |
| `text_align` | left, right, center | yes |
| `vertical_align` | baseline, top, middle, bottom (cells and replaced boxes) | no |
| `white_space` | normal, pre, nowrap, pre-wrap | yes |
| `list_style` | disc, circle, square, decimal, lower-alpha, upper-alpha, lower-roman, upper-roman, none; position outside | yes |
| `text_decoration` | underline, line-through, none | no (but propagates through inline descendants, the standard's odd rule) |
| `visibility` | visible, hidden (takes space, paints nothing) | yes |
| `border_spacing`, `border_collapse`, `caption_side` | tables | yes |
| `float`, `clear` | recorded from `align=left/right` and `<br clear>` so the struct is complete — NOT ACTED ON in the first cut (booked below) | no |

**The first producer** is `style.c`: the Rendering chapter's rules, by
element — `body { margin: 8px }`, `p { margin: 1em 0 }`, `h1 { font-size:
2em; margin: 0.67em 0; font-weight: bold }` down to `h6`, `blockquote {
margin: 1em 40px }`, `ul`/`ol` `{ padding-left: 40px }` and their
markers, `pre`/`code`/`tt`/`kbd`/`samp` monospace, `b`/`strong` bold,
`i`/`em`/`cite`/`var`/`dfn` italic, `u`/`ins` underline, `s`/`strike`/`del`
line-through, `small`/`big` size, `sub`/`sup` size (their vertical shift
booked), `center { text-align: center }`, `table { border-spacing: 2px }`,
`td`/`th { padding: 1px }` and `th` bold centered, `hr` as a 2px inset
line with 0.5em margins, `address` italic, `dd { margin-left: 40px }`,
`listing`/`xmp`/`plaintext` as `pre`,
`nobr { white-space: nowrap }`, `wbr` (a break opportunity and nothing
else — the chapter spells it as a zero-width space, and pass 3 reads it
as the one element boundary that IS a break),
`fieldset` bordered with its legend,
`[hidden] { display: none }` and the list of elements displayed `none`
(`head`, `title`, `meta`, `link`, `style`, `script`, `template`, `area`,
`base`, `param`, `datalist`, `rp`, `input[type=hidden]`, a `dialog` not
`open`, a closed `details`' children past its first `summary`) — and then
the PRESENTATIONAL ATTRIBUTES the chapter maps to properties: `align` on
blocks, headings, `p`, `div`, `hr`, and cells (text-align, or the
horizontal margins for `hr` and `table`); `width`/`height` on `img`,
`table`, `td`, `th`, `col`, `hr`, `iframe`; `bgcolor` and `text`/`link`
on `body`, and `body`'s margin attributes — `marginwidth`/`leftmargin`/
`rightmargin` to the horizontal margins, `marginheight`/`topmargin`/
`bottommargin` to the vertical ones — which is how a 1990s page gets its
text flush to the edge; `bgcolor` on tables and cells; `color`, `size` (the 1..7
table: xx-small…xxx-large) and `face` on `font` (its comma list goes into
the family list AS WRITTEN, and the generic appended as its tail is
guessed by keyword — "mono"/"courier" → mono, "serif"/"times"/"georgia"
→ serif, else sans — so a resolver that matches names gets them and one
that does not still lands on the right generic); **`a[href]`: the link
colour and `text-decoration: underline`** — the Rendering chapter's
`:link` rule, the colour from the face's `link_ink` unless `body link=`
overrides it, and ONLY where libpage says the node is a link
(`os64_page_link_for`), so "is this a link" has one decider and "what
colour is it" has one field: a `<font color>` inside the `a` beats it by
ordinary cascade order, and the face paints what the struct says and
never consults the link list for ink; `border`, `cellpadding`,
`cellspacing` on `table`; `nowrap` on cells; `hspace`/`vspace` on `img`;
`type`/`start`/`value`/`reversed` on lists (the counter rules `wend`
already honours, moved here whole); `valign` on cells and rows; `align`
on `img` (top/middle/bottom → vertical-align; left/right → `float`,
recorded and not acted on). **Quirks mode has THREE states** (`doc->quirks`:
no-quirks, limited-quirks, quirks), and the producer honours all three
as the chapter's quirks section lists them. Full quirks: the body margin,
`p` and heading margins inside cells, table font inheritance (a table
does not inherit the body's font), the percent-height quirks. **Limited
quirks and full quirks BOTH keep the line-height calculation quirk**,
which is the visible one: a line box holding nothing but replaced boxes
ignores the strut, so images stacked in table cells touch — a page made
of sliced images (the old web's whole navigation idiom) shows gaps
between the slices without it, and limited-quirks mode exists precisely
to keep this quirk while dropping the rest. Hacker News in the corpus has
no doctype and runs in full quirks; a sliced-image table is a fixture.
The quirk switch lives in this one producer, so a cascade that arrives
inherits it.

Inheritance walks parent to child; `em` and percent font sizes resolve
against the parent's computed size; a relative size on the root resolves
against the viewport size the face passed. A `font` size of `+1` is the
legacy relative form and resolves against the basefont (3). The result is
absolute pixels everywhere except `width`/`height`/margins in percent,
which wait for pass 3.

### Pass 2 — boxes: tree + style → box tree

The walk `wend`'s renderer does, without the drawing: `display: none`
subtrees are skipped whole (their controls still belong to libpage's
model, which is what makes a hidden field submit); `template` contents
are the fragment branch and are skipped; comments and `head` are skipped;
`noscript` contents are shown (we run no script). Foreign elements (SVG,
MathML) generate no boxes of their own and their HTML descendants are
walked, `wend`'s rule. A `frame` becomes a replaced box the face draws as
the link libpage already lists, `wend`'s departure kept, because a
frameset page has no other content — and an `iframe` becomes the same
kind of box, drawn as a link to its `src` (libpage lists an iframe with a
non-empty `src` as a link for exactly this face). An iframe with an empty
`src` is the blank page: its box is laid out at its size and draws
nothing to follow.

Text nodes get **white-space processing** (CSS 2.1 §16.6.1) HERE, once,
into a collapsed copy the box owns: under `normal` and `nowrap`, runs of
spaces, tabs and newlines become one space, a space at a line's start or
end is removed at layout, leading and trailing collapsible space across
element boundaries obeys the standard's "segment break" rules; under `pre`
and `pre-wrap` bytes are kept and a newline ends a line; tabs under `pre`
land on eight-column stops the way the terminal and the textview place
them. libhtml kept the bytes verbatim so this pass could read them; the
copy is what the text fragments' byte ranges index.

**Anonymous boxes**, the two kinds CSS 2.1 requires. The first: a block
container whose children mix inline content and block boxes wraps each
inline run in an anonymous block (§9.2.1.1). A text node of nothing but
collapsible white space is not inline content for this purpose (the
newline between two `<p>`s makes no wrapper), which is why the question
is asked after pass 1 knows each node's `white-space`.

**A block INSIDE an inline is the old web's normal case, not an edge
case**, and §9.2.1.1's second paragraph is the rule for it: the inline
box is SPLIT around the block — the inline content before the block goes
into an anonymous block, the block sits between, and the content after
goes into another, with the inline box continuing (as a second, third…
piece) in each. `<font face=arial size=2>` wrapping a whole body of
`<p>`s and `<table>`s is how pages were written from 1996 on, and libhtml
keeps them nested exactly as written, because a `<p>` does not close a
`<font>`; `<a href><div>…</div></a>` is the same shape today. A container
whose only child is that `<font>` therefore MIXES, even though every one
of its children is inline — the blocks are a level down. So pass 1
computes, bottom-up, one bit per element: **contains an in-flow block**
(itself a block-level box, or an inline whose subtree holds one, not
looking past a block, which answers for its own subtree). The mixing
decision reads that bit on each child, and an inline child with the bit
set is split rather than wrapped whole. The pieces of a split inline are
one inline box for painting purposes — its per-line fragment ranges
simply continue into the next anonymous block — and its link or control
index is on every piece.

Whether a container mixes is decided BEFORE its children are built, from
those bits (each child is read once by its parent, and the bit was
computed once by pass 1, so the cost is linear), so the wrapper exists
from the first inline child rather than being made retroactively when a
block sibling turns up. That is what lets a build that stops early still
be a prefix of the whole build (*Proof*).

The second kind: table structure that is missing gets anonymous table
objects (§17.2.1) — a cell whose parent is not a row, a row outside a row
group. libhtml's tree builder already repairs most table markup, so the
second kind is rare here and is implemented because the rule is the
standard's, not because the corpus has a case (it will get one).

`li` under `ul`/`ol`/`menu`/`dir` gets its marker box; a `list-item` with
no list parent still gets one (the standard's rule; the marker is a
bullet). Counters walk `ol start`, `li value` and `reversed` exactly as
`wend` counts them today — that code moves, it is not rewritten.

`img`, `input` (except hidden), `button`, `select`, `textarea`, `iframe`,
`embed`/`object` (drawn as their fallback content, since nothing plays
them), `hr` and `frame` are **replaced boxes**; the walk records them in
the two lists the face reads.

### Pass 3 — layout: box tree + width → rectangles

**Block formatting** (CSS 2.1 §9.4.1, §10.3.3, §10.6.3): children stack
vertically; a block's content width is the containing block's width minus
its own margins, borders and padding, with `auto` horizontal margins
sharing the remainder (which is how `<table align=center>` and
`<hr align=right>` centre and right-align); `width` in px or percent
overrides `auto`; height is the content's unless set, and a set height
that is too small still shows the content (no overflow clipping in the
first cut — `overflow` is the cascade's and booked). **Vertical margins
collapse** (§8.3.1) between siblings, between a parent and its first or
last child when nothing separates them, and through an empty block — the
whole rule, with one stated simplification: NEGATIVE margins cannot arise
from any presentational attribute, so the first cut asserts they are
zero and the collapsing arithmetic is written for non-negative values.
The day the cascade brings a negative margin, the assertion names this
paragraph and the arithmetic grows the `max(positive) + min(negative)`
rule.

**Inline formatting** (§9.4.2, §10.8): a block container's inline content
is broken into line boxes as wide as the content width. Each line has the
**strut** — the block's own font's ascent, descent and line height, the
invisible rule TeX invented (Knuth's `\strut`) and CSS borrowed by name,
which is what keeps an empty line and a line of small text the same
height as their neighbours. Fragments align by BASELINE by default; a
replaced box or a fragment with `vertical-align` top/middle/bottom aligns
as it says; the line's height is the span from the highest top to the
lowest bottom of what is on it, strut included. `text-align` places the
line's fragments; `br` ends a line; under `nowrap` nothing breaks; under
`pre` only a newline does.

**Line breaking** is done over the INLINE FORMATTING CONTEXT'S ITEM
SEQUENCE, never per node: a block container's inline content is
flattened into items — text pieces from every text node in it, replaced
boxes, `br`s, the opening and closing edges of inline boxes — and a
WORD is whatever lies between two break opportunities in that sequence,
whichever nodes it crosses. `foo<b>bar</b>` is one word in two nodes and
is never broken between them; `wend`'s renderer holds a word across
elements for exactly this reason and yonder must not wrap worse than
`wend`. The element boundary is not a break opportunity; a space is, and
so is `wbr`, whose whole meaning is one (the Western profile's rule; soft
hyphens and CJK are booked). Measurement
uses the text engine and never a width table: each text piece is laid
out ONCE as a measuring run with its resolved fonts — a piece longer
than the engine's run cap, 1 MiB, is measured in windows cut at spaces —
and the run's carets give the pen position at every byte, so a word that
spans pieces is the sum of its parts' caret distances (kerning across
the boundary is not attempted, and neither is it by the engines that
matter). A word wider than the line is broken at the last caret that
fits, `wend`'s rule. Each line's fragment of each piece is then laid out
as its own run, so the face can paint it with one `os64_text_draw` and
hit-test it with `os64_text_hit`. That is two layouts per text piece and
the memory of one run per line-fragment; it works with the engine as it
stands, and a range-draw on the measuring run (one run per piece,
painted by byte range) is the F2 ask that halves both, booked with its
trigger below. Min-content width (tables, below) is the widest WORD of
the sequence under the same definition — across nodes.

**`pre` and `pre-wrap` are measured PER LINE, because the engine refuses
a literal LF** (`os64_text_layout` answers BAD_ARGUMENT; one run is one
logical line by contract). Pass 2's white-space processing already
splits preformatted text at newlines, so a preformatted piece is a
sequence of line-pieces and each is its own measuring run. **Tabs** are
defined in pixels, since the engine takes `tab_origin`/`tab_interval` in
26.6 units: the interval is eight times the advance of the SPACE glyph
of the block container's own font (the strut's face — one font per
paragraph decides the stops, or a `<b>` mid-line would move them), and
the origin is the line's content-edge start expressed relative to the
fragment's origin (`tab_origin = -(fragment_x − line_x)`), so a fragment
that begins mid-line after an inline boundary still lands its tabs on
the paragraph's stops. Under `normal` and `nowrap` tabs collapsed to
spaces in pass 2, so only the preformatted values ever set them.

**Rounding: 26.6 within a line, integer pixels at the box, and the same
rule the painter uses.** The engine measures in signed 26.6 fixed point
and `os64_text_draw` places a glyph at `floor((pos + 32) / 64)` — round
to nearest, ties up — so layout keeps the PEN in 26.6 across a whole
line (fragment after fragment, so fractional advances accumulate rather
than being lost at every boundary) and rounds ONCE, by the painter's own
formula, when a fragment's origin becomes a box coordinate. A fragment's
pixel width is `round(end) − round(start)` and never `round(end −
start)`, so adjacent fragments on a line abut with neither a gap nor an
overlap. Vertical extents — ascent, descent, a line's height — round
OUTWARD (ceiling), so no glyph is ever clipped by its own line box;
baselines, being positions, round to nearest like an origin. Everything
that reaches a `flow_box_t` is an integer pixel; nothing in 26.6 leaves
the library.

**Replaced boxes** take the oracle's size; when it is unknown, an image
with `width` and `height` attributes gets a box of that size, an image
with neither is laid out AS ITS ALT TEXT inline (the standard's rendering
of an unavailable image with alt, and the reason a page of missing images
still reads), and an image with empty alt and no size takes no space. A
form control with an unknown size (the face has not measured it) gets a
placeholder the size of one row of its font, which the face will correct
on the next layout — the oracle is the truth and the engine never caches
it. `hspace`/`vspace` are margins; `hr` is a replaced block of the
content width and a 2 px rule.

**Tables** (§17.5, automatic layout §17.5.2.2): for every column the
MIN-CONTENT width (the widest thing that cannot break: the longest word,
the widest replaced box, a `nowrap` cell whole) and the MAX-CONTENT width
(the content on one line), each cell's contribution found by laying its
content out as a block container at width 0 and at unbounded width — the
three layouts per cell every table engine pays, MEMOIZED per box so a
nested table costs its cells three times and not three to the power of
its depth (the classic exponential, and the *Bounds* section's first
rule). A spanning cell's minimum and maximum are spread over its columns
in proportion to what the columns already have, the rule Netscape's
engine used and every engine since. Then: a table with a `width` takes
it; without one, it takes its max-content width when that fits the
containing block, else the containing block's width, never less than its
min-content width (it overflows rather than squashing a word). Column
widths: `col`/`td` `width` attributes as minimums where they fit, then
the remainder distributed from min toward max in proportion to each
column's max-minus-min. Rows are as tall as their tallest cell; `rowspan`
spreads a cell's height over its rows; `valign` places the cell's content
within its row; captions above (or below, `caption-side`) at the table's
width; `border-spacing` between cells, `cellpadding` inside them; the
`border` attribute draws the old web's inset frame. `border-collapse` is
recorded and rendered as separate borders in the first cut (booked).

**Lists**: the marker box sits in the left padding the Rendering chapter
gives lists, right-aligned to the content edge with a space, on the
first line's baseline; `list-style: none` (a `menu` in a nav) draws none.

**What is not laid out in the first cut, by name, and how the tree
degrades honestly:** `float` and `clear` are recorded and ignored, so an
`<img align=left>` sits inline at its baseline and the text runs after it
rather than beside it — the page still reads, in order; `position`,
`z-index`, `overflow`, `display: inline-block` from a cascade,
`inline-table` — none can arise from a presentational attribute, so the
first producer never asks for them. Each is a row in the booked table.

## The dump

The harness's artefact and the reviewer's view of a page — one line per
box, indented by depth, coordinates in document pixels, text quoted, so a
layout change is a diff to a page and not "it looks different":

```
block body 8 16 784 1302
  block p 8 16 784 57 margin 16/16
    line 8 16 784 19
      text "The Floodgap Gopher" 8 16 152 19 sans 16 baseline 14
      text "is" 164 16 14 19 sans 16 baseline 14
      inline a 8 16 ... link 3
  table 8 89 600 240 cols 120 360 120 spacing 2
    row 10 91 596 24
      cell td 10 91 120 24 valign middle
        line 11 92 118 19
          text "Name" 11 92 40 19 sans 16 bold baseline 14
  img 8 337 200 150 known
  control input 216 337 180 22 control 4
  marker "3." 24 512 16 19
```

(The body's 8 px margin and the paragraph's 16 px collapse into one, so
both boxes start at 16: the dump is where a margin that did not collapse
shows up as a number.) Rendered at 800 px and at 400 px (wrapping is where a layout engine goes
wrong, and a page that fits proves nothing), under the fake backend, so
the numbers are the algorithm's and not FreeType's hinting. The fake
backend's metrics are fixed and documented in `tools/fonts/`; a case that
needs a kerned pair or a fallback face uses the ones it defines.

## The one door

```c
typedef struct flow_tree flow_tree_t;

typedef struct {
    // Fonts: the face's resolver. On the host, the fake backend.
    void *ctx;
    // `families` is the struct's list: names as the page wrote them,
    // then a generic. A resolver that matches no name takes the generic.
    os64_font_status_t (*fonts)(void *ctx, const flow_family_list_t *families,
                                bool bold, bool italic, uint32_t px,
                                os64_text_font_t *const **list, size_t *count,
                                os64_font_face_info_t *primary);
    // Replaced sizes: the face's oracle. False = unknown.
    bool (*replaced_size)(void *ctx, const os64_html_node_t *node,
                          int32_t *w, int32_t *h);
    os64_text_context_t *text;      // the engine the runs are laid out on
    uint32_t viewport_font_px;      // 16 unless the face says otherwise
    uint32_t ink, link_ink, paper;  // XRGB; the dump never prints them
} flow_env_t;

// Build the box tree and lay it out at `width`. NULL on no memory only;
// otherwise a tree whose `incomplete` says whether it is whole. Every
// call is a whole rebuild (ruling 2): the face calls it on load, on
// resize, when an image's size arrives, when a control is edited in a
// way that changes its size, and never more than once per frame.
flow_tree_t *flow_layout(const os64_html_document_t *doc, const os64_page_t *model,
                         int32_t width, const flow_env_t *env);
void flow_free(flow_tree_t *);

// The tree, for painting: the root box, and a walk pruned by a rectangle
// — against each box's OVERFLOW rect, never its own, so a child that
// hangs out of a too-short parent is still visited — so painting the
// viewport costs the visible boxes and the depth above them. Boxes are
// in painting order (backgrounds before content, CSS 2.1 Appendix E
// without z-index or positioning, which the first cut has none of).
const flow_box_t *flow_root(const flow_tree_t *);
void flow_visit(const flow_tree_t *, os64_gui_rect_t viewport,
                void (*visit)(void *ctx, const flow_box_t *), void *ctx);

// Hit-testing: the deepest box whose OWN rect contains (x, y), searched
// through overflow rects, or NULL. The face then asks libpage what the
// box's node MEANS (link_for / control_for) and asks the fragment's run
// where in the text the pointer is (os64_text_hit).
const flow_box_t *flow_hit(const flow_tree_t *, int32_t x, int32_t y);

// Geometry for a node — the box a fragment link scrolls to, the rectangle
// a control widget is placed in. NULL for a node with no box (hidden,
// display none, or the tree is incomplete past it).
const flow_box_t *flow_box_for(const flow_tree_t *, const os64_html_node_t *);

// The two lists the face schedules from: image boxes (with their nodes, so
// the face fetches libpage's resolved src) and control boxes (with their
// libpage control index, so the face places a widget per control).
int32_t flow_nimages(const flow_tree_t *);   const flow_box_t *flow_image(const flow_tree_t *, int32_t);
int32_t flow_ncontrols(const flow_tree_t *); const flow_box_t *flow_control(const flow_tree_t *, int32_t);

int32_t flow_height(const flow_tree_t *);
int32_t flow_width(const flow_tree_t *);     // the root's overflow width — at least the width
                                             // the tree was laid out at, more when something overflowed
bool    flow_incomplete(const flow_tree_t *);

// The dump, for the harness and for /proc-style forensics in the guest.
int64_t flow_dump(const flow_tree_t *, char *out, size_t cap);
```

`flow_box_t` is public and read-only: kind, node, rect, style pointer, the
run handle and byte range for a text fragment, the link or control index
for a box that is one (settled at build from libpage, so the face never
keeps a counter — LIBPAGE.md's "node names its item" rule, applied
downward), first child, next sibling, parent.

## Bounds

The page is an attacker's lever and libhtml's size limit is the budget,
LIBPAGE.md's rule restated for geometry:

- **Linear in the page, tables included — and SPANS ARE CLAMPED, because
  a span is the one thing that is not linear in the cells.** Intrinsic
  widths are memoized per box, so a nested table costs its cells three
  times and not three to the power of its depth. Spreading a spanning
  cell over its columns costs `span` per cell, which is quadratic on a
  page whose every cell spans every column, so the standard's own clamps
  are applied at pass 2: `colspan` above 1000 is 1000, `rowspan` above
  65534 is 65534, `rowspan=0` means "to the end of the row group" and is
  resolved to that number when the group is complete. With the clamp the
  spreading work is bounded by cells × 1000 and stated as such. The host
  suite lays out a table nested forty deep AND a table whose cells all
  span, and asserts the layout count against the linear bound in both.
  The measuring run per text piece is laid out once per layout;
  fragments once per line.
- **Memory is the text engine's budget plus the boxes, and REBUILDING
  DOUBLES THE PEAK.** Runs are the big cost (a glyph placement is tens of
  bytes) and they live in the text context the face hands in, which has
  one `memory_cap` and is caller-serialised. A rebuild builds the NEW
  tree while the OLD one is still retained — the face must keep the old
  tree until the new one exists, or a rebuild that fails leaves it with
  nothing to paint — so the peak is two layouts of the page, and a page
  using more than half the budget would go `incomplete` on every resize.
  Three rules follow, and yonder owes all three: **ONE PAGE CONTEXT,
  separate from the chrome's and shared by every page view the program
  has** — a page must not starve the address bar, and libui already lets
  a window borrow a context (`os64_ui_font_borrow_context`), so the
  chrome keeps its own; but NOT a context per page, because a run may
  only use fonts opened on its own context (FONT_CONTRACTS.md: a
  foreign-context font is BAD_ARGUMENT) and every context owns its own
  copy of each font file it opens, so a context per page would be a copy
  of the twelve web faces per page, charged to that page. The family
  cache of packet 04 therefore lives ON the page context, one per
  context, and its fonts are charged to the same budget as the runs;
  **the page context's cap is sized for two trees** of the largest page
  the face wants whole PLUS the faces; and **the old tree is freed AFTER
  the new one returns, whatever it returns**. A layout that hits the cap
  stops, keeps what it has, and reports `incomplete` — the same shape as
  libhtml's refusal and `wend`'s half-page: what is there is real. The
  face shows it and says so. A page too big to lay out is not a crash
  and not a blank. And because the page context is touched by nothing
  the chrome does, layout CAN run on a worker thread while the face
  draws its chrome on its own context — the serialisation rule is per
  context, one page laying out at a time, and that is the reason for the
  split beyond memory. Tabs, when they come, share the page context and
  its budget, or the design here is revisited with that consumer in hand.
- **Depth.** Node depth is libhtml's `max_depth`. BOX depth is larger —
  a nested table adds a table box, a row-group, a row, a cell and
  possibly anonymous boxes per level — but by at most a constant per
  node, so it is bounded by a multiple of `max_depth`, and the walk is
  either iterative or written against that bound. The intrinsic-sizing
  RECURSION is bounded by table NESTING, which is at most a third of the
  node depth (a nested table is at least `table > tr > td` deeper), and
  is written as a recursion only because that bound is stated here. A
  page as deep as libhtml allows lays out without a stack the width of
  the page.
- **Nothing blocks and nothing is cached across calls.** Every layout is
  from scratch; the face owns the pacing.
- **The run cap** (1 MiB per run) is honoured by windowing a long text
  node, never by refusing the page.

## What the consumers owe

- **yonder** paints, scrolls, hit-tests, fetches images and reports their
  sizes, measures and places control widgets, and calls `flow_layout`
  again at most once per frame when any of those changes. It draws a
  fragment with `os64_text_draw` at the box's baseline, an inline box's
  underline and background from its per-line ranges, a marker's text,
  and a replaced box as its widget or its image or its alt text. It
  paints every fragment in the colour ITS STYLE says — a link's colour is
  in the struct, put there by the style producer where libpage said the
  node is a link — and never consults the link list for ink; visited is
  booked with the history. It keeps the page's runs and the web faces on
  one page context apart from its chrome's (*Bounds*), keeps the old
  tree until the new one returns, and sizes that context for two trees
  and the faces.
- **libpage** keeps the IMAGES list (`os64_page_nimages`,
  `os64_page_image`, `os64_page_image_for`): every `img` and `input
  type=image` whose `src` is not empty, the `src` resolved through family
  A the way links are, the `alt` as written. It was owed by this design
  and paid in slice 0 — with one change from the ask as first written:
  `width` and `height` are NOT read there. They are geometry, pass 1 reads
  them as presentational attributes, and two readers of one attribute is
  the thing LIBPAGE.md exists to prevent. `iframe` joined the link list
  beside `frame` in the same slice (a non-empty `src` only — an empty one
  is the blank page), reversing libpage's earlier "no face offers one":
  this one does, and libpage's host harness carries the case.
- **The text engine (F2)** is asked for nothing in the first cut. The
  range-draw is booked below with a measured trigger.
- **packet 04** supplies the resolver the face hands in.

## Proof before integration

**The corpus is the durable artefact, and it is `wend`'s corpus grown.**
`tools/html_corpus/` already holds six real pages with their trees;
they get `.boxes` dumps at 800 and 400 beside their `.lines`. Added to
them, hand-written fixtures per family, each with a HAND-COMPUTED expected
dump (F2's rule: fixed expected geometry, never a self-consistency test):

- **Block**: nested margins collapsing every way §8.3.1 lists, padding and
  border stopping a collapse, an empty block collapsing through, `auto`
  margins centring, percent widths, a set height smaller than content,
  and a block inside an inline — `<font>` round `<p>`s and a `<table>`,
  `<a>` round a `<div>`, the split inline's pieces and its underline
  ranges continuing across them.
- **Inline**: mixed sizes on one line (the strut, the baseline), `br`,
  `nowrap`, a word wider than the line, trailing and leading collapsible
  space at every element boundary the standard names, `pre` with tabs,
  `pre` inside a cell, an inline box spanning three lines, underline
  ranges, alt text inline.
- **Tables**: the min/max algorithm on a table wider and narrower than
  its container, `colspan` and `rowspan` spreading, `width` on `col` and
  `td`, `nowrap`, captions both sides, nested tables (the memoization
  case, with a layout-count assertion), a layout table from the corpus
  (textfiles, Hacker News — the pages that ARE tables).
- **Lists**: every marker style, `start`/`value`/`reversed`, nesting,
  a `menu` with `list-style: none`.
- **Replaced**: known and unknown image sizes, attrs with and without
  alt, controls with and without an oracle answer, `hr` under every
  `align`.
- **Quirks**: the same page under all THREE modes — no-quirks,
  limited-quirks, quirks — with the margins full quirks changes and the
  sliced-image table that limited-quirks keeps gapless and no-quirks does
  not.
- **The pathological page** of LIBPAGE.md's suite laid out (lists five
  deep round a `pre`, form-in-table-in-form, an unclosed inline tag over
  a table).
- **The allocation-failure sweep**: an injected failure at every
  allocation of one corpus page, asserting no crash, no leak, and either
  NULL or a tree whose `incomplete` is set — the wend harness's shape, at
  the depth that harness already runs. **The partial tree's relation to the whole one is stated here
  so the assertion is not invented at the keyboard**: a dump prints a
  box's height before its children, so a truncated tree differs from the
  whole at its first line, and a table laid out from partial content has
  different column widths. What holds, and what is asserted: (a) the
  pre-order sequence of `(kind, node, byte range)` in the partial tree is
  a PREFIX of the whole tree's (which holds only because anonymous
  wrappers are decided before their children, pass 2); (b) every box in
  the partial tree whose subtree is COMPLETE and that has no unfinished
  ancestor TABLE and no unfinished ancestor LINE has the same rectangles
  as its twin in the whole tree — a line's height, baseline and
  alignment depend on what lands on it later, so a fragment that finished
  early can still move when its line completes; (c) every unfinished box
  is an ancestor of the last box. Heights and table widths of unfinished
  ancestors, and the geometry under an unfinished line, are not compared.
- **Fuzz**: every corpus page truncated at every 64 bytes and tag-soup
  mutations of them, under ASan/UBSan, asserting the invariants of a box
  tree (every child's overflow rect inside its parent's OVERFLOW rect —
  not its own rect, which content may legitimately exceed; every rect
  non-negative; every fragment's byte range within its piece's text;
  every memoized width COMPUTED at most once per box; every span within
  its clamp).
- **In the guest**: yonder's page view on the corpus pages screendumped
  and read; a table page and a form page by hand. Runtime claims want a
  guest probe, as always.

## Booked before the first line

| Debt | Why it waits | Trigger |
|---|---|---|
| Floats and `clear` (`align=left/right` on `img`/`table`, `<br clear>`) | the float rules (§9.5) are a second placement pass with their own line-box shortening; the struct records them so the cascade and the first cut agree on the field | the first page whose layout is unreadable without a float — image-beside-text pages of the old web will vote early |
| Negative margins | no presentational attribute produces one; the collapsing arithmetic asserts non-negative and names this row | the cascade's first `margin: -` |
| `position`, `z-index`, `overflow`, `inline-block` from a cascade, `inline-table`, `border-collapse: collapse` | none can arise from the first producer | the cascade |
| A range-draw on a measuring run (F2 ask) | halves layout work and run memory; works without it | a page whose layout time is visible, measured, or a page that hits the memory cap through runs |
| Incremental relayout | ruling 2 says rebuild; the face paces it | the engine, or a page whose rebuild is visibly slow |
| Selection and copy | needs the fragment byte ranges (kept) and a face gesture | yonder's second slice |
| `:visited` colour | the history lives in the navigator | packet 06's history and a face rule |
| `sub`/`sup` vertical shift | one `vertical-align` value each, cheap, and the first cut's fixtures do not cover it | the first page that reads wrong without it (footnotes) |
| Soft hyphen breaks, CJK and script-aware breaking, bidi/RTL layout | the text profile is Western v1; bidi classes exist in libos64 for `dirname`, the layout half is a real slice | a page in one of those scripts worth reading |
| `marquee` | the Rendering chapter has it; it is a timer in a face | a page whose meaning scrolls, which is none |
| `iframe` content | a document inside a document is a second fetch and a second tree; drawn as a link to its `src`, `wend`'s frame rule | a page whose meaning is in the frame |
| `object`/`embed`/`video`/`audio`/`canvas` | nothing plays or scripts them; fallback content is shown | media, after JS |
| SVG drawing | a renderer of its own | the first inline SVG worth seeing |

## Review tier

Not app code, but its failure class is a crash or an exponential on a
hostile page, not a credential in a query string: the harness — ASan, the
allocation sweep, the fuzz, the linear-cost assertion — is the gauntlet,
and the review is here (Fable), whole-file each round the way the
security-shaped files got one. Codex rounds are Chris's call, as always.
Chris tests as he is able, AFTER the commit (below).

## Who builds it

One designer, one spec, one library: it cannot be split across people the
way the packets can, and its first consumer (the face) is written by
whoever writes the second slice of yonder against this door. The
constitution is written so that the builder is not necessarily the
author: every rule names its section of CSS 2.1, every fixture its
expected geometry, and the dump is the contract.

Opus builds it, as a STACK of branches the way `vncd` was built: each
slice is finished, tested by its builder (the host harness under ASan
with hand-computed dumps; the guest probe once there is geometry to
probe), committed and pushed, and the next slice branches from it.
Chris tests as he is able and requests the review when he is happy;
a fix lands on the slice it belongs to and the stack above is rebased.

| Slice | What | Proof |
|---|---|---|
| 0 | This document, and what libpage owes: the IMAGES list and `iframe` in the link list | libpage's own host harness and its allocation sweep |
| 1 | The library's skeleton and pass 1: `flow_style_t`, the Rendering chapter, presentational attributes, the three quirks modes, the contains-a-block bit | a STYLE dump per element, hand-computed |
| 2 | Pass 2: the box tree, both kinds of anonymous box, white-space processing, markers and the counters moved from `wend`, the replaced lists | a box dump without coordinates |
| 3 | Pass 3 without tables: block formatting and margin collapsing, the strut, line breaking over the item sequence, `pre` and tabs, rounding, replaced boxes and alt text, overflow rects, `flow_width` | hand-computed `.boxes`; `/tests/flowdump` in the guest |
| 4 | Tables: intrinsic widths memoized, span clamps, rowspan, captions, the line-height quirk | the nested and all-spanning cost assertions |
| 5 | The door and the gauntlet: `flow_visit`/`flow_hit`/`flow_box_for`, the allocation-failure sweep, fuzz, the corpus `.boxes` at 800 and 400 | the whole *Proof* list |

The face (yonder's page view) is the next arc and is built against a
door that has already passed its gauntlet.

## Review record

**2026-09-25, Opus's read of the first draft: twelve findings, all
taken.** The design test contradicted its own struct and the resolver's
signature baked the gap in (the family list, above, settled before packet
04 freezes); link colour was decided in the face instead of the style
(now `a[href]` in the producer, with `body link=` and `<font color>` in
cascade order); pruning and hit-testing were unsound wherever the doc
itself allowed overflow (the overflow rect); the page had a height and
no width (`flow_width`); a whole rebuild doubles the peak on one shared,
serialised text context (first answered as a context per page, which the
second read below corrected to one page context apart from the chrome's;
cap for two trees, old tree freed last — and the worker-thread door that
opens); anonymous boxes
"inheriting the parent's style" read as a struct copy (§9.2.1.1's rule
written down); line breaking was described per node when words cross
nodes (the item sequence); `pre` collided with the engine's LF contract
and tabs had no pixel definition (per-line measuring runs; the strut
font's space × 8, origin line-relative); quirks has three states and the
draft omitted the line-height quirk that limited-quirks mode exists to
keep (all three, the sliced-image fixture); the iframe rule contradicted
libpage's stated reason (a reversal, said out loud); spans are not linear
in the cells and box depth is not node depth (the standard's clamps; the
depth bounds stated); and the allocation sweep's "prefix" invariant could
not hold against a dump that prints heights first (the three-part
relation). Every one of them was a paragraph a builder would have hit at
the keyboard in the code session; that is what a second read is for.

**2026-09-25, Opus's second read: three more, all taken.** My own
memory fix had contradicted packet 04 and the font contract — a run may
only use fonts opened on its own context, and a context copies every
font it opens, so "a context per page" would have refused every run or
copied the web faces per page; it is now ONE page context apart from the
chrome's, with the family cache on it. The allocation-sweep relation
still broke where anonymous wrappers were made retroactively (now
decided by a scan of the children before they are built) and where a
finished fragment sits on an unfinished line (added to the rule). And the
leftovers: the example dump's paragraph now sits at 16 where its margin
collapsed with the body's, the corpus has six pages, the depth claim
names libhtml's limit instead of a number that exceeded it, the memoized
width is COMPUTED at most once, the quirks fixture covers all three
modes, `flow_width`'s comment finishes its sentence, `nobr`, `wbr` and
`body`'s margin attributes are in the producer, and the 26.6-to-pixel
rounding policy is written down beside the tabs.

**2026-09-25, Opus's third read, as the new owner: one more.** A block
inside an inline (§9.2.1.1's second paragraph) was not handled: the
mixing scan read only the children's `display`, so a body whose only
child is a `<font>` wrapping every `<p>` — the 1998 web's standard
layout — scanned as all inline and would have laid out as one enormous
line. Pass 1 now computes a contains-an-in-flow-block bit bottom-up, the
scan reads it, and a split inline's pieces are one inline box for
painting. The example dump's marker moved with everything else.

## Lineage, for the commit message and the person who likes it

Mosaic laid out a page in one pass with no reflow, which is why it had
no tables: a table needs to know how wide its widest cell will be before
it draws its first, and that is a second pass. Netscape 1.1 (1995) paid
for the second pass and shipped tables, and every layout engine since has
been an elaboration of "measure, then place". CSS1 (Lie and Bos, 1996)
named the box model; CSS2 (1998) wrote down the visual formatting model —
anonymous boxes, line boxes, margin collapsing, the automatic table
algorithm — mostly by describing what Netscape and IE already did; CSS
2.1 (2011) is the same text with the parts nobody implemented removed,
which is why it is the one to build to. The strut is Knuth's, from TeX
(1978): an invisible rule of a line's full height, so that a line with
nothing tall on it is as tall as its neighbours. The Rendering chapter of
the WHATWG standard is the browsers' shared built-in stylesheet written
down at last (2011 onward), and it is what a browser with no author
stylesheet shows — which is to say, it is what the web looked like from
1993 to 1996, and it is where yonder starts.
