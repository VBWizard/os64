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
  nothing here changes), and on the host the harness's own backend
  (`tools/test_libflow_fonts.c`), which is what makes the dumps
  deterministic across machines and FreeType versions.
- **A replaced-size oracle**: a callback answering `node → (w, h, known)`
  for images and form controls, because an image's intrinsic size arrives
  from a fetch the engine never sees, and a control's natural size is the
  face's theme and fonts (a libui textfield's row height is libui's to
  say). Unknown is an honest answer; the engine has a rule for it.
- **The viewport's font size** (16 px unless the face says otherwise), the
  generic family a page that names none is drawn in, and the three colours
  a face owns — the page's default ink, the link colour, the paper — so the
  dumps carry no theme.

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
- **Replaced boxes**: an image, a form control, a frame's or an iframe's
  placeholder, a meter or a progress bar.
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
| `display` | inline, block, list-item, inline-block (form controls, `meter`, `progress`, `marquee`), table, table-caption, table-row-group, table-header-group, table-footer-group, table-row, table-cell, table-column-group, table-column, contents (`slot`, and an SVG or MathML element, whose HTML descendants still render), none | no |
| `family` | a list of names as the page wrote them (slices of the attribute, never copied), ending in a generic: serif, sans, mono | yes |
| `font_weight`, `font_style`, `font_size` | 100..900 (`bolder` by CSS Fonts' table); normal or italic; 26.6 px | yes |
| `color`, `background` | XRGB; background may be absent (transparent) | color yes, background no |
| `margin[4]`, `padding[4]` | px, percent, or (margins) `auto` | no |
| `border_width[4]`, `border_style[4]`, `border_color[4]` | 26.6 px, computed to 0 where the style is none or hidden; none, hidden, solid, inset, outset, groove; XRGB with `currentColor` resolved | no |
| `width`, `height` | auto, px, or percent (kept as percent until layout knows the containing block) | no |
| `text_align` | left, right, center, justify, and the HTML alignments html-left, html-right, html-center, html-justify: the text aligned AND the element's block descendants aligned with it, which is what `<center>` and `<div align>` do and `text-align` alone does not | yes |
| `vertical_align` | baseline, sub, super, top, text-top, middle, bottom, and html-middle (`align=middle` on a picture: its middle on the baseline, not CSS's baseline plus half an x-height) | no |
| `white_space` | normal, pre, nowrap, pre-wrap | yes |
| `list_style_type`, `list_style_position` | disc, circle, square, decimal, lower/upper-alpha, lower/upper-roman, disclosure-closed/open, none; outside or inside | yes |
| `text_decoration` | underline, line-through — this element's own; an ancestor's reaching its inline descendants is derived at layout, in the ancestor's colour | no |
| `visibility` | visible, hidden, collapse | yes |
| `border_spacing[2]`, `border_collapse`, `caption_side` | tables | yes |
| `float_side`, `clear` | recorded from `align=left/right` and `<br clear>` so the struct is complete — NOT ACTED ON in the first cut (booked below) | no |

Lengths are 26.6 fixed point — the text engine's unit — so a margin of
`0.67em` on a 32px heading is 1372/64 px and nothing rounds until a box
coordinate is written (*Rounding*, below).

**The first producer** is `style.c`: the Rendering chapter's sheet, section
by section, each rule naming its section so the file can be held against
the standard line by line — the hidden elements, `body`'s 8px, `p`'s 1em,
the headings (size in the parent's em, margins in their own), `blockquote`
and `figure`, the monospace and `white-space: pre` elements, the phrasing
elements (italic, `bolder`, `larger`/`smaller` by CSS Fonts' 6/5 ratio,
`sub`/`sup`, `mark`, the decorations, `nobr`), the lists (with the nesting
rules that make a nested `ul` circle and a third level square and take
nested lists' block margins away), the tables (`border-spacing: 2px`,
cells' 1px padding, `th` bold and centred when its row's alignment was
never set, rows and cells taking `vertical-align` from their group), `hr`
(gray, a 1px inset border all round, `0.5em auto` margins), `fieldset`,
`iframe`'s 2px inset border, form controls as inline-blocks, `details`
(the first `summary` is the disclosure; the rest of a closed one is not
drawn), an open `dialog` laid out where it stands (positioning is booked),
and a `form` the parser left inside table structure displayed `none`.
`wbr` is a break opportunity and nothing else; pass 3 reads it as the one
element boundary that IS a break. `noscript` is SHOWN — the chapter hides
it only when scripting is on.

Then the PRESENTATIONAL ATTRIBUTES, as the chapter maps them: `body`'s
margins (`marginheight`/`topmargin` vertical, `marginwidth`/`leftmargin`
horizontal, the FIRST one present deciding, 8px when it will not parse),
`bgcolor`, `text` and `link`; `pre wrap`; `align` on `div` and on the
table parts (the HTML alignments), on `p` and the headings (plain), on
`table` (`center` is auto margins, `left`/`right` a float), on `caption`
(`bottom`), on `hr` (its margins) and on pictures (`float` or
`vertical-align`); `valign`; `bgcolor` on the table parts; `table`'s
`width` (nonzero), `height`, `border` (outset, and 1px inset on its OWN
cells — a parse error counts as 1, a zero as nothing), `bordercolor`,
`cellspacing`, `cellpadding` (onto its own cells only), `rules` and
`frame`; `nowrap`, `width` and `height` on cells; `col width`; `hr`'s
`width`, `size`, `noshade` and `color`; `hspace`, `vspace`, `border`,
`width` and `height` on pictures; `iframe frameborder`; `br clear`;
`ol`/`ul`/`li` `type` (case-SENSITIVE for the letters, where `a` and `A`
are two lists; case-insensitive for the names); and `font`: `color`,
`size` by the chapter's legacy-size rules (1..7 → x-small..xxx-large,
`+n`/`-n` from 3, clamped), and `face`, whose comma list goes into the
family AS WRITTEN with its generic tail — the first generic keyword the
page wrote, else guessed from the first name ("mono"/"courier" → mono,
"sans" → sans, "serif"/"times"/"georgia" → serif, else sans) — so a
resolver that matches names gets them and one that does not still lands
on the right generic. A `face` of nothing declares nothing. Values are
read by HTML's own microsyntaxes (`attrs.c`): dimensions, integers, and
the legacy colour algorithm that has made `bgcolor=chucknorris` red in
every browser since Netscape. The keyword sizes are CSS Fonts' scaling
factors on the face's `medium`.

**`a[href]` — the link colour and underline** — is the chapter's `:link`
rule: the face's `link_ink`, or `body link=` (a hint on the same selector,
so it beats the sheet), and ONLY where libpage says the node is a link
(`os64_page_link_for`). "Is this a link" has one decider and "what colour
is it" has one field: a `<font color>` inside the `a` beats it by ordinary
cascade order, and the face paints what the struct says and never consults
the link list for ink.

**Quirks mode has THREE states** (`doc->quirks`: no-quirks,
limited-quirks, quirks), and two standards say what they change. What is
STYLE, pass 1 applies in full quirks: the chapter's `form` bottom margin,
its `table` font reset (weight, style, size, white-space and alignment do
not inherit into a table), its `li` marker drawn inside when the item has
no list, its 3px beside a picture floated by `align`, its `nowrap` cell
with a pixel width wrapping after all, its four margin-collapsing rules
(§15.3.9: a paragraph's margin at the top of a body or cell, and at the
bottom of a cell, is zero) — and the Quirks Mode standard's 3.12, a table
taking the BODY's colour rather than its parent's. What is GEOMETRY is
pass 3's and 4's, each with its fixture: **the line-height calculation
quirk and the blocks-ignore-line-height quirk (3.3, 3.4) apply in
limited-quirks mode too** — which is why limited-quirks mode exists: a
line holding nothing but pictures makes no strut, so images stacked in
table cells touch, and a page made of sliced images (the old web's whole
navigation idiom) shows gaps between the slices without it. Hacker News in
the corpus has no doctype and runs in full quirks; a sliced-image table
is a fixture. The rest are full-quirks and geometry: percentage heights
(3.5), `html` and `body` filling the viewport (3.6, 3.7), text decoration
not reaching into tables (3.11), and the table quirks (3.8, 3.9, 3.10,
3.13). The hashless-hex and unitless-length quirks are CSS parsing, the
cascade's. The quirk switch lives in the producers, so a cascade that
arrives inherits it.

Inheritance walks parent to child, iteratively over the tree's parent
pointers; `em` and percent font sizes resolve against the parent's
computed size, and every other `em` against the element's own. The result
is absolute everywhere except percentages of a containing block, which
wait for pass 3. Pass 1 is ALL OR NOTHING: a style is a few hundred bytes
an element, so a page that cannot have its styles cannot have its boxes,
and a half-built table would let pass 2 decide a container's shape from
half its children — `flow_layout` answers NULL instead, which the door
allows.

### Pass 2 — boxes: tree + style → box tree

**What makes a box.** A block-level element makes a block-level box; an
inline element makes no box of its own here, only the OPENING and
CLOSING EDGES of an inline box in its container's item sequence;
`display: contents` makes nothing and its children are its parent's;
`display: none` makes nothing and nothing inside it does either (its
controls still belong to libpage's model, which is what makes a hidden
field submit). `template` contents are the fragment branch and are never
reached; comments make nothing; `noscript` is shown (we run no script).
Foreign elements (SVG, MathML) are `contents`: their HTML descendants are
walked and their own text is not drawn, `wend`'s rule — a formula's
tokens in reading order would say something the page does not. A closed
`details` shows its first `summary` and nothing else, text included.

**The item sequence.** A block container whose content is inline holds,
instead of children, its inline formatting context as ONE flat sequence —
text pieces, the open and close edges of inline boxes, atomic inlines,
`br`s and preserved newlines, `wbr`s, an inside list marker — because a
line is broken across that sequence and not per node (pass 3). An inline
box is one record shared by every OPEN that names it, so a split `<a>`
is one link to paint.

**Replaced elements are atomic**: `img`, `input` (except hidden),
`button`, `select`, `textarea`, `iframe`, `meter`, `progress` and `frame`
are laid out at a size the face supplies and never walked inside — a
button's label and a select's options are its widget's. `frame` is
block-level (a frameset stacks its frames); the rest are atomic inlines.
A frame and an iframe are drawn as the link libpage lists for them,
`wend`'s departure kept, because a frameset page has no other content
(libpage lists an iframe only when its `src` names something; an empty
one is the blank page and draws nothing to follow). A `marquee`, the one
inline-block that is not replaced, is an atom with a block container of
its own inside it. `object`, `video` and `canvas` show their fallback
content, since nothing plays them; `embed` and `audio` have none and make
nothing, and neither do `source`, `track` and `keygen`. `hr` is not
special: it is an empty block whose borders are the rule, which is what
the chapter's sheet says it is.

Text gets **white-space processing** (CSS 2.1 §16.6.1) HERE, once, into
the item: under `normal` and `nowrap`, a run of spaces, tabs and newlines
becomes one space, and a collapsible space that follows another ANYWHERE
in the same inline formatting context — across element boundaries — is
removed; a context starts as if after a space, which is the rule that
removes a line's leading space applied to its first line. A line's
TRAILING space stays for pass 3, which removes it where a line ends.
Under `pre` and `pre-wrap` every byte is kept and a newline is a forced
break, so a preformatted text node becomes a text item per line — the
engine refuses a literal LF (*Line breaking*, below). A text item POINTS
into the tree when processing changed nothing and owns a copy when it
did; its offset into the node's processed text is what a selection maps
a pixel back through. Tabs under `pre` land on eight-column stops the
way the terminal and the textview place them (pass 3).

**Generated content** is the sheet's: `q` draws its quotation marks,
nested levels alternating “double” and ‘single’, and an `rt` whose
`ruby` has no `rp` is drawn in parentheses — the chapter's fallback for a
browser that lays out no ruby.

**Anonymous boxes**, the two kinds CSS 2.1 requires. The first: a block
container whose children mix inline content and block boxes wraps each
inline run in an anonymous block (§9.2.1.1). A text node of nothing but
collapsible white space is not inline content for this purpose (the
newline between two `<p>`s makes no wrapper), which is why the question
is asked after pass 1 knows each node's `white-space`. An anonymous block
is made on the first CONTENT of a run and never before, so a run of
nothing but collapsible space — or an empty `<span>` — makes no box.

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
computes, bottom-up, one bit per element: **holds a block** (an inline
with a block-level child, or with an inline child that holds one; a
block answers for its own subtree). The mixing decision reads that bit on
each child, and an inline child with the bit set is split rather than
wrapped whole: at the block, every inline open around that point is
CLOSED (marked `split`), and after it the same inlines are OPENED again,
outermost first (marked `continued`). A block that splits a LINK carries
the link's index itself, since no inline edge is left to say so — Hacker
News's upvote arrow is `<a href><div></div></a>`, and a click on it must
still go somewhere.

Whether a container mixes is decided BEFORE its children are built, from
those bits (each child is read once by its parent, and the bit was
computed once by pass 1, so the cost is linear), so the wrapper exists
from the first inline child rather than being made retroactively when a
block sibling turns up. With boxes and items attached the moment they
are made and nothing ever discarded, a build that stops early is a
PREFIX of the whole build (*Proof*); the one allocation that would break
that — a list item's marker text — is made before its box.

The second kind: table structure that is missing gets anonymous table
objects (§17.2.1), level by level. Collapsible space between table parts
belongs to none of them; in a table or a row group, a cell or other loose
content gets an anonymous row (consecutive ones sharing it); in a row,
loose content gets an anonymous cell, which lays its run out like any
block container — so a table part inside it gets an anonymous table of
its own, and so does an internal table box found in ordinary flow.
libhtml's tree builder already repairs table markup, so these are rare
here and are implemented because the rule is the standard's.

**List markers.** An element displayed `list-item` gets a marker: its
text is generated here (CSS Counter Styles' symbols and suffixes — `• `,
`◦ `, `▪ `, `3. `, `c. `, `iv. `, the disclosure triangles — with a number
a style cannot spell falling back to decimal), drawn OUTSIDE by pass 3
from the box, or INSIDE as the first item of the item's content, where it
is inline content and can mix the item. The counters are the chapter's
sheet: `ol`, `ul` and `menu` reset the list-item counter (NOT `dir`, which
is `wend`'s one departure from the sheet, corrected here), `ol start`
names the first number and `li value` the current one, and `ol reversed`
counts down from `start` or from how many items the list holds — its
items, not a nested list's. A `summary` is a list item that counts
nothing.

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
line's fragments (justify grows the gaps of every line but the last and
one a `br` ends); `br` ends a line and holds it open with its own font's
box — which is what makes `a<br><br>b`'s blank line — except that in
either quirks mode a `br` beside other content holds nothing open, or a
`<br>` between two sliced images would open a gap under the first; under
`nowrap` nothing breaks; under `pre` only a newline does.

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
matter). A word wider than the line goes on a line of its own and
OVERFLOWS it, CSS's `overflow-wrap: normal` — the page grows wider and
`flow_width` says so; `wend` cuts such a word at the margin because a
terminal cannot scroll sideways, and a browser window can. A fragment is
one run, so the words of a line are grouped into fragments that stop
short of the run cap. Each line's fragment of each piece is then laid out
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

