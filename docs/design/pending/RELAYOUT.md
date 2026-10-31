# RELAYOUT.md — the change log, and a layout that reuses what did not move

*Slice 5 of [MODERN_WEB.md](MODERN_WEB.md): D13, incremental relayout,
then `requestAnimationFrame`. Original design by Fable, 2026-10-21;
5a implementation and reconciliation by Quinn, 2026-10-29.
5b adds incremental cascade and computed-style reuse and merged in #424 after
P5 acceptance. Fable reviewed the restyle rule. The sequence with Canvas is
recorded in [CANVAS.md](CANVAS.md). The implemented 5c contract is
in [RELAYOUT_GRAFT.md](RELAYOUT_GRAFT.md). Fable approved the revised
contract on 2026-10-31. Its four implementation steps are in the integration
branch; [P5 acceptance](../../relayout-evidence/5c-p5-acceptance.md) passed
on 2026-10-31 after the fallback and text-allocation follow-ups.*

## What this settles

1. **A bounded native change log in libhtml.** Readers ask what changed
   since a document version. It retains node identities and attribute names
   for restyle and relayout; loss requests a whole rebuild. The synchronous
   mutation listener independently serves libdom's exact mutation-time
   consumers through its shared dispatcher.
2. **The layout stays DERIVED. The incremental entry takes the
   previous tree, and its output must equal a whole rebuild.** The
   5c contract preserves fresh box construction while copying eligible derived
   geometry. It keeps LAYOUT.md ruling 2's ownership principle: no author state
   lives in the layout tree, and a whole rebuild can recreate its answers.
   The harness compares whole and incremental results after each mutation;
   a difference is a bug in reuse.
3. **Reuse is by node identity, style equality and equal inputs, grafted by
   COPY.** A subtree is reused when its nodes did not change, its computed
   styles came out equal, and the layout hands it the same inputs it had
   last time. What is reused is copied from the old tree into the new one's
   arenas, runs retained, never pointed at: no tree holds another tree's
   arena, so there is no chain of arenas to compact and the old tree is
   freed when the new one is shown, exactly as today.
4. **The cascade is updated in place** for the restyle set, with TWO
   counters: a SHEET generation that moves when the sheets or the
   environment change or the cascade is otherwise rebuilt, and an UPDATE
   counter that moves on each successful update and invalidates nothing by
   itself (the restyle set and style equality do that). yonder's "same
   cascade?" checks read the first.
5. **The public tree, the stacking order, the ink reach and libpage's model
   stay whole**, and the instrument that lands first decides, with numbers
   from the P5, whether any of them follows.
6. **`requestAnimationFrame` rides the loop's rendering step** at a frame
   clock that exists only while a callback is pending. The custom-elements
   design owns script mutation notification and its checkpoint integration.

## The full-rebuild path and its cost

When a script changes the tree, the next turn's rendering step
(`script_rebuild`, yonder.c) follows this pipeline. 5b can reuse cascade winners
and computed styles; the other stages remain whole builds:

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
(YONDER_PERF.md). The [5a P5 baseline](../../relayout-evidence/5a-validation.md)
measures about 80% in layout and 14% in cascade on the synthetic mutation
workloads. That split directs 5c toward layout; it is not a claim about every
page or workload.

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

**The restyle set depends on the kind of mutation.** Parsed selectors record
following-sibling combinators, front/back positional pseudo-classes,
`:empty`, filtered `:nth-child(... of S)` / `:nth-last-child(... of S)`,
and whole-tree dependencies. Discovery recurses through functional selector
lists, including `:is()`, `:not()` and `:where()`.

Attribute/state changes restyle the target's subtree; text changes use the
text's parent. Following-sibling combinators also mark following siblings'
subtrees; filtered positional selectors mark all siblings' subtrees.
Unfiltered positional selectors alone do not widen a text/attribute change.

Child-list changes mark added subtrees. Front-position selectors and
following-sibling combinators mark the following suffix of children for a
pure insertion batch. A batch containing a removal under that parent marks
all its children: a saved previous-sibling identity can have moved within
the same parent, so its current links are not a historical boundary.
Back-position and filtered positional selectors also mark all children.
`:empty` marks the parent and its descendants, with following/filtered
propagation, since its changed winners can affect inheritance and matches
outside the parent. The UA first-summary/first-legend behavior likewise
marks a changed `details`/`fieldset` subtree.

`:has()` and `:dir()` request a full cascade: their ancestor matches can
depend on descendants beyond these local sets. Changes to the body's
`link`, `vlink`, `alink`, `text` or `bgcolor` attributes also request a full
cascade. Appending children to `body` does not trigger that hint rule.

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

