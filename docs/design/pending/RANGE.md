# RANGE.md — DOM Range, the page Selection, and hit testing

Status: DESIGN, reviewed (Fable, 2026-11-06: sound; nine findings folded
in below) and ruled (Chris and Fable: the cut, the registry in libhtml,
MessageChannel apart). Chris asked for it on 2026-11-06,
after a large social site's endless feed showed "There was an error
loading the next page" on yonder: its next page arrived whole (200, about
560 KB of HTML) and died turning into nodes, because the component that
inserts it calls `document.createRange().createContextualFragment(html)`
and yonder has no `createRange`.

## 1. What pages ask for

A census of the local corpus pulls (324 hosts, 4,503 distinct scripts;
`~/os64-opus-kit/tools/range_census.py`, counts in
`census-range-1106.json`; a regex over script text, so a floor for what a
page may call and a ceiling for what it does call):

| Surface | Hosts | Note |
|---|---|---|
| `createRange()` | 110 | `new Range()` in 2 more |
| `setStart` / `setEnd` | 69 of the 110 | the common way to place one |
| `collapse` | 43 | |
| `selectNodeContents` | 33 | `selectNode` 10 |
| `createContextualFragment` | 27 | the feed that started this |
| `commonAncestorContainer`, `startContainer`… | 15–17 | |
| `compareBoundaryPoints`, `comparePoint` | 8, 7 | |
| `cloneContents`, `extractContents`, `deleteContents`, `insertNode`, `surroundContents` | 7, 3, 5, 4, 2 | the content algorithms are rare but present |
| a range's `getBoundingClientRect` / `getClientRects` | up to 71 / 52 | the regex over-counts; the shape `selectNodeContents(e); r.getBoundingClientRect()` is in a widely used UI framework |
| Selection with ranges: `addRange` / `removeAllRanges` / `getRangeAt` | 80 / 83 / 32 of the 110 | a range is most often made to be put in the Selection |
| `elementFromPoint` / `elementsFromPoint` | 66 / 24 | the feed's timers call it every few seconds and fail |
| `caretPositionFromPoint` / `caretRangeFromPoint` | 3 / 3 | |
| `StaticRange` | 2 | |
| `MessageChannel` | 115 | 73 hosts' records say MISSING; ruled its own slice, not this design (§ 11, and its DEBTS row) |

Records: in the pulls, one host dies on `createRange` outright (FAILED …
`'createRange' is not a function`), and the feed's record on the P5 is a
second; three name `Range` as a missing global. The rest test for it
first (`typeof document.createRange == "function"`) and take a fallback,
or never reach it because an earlier member they needed was missing — so
the records undercount what a Range would change.

## 2. The cut: the whole Range, behind one switch

A Range is reached for in two ways, and both forbid a partial one.
Pages test `typeof document.createRange` and then use whatever they need
— the feed's own guard is exactly that test — so a `createRange` whose
range lacks `getBoundingClientRect` turns a page that takes its fallback
today into one that throws. And the members that look independent are
not: `deleteContents` is "replace data" and removals, `cloneContents` is
clones and "substring data", a boundary point moves under every mutation.
SHADOW_DOM.md § 2 reached the same answer for the same reason.

So: the whole Range interface (§ 5), its rects (§ 7), and the Selection's
range members (§ 9) are behind **one libdom switch, `options.ranges`**,
off until the last slice turns it on in yonder, as `shadow_trees` was.
The switched names are `Range`, `AbstractRange`, `StaticRange`,
`document.createRange` and the Selection's range members, and **off means
absent**, not present and throwing: `typeof Range` and `'Range' in window`
are tests too.

What is NOT behind it, because nothing tests for it as part of a set and
today a call to it simply throws:
- the CharacterData members (`length`, `appendData`, `insertData`,
  `deleteData`, `replaceData`, `substringData`), and `splitText` and
  `normalize` moving onto the new verbs (§ 4) — they land switched on in R1;
- `elementFromPoint` and `elementsFromPoint` (§ 8).

## 3. The model: live ranges live in libhtml

A **live range** is two boundary points, each a node and an offset: for a
CharacterData node (Text, Comment, ProcessingInstruction) the offset is in
**UTF-16 code units** into its data; for any other node it is a child
index. A boundary point never sits in a doctype or an attribute. The DOM
Standard keeps every live range correct across every mutation of the tree
by steps written into the mutation algorithms themselves (§ 4).