**Replaced boxes** take the size the page gave (`width`/`height`), else
the oracle's, scaled to keep its shape when the page gave one side
(CSS 2.1 §10.3.2, §10.6.2). When neither knows, the chapter's rule for a
picture that has not arrived: with alt text it is laid out AS ITS ALT
TEXT inline, breaking like any text (the reason a page of missing images
still reads); with an empty alt it takes no space; with no alt at all it
is the chapter's small icon, 16x16. A frame or an iframe with no size is
CSS's default 300x150. A form control the face has not measured gets a
placeholder ten ems wide and one line of its font high, which the face
corrects on the next layout — the oracle is the truth and the engine
never caches it. A replaced box sits on the baseline by its bottom margin
edge, or where its `vertical-align` puts it. `hspace`/`vspace` are
margins; an `hr` is an ordinary empty block whose borders are the rule.

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
box, per line box and per fragment, indented by depth, coordinates in
document pixels rounded by the painter's rule, text quoted, so a layout
change is a diff to a page and not "it looks different". This is a page
flowdump laid out in the guest with the real DejaVu faces:

```
page 500 156
block html 0 0 500 156
  block body 8 8 484 132
    block h1 8 8 484 38
      line 8 8 484 38 base 30
        text "Hello" 8 8 82 38 sans 32 bold
    block p 8 67 484 19
      line 8 67 484 19 base 15
        span b 118 67 54 19
        text "os64 lays out " 8 67 110 19 sans 16
        text "its first" 118 67 54 19 sans 16 bold
        text " page." 172 67 50 19 sans 16
    block ul 8 102 484 38
      block li 48 102 444 19
        marker "• " 34 102 14 19
        line 48 102 444 19 base 15
          text "one" 48 102 30 19 sans 16
```

