# RELAYOUT.md — the change log, and a layout that reuses what did not move

*Slice 5 of [MODERN_WEB.md](MODERN_WEB.md): D13, incremental relayout,
then `requestAnimationFrame` and `MutationObserver`. Design by Fable,
2026-10-21, from a read of libhtml, libgarb, libflow and yonder as they
stand at `userland` 44768c2f. Opus builds it in four cuts; Fable reviews
the two that change what libflow's tree is. Chris rules and tests.*

## What this settles

1. **One change log, in libhtml, read by three consumers.** The document
   already moves a version on every change a reader could see; it does not
   say WHAT changed, so every consumer rebuilds whole. The log says what:
   which node, which kind of change, and for `MutationObserver` the details
   the standard's record carries. The restyle, the relayout and the
   observer read the same entries. Nothing is told anything; readers ask
   by version, as they ask today.
2. **The layout stays DERIVED. A relayout is `flow_layout` handed the
   previous tree, and its output is identical to a whole rebuild.** LAYOUT.md
   ruling 2 ("the box tree is derived, rebuilt whole, and holds nothing a
   rebuild cannot recreate") is reversed only in its middle clause, by the
   trigger it named (a real page whose rebuilds are visibly slow). The
   first and last clauses are what make the whole thing provable: the
   harness lays every fixture out twice after every mutation, whole and
   incrementally, and diffs the dumps. A difference is a bug in the memo,
   by definition.
3. **Reuse is by node identity, style equality and equal inputs, grafted by
   COPY.** A subtree is reused when its nodes did not change, its computed
   styles came out equal, and the layout hands it the same inputs it had
   last time. What is reused is copied from the old tree into the new one's
   arenas, runs retained, never pointed at: no tree holds another tree's
   arena, so there is no chain of arenas to compact and the old tree is
   freed when the new one is shown, exactly as today.
4. **The cascade is updated in place** for the restyle set, generation-
   counted, so yonder's "same cascade?" checks keep their meaning.
5. **The public tree, the stacking order, the ink reach and libpage's model
   stay whole**, and the instrument that lands first decides, with numbers
   from the P5, whether any of them follows.
6. **`requestAnimationFrame` rides the loop's rendering step** at a frame
   clock that exists only while a callback is pending; `MutationObserver`'s
   records are the log's entries, delivered at the microtask checkpoint.

## What a change costs today

When a script changes the tree, the next turn's rendering step
(`script_rebuild`, yonder.c) does five whole-document things:

| Step | Library | What it walks |
|---|---|---|
| `os64_page_rebuild` | libpage | every node: links, forms, images, sheets |
| `garb_cascade` | libgarb | every element: the rules that match it, weighed; var() replaced |
| `flow_layout` pass 1 | libflow `style.c` | every element: the Rendering chapter, the hints, the winners, into `flow_style_t` |
| pass 2 and pass 3 | `boxes.c`, `layout.c` | every box; every inline formatting context measured and broken into lines |
| the public tree | `flow.c` | every box again: rects, overflow, ink, stacking contexts, layers |

then `forms_build`, and a whole-view paint. A geometry read inside a
script (`offsetWidth`, `getBoundingClientRect`) forces the same chain
synchronously, charged to the script (DOM.md ruling 7). The numbers that
exist: Wikipedia lays out in 600 ms on the P5 and fetch.spec.whatwg.org in
6 s (YONDER.md § Booked); danlegt's layouts take over 200 ms in QEMU and
run 11.6 a second on the P5 when its animations drive them
(YONDER_PERF.md). Nothing yet says how the chain's cost divides between
its steps. The first cut's instrument does; it is the number that decides
what beyond libflow follows.

## The change log (libhtml)

**What is logged.** Every verb that moves the version writes one entry,
and so does the parser for each insertion it makes while a page streams
(the standard makes the parser's mutations observable, and the layout
wants them for the same reason). The entry is the standard's
`MutationRecord`, no more:

```c
typedef enum {
    OS64_HTML_CHANGE_CHILDREN = 1,  /* target's children changed: insert, remove, replace, a fragment drained */
    OS64_HTML_CHANGE_ATTR,          /* an attribute of target set or removed */
    OS64_HTML_CHANGE_TEXT           /* target's text replaced (a text or comment node) */
} os64_html_change_kind_t;

typedef struct {
    uint64_t version;               /* the version this change made the document */
    os64_html_change_kind_t kind;
    const os64_html_node_t *target; /* CHILDREN: the parent; ATTR: the element; TEXT: the node */
    /* CHILDREN: the nodes put in and taken out, as children of target, in
     * tree order, and the siblings they stood between (NULL at an end). */
    const os64_html_node_t *const *added;
    const os64_html_node_t *const *removed;
    size_t nadded, nremoved;
    const os64_html_node_t *previous_sibling, *next_sibling;
    /* ATTR: the name, and the value before the change (NULL unless old
     * values are kept; NULL for an attribute that was absent). */
    const char *attr_name, *attr_old;
    size_t attr_old_len;
    /* TEXT: the text before (NULL unless kept). */
    const char *text_old;
    size_t text_old_len;
} os64_html_change_t;
```

**Readers register, so the log keeps only what somebody still needs.** A
document with no reader logs nothing, which is wend's and the parser
fixtures' cost: zero. yonder registers two, the layout's and the
binding's.

```c
typedef struct os64_html_reader os64_html_reader_t;
/* A reader that has seen the document at `version` (the document's current
 * version at registration is the usual answer). NULL on no memory. */
os64_html_reader_t *os64_html_reader_new(os64_html_document_t *doc, uint64_t version);
void os64_html_reader_free(os64_html_reader_t *r);
/* The changes since the reader's version, in order, as a borrowed array
 * valid until the next call on this document; the count, or -1 when the
 * log no longer reaches the reader's version, which a reader treats as
 * "everything may have changed". Marks the reader as having seen the
 * document's current version. */
int64_t os64_html_changes(os64_html_reader_t *r, const os64_html_change_t **out);
/* Keep old attribute values and old text in the entries (the observer's
 * attributeOldValue and characterDataOldValue). Costs a copy of each old
 * value while on; off is the default. */
void os64_html_changes_keep_old(os64_html_document_t *doc, bool attrs, bool text);
```

An entry lives until every reader has passed it; the document frees
entries below the oldest reader's version. Each entry HOLDS (`os64_html_
hold`) its target, its siblings and every added and removed node while it
lives, so a removed subtree a reader has not seen yet is not reclaimed
under it; a reader that reads promptly, as both of yonder's do (once per
turn, once per microtask checkpoint), holds little. The log has a byte cap
(`max_log_bytes`, an option; the default is a sixteenth of the arena
budget): past it the oldest entries go and every reader behind them is
told -1 at its next read. A -1 is never wrong, only slow.

**Not logged:** form-owner changes and the control state (libpage's own
version covers those; the observer does not see them either); a verb that
left the tree as it was (the version does not move, so no entry).

## Restyle: the cascade and pass 1

**The restyle set.** For an entry whose target is T: T itself, every
descendant of T, and, when the cascade has any SIBLING-SENSITIVE selector
(`+`, `~`, `:nth-child` and its family, `:first-child`/`:last-child`/
`:only-child`, `:empty`, `:nth-of-type` and kin), T's parent and the
parent's whole subtree, because a change under T can change which rule
matches T's siblings and the parent's own `:empty`. Descendant combinators
look UP, so nothing outside those sets can change its match. A cascade
with any `:has()` restyles everything, every time: `:has()` makes an
ancestor's match depend on its descendants, and the honest bound is the
document. The cascade computes the two facts at build (`uses_has`,
`sibling_sensitive`), and `garb_cascade_dump` prints them.

**The cascade is updated in place.**

```c
/* Recompute the winners of the elements `changes` reach, by the restyle
 * rule above, drop the slots of removed nodes, and move the generation.
 * False when memory ran out or a change asks for everything: the caller
 * then cascades afresh, as today. Entries are the document's log's; the
 * cascade must have been built over `doc` at this env. */
bool garb_cascade_update(garb_cascade_t *c, const os64_html_change_t *changes, size_t n);
uint64_t garb_cascade_generation(const garb_cascade_t *c);
```

In place, because the cascade is the window thread's alone and nothing
points into it: libflow copies winners into its own `flow_style_t` and its
own lists (the `F_STYLE_LISTS_BUDGET` arena), and the document strings a
winner names are pinned by the cascade itself. Dropping a removed node's
slot is not optional: the slots are hashed by node POINTER, a freed node's
address may be given to a new node, and a stale slot would hand that node
another element's winners. The log names every removed node; the update
drops them before any element is asked again, which yonder guarantees by
updating the cascade before every layout of a moved document.

**Pass 1 reuses styled elements.** `flow_env_t` gains `previous`, the tree
last shown of this document, and `f_style_build` walks as it does today but,
for an element outside the restyle set whose ancestors are all outside it,
COPIES its `FStyled` from the previous tree's styles (the struct, and its
lists into the new arena) instead of computing it. Then the bits that are
computed bottom-up (`holds_block`, `has_rp`) are recomputed along the
ancestor chain of every restyled root, since a child's change can change
them; the top-down ones (`anchor`, `link`, `transparent`, `lays_out_items`,
the list depths) are stable for an element whose ancestors are stable,
which is what "outside the restyle set" means. An element with animations
(`FStyled.motion`) is never copied: its values are the timeline's at this
layout's time, computed as today.

**Style equality is the second gate.** After pass 1, every restyled
element's new `flow_style_t` is compared with its previous one (the struct
by `memcmp`, the lists by content): equal means `steady`. A class toggled
that no rule reads, a `style` attribute set to what it already said, a
`:hover`-shaped rule that changed only colours: steady. `subtree_steady`
is bubbled up, one more bit beside `holds_block`, and it is what pass 3
reads. (Today `:hover` never matches in libgarb, select.h's rule; the day
the between-arcs batch makes it match, the hovered chain enters as a change
through this same door, the old chain and the new one, and a hover that
changes only paint will be steady and cost the parts path, not a layout.)

## The layout memo: passes 2 and 3

**The rule.** A box B is REUSABLE for this layout when ALL of these hold:

1. `subtree_steady(B)`: every element in B's subtree, B included, is
   either outside the restyle set or came out style-equal, and none is
   animated.
2. B had a placed, finished box in the previous tree (`placed`, not
   `unfinished`; a `display: none` element has none and nothing to reuse).
3. B is a box pass 3 lays out as a unit from a given width: an in-flow
   block container, a flex or grid item, a float, an absolutely positioned
   box, an inline-block's content, or a whole table. NOT a table's row,
   row group, column or cell (the table algorithm sizes those together),
   and NOT the pieces of an inline (an inline's lines belong to its
   container's inline formatting context, which is laid out whole).
4. The width B is given equals the width it was given last time, and, for
   a flex or grid item whose container sets its height, the height too.
5. Floats cannot cross B's edge in either direction: B is a block
   formatting context root, or `holds_float(B)` is false; and no float
   from outside intrudes into B's old vertical range at B's x and width
   (the exclusion space is asked, `band` in layout.c, for the range
   [y, y + old h) before B is placed).
6. Nothing in B's subtree reads its containing block's height unless that
   height is the same as last time: a pass 1 bit `pct_height_in` (any
   percentage height, min/max-height, or vertical inset in the subtree)
   bubbled up like `holds_block`; when set, B is reusable only under a
   containing block whose height is definite and equal to the old.
7. The environment is the old tree's: same width, zoom, cascade
   generation, fonts (a web font that arrived or a face that moved is
   "everything changed", as `s_web.moved` already says), same `medium`,
   same `static_only` and `scripting`.

When B is reusable, pass 2 builds B's own `FBox` and no children (it marks
the box `reused`), and pass 3, on reaching it, GRAFTS: it deep-copies the
old subtree (`FBox`, `FItem`, `FLine`, `FFrag`, `FSpan`, `FInline`) into
the new arenas, translated by (B.new − B.old) in x and y, retains every
run the fragments hold (the text engine gains `os64_text_run_retain`, or
the graft copies the run's bytes and glyphs; the builder measures which
and says), re-files the subtree's containing-block entries (`FPos`) under
the new tree's entries, and takes from the old box what the parent's
algorithm reads next: the margin summary the parent collapses with, the
baselines (`has_baseline`, `last_baseline`'s answer), the intrinsic
widths (`intrinsic_min/max`, `content_min/max`, `content_h`). The parent
goes on as if B had been laid out, and the dump cannot tell.

**Intrinsic sizes are reused on their own.** A flex container, a grid, a
table cell and a shrink-to-fit box ask a child for its min-content and
max-content widths before laying it out, and that measurement IS the cost
on a modern page (every item of every flex row). A `subtree_steady` box
answers those from its old box without measuring, whatever its width
turns out to be this time: intrinsic widths do not depend on the
containing block. So a flex row whose one item changed measures one item,
and the rest graft if their final widths match or lay out fresh if not.

**Where the memo lives.** Pass 3's state (`L`) gets the previous tree and
a map from node to its old `FBox` (built once per layout from the old
tree, linear in its boxes). The old tree is `flow_env_t.previous`; it is
the tree yonder shows, alive until `page_show` frees it, which is after
`flow_layout` returns. `flow_layout_parts` (YONDER_PERF.md § perf-4b)
keeps working unchanged: the new tree is a whole tree.

**What is NOT memoised, on purpose.** The public tree, the stacking
contexts, the layers, the overflow and ink rects: rebuilt whole from the
new boxes. They are linear walks with no measuring, and the instrument
will say what they cost before anything is built for them. Tables'
interiors. Any layout at a new width, zoom or font set, which is every
resize. The first layout of a page (no previous tree: the memo costs one
NULL check).

## The oracle, the bound, and the proof

- **Dump-identical.** `tools/test_libflow_relayout_host.sh`: for each
  fixture of (page, a list of mutations as libhtml verbs), lay out whole,
  then for each mutation apply it, lay out incrementally from the previous
  tree AND whole from nothing, and `flow_dump` both: byte-identical, or
  the harness names the first differing line. The existing dump corpus
  runs with `previous == NULL` and must not change by a byte.
- **The bound.** The layout counts what it measured (`measures`, already
  kept) and what it reused (`reused_boxes`, new). For a mutation inside
  one block of a page of N steady blocks, `measures` is at most the
  measures of a whole layout of the changed block's formatting context
  plus its ancestors' own lines; the harness holds that per fixture, as
  LAYOUT.md's harness holds its linear bounds.
- **Each clause of the rule has a fixture that breaks it**: a float
  intruding into a steady block whose sibling grew; a steady block with a
  percentage height under a container that got shorter; a steady flex
  item whose container's width changed; a table cell; an inline's pieces;
  a subtree with an animation; a removed node whose address is reused by
  an inserted one (the cascade's stale slot). These are the mutants'
  targets: skipping any clause must fail a fixture.
- **The scripted-page host** proves the chain end to end: a page's script
  appends to a list and the record's layout line shows the pass times and
  `reused`; a geometry read inside a script after a one-line change is
  charged a layout whose `measures` is small.
- **The allocation sweep**, as every libflow slice: every allocation the
  graft makes failed in turn, and a failed graft lays the box out fresh or
  reports `incomplete` exactly as a failed layout does today; never a
  half-grafted subtree.

## The instrument, and what it decides

The first cut adds to the page file's `layout:` line the time of each
step and its counts: `model <us> cascade <us> style <us> boxes <us>
layout <us> public <us>; elements N, boxes N, measures N, reused N`.
From Chris's P5 on danlegt, Wikipedia and fetch.spec.whatwg.org, with a
script that touches one element, the numbers say which steps dominate
after the memo lands. Three rows are booked with those triggers:

| Booked | Trigger |
|---|---|
| libpage's model rebuilt from the change log (links, forms, images by dirty subtree) | `model` over a tenth of the chain on a real page |
| the public tree, stacking and ink rebuilt for changed subtrees only | `public` over a tenth of the chain |
| the cascade's rule hash kept across a sheet added by script | a page that adds `<style>` elements per turn (SPA frameworks that inject CSS) |

## `requestAnimationFrame`, and the observer

**rAF rides the rendering step.** libdom keeps per page an ordered list of
callbacks with ids; `requestAnimationFrame(cb)` appends, `cancelAnimation
Frame(id)` removes. While the list is non-empty, the page asks yonder's
ticker for a frame at the frame cadence: the earliest of the animations'
next frame (`flow_motion_due`) and the rAF cadence, which on a 10 ms tick
is 20 ms (50 frames a second; 60 is not a thing a 100 Hz tick can do, and
the as-built says so). At the frame, before the rendering step's layout:
the timestamp is taken once (`performance.now()`'s clock, slice 1a), every
callback pending at that moment runs in order with that timestamp, each
followed by its microtask checkpoint, exceptions reported as a task's are;
callbacks registered during the run wait for the next frame; then the
page's rendering step runs ONCE, with the memo above, which is why D13
comes first: rAF on a whole-layout page is a whole layout per frame
forever. A covered window naps the frame clock as the animations' does
(DEBTS § Occlusion); a page whose frame costs more than the cadence is
paced by its cost (the perf record's rule for animations), never queued
behind itself.

**MutationObserver reads the log.** `new MutationObserver(cb)`,
`observe(target, {childList, attributes, characterData, subtree,
attributeOldValue, characterDataOldValue, attributeFilter})`,
`disconnect()`, `takeRecords()`; `MutationRecord` with `type`, `target`,
`addedNodes`, `removedNodes`, `previousSibling`, `nextSibling`,
`attributeName`, `attributeNamespace` (null), `oldValue`. The binding
registers a reader at runtime creation; at each microtask checkpoint it
reads the entries since its last read, builds records for the observers
whose options match (the standard's "queue a mutation record" filter:
target or ancestor with `subtree`, the kind, the filter), and delivers to
each observer with a non-empty queue, in registration order, as the
standard's "notify mutation observers" microtask does. Old values: the
first `observe` asking for either old-value option sets `keep_old` on the
document, and it stays on (turning it off is not worth its edge cases).
Removed nodes in a record are wrappers over held nodes, which the binding
already does for detached nodes. The parser's insertions while a page
streams are entries like any other, so an observer on `document` sees the
page arrive, as it does in a browser.

## The cut, in order

| Cut | Builds | Oracle | Owner |
|---|---|---|---|
| 5a | the change log and its readers in libhtml; `MutationObserver` in libdom; the instrument line in the page file | the libhtml host: every verb logs the record the standard names, the parser logs its insertions, eviction and -1, holds released; WPT `dom/nodes/MutationObserver-*` | Opus |
| 5b | `garb_cascade_update`; pass 1's copy and the steady bits | `garb_cascade_dump` identical after update and after a fresh cascade, for every fixture mutation; the libflow dumps identical with reuse of pass 1 alone | Opus; Fable reads the restyle rule |
| 5c | the layout memo: the eligibility rule, the graft, the intrinsic memo | the relayout host above: dumps, the bound, the clause fixtures, the sweep | Opus; Fable reads the rule and the graft before merge |
| 5d | `requestAnimationFrame` on the loop | WPT `html/webappapis/animation-frames/`; the scripted-page host's frame cases with the fake clock | Opus, after 5c |

5a and 5d touch no layout and may be built by one builder in sequence or
two at once; 5b and 5c are one lineage and follow each other. Every cut
is a PR to `userland` with its as-built section here, in the shape of
DOM.md's.

## Acceptance

- The scripted-page host: appending one item to a 2,000-item list
  measures the new item's lines and reuses the rest (`reused` near
  2,000, `measures` under ten).
- The P5, danlegt: a geometry read's `elapsed_us` after a one-element
  change, before and after; the record's layouts a second with rAF
  driving a page that moves one element, unchanged from the perf record's
  animation numbers.
- The P5, Wikipedia: the first layout no slower than 600 ms (the memo is
  one NULL check when there is no previous tree), and a script that
  changes one paragraph laid out in a fraction of it (the instrument's
  line is the number).
- WPT: `dom/nodes/MutationObserver-*` and
  `html/webappapis/animation-frames/` counts before and after.

## What this does not do, and the risks named

- A document whose sheets use `:has()` restyles whole; the page file says
  `restyle: whole (:has)`. The memo still applies below pass 1 where
  styles come out equal, so the layout is still partial.
- Resize, zoom, a web font's arrival, a sheet added or removed, an
  appearance or motion setting change: whole, as today. Those are the
  inputs the memo compares, and they changed.
- Tables' interiors and inline pieces are never reused.
- Memory: the graft copies, so a page that changes one element each turn
  costs one tree's worth of arena per layout, as today, plus the old tree
  until it is shown, as today. The log's holds are bounded by the slower
  of two readers, both prompt. `max_log_bytes` is the cap and -1 the
  escape.
- A subtle one, written down so the builder writes the fixture: the
  cascade's slot of a REMOVED node must be dropped before the node's
  address can be reused by a created node. The log names removals; the
  update drops them; the harness inserts a node right after removing one
  and checks the new node's winners.
- `requestAnimationFrame` at 50 frames a second, not 60, by the tick.
- `MutationObserver` records for the control state (a value typed by a
  person) are not a thing in the standard either; nothing to do.