**Where the registry lives: libhtml.** Not in libdom's mutation listener,
for three reasons the code map found:
- The listener is told after a verb has run, and a node moved from
  another parent reports its removal after it has already been placed in
  the new one, so its old index cannot be recovered from the record
  (walker.c steps around this for NodeIterator, at some cost).
- A text change reports only the old string, no offset or count — and
  the range steps for appendData, deleteData and splitText depend on them.
- The parser, the fragment parser, libpage and libdom all mutate the tree;
  only libhtml sees all of them, and it already funnels every link and
  unlink through two primitives (`h_attach`, `h_detach`, core.c) that call
  `d_linking` / `d_unlinking` at exactly the moments the standard's steps
  name: before the unlink, with the old index still computable; at the
  link, with the new one.

So libhtml gains **`os64_html_range_t`**: start node and offset, end node
and offset, owned by its document, on a per-document list, holding both
boundary nodes (`os64_html_hold`) so a removed subtree a range points into
is not reclaimed under it. Its verbs:
- `os64_html_range_new(doc)` (both points at (document, 0)),
  `_free` (releases the holds), `_set(range, which, node, offset)` with
  the node-type and offset checks the standard's "set the start" makes
  (InvalidNodeTypeError for a doctype, IndexSizeError past the length),
  and readers.
- Everything else about a range — comparing points, collapsing when the
  start passes the end or the roots differ — is libdom's (§ 5), because it
  is the script's algorithm, not the tree's.

The list costs nothing when empty, which is every page that never makes a
range: each mutation primitive tests one pointer. With ranges:
- **an append moves no boundary and computes nothing** (the insert step
  wants the reference child's index, and an append has none) — the
  parser's case and the feed's, so a long page parsed while one range
  lives stays linear;
- **a removal walks each range's boundaries up toward the removed node**
  (O(ranges × depth)), and computes the removed node's index — a sibling
  walk — only when some boundary turned out to be in the removed subtree or
  at the parent, which that same walk discovers;
- an insert before a child computes that child's index under the same
  condition.
QuickJS frees an unreferenced Range object at once (reference counting),
so the list holds the ranges scripts still hold, not every range ever
made. R1 measures the reference-model walk at 1, 100 and 1,000 live
ranges, before and after the conditional index, and writes the numbers
here — the argument a future child-index cache would have to win.

The walks are plain-tree, in SHADOW_DOM.md Appendix A's vocabulary
(STOPS): the standard's remove step says "inclusive descendant", not
shadow-including, and a range's root is its plain tree root. libhtml has
both kinds of parent walk; these use `->parent`, never
`os64_html_shadow_including_parent`.

## 4. The mutation steps, and the verbs that carry them

Each is the DOM Standard's text, run inside libhtml at the point named:

| Mutation | Where | Steps |
|---|---|---|
| insert (one node at a time) | `d_linking`, before the link, which grows a `before` argument | if `before` is non-null, each boundary at (parent, offset > index of `before`) gains 1; an append (`before` null) moves nothing and computes no index |
| remove | `d_unlinking`, before the unlink | the node's index first; each boundary whose node is an inclusive descendant of the removed node moves to (parent, index); then each boundary at (parent, offset > index) loses 1 |
| replace data (node, offset, count, data) | a new verb, below | each boundary in the node with offset in (offset, offset + count] goes to offset; each past offset + count shifts by data length − count |
| split a Text node | a new verb | the new node is inserted (insert steps), boundaries in the old node past the offset move to the new node, boundaries at (parent, index + 1) gain 1, then replace data truncates |
| normalize | a new verb | boundaries in each merged node move into the first node, shifted by the length merged before it; boundaries at (parent, index of a merged node) move into the first node at that same length |

The parser's own text appends (`h_text_put`, and the fragment parser's)
are "append data", whose range steps move no boundary that was valid
before: they carry no hook, on purpose.