`page` is the laid-out width and height. A box is `x y w h` of its border
box; a `line` adds how far below its top the baseline lies; a `text`
fragment's rectangle is its content area (the baseline less the ascent,
ascent plus descent tall), then the face, size and what decorates it; a
`span` is one inline box's piece on one line; an `atomic` is a replaced
box's border box. (That page has no doctype, so the heading's top margin
at the top of the body is the quirks-mode zero.)

The host harness renders against its own backend,
`tools/test_libflow_fonts.c`, whose every metric is a simple fraction of
the pixel size — at 16 px an ascent of 12, a descent of 4, a line of 20,
advances of 4, 8 or 12 — so each expected dump is worked out with a
pencil and the numbers are the algorithm's, not FreeType's hinting.
`tools/fonts/`' fake backend proves the text engine and deliberately does
not scale with size, which a layout test needs it to.

## The one door

```c
typedef struct flow_tree flow_tree_t;

typedef struct {
    // Fonts: the face's resolver. On the host, the harness's backend.
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
    uint32_t viewport_font_px;      // `medium`; 16 unless the face says otherwise
    flow_generic_t default_generic; // the family a page that names none is drawn in
    uint32_t ink, link_ink, paper;  // XRGB; the dumps name these, never print them
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
  `hr` under every `align` and `size`,
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
  alt, controls with and without an oracle answer.
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
| A `legend` drawn on its `fieldset`'s border | not CSS 2.1's model; laid out inside the fieldset, first, where it still reads | a form page where it misleads |
| Ruby annotations | CSS 2.1 has no ruby; `rt` is laid out inline after its base, `rp` hidden | a page in a script that uses it |
| `dialog` and `popover` positioning | an open dialog is laid out where it stands (the `position` row) | the `position` row |
| Dotted underlines (`abbr[title]`) | drawn as a plain underline; the line's style is a cascade property | the cascade |
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