The cascade owns an optional native reader. Incomplete cascades and cascades
with whole-tree selector dependencies do not register one: they must rebuild
after mutation, so retaining mutation history cannot help them. Other native
readers retain their independent subscriptions. Updating consumes its linked
change records and keeps its node slot table, erasing removed subtrees and
inserting connected added subtrees before marking the affected set. The
cascade's pin predates removals and prevents address recycling while a slot
can still name that node. The table grows when needed; deletion closes hash
clusters instead of leaving tombstones. A reusable dirty-node vector supplies
minimal roots for parent-first winner computation. Sibling suffix marks avoid
repeated walks for parser batches; removal stamps avoid rescanning the log
per parent. Once marking exceeds half the current element count, the update
requests a full cascade before computing winners. Sparse updates do not
count, copy or traverse the whole document's slots.
The generation identifies a full build; successful updates advance a
separate counter. Each computed-style tree records both, so a measured
layout can advance the shared cascade while the displayed tree still
knows which updates it has not incorporated.

The earlier claim that nothing points into the cascade was wrong:
animation keyframe records borrow declaration data. Updates preserve the
cascade's prior arena allocations while its users are alive. Before another
update, the cascade renews when additional state payload plus retained inline
attribute parse storage reaches the initial state payload (at least 4 KiB).
An update can cross this soft threshold; state allocations remain hard-bounded
by `state_bytes`, and additional inline-attribute parses also have a cumulative
`state_bytes` ceiling. The 64-update limit remains an additional renewal guard.
This ties retention to allocation growth: many small edits can reuse a cascade,
while repeated broad restyles cannot accumulate dozens of states. Reaching a limit, losing
history, lacking a reader, or encountering broad selector dependencies
requests a fresh cascade. A failed update preserves old borrowed values
but requires replacement before further queries. A frame layout that cannot
obtain a complete replacement can be refused; the shown tree does not make
the failed cascade valid again. The displayed tree keeps
its own document pin; successful updates rotate the cascade's pin.

Yonder shares immutable stylesheet parses through counted owners. A staged
or measured sheet table retains matching linked parses and inline parses
whose complete text is equal. Media conditions, import metadata, sheet
identity/order and the complete cascade environment must also match.
Changed inline styles therefore affect synchronous geometry reads before
the page is published. A live page's explicit stylesheet invalidation still
forces a full cascade. Cascades themselves have counted owners; updates
and queries remain serialized on the window thread. When a measured layout
needs a full cascade, it releases the old hidden measurement before building
the replacement, including when changed sheets prevented selecting that
measurement as a reuse source. Its new sheet table has already retained the
parses it needs. The displayed page keeps its own tree and cascade owners;
a failed measurement leaves no stale geometry answer.

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

## The layout memo: implemented 5c contract

[RELAYOUT_GRAFT.md](RELAYOUT_GRAFT.md) specifies the copy, invalidation,
resource, cursor and ownership rules approved for implementation. It refines the
original sketch in several material ways:

- Pass 2 builds fresh boxes and inline items. Pass 3 copies eligible geometry
  into them, remapping references and retaining immutable text runs. The first
  geometry scope is ordinary block subtrees; flex/grid placement, tables,
  floats and positioned subtrees use their existing algorithms.
- Untouched history belongs to the old layout snapshot, independently of the
  cascade reader. Cumulative dirty bits survive repeated measurements from
  an older displayed tree. Missing history disables that source.
- Fresh structural/content equality and external font, replaced-size and form
  state snapshots supplement style equality. Markers, processed text and model
  indices cannot be inferred from unchanged CSS.
- Width equality alone is insufficient: the saved call signature includes
  containing-height inputs and pending margins. Cursor effects and relative
  coordinates must replay correctly after earlier content moves.
- Intrinsic widths have their own memo, guarded by untouched content as well
  as equal styles/resources. Old placed content height is not an intrinsic
  width result.
- The public tree, stacking, clipping and ink remain freshly derived. The
  old tree can be freed before the new result is inspected or painted.