**A boundary between the halves of a surrogate pair** (web semantics,
ruled in Fable's review): offsets are UTF-16, text
is stored UTF-8, and libhtml refuses a lone surrogate, so `splitText`,
`deleteData`, `substringData`, `toString` and every boundary at an offset
inside a non-BMP character cannot make the lone surrogates other browsers
make. The verbs snap such an offset **down to the start of the code
point**: a pair is never split, and a boundary set there reads back at the
pair's start. DIVERGENCES names it.

**The character-data verbs.** libhtml's `set_text` replaces a whole string
and is the standard's "replace data(0, length, data)", so it carries its
range steps too (positive offsets collapse to 0). Three verbs join it:
- `os64_html_replace_data(doc, node, offset16, count16, utf8, len)`: the
  spec's replace data, offsets in UTF-16 units, the string converted at
  the edges with the UTF-16/UTF-8 helpers libdom already has
  (`d_utf16_length`, `byte_of_unit`), which move to libhtml where both
  sides can use them.
- `os64_html_split_text(doc, node, offset16, *out)` and
  `os64_html_normalize(doc, node)`, which do what libdom's `splitText` and
  `normalize` do today (mixin.c) but in the standard's order — today's
  splitText shortens first and inserts after, the reverse of DOM § 4.11,
  which matters only once ranges exist.

Each logs the change it makes as `set_text` does (a TEXT record with the
old string; MutationObserver's characterData records come from it
unchanged). libpage wraps each (`os64_page_node_replace_data`…) as it
wraps `set_text`, so control state and the model see them, and libdom
uses the wrappers.

**libdom's CharacterData members** arrive with them, because Range needs
"replace data" and pages need the members (`tools/wpt/expected/dom/nodes.txt`
lists them missing): `length`, `appendData`, `insertData`, `deleteData`,
`replaceData`, `substringData`; `splitText` and `normalize` move onto the
new verbs.

## 5. The Range interface (libdom `range.c`)

`AbstractRange` (`startContainer`, `startOffset`, `endContainer`,
`endOffset`, `collapsed`), `Range` over it, `StaticRange` beside it. A
Range object is a JS object whose record holds an `os64_html_range_t`;
`document.createRange()` and `new Range()` make one on the window's
document. The members, each the standard's algorithm:

- **Placing**: `setStart`, `setEnd`, `setStartBefore/After`,
  `setEndBefore/After`, `collapse`, `selectNode`, `selectNodeContents`.
  "Set the start or end": if the new point is in another root than the
  other point, or would pass it, the range collapses to the new point.
  The root is the plain tree root, as the standard says: a range does
  not cross into or out of a shadow tree.
- **Comparing**: `compareBoundaryPoints` (and its four constants),
  `comparePoint`, `isPointInRange`, `intersectsNode`,
  `commonAncestorContainer`. Boundary-point comparison needs tree order
  and child indexes; compare.c's `precedes`, `root_of` and `is_ancestor`
  become shared libdom helpers. A child index is a sibling walk (libhtml
  keeps none) — the cost of one comparison, not of the tree.
- **The content algorithms**: `cloneContents`, `extractContents`,
  `deleteContents`, `insertNode`, `surroundContents`, each built from the
  libpage-wrapped verbs (`d_insert`, `d_remove`, `d_tree_change`, the new
  replace-data wrapper) so MutationObserver records, form-control state
  and script insertion behave as they do for every other mutation. Their
  edge cases are what WPT's dom/ranges tests are for (§ 10).
- **`cloneRange`, `detach`** (a no-op, as the standard says), **`toString`**
  (the text of the Text nodes in the range, UTF-16-cut at the ends).
- **`createContextualFragment(html)`** (DOM Parsing): the context is the
  start node if it is an element, its parent element otherwise, or a new
  `body` element when there is none or the context is `html` in an HTML
  document — insertAdjacentHTML's substitution (mixin.c) — then
  `os64_html_parse_fragment`. The scripts are **not** marked already
  started (innerHTML and insertAdjacentHTML mark them; this does not), so
  each runs when the fragment is inserted through `d_tree_change`. That
  is the feed's whole use: it parses each streamed piece and inserts it.
- **`getClientRects`, `getBoundingClientRect`**: § 7.

**Lifetime.** The record follows walker.c's `DWalk`: the finalizer marks it
dead (a finalizer must not reach into libhtml while the runtime is being
torn down), dead records are freed at the next creation **and at the top
of libdom's own mutation wrappers** (`d_tree_change`, `d_insert`,
`d_remove`, which run on the owner thread outside any finalizer) — so a
dropped range on a large detached subtree does not pin it until the next
Range is made, and the mutation walk does not keep visiting it — and the
binding drains the rest when the document goes. A range in a document that is
being freed is released before the document, since a hold left behind is
fatal (`OS64_HTML_FATAL_EXIT`).

