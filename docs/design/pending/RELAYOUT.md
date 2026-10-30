# RELAYOUT.md — the change log, and a layout that reuses what did not move

*Slice 5 of [MODERN_WEB.md](MODERN_WEB.md): D13, incremental relayout,
then `requestAnimationFrame`. Original design by Fable, 2026-10-21;
5a implementation and reconciliation by Quinn, 2026-10-29.
5b implementation adds incremental cascade and computed-style reuse; hardware
acceptance and the restyle design review are pending. The sequence
with Canvas is recorded in [CANVAS.md](CANVAS.md). Fable reviews the
restyle rule and layout graft before those cuts merge.*

## What this settles

1. **A bounded native change log in libhtml.** Readers ask what changed
   since a document version. It retains node identities and attribute names
   for restyle and relayout; loss requests a whole rebuild. The synchronous
   mutation listener independently serves libdom's exact mutation-time
   consumers through its shared dispatcher.
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
4. **The cascade is updated in place** for the restyle set, with TWO
   counters: a SHEET generation that moves when the sheets or the
   environment change (then every subtree is stale), and an UPDATE
   counter that moves at every element update and invalidates nothing by
   itself (the restyle set and style equality do that). yonder's "same
   cascade?" checks read the first.
5. **The public tree, the stacking order, the ink reach and libpage's model
   stay whole**, and the instrument that lands first decides, with numbers
   from the P5, whether any of them follows.
6. **`requestAnimationFrame` rides the loop's rendering step** at a frame
   clock that exists only while a callback is pending. The custom-elements
   design owns script mutation notification and its checkpoint integration.

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

## The change log (libhtml, implemented in 5a)

The public contract lives in `html.h`, **BOUNDED CHANGE LOG**. The log and
synchronous listener are produced at the mutation sites. The log records
version-changing child, attribute, text and custom-element state changes,
including parser mutations. No-op setters still notify the listener but do
not create invalidations. Form ownership and control state retain their
existing separate versioning.

A native reader registers with `os64_html_reader_new(doc, version)` and
calls `os64_html_changes(reader, &entries)`. The result is a count and a
borrowed linked chain of `os64_html_change_entry_t`, or **-1** if history
was lost. Both success and -1 advance the cursor to the current document
version. A reader receiving -1 rebuilds from the current document. A future
version refuses registration. Free readers before their document.

The chain lasts until the next mutation, reader operation or limit change
on the document. Its child lists are immutable arrays of identities:
walking a removed node's current `next` link cannot recover the original
list after that node moves again. Each entry copies attribute names and
holds its target, siblings and added/removed nodes. Those holds keep node
identity alive, not historical contents. Consumers needing nodes after a
read boundary take their own holds. Reader operations must happen outside
mutation callbacks and parser calls.

The chain avoids a second allocation to flatten records into an array at
read time. Internal holds are released without collecting during a mutation;
collection happens at stable verb/reader boundaries. Entries passed by all
readers are pruned at the next boundary. With no readers, no log records or
holds are created. The cascade uses readers in 5b; 5a exposes and tests
the native contract without retaining an unused log for browsing.

The default record budget is one sixteenth of the document arena budget.
`os64_html_changes_limit(doc, bytes)` overrides it, including zero for no
retention. `os64_html_changes_bytes` reports charged record bytes, including
allocator headers, identity arrays and copied names. Reader allocations
and the nodes kept alive by records also consume the document arena budget;
the record cap is not a bound on retained subtree size. Eviction or record
allocation failure invalidates lagging readers. A log allocation failure
must not refuse an already-applied mutation or suppress its synchronous
notification. One parser feed or native verb can create several records at
the same version; dropping any of them makes that version incomplete for
readers that have not reached it.

The synchronous listener retains its own `os64_html_change_t` contract,
with borrowed old values valid during the callback. The native log does
not retain old values. libdom owns the document's one listener and fans it
out to registered consumers; Canvas and custom elements share it. Script
mutation notification remains in [CUSTOM_ELEMENTS.md](CUSTOM_ELEMENTS.md),
not in this bounded, lossy history.

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
document. `:dir()` also takes the whole-cascade fallback: resolving an
ancestor's `dir=auto` can depend on changed text below the immediate parent.
Dependency discovery walks nested selector lists, including `:is()`, `:not()`,
`:where()` and the selector filter in `:nth-child()`.