The additive entry leaves the existing layout/environment/profile ABIs
unchanged. An opt-in first layout does incur bounded snapshot bookkeeping;
its cost must be measured. The detailed contract defines the allocation
failure behavior, run-retention ownership, public-observation oracle,
adversarial fixtures and the P5 performance gates. Fable accepted the scope,
amended float qualification and synthetic positive-reuse fixture; the
unchanged captured article remains a no-regression control.

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
| 5b | `garb_cascade_update`; pass 1 copying and steady style inputs | Updated and rebuilt cascade dumps agree; libflow dumps agree with pass 1 reuse alone; Restyle design reviewed; controlled and alternating manual P5 comparisons cleared the performance gate; reviewed and merged in #424, with earlier timing outliers disclosed |
| 5c | Layout eligibility, graft and intrinsic memo | Whole/incremental dumps, bounds, clause fixtures and allocation sweeps; Implemented in integration; contract in RELAYOUT_GRAFT.md; P5 acceptance passed after the final three-pair comparison (5c-p5-acceptance.md) |
| 5d | `requestAnimationFrame` on the loop, after 5c | WPT animation-frame tests and scripted-page fake-clock cases |

With one builder the cuts run in order. The Canvas integration branch
carries this sequence before C6; PRs state their actual base. Hardware
baseline measurements precede optimization claims.

## Booked selector invalidation follow-up

Fable proposes a separate slice to avoid full cascade replacement after every
mutation on sheets with `:has()` or `:dir()`. These dependencies are currently
classified as `whole_tree`; replacement also defeats the generation gate for
style and geometry reuse. This is a limitation of the mutation optimization,
not a reason to weaken 5c's correctness or lifetime guards.

The proposed approach recomputes affected ancestor/sibling matches separately
and expands invalidation into descendants when changed winners require it.
Its detailed dependency, inheritance and failure rules need their own design
and oracle. Fable has offered to design it; the implementation is not assigned
by that offer. The real captured article is its intended positive-reuse gate.

**Before scheduling that separate slice**, perform a host-side corpus census:
count captured pages whose available CSS contains `:has(` or `:dir(`. Report
the denominator and missing stylesheet coverage, and distinguish repeated
captures from distinct pages/sites so one heavily captured site cannot silently
determine the priority. A text-occurrence census estimates prevalence; it does
not establish that a selector was parsed, active or expensive on each page.

If these dependencies dominate the captured workloads, Fable recommends
placing that slice before 5d, because more pages would then benefit from
mutation-driven reuse. The census has not been run and no reordering has been
decided. It does not block the approved 5c implementation. 5d still follows
5c; inserting additional work ahead of it is a separate scheduling decision.

## Acceptance

- The scripted-page host: appending one item to a 2,000-item list
  measures the new item's lines and reuses the rest (`reused` near
  2,000, `measures` under ten).
- The P5, danlegt: a geometry read's `elapsed_us` after a one-element
  change, before and after; the record's layouts a second with rAF
  driving a page that moves one element, unchanged from the perf record's
  animation numbers.
- The P5, Wikipedia: compare first-layout time to the matched control, including
  opt-in snapshot overhead (the historical 600 ms is not a portable threshold).
  A script changing one paragraph should perform fewer measurements; the
  profile and matched timings establish the improvement.
- WPT: `html/webappapis/animation-frames/` counts before and after 5d.

## What this does not do, and the risks named

- A document whose sheets use `:has()` restyles whole. The implemented
  graft keeps the cascade-generation gate, so that full cascade replacement
  also disables geometry reuse. The stream profiler records its full work.
- Resize, zoom, a web font's arrival, a sheet added or removed, an
  appearance or motion setting change: whole, as today. Those are the
  inputs the memo compares, and they changed.
- The geometry graft excludes table, float, flex/grid-item and
  positioned placement, and inline pieces that escape their candidate root.
  The detailed contract states the intrinsic-width scope separately.
- Memory: the graft copies, so a page that changes one element each turn
  costs one tree's worth of arena per layout, as today, plus the old tree
  until it is shown, as today, plus bounded reuse metadata and shared run
  references. Log records are capped by
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
attached to the page or active stream for costs to be recorded.

Geometry queries made by scripts during parsing accumulate a separate
`layout stream:` fact in the stream diagnostic. The measurement borrows that
record; it does not take ownership. The fact survives transfer to the displayed
page, whose subsequent layouts use `layout:`. Receiving a parser chunk alone
does not trigger a layout. These counters measure script-forced geometry, not
progressive painting after each chunk. The cascade phase includes releasing
an unusable hidden measurement on the full-cascade fallback; phase boundaries
therefore differ from builds that kept that measurement until after layout.

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
`reused` field counts copied layout boxes; 5c populates it independently of
style reuse. Intrinsic hits, marker measurements, peak new metadata and the
refusal histogram are reported on the separate `layout graft` line.
Measured and displayed layouts both use the incremental door. Validation
and the history of the P5/design gates are recorded in
[5b-validation.md](../../relayout-evidence/5b-validation.md).

The matched control/candidate procedure, captured-article workload and stream
profiling evidence are in [5b-comparison.md](../../relayout-evidence/5b-comparison.md).
