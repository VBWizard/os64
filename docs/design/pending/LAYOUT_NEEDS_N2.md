# N2: a read asks for what it reads

The design of BRIEF_LAYOUT_NEEDS.md § N2, as written for Fable's review
before code and accepted on 2026-11-21, with his rulings of that day (Q5:
a PAINT write nobody reads or sees costs nothing; entries for one element
merge, with a bound and a stated fallback; IntersectionObserver, the
pointer's re-hit and `elementFromPoint` make STYLE current first, never
GEOMETRY). What changed in the building is at the end: "As built" and
"N2a".

## What N2 does, in one paragraph

The playing page writes a paint-only inline style and reads `clientWidth`,
twelve times an update. After N1 each read costs a model refresh, a cascade
update and a whole restyle, with no placement. After N2 a `client*` or
`offset*` read whose only pending changes are paint-only inline style
writes is answered from the tree on screen, with no model refresh, no
cascade update and no restyle. The writes stay pending, and the next asker
that needs STYLE (the paint at the task's end, `getBoundingClientRect`, a
hit test) takes N1's path for all of them at once. A window nobody can see
skips that asker too, so the writes cost nothing until it is seen.

## 1. What is pending, and who keeps it

**A tree is CURRENT at a version**: the document version it was laid out
at, or the version at which N1 last kept a restyle in it. What is pending
is the change log after that version.

**`tree->pending`** (libflow, new): a change-log reader of its own, opened
at the version the tree is current at, and reopened whenever N1 keeps a
restyle (the graft's snapshot reader keeps reading from the tree's layout,
as now, and is not touched). With it:
- the elements whose `style` attribute changed since then (a set);
- a sticky verdict.

Lifecycle:

| event | the pending reader | the verdict |
|---|---|---|
| a layout makes a tree | opened at the tree's version | NONE |
| N1 keeps a restyle in it | reopened at the doc's version then | NONE |
| N1 declines | (the layout that follows makes a new tree) | |
| a read asks (`flow_pending`) | reads what is new | NONE → PAINT → RESTYLE, never back |
| the reader reports loss (-1) | stays lost | RESTYLE |
| the tree is freed | freed | |

**`flow_pending_t flow_pending(flow_tree_t *tree, const os64_html_document_t *doc, const garb_cascade_t *cascade)`**
answers NONE, PAINT or RESTYLE. It reads the new entries:
- an ATTR entry for `style` on an element: the element joins the set and
  is judged (§ 2);
- any other entry (another attribute, children, text, an adopted sheet, a
  custom element's state): RESTYLE.

The verdict is PAINT while every judged element is PAINT. An element is
judged again at each new entry for it, against the same baseline (§ 2), so
writing a transform sixty times judges one element sixty times and keeps
one set entry.

**Preconditions, else RESTYLE:** the cascade's version equals the version
the tree is current at (`garb_cascade_version`; N1's keep and every layout
leave them equal, and nothing else updates the page's cascade); the cascade
is complete and tracked; no selector of the page's sheets tests the `style`
attribute (§ 2).

## 2. Judging one element's style write

**The baseline is what the cascade parsed**, which is what the tree was
styled from. The log keeps no old values, so the comparison is between the
cascade's parse of the element's `style` attribute and the attribute as it
is now.

**libgarb, new:**
- Each slot remembers which entry of the cascade's parsed attributes
  (`c->attrs`) is its element's inline style (`inline_decls` sets it).
- **`garb_cascade_inline_changes(c, el, out, cap)`**: parses the attribute
  as it is now and compares it, longhand by longhand, with the slot's
  parse. It lists each longhand whose declaration was added, removed, or
  changed in value or importance, and for each: whether it is declared
  now, whether its declared value is the keyword `none`, and whether it
  holds `var()`. It answers -1 (unknown) for no slot, an element the
  cascade never styled, a parse that fails or is cut short, or more
  changes than `cap`.
- **`garb_cascade_reads_style_attribute(c)`**: true when any selector in
  the cascade's sheets (any compound, inside `:is()`, `:where()`, `:not()`
  and `:has()` included) tests the `style` attribute. Recorded when the
  sheets are compiled. A rule like `[style*=transform] { width: 50% }`
  makes a paint write a geometry write, so with one, every style write is
  RESTYLE.

**libflow judges each listed longhand against the N2 table** (§ 3) and the
element's style in the tree (its record in `tree->styles`). The element is
PAINT when:
- every listed longhand is in the table;
- none holds `var()`, and none is removed (an absent declaration falls back
  to the sheets, whose value is not known without the cascade);
- for the side-of-`none` longhands, the side holds: the new value is not
  `none` and the tree's style is on the drawn side (`f_transformed`,
  `f_filtered`), or the new value is `none` and the tree's style is on
  the `none` side. Anything else is RESTYLE, conservative where other
  longhands of the family might keep it drawn;