## 6. StaticRange

`new StaticRange({startContainer, startOffset, endContainer, endOffset})`:
four fields checked once (no doctype or attribute; offsets are not checked
against lengths, as the standard says) and never updated. It holds its
nodes like any binding object. InputEvent's `getTargetRanges` keeps
answering an empty list (event.c), now with an interface to name.

## 7. Geometry: a range's rects

CSSOM View: a range's client rects are the border boxes of each **selected
element whose parent is not selected** (a selected element's descendants
add nothing of their own — without that rule `selectNodeContents(body)`
answers every border box in the page, and the list, not just the bounding
rect, is what the framework in § 1 reads), and for each Text node the
range touches, the boxes of the selected part of its text. The element
half exists (the geometry provider). The text half needs a map.

**Three coordinate systems** meet at a text boundary:
1. the boundary's offset, in UTF-16 units into the node's data;
2. the node's data as stored, in UTF-8 bytes — `byte_of_unit` is that hop;
3. the PROCESSED text libflow laid out, after white-space collapse
   (`f_collapse_white`); a TEXT box's `begin` is already an offset into it
   (flow.h: what a selection maps a pixel back through).

The missing hop is 2 → 3, and a box's two ends would not give it — a
partly selected box is exactly a boundary inside one. It needs no table:
`f_collapse_white` deletes and substitutes, never reorders, so replaying
it over a node byte's source prefix, with the `space` state the item
started in, gives that byte's processed offset (and the inverse). libflow
records per text item its source start byte and that entry state;
`text_item` is already handed the offset, and pre-line's `seg` (a
processed index) carries the source `from` with it. `text-transform` needs
no map: boxes.c keeps it length-preserving by construction (the ß/ÿ
exclusion exists for exactly this).

The design:
- **libflow keeps the item's source start and entry state**, and offers
  the replay (DOM byte → processed offset) beside its other text queries.
- **A range-rects provider** beside the geometry one in dom.h
  (`os64_dom_range_rects_provider_t(opaque, start, start16, end, end16,
  out, cap)`): libdom hands the boundary points; yonder makes the layout
  current (as `script_geometry` does), walks the boxes of the nodes the
  range touches, and for a partly selected TEXT box measures the
  selected processed bytes with `os64_text_caret`; it answers CSS-pixel
  rects in the viewport of the document the range is in — the same choice
  the geometry provider makes for a frame's document, which this follows.
- `getBoundingClientRect` is their union (or the empty rect).
- Generated content and list markers name their element, not a Text node,
  and are skipped, as other browsers skip them.

## 8. Hit testing: `elementFromPoint`, `elementsFromPoint`

`document.elementFromPoint(x, y)` and `elementsFromPoint` on Document and
ShadowRoot (DocumentOrShadowRoot), through a **hit provider** beside the
others in dom.h. yonder answers with what it already does for the pointer
(`element_hit`: `flow_hit` at the point in device pixels, the zoom applied,
an image map's area resolved, text retargeted to its element through the
flat tree), after making the layout current as the SVG hit provider does.
libdom retargets the answer against the root asked (a node inside a
shadow tree answers its host to the document) — the one FLAT walk in
this design, in SHADOW_DOM.md Appendix A's vocabulary.

A point inside the viewport but on bare canvas answers **the root
element**, never null (the canvas is the root's box), and
`elementsFromPoint` ends in `body, html` for the same reason; if
`element_hit` answers otherwise there for the pointer, the provider
adjusts it. Like the geometry provider, the hit provider serves the
document it is asked about: a call on a frame's document hit-tests in
that frame's viewport, in its coordinates.

`elementsFromPoint` needs every box at the point in paint order, which
libflow does not have: a `flow_hit_all` beside `flow_hit` collects them
in the order painting would draw them, topmost first. Points outside the
viewport answer null and an empty list, as the standard says.
`caretPositionFromPoint` (3 hosts) waits: a DEBTS row.

## 9. The page Selection

Today `getSelection()` answers a fixed "None" object (selection.c). With
ranges the Selection becomes real, as a model: it holds zero or one live
range (`os64_html_range_t`) and a direction, and its members are the
Selection API's: `rangeCount`, `getRangeAt` (the same Range object each
time), `addRange` (ignored when one is already there, as browsers do),
`removeRange`, `removeAllRanges`/`empty`, `collapse`/`setPosition`,
`collapseToStart/End`, `extend`, `setBaseAndExtent`, `selectAllChildren`,
`containsNode`, `deleteFromDocument`, `toString`, and the anchor/focus
readers, `isCollapsed`, `type`, `direction`.