**The cascade is updated in place.**

```c
bool garb_cascade_matches(const garb_cascade_t *, const garb_sheet_in_t *,
                          int32_t count, garb_env_t);
bool garb_cascade_update(garb_cascade_t *);
uint64_t garb_cascade_generation(const garb_cascade_t *);
uint64_t garb_cascade_updates(const garb_cascade_t *);
uint64_t garb_cascade_version(const garb_cascade_t *);
bool garb_style_updated_since(const garb_cascade_t *, const os64_html_node_t *,
                              uint64_t update);
```

The cascade owns an optional native reader. Updating consumes its linked
change records, rebuilds the node slot table from the current document,
and recomputes affected winners in parent-first order. Removed nodes have
no slot in the new table. New elements start dirty. Body hints invalidate
the document because they can colour links beyond the body's subtree.
The generation identifies a full build; successful updates advance a
separate counter. Each computed-style tree records both, so a measured
layout can advance the shared cascade while the displayed tree still
knows which updates it has not incorporated.

The earlier claim that nothing points into the cascade was wrong:
animation keyframe records borrow declaration data. Updates preserve the
cascade's prior arena allocations while its users are alive. This first
cut renews the cascade after at most 64 updates; state allocations remain
bounded by `state_bytes`, and additional inline-attribute parse allocations
also have a cumulative `state_bytes` ceiling. Reaching a limit, losing
history, lacking a reader, or encountering broad selector dependencies
requests a fresh cascade. A failed update preserves old borrowed values
but requires replacement before further queries. The displayed tree keeps
its own document pin; successful updates rotate the cascade's pin.

Yonder shares immutable stylesheet parses through counted owners. A staged
or measured sheet table retains matching linked parses and inline parses
whose complete text is equal. Media conditions, import metadata, sheet
identity/order and the complete cascade environment must also match.
Changed inline styles therefore affect synchronous geometry reads before
the page is published. A live page's explicit stylesheet invalidation still
forces a full cascade. Cascades themselves have counted owners; updates
and queries remain serialized on the window thread.

**Pass 1 reuses computed styles.** The additive `flow_layout_restyled`
entry takes a previous tree without changing `flow_env_t` or
`flow_profile_t`. Ordinary layout calls still compute fresh styles. It
copies unaffected scalar styles and deep-copies their lists into the new
arena, memoizing shared slices. The new tree can outlive the old one.
List-budget refusal falls back to a whole pass; allocation failure remains
an explicit layout failure. Full quirks mode uses the whole style pass
because its content-sensitive UA rules need broader invalidation.

The root and body are computed fresh to establish root-font and body-link
context. Derived tree/model facts (link indices, list depths, decoration
ancestry, `holds_block`, `has_rp`) are recomputed rather than copied. Link
indices can change after an insertion elsewhere even when the link's CSS
does not. Styles with animation names or active motion are computed fresh;
unchanged nonanimated styles may be reused across timeline creation.

**Style equality is the second gate.** Scalar fields and list contents are
compared independently of storage addresses. Equal, nonanimated values set
`steady`; `subtree_steady` is propagated bottom-up. A class toggle that no
rule reads can be steady. A colour change is not whole-style equality:
the previous design incorrectly called that steady. A future paint-only
path must prove its own narrower equivalence. These bits do not prove that
text or child lists are unchanged; 5c still needs its separate untouched
condition. Passes 2 and 3 remain whole builds in 5b.

## The layout memo: passes 2 and 3

**The rule.** A box B is REUSABLE for this layout when ALL of these hold:

1. B's subtree is UNTOUCHED and STEADY, two bits bubbled up separately
   (Quinn's finding 7: style equality says nothing about text). Untouched:
   no entry of the log names a node in B's subtree, B included, as a
   target or an added or removed node — no child list, attribute or text
   changed anywhere under B, so every run's text and every item sequence
   is what it was. Steady: every element in the subtree is either outside
   the restyle set or came out style-equal, and none is animated. A text
   change under B with every style equal is untouched-false and B lays
   out fresh; a class toggle no rule reads is untouched-false too (the
   attribute changed) unless the log entry's target is OUTSIDE B and only
   the restyle reached in, in which case equality decides.
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
7. The environment is the old tree's: same width, zoom, same cascade
   SHEET generation (below: the one that moves when sheets or the
   environment change, NOT the one that moves at every element update),
   fonts (a web font that arrived or a face that moved is "everything
   changed", as `s_web.moved` already says), same `medium`, same
   `static_only` and `scripting`.

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

## `requestAnimationFrame`

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

## The cut, in order

| Cut | Builds | Oracle |
|---|---|---|
| 5a | Bounded native change log and readers; opt-in phase profiling; repeatable measurement page | Native mutations, parser entries, immutable lists, eviction/OOM and lifetime checks; fake clocks; page completion/cancel/resize tests; P5 baseline |
| 5b | `garb_cascade_update`; pass 1 copying and steady style inputs | Updated and rebuilt cascade dumps agree; libflow dumps agree with pass 1 reuse alone; Fable reviews the restyle rule |
| 5c | Layout eligibility, graft and intrinsic memo | Whole/incremental dumps, bounds, clause fixtures and allocation sweeps; Fable reviews the rule and graft |
| 5d | `requestAnimationFrame` on the loop, after 5c | WPT animation-frame tests and scripted-page fake-clock cases |

With one builder the cuts run in order. The Canvas integration branch
carries this sequence before C6; PRs state their actual base. Hardware
baseline measurements precede optimization claims.

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
- WPT: `html/webappapis/animation-frames/` counts before and after 5d.

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
  until it is shown, as today. Log records are capped by
  `os64_html_changes_limit`; retained nodes consume the document arena
  budget as well. A lagging reader receives -1 and rebuilds.
- A subtle one, written down so the builder writes the fixture: the
  cascade's slot of a REMOVED node must be dropped before the node's
  address can be reused by a created node. The log names removals; the
  update drops them; the harness inserts a node right after removing one
  and checks the new node's winners.
- `requestAnimationFrame` at 50 frames a second, not 60, by the tick.
- `MutationObserver` records for the control state (a value typed by a
  person) are not a thing in the standard either; nothing to do.

## 5a implemented profile

Run `yonder --layout-profile /tests/pages/layout-measure.html` with Modern
Web and scripts enabled. Profiling is off by default; `--script-audit` may
be combined with it. `flow_layout_profiled` takes an optional result sink
and monotonic microsecond clock without changing `flow_env_t`. The ordinary
entry reads no profiling clock. The Os64 caller uses `os64_micros`.
Negative or backward readings contribute zero time and increment a clock
error count; they are not valid timing evidence.

The page diagnostic's `layout:` fact accumulates model rebuild, cascade,
style, box construction, line layout and public-tree times. It also counts
elements, boxes, measures, reused work (zero for whole rebuilds), model
rebuild calls, layout attempts, refusals/partial results and clock errors.
Counts come from stages returning results; a failed stage cannot supply
its partial count. Times include attempted stages. Public time includes
stacking, ink reach and ticking construction. Initial loader model creation,
paint, form-widget rebuilding, input handling and cleanup are outside these
phase totals. Model rebuilds forced by geometry queries are included.

Totals belong to the page diagnostic, not an individual JavaScript sample.
They include setup, warm-ups, presentation layouts and retries. Reload for
a fresh comparable page record; do not divide the phase totals by the
sample count and call that a per-operation measurement. Press `d` with page
content focused to save the existing corpus report. Diagnostics must be
attached to the page for costs to be recorded.

The measurement page separately times a geometry read after no change,
a paragraph text replacement, and removal/insertion of one list item. It
warms each workload five times and records raw `performance.now()` samples,
median and maximum. The default is 2,000 rows and 20 measured samples per
workload; 200/3 is a correctness smoke check. Timer waits between samples
are excluded. It rejects viewport or zoom changes, exposes cancellation,
and labels incomplete/failed runs. Its synthetic rows establish a repeatable
baseline; they do not substitute for the real-page acceptance above.

Validation and remaining hardware evidence are recorded in
[5a-validation.md](../../relayout-evidence/5a-validation.md).

## 5b implemented profile

`styles reused` counts copied computed styles; `cascade updates` counts
successful DOM-version advances within an existing cascade. The existing
`reused` field continues to count layout reuse and remains zero until 5c.
Measured and displayed layouts both use the incremental door. Validation
and the outstanding P5/design gates are recorded in
[5b-validation.md](../../relayout-evidence/5b-validation.md).