- for `visibility`, neither value is `collapse`;
- the element has a record and no motion (an animation may set the same
  property, and its keyframes are the restyle's to judge, as in N1).

A sheet's `!important` rule that overrides the inline value leaves the
computed value as it was, which is still PAINT. An inherited longhand
(`color`, `visibility`, `pointer-events`) reaches the subtree with nothing
placement reads.

## 3. The N2 table: longhands placement and boxes never read

From FIELD_TABLE.md, rows whose readers are PAINT, PUBLIC or HIT and never
PLACE, BOXES or GEOM ANSWERS. It is wider than N1's keepable set, because
N2 answers a box-size read and defers the rest. `z-index`, `isolation`,
`border-radius` and `object-fit` are safe for `clientWidth` even where N1
must decline them for the paint.

- **Plain:** `color`, `background-color`, `background-image` and the other
  background longhands, the four `border-*-color`, the eight radii,
  `box-shadow`, `text-shadow`, the `outline-*` longhands, `opacity`,
  `z-index`, `isolation`, `mix-blend-mode`, the `mask-*` longhands,
  `clip-path`, `object-fit`, `object-position`, `image-rendering`,
  `cursor`, `pointer-events`, `transform-origin`, `fill`,
  `text-decoration-color`, `text-decoration-style`, `caret-color`,
  `accent-color`.
- **Side of `none`:** `transform`, `translate`, `rotate`, `scale`
  (`f_transformed`); `filter`, `backdrop-filter` (`f_filtered`).
- **Not `collapse`:** `visibility`.

Left out on purpose (RESTYLE): `border-*-style` (GEOMETRY across `none`,
and not worth a side test here), `text-decoration-line` (N1's decoration
copies), `will-change`, every custom property, anything not listed. A
longhand missing from the table costs a restyle, never a wrong answer.

**The table's test** (libflow host): for each longhand, sample value pairs
on the allowed side; on generated trees, set it alone on a random element
and require every box's border box, padding box and scroll range, and
every client/offset answer, to equal a whole layout's before the change.

## 4. The askers

**libdom says what it reads.** `os64_dom_geometry_t` gains an input field,
`need`, which `d_geometry_read` sets before calling the provider:
`OS64_DOM_NEED_BOXES` for `offset*`, `offsetParent` and `client*`;
`OS64_DOM_NEED_ALL` for `scroll*`, `getBoundingClientRect` and every other
caller. A provider that ignores it is right, only slower. No ABI break for
other providers.

**yonder's `script_geometry`**, for the page on screen, answers from
`g.page.tree` with no `script_rebuild` when every one of these holds:
- the need is BOXES;
- the tree is complete, laid out at the view's size and zoom;
- no sheets, pictures or fonts moved (the conditions `relayout` already
  asks);
- `flow_pending` says PAINT (or NONE).

Otherwise it does as now (N1's path).

What each asker makes current after N2 (`—` nothing, `S` N1's path: the
restyle in place, and a layout if it declines):

| asker | NONE | PAINT pending | RESTYLE pending |
|---|---|---|---|
| `client*`, `offset*`, `offsetParent` | — | **—** | S |
| `scroll*` (range, and a scroll a shrunk range may clamp) | — | S | S |
| `getBoundingClientRect`, client rects, text rects | — | S | S |
| `elementFromPoint`, the pointer's re-hit, hover | — | S | S |
| IntersectionObserver, ResizeObserver | — | S | S |
| `getComputedStyle`, innerText, `getAnimations` | — | S | S |
| the paint at a task's end, window seen | — | S | S |
| the paint at a task's end, window covered | — | **—** (owed) | S |

**Two rows change and one obligation is made explicit.** The pointer's
re-hit and IntersectionObserver read the tree as it stands; today nothing
is pending when they run, because the task's end made it current. With the
covered row, PAINT can be pending when they run, so each asks
`flow_pending` first and takes N1's path for anything but NONE. Neither
runs on a covered window today (no frames; no pointer over it), so this is
the guarantee and not a cost.

**The covered window** (`g.covered`). At a task's end yonder skips the
rebuild while `flow_pending` says PAINT, and the rebuild stays owed
(`page_rebuild_owed` already says so). When the window is seen again the
owed rebuild runs before the first paint. A RESTYLE pending is rebuilt as
now, covered or not: observers and timers may read geometry, and a covered
page's timers still run.

## 5. The log, merged and bounded

A page writing a transform sixty times a second behind another window adds
sixty entries a second that no reader takes. **libhtml merges:** a new ATTR
entry (same target, same attribute name) or TEXT entry (same target) takes
the place of the existing one, moved to the tail with the new version,
unless a CHILDREN entry lies after the existing one. A reader that had
read the old entry reads the same change again; a reader behind it reads
one entry instead of two. Both are what an invalidation log promises. The
structural case keeps its order.

**The bound and what happens at it.** Merged, paint-only and text writers
hold at most one entry per element and attribute (or text node), bounded
by the document. The log's byte cap stays as it is. At it the log is lost
(`os64_html_changes` answers -1, as today): `flow_pending` answers RESTYLE
and N1 declines NO_HISTORY, so the next asker lays out whole. **Here I
depart from your wording.** The fallback you named was a restyle-needed
mark on the element. Our log's loss is document-wide, and I lean to
keeping it so. With merging, only structural churn can reach the cap, and
structural churn lays out whole anyway (Question 3).

## 6. The record and the check

- `layout needed` gains `reads from the kept tree N` (BOXES reads answered
  with no work) and `rebuilds owed while covered N`.
- **`--geometry-check`** covers N2's reads: every Nth read answered from
  the kept tree is then answered the old way too (`script_rebuild`,
  `relayout`, the snapshot again), and the two compared field by field. A
  difference is logged with the element, the field, both values and the
  elements judged PAINT. Diagnostics only: it does the work it checks.

## Proof

- **The random walk** (yonder scripts host, fixed seed, ASan): a page with
  positioned, flex, inline, scrolling and transformed elements; two
  thousand random operations: paint writes (by `style.x`, `cssText` and
  `setAttribute`), geometry writes, class changes, text changes, custom
  properties, and reads of every kind. With `--geometry-check` at every
  N2-answered read: zero differ, and N2 answered some (the counts are
  asserted).
- **The table's test** (§ 3), and garb host cases for the diff: added,
  removed, changed, importance, shorthands into longhands, `var()`, a
  failed parse.
- **Cases**: a transform written then `clientWidth` read is answered with
  no layout (the ledger shows none); `[style*=x]` in a sheet makes it
  RESTYLE; a class write is RESTYLE; a transform from `none` is RESTYLE; a
  covered window owes the rebuild and pays it when seen; a merged log holds
  one entry for sixty writes.
- **Mutants** on each PAINT condition (§ 2), the preconditions, the
  table's side tests, the BOXES need, merging's CHILDREN rule, and the
  covered gate.
- **The fixture** `tools/pages/layout-ticks.html` at 1, 12 and 40 parts:
  `layout needed` should show the reads from the kept tree growing with K
  and the forced layouts not.
- **P5 for Chris:** as N1's steps, plus the fixture at 12 and 40 parts.

## Not in N2 (said so the cut is visible)

- **`getComputedStyle` making STYLE current without GEOMETRY** when the
  restyle declines. Its answers come from the tree's styles (computed.c),
  so a styles-only answer needs a second source; with N1 it already costs
  no placement when the change is PAINT.
- **`getBoundingClientRect` restyling only the element and its ancestors.**
  A partial restyle is a new capability; N1's whole restyle answers it
  correctly.
- **Reach recomputed without a restyle for `scroll*`**: a transform's new
  value is only known after the restyle.

## Questions, each with my lean

1. **The cut above** (`getComputedStyle` and partial restyles out of N2).
   Lean: yes. The playing page reads `clientWidth`, and N3 is worth more
   than either; book both in DEBTS with what the records show.
2. **Judge at read by a diff against the cascade's parse** (covers
   `style.x`, `cssText` and `setAttribute`), rather than a hint written
   with the change by libdom. Lean: the diff. The hint would cover the
   CSSOM path only, and would need a field on libhtml's entries that libhtml
   cannot interpret.
3. **The log's fallback stays document-wide loss**, not a per-element mark
   (§ 5). Lean: yes, for the reason given.
4. **"Nobody sees" means a covered window only**, not an element scrolled
   out of view. Lean: yes. An element out of view is N3's damage question,
   and a covered window is a fact yonder already keeps.

## As built, after the design pass of 2026-11-22

The design above was accepted on 2026-11-21. N2 was built on N1's branch,
parked with three open items, and rebased onto userland once N1 and
WHOLE_REPAINTS.md's first slice had merged. Fable's conditions of
2026-11-22 are in the code as follows.

**A lost log is everything owed.** A pending reader that finds the change
log lost leaves the verdict RESTYLE until the tree is made current again,
never nothing pending (`flow_pending_t.lost`). libhtml: a reader whose
version is below the log's floor is LOST and holds nothing back. Pruning
passes over it, and with only lost readers a change is kept for nobody and
the floor rises past it, so a reader made later starts after what was not
kept.

**The merge.** Its key is the element and the attribute's name; the
match's own comparison of the name guards a collision in the index, and a
hundred names written twice to one element prove it. Merging is sound
because an entry carries no value: the newest says everything the earlier
did. What must hear every change in order (mutation records) hears it from
the synchronous listener, which merging never touches. The cascade reads
the log as a set (removals, additions, then marks), and merging never
crosses a CHILDREN entry.

**CSS-wide keywords** (`inherit`, `initial`, `unset`, `revert`,
`revert-layer`) in an inline declaration are RESTYLE whatever the
longhand's class: the value comes from elsewhere.

**Transitions.** yonder runs none (`CSSTransition` is an interface only).
The checkpoint N2 owes them is written beside `flow_pending` and in the
transitions row of DEBTS.md.

**The record** (`--layout-profile`): `forced layouts by what was pending
reading N` names each layout a script's ask forced by the class and writer
of what was pending, or why nothing could be said, and where;
`pending reader lost the log reading N` counts the losses; and
`style writes restyled for a style selector reading N` counts N2a's
refusals by element.

## N2a: a style reader narrows to the elements it may match

The first P5 run with the record above showed N2 never engaging on the
video watch page: every forced layout was refused because one selector of
the player's sheet tests the `style` attribute
(`.area:has(>.button[style*="display: none"])`), and the rule then was
document-wide. Ruled 2026-11-22.

A compound is a conjunction, so an element that fails one of its exact
tests cannot be matched by it whatever its style says, and nothing else
about a style write reaches a selector. So libgarb records each compound
that tests `style` (`garb_style_reader_t`, select.h § A STYLE READER), and
a style write is RESTYLE only on an element some recorded compound may
match (`garb_cascade_style_may_read`); other writes are judged as before.

1. Only exact tests on unmoved state narrow: a type, an id, a class, an
   attribute other than `style`. Every pseudo-class, and each :not(),
   :is(), :where() or :has() the test is not held in, is taken as passing;
   dropping a test only widens the restyle, the safe direction.
2. The element is tested with the matchers the cascade uses
   (`simple_matches`: quirks-mode folding of class and id, attribute name
   case, namespaces), never a second spelling.
3. Anything the recorder does not fully understand is BARE, which keeps
   the document-wide rule: a compound holding :host or ::slotted or inside
   their arguments, more than GARB_STYLE_READERS readers, and a declaration
   reading `attr(style)`.
4. A test inside a pseudo-class's argument is recorded as the compound
   there that holds it, alone: `.a:not([style*=x])` records a bare
   compound and stays document-wide. Coarse and correct; the outer
   compound's tests are not borrowed unless a page is seen paying for it.
5. Proof: the random walk runs with readers in each position, narrowed
   (plain, inside :has(), on an ancestor) under `--geometry-check`, and
   bare (inside :not(), alone), where the tree answers no read; mutants
   for each rule above.