What it is not, and says so in DEBTS: **yonder draws no page-text
selection** (it keeps none, selection.c's header), so a selection a script
sets is not painted and a person's drag sets none; and `selectionchange`
is not fired. Both wait for yonder to have text selection of its own —
the census's uses are scripts that save and restore a selection around
their own edits (the framework that keeps `focusedElem` and
`selectionRange`), which a model serves.

## 10. Proof

- **WPT dom/ranges**: `tools/wpt_fetch.sh` gains `dom/ranges` and an
  expectation file with a `[required]` list — the Range-set, compare,
  cloneContents, extractContents, deleteContents, insertNode,
  surroundContents, mutations and stringifier tests — run through
  `tools/test_yonder_wpt_host.sh`. Their mutation tests are the live-range
  steps' real examination.
- **libhtml, a reference model**: a host test that applies random
  mutations (the existing random walk) to a tree holding random ranges and
  checks every boundary after every step against a model that recomputes
  it the slow way from the standard's text; plus hand mutants of each
  step.
- **libdom host cases** for the interface, the CharacterData members, the
  Selection, `createContextualFragment`'s scripts running on insert, and
  the switch off (no name exists).
- **yonder**: an end-to-end case shaped like the feed (a streamed partial
  parsed with `createContextualFragment` and inserted piecewise), range
  rects against laid-out text (collapsed whitespace, `text-transform`, a
  boundary inside a box, `selectNodeContents(body)`'s list), and
  `elementFromPoint` through a zoom, a shadow tree and a point in the
  margin.
- **Measured** (R1): the reference-model walk at 1, 100 and 1,000 live
  ranges, mutations per second, before and after the conditional index
  (§ 3), one line here.
  `tools/wpt/expected/css/cssom-view.txt` lists the elementFromPoint
  parameter tests as passing today because calling an undefined method
  also throws TypeError; they are re-read honestly once it exists.
- **The census again**, and the feed on the P5.

## 11. Slices, and what Chris rules

- **R1 Live ranges in libhtml**: the registry, the mutation steps in
  `d_linking`/`d_unlinking`/`set_text`, the three character-data verbs,
  libpage's wrappers, libdom's CharacterData members and splitText /
  normalize on the new verbs, and the reference-model test. No script
  sees a Range yet. M.
- **R2 The Range interface**: range.c (§ 5, without rects), StaticRange,
  `createContextualFragment`, WPT dom/ranges, behind `options.ranges`. M.
- **R3 Rects and hit testing**: libflow's source spans, the range-rects
  provider, `elementFromPoint`/`elementsFromPoint` with `flow_hit_all`
  and the hit provider (no switch for these two). M.
- **R4 The Selection, and the switch on** (§ 9); the census again. S–M.

Stacked, as Slice D was; reviews by the same rule — Fable on this design,
Codex on each PR.

**Ruled (Chris and Fable, 2026-11-06):**
1. The cut (§ 2): the whole interface behind one switch, as with shadow
   trees, rather than exposing members slice by slice.
2. The registry in libhtml (§ 3) rather than on libdom's listener.
3. `MessageChannel` (115 hosts, 73 MISSING) is its own small slice, not
   folded in: it shares nothing with Range but the census. When: Fable's
   lean is right after R2 rather than after R4 — it is S and needs no part
   of this arc — and that is the plan unless Chris places it otherwise.

## 12. What waits (DEBTS rows the arc books)

- `MessageChannel` and `MessagePort`, booked with R1 so the census's
  largest missing name has its row while its slice waits.
- `caretPositionFromPoint` / `caretRangeFromPoint`.
- Painting the page Selection, a person's own text selection, and
  `selectionchange`.
- A range's rects for generated content and markers (skipped).
- Ranges in XML documents' processing instructions behave as text
  (CharacterData), which they are; nothing else differs, so no row unless
  a test finds one.
