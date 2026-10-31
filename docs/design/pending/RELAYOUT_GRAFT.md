# Relayout 5c: copying layout results into a fresh box tree

*Implementation contract based on `canvas-integration` at `4184f91f`, after
5b merged in #424. Fable approved the architecture with amendments on
2026-10-30 and accepted the revised float bound and workload gate on
2026-10-31. All four implementation steps are approved to build. This
refines [RELAYOUT.md](RELAYOUT.md).*

## Implementation checkpoint

Steps 1 through 4 are implemented: checked text-run retention, independent
snapshot history, bounded structural correspondence, refusal diagnostics,
ordinary-block geometry copying and an intrinsic-width memo. The explicit
`flow_layout_incremental` entry enables reuse; ordinary entries do not register
snapshot readers. Yonder opts in for displayed and hidden geometry; hardware
acceptance remains pending. Validation is recorded in
[5c-foundation.md](../../relayout-evidence/5c-foundation.md) and
[5c-geometry.md](../../relayout-evidence/5c-geometry.md) and
[5c-browser.md](../../relayout-evidence/5c-browser.md).

## Decision and scope

Keep pass 2's fresh box/item construction. Reuse pass 3's completed results
when the new structure, content, styles and layout inputs prove equivalent.
Copy lines, fragments and spans into the new layout arena, bind them to the
new boxes/items/styles, and retain immutable text runs. The old tree may be
freed immediately after the call. No new tree borrows an old tree's arena. Correspondence maps that contain old
addresses are call-local and freed before return. The new tree's persistent
snapshot records its own structure, not a link back to its source.

This changes the earlier proposal to suppress children in pass 2. That proposal
would need a second, deferred box builder when pass 3 discovers a different
width, float exclusion or margin input. Fresh pass 2 also supplies current
processed text, generated quotation marks, list markers, form text and model
indices. These are useful correctness evidence, not just storage to copy.
The 5a/5b timings put the large opportunity in layout; pass 2 elimination is
not a prerequisite for avoiding repeated shaping and line breaking.

The proposed 5c scope is deliberately narrower than the original list of
reusable placement units:

| Work | Reuse in this cut |
|---|---|
| Ordinary in-flow block subtree, including a whole inline formatting context | Graft when the contract below passes |
| Intrinsic min/max widths requested by flex, grid or shrink-to-fit | Memoize eligible completed results independently of final placement |
| Flex/grid placement and stretch/refit | Execute the existing algorithm; no geometry graft across its finalization boundary |
| Tables, floats, absolute/fixed positioning and positioned inline pieces | Existing layout; no geometry graft of a subtree containing them |
| Box construction, public boxes, stacking, clipping, ink and overflow | Rebuild from current inputs |

These exclusions are local: a disqualified ancestor can still visit eligible
ordinary block descendants. They are not promises that these layout classes
can never be reused. Expanding the graft to them needs its own inputs, side
effects and oracle cases. In particular, `sized_later`, `refit` and
`positioned_done` mean an old final flex/grid box is not the result that a
new parent's first sizing pass expects. Replaying that final state early is
not safe just because its width matches.

## Entry point and caller-owned inputs

Keep `flow_layout`, `flow_layout_profiled` and `flow_layout_restyled` contracts
unchanged. Add an explicit layout-reuse entry, with the same document/model,
width, environment, previous tree and optional profile/clock arguments as
`flow_layout_restyled`, plus a small `flow_reuse_inputs_t` descriptor. The
implementation shares the existing pipeline rather than duplicating it.
A caller using the old entries gets no geometry reuse or layout log reader.

The descriptor identifies a stable resource owner and monotonically changing
font and replaced-size revisions. It also snapshots the shared form-state
owner and version. These are values captured in the tree, not pointers to
mutable counters that are reread as the old answer. A new owner invalidates
the comparison even if its counters restart at the same number. Revision
wrap disables reuse until the owner identity is renewed.

A resource revision changes when an answer that layout can observe changes:
resolved font faces/metrics, intrinsic picture dimensions or availability,
SVG/replaced sizing, or control state. Yonder uses native DOM history for
SVG and canvas dimension attributes; these are not asynchronous resource
answers. Canvas pixel-only dirtiness changes neither a size revision nor
geometry history. Callback addresses alone
cannot certify these answers. Equal `env.ctx` addresses cannot either, and
requiring pointer equality would wrongly reject Yonder's temporary `Page`
copies. The stable owner/revisions certify the resolver contract across those
copies. Text context and resolver function identities must also match.

Yonder opts in through its shared layout wrapper, covering both the displayed
and hidden measurement trees. Resource revisions must advance at the existing
font/image-change producers, before a layout can consume their answers; the
consumer's reset of `s_web.moved` or `g.pictures_moved` must not reset revisions.
Those flags identify sites to audit, not a sufficient revision mechanism by
themselves. Form-state comparison uses `os64_page_shared_state` and
`os64_page_state_version`, captured on the actual layout, including synchronous
reads before the displayed page's `state_version` catches up.

Failure to establish any descriptor input disables geometry and intrinsic
reuse. Existing 5b style reuse remains independently available. Navigation,
font release, and context destruction retain their current ownership rules.
No frame loop or Canvas scheduling changes belong to this cut.

## Mutation history belongs to a snapshot

A tree created by the new entry captures the document version and owns an
optional independent libhtml reader registered at that version. It does not
consume the cascade's reader. The old tree's document pin prevents identity
recycling while its maps can still name a node.

A reader advances when consumed, whereas a displayed tree can be used more
than once while hidden geometry reads move ahead. Therefore the tree owns a
monotonic dirty set accumulated since its snapshot. Advancing the reader never
clears that set. If the shown tree is version A, reads at B and C both consult
changes since A, not merely the last read's delta. Each successful replacement
tree starts its own version/reader/dirty set. Reader loss or bookkeeping
allocation failure permanently disqualifies that snapshot as a reuse source.
Free its reader immediately on disqualification, outside any borrowed-chain
traversal, rather than waiting for tree destruction. The slowest live reader
can retain log entries. A displayed tree may be that reader while hidden
measurements advance; Yonder frees it when `page_show` replaces the old page.
That lifetime must not be extended by a reuse reference chain.

Process the complete borrowed change chain before another reader operation.
Do not retain pointers into that chain. Mark identities named as mutation
targets and added/removed roots. Preserve old ancestry and structural membership
in the snapshot index; current `parent`/`next` links cannot reconstruct the
place a removed or moved node previously occupied. Propagate dirty flags
bottom-up over that saved membership. New ancestry and order are checked by
the fresh structural comparison. Attributes changed back to their old value
still disqualify the touched subtree; whole-style equality is a separate test.

Use fixed records indexed by the old snapshot, with dirty bits that only turn
on. Do not grow a retained list of historical nodes on each update. The native
reader's bounded log remains authoritative; on loss, rebuild. Reader creation
failure leaves a useful complete tree that simply cannot supply a later graft.
Free the reader and bookkeeping before releasing the tree's document pin.
These optimization records may change behind an otherwise read-only previous
tree; calls remain serialized and must not reenter through layout callbacks.

The first opt-in layout has real snapshot/index overhead even without a
previous tree. The original "one NULL check" prediction does not describe
that path. Measure first-layout time and memory instead of assuming them free.

## Match fresh structure before trusting geometry

Build a bounded old-to-new correspondence once, after fresh pass 2. Node
identity alone is not a box identity: anonymous boxes, split inlines and atomic
content can have absent or repeated nodes. Match named roots by identity and
role, then compare their ordered box/item structures. Ambiguous matches miss;
there is no arbitrary "first old box" shortcut.

For each candidate, prove all of the following bottom-up:

- Its saved DOM membership is untouched; both old and new subtrees are complete.
- Computed styles are equal by value and no contributing element is animated.
  Retain 5b's cascade-generation gate. A cascade renewal can lose reuse even if
  a hypothetical fresh comparison might find equal styles; that is a safe miss.
- Box kinds, inline-item kinds, child order and node identities correspond.
  Generated flags, text offsets, processed text bytes, inside/outside marker
  bytes, and whitespace/split boundaries agree. Equal DOM text alone is not
  enough when inherited text processing or surrounding inline structure changes.
- Derived layout inputs agree, including decoration ancestry and relevant
  anonymous styles. Fresh link/control indices are rebound, not copied from
  the previous model. Inserting an earlier link may renumber later ones.
- External resource and shared control-state snapshots match. A typed value
  can change without an HTML mutation. Replaced content uses the resource
  contract even when its DOM and CSS have not changed.

Do not re-walk a subtree for every ancestor. Build correspondence and equality
summaries once and consume them while traversing layout. Hashes may accelerate
lookup; a collision is not proof of equal text, styles or structure.

Full quirks mode, an incomplete old tree, missing history, incompatible
resource ownership, or changed viewport/zoom/environment use fresh layout.
Environment comparison includes viewport width and height, medium/cascade
sheet generation, text context, font settings, appearance, `static_only`,
`scripting`, SVG replacement policy and animation/tick participation. Snapshot
values are immutable even when the source tree's public paint state ticks.

## Geometry eligibility and replay point

Use a shared fresh prefix in `block_at`: run `clearance` before the replay
point, and capture its resulting cursor as the signature input. A call that
settled pending floats or gained nonzero clearance takes the fresh path.
The qualified replay path therefore has no clearance effect to reproduce.
Do not place the graft ahead of that prefix. A candidate is an `FB_BLOCK` in
normal block flow, not a flex/grid item, table part, replaced block, float,
positioned/transformed box, or the viewport root. Its mapped descendants must
not contain excluded placement classes, split inline content spanning beyond
the root, or positioning entries/edges escaping the candidate. A plain inline
replaced item is eligible only with equal resource inputs and matching fresh
items; inline-block content takes fresh layout in this cut.

A formatting context having encountered a float does not by itself prohibit
reuse. Require no pending floats at entry or at the replay point, no floats
inside the candidate, and proof that both the source and proposed result's
float-sensitive layout positions lie below the placed floats' lowest bottom.
With no pending/descendant floats, skipped `settle` calls have no placements
to perform. If there are no placed floats, this qualification is trivial.

The root's proposed border top alone does not prove that condition. A child's
negative top margin can collapse through an ordinary block, or an internal
negative margin can bring later content back upwards. An executable probe on
the existing layout engine demonstrates this: an 800px page has a left float
300px wide and 100px tall, then a spacer, then a candidate with a paragraph
whose top margin is -80px. Reducing the spacer from 200px to 100px changes the
candidate's actual top from 120px to 20px. The simple proposed top is 100px,
at the float bottom, yet the paragraph must now wrap at 500px instead of
800px (three lines instead of two). It cannot copy the old line breaks.

Record a lower bound on float-sensitive y positions reached during the source
call, relative to its saved origin. Collect that bound at the exclusion and
clearance query sites while they are executed, even when no float is active,
and propagate it to enclosing eligible candidates in the same inherited
`Space`. Include upward excursions caused by collapsing and negative margins;
unknown bounds refuse reuse. A source which actually encountered an external
float's influence cannot certify a float-free replay. Independent inner BFCs
have their own exclusion space and must not be confused with the inherited one.

At replay, translate that lower bound into the current inherited Space's
unmoved coordinates, using `rel_y`, and require it to be at or below the
maximum of the two placed-float bottoms (numerically greater than or equal).
Do not compare moved document coordinates directly with `Space.bottom`.
The bound is conservative; it need not summarize the whole exclusion profile.
For a simple paragraph below an infobox it passes, while the negative-margin
counterexample fails. Add fixtures for relative-positioned ancestors as well:
the candidate can be ordinary while its containing Space has a nonzero offset.
Positioned descendants remain excluded by this cut's local scope.

There must be no outside list marker waiting on the candidate's first line.
Markers owned entirely inside the candidate can be copied after their freshly
generated text matches. An external marker obligation takes the normal path.
The graft cannot skip the `L.markers`/`marker_floor` side effects it owes to an
ancestor.

Record an explicit call-input signature and result at a successful eligible
layout boundary. The signature contains the containing width, incoming cursor's
positive/negative pending margins, whether its first-content position is set,
containing-height definiteness and value, sizing mode, and the environmental
and structural qualifications above. Compare resolved inputs, not merely the
old border width. Equal border widths can hide different percentage bases.
Sizing mode includes formatting-context role and the containing box's HTML
alignment value: `widths` reads that parent value through `html_align_shift`,
even when the child's own `text-align` overrides inheritance.
For this first cut require equal containing-height inputs even if no percentage
height is detected; dependency narrowing is an optimization, not a safety need.

Absolute x/y position may differ. Record geometry and cursor effects relative
to the call origin in 26.6 units. Replay with checked translation. Do not copy
an ancestor's old `Cursor.first`: preserve an already-set incoming first
position, or reproduce the relative position established by this call when
it was previously unset. Preserve both pending-margin components, not just
their sum: later collapsing depends on the positive and negative extrema.
Use one cursor-result record for both normal and collapse-through exits:
`delta_y`, outgoing positive/negative pending margins, and first-content
position either unchanged or set at a relative y. An empty block can leave
the incoming y unchanged while joining margins; never infer its cursor effect
from its border-box bottom.

Store the replay-point origin (`cbx`, the post-clearance cursor y) on each
candidate `FBox`, with a validity flag. Its saved signature/result and float
query bound are relative to that origin. `translate` must translate a valid
origin by exactly the dx/dy applied to the box and its lines. This keeps
absolute geometry minus origin invariant after a grid/flex parent places an
ordinary descendant. The two coordinates cost sixteen bytes per box; include
that cost in peak-memory evidence. Audit `translate_content` and direct geometry
writes too: a path that changes geometry without the matching origin update
must invalidate the signature. A struct-size check alone cannot prove that
translation covers an already-existing field; explicit translation tests do.

`FBox.laid` records logical placements in the current layout. Increment a
replayed root as a fresh `block_at` would; mapped descendant placements are
recorded once for this replay, without importing the source tree's counter.
If a later fresh pass places one again, that is another placement and must
remain visible to the existing bounds checks. It is not a count of expensive
measurements; `measures` and the reuse counters retain that distinction.

On success, copy the root and descendant geometry outputs, line results,
content-height and baseline results from that signature's completed layout;
leave fresh structural and parent-supplied input fields in place. Apply the
saved cursor effect and increment the reuse counter. On any qualification
miss, execute the existing layout path. Parent flow and final public-tree
construction continue normally.

## Ownership inventory and publication

This is a field-by-field graft, not a `memcpy` of the old internal graph.

| Record/reference | Destination and rule |
|---|---|
| `FBox` parent/child/sibling links, style, node, model indices | Fresh pass-2 records; retain their identities and links |
| `FBox` placed geometry, content height, baseline, lines, marker fragment | Explicit copied outputs after eligibility; translate coordinates |
| `FItem`, processed/generated text, markers | Fresh records and bytes; old fragments map to the corresponding new item |
| `FInline` | Fresh shared record; reconnect first/last span to copied spans; reject escaping split pieces |
| `FLine`, `FFrag`, `FSpan` | Allocate in the new layout arena; remap item/inline/style/text references and translate geometry/baselines |
| Anonymous styles and decoration data | Fresh box/item styles and verified current derived values |
| Text runs and ellipsis | Retained immutable owners, registered in the new layout's run-release ledger |
| `FPos`, `home`, containing-block and absolute lists | Built fresh; geometry grafts containing these relationships are excluded |
| Public boxes, frames, stacking lists, hit/selection data, overflow/ink | Regenerate from the resulting fresh internal tree |

The new text primitive is checked `os64_text_run_retain`: increment the opaque
run's reference count without allocating, fail without changing ownership on
invalid input or count overflow. Creation starts with one reference, and
release frees glyph pins, retained fonts and storage only on the last one.
The context's live-run count counts run objects, not references. Context destroy
remains busy while a shared run survives. Retain/release follow the existing
caller-serialized text-context contract; no new concurrent mutation guarantee.

Sharing a run shares immutable glyph/caret/byte data, not the old tree or its
arenas. Existing `FLayout.runs` owns one reference per registered ledger entry;
a successful retained reference must enter that ledger exactly once. Failed
publication releases every retained reference exactly once. Test ellipsis and
outside markers as owners, not only ordinary text fragments.

Do not assume the existing `translate` helper covers the graft: its fields and
traversal order were written for an already-owned graph. Audit the copy/translate
inventory against the struct declarations, including absolute baselines,
relative offsets and first/last pointers. Add size guards beside copy helpers
so struct changes require revisiting that inventory, and ownership tests that
free the source before inspecting/painting/hit-testing the result.

Build copied lines and pointer patches privately; publish to fresh boxes only
once the graft is complete. Structural mismatch is a normal miss. Allocation,
run-retain or budget failure after copy work begins returns through the existing
incomplete-layout path, with no half-published candidate. Do not silently retry
the whole subtree with already-spent arena budget. A future rollback allocator
could permit such retry, but it is not part of this contract. Earlier completed
boxes must remain valid under the existing prefix/refusal rules.

## Intrinsic widths have a separate memo

Before `intrinsic()` measures a new box, it may copy a completed old result
when the same untouched/content/style/resource correspondence holds and the
intrinsic-query mode matches. Require a complete source tree and a placed,
finished source box for this path too: `intrinsic_known` alone is insufficient
if later layout stopped or failed. Equality of styles alone cannot permit this:
unchanged styles with new text have different min/max content widths.

The reused payload is `intrinsic_min`, `intrinsic_max`, `content_min`,
`content_max` and the successful-known bit. Do not import `content_h` as an
intrinsic width result; it belongs to a placed layout at a particular width.
The fresh `intrinsic_computed` counter records work performed in this layout.
Reused results get a separate count so tests can distinguish a cache hit from
recomputation.

The current width algorithm treats unresolved percentages through its zero-base
intrinsic rules. Preserve that algorithm's query semantics rather than claiming
all intrinsic sizes are universally independent of context. A changed resource,
ratio input, text/font, generated marker or sizing role invalidates the memo.
Tables and their internal parts use the fresh intrinsic algorithm in this cut;
eligible ordinary children queried by flex/grid/shrink-to-fit may reuse theirs.
Final width changes can defeat geometry reuse without defeating a valid
intrinsic-width result. Flex/grid placement still runs.

## Costs, failure boundaries and diagnostics

Charge maps, snapshots, signatures and staging records to a separate reuse
metadata ceiling equal to one effective `max_arena_bytes` (the existing default
when zero is supplied). New-tree persistent records and call-local staging
share that ceiling. An old source keeps its separately bounded snapshot until
freed; account for both in peak-memory measurements. No per-node heap
allocation and no per-mutation retained arena chain. If index preparation
cannot fit, disable reuse and build normally. Include metadata preparation in
the reported layout cost; do not hide it before the profiling interval.

Keep `flow_profile_t` ABI intact: its existing `reused` field counts internal
boxes whose placed geometry was copied, once per box even inside a larger
copied subtree. It must not count style copies or intrinsic-width memo hits.
Expose intrinsic-hit and refusal-reason totals through an additive accessor.
Build the refusal histogram in step 2, before enabling grafts, so the host
oracle and P5 diagnostic can explain zero reuse. Existing `measures` counts actual IFC
measurements. Initial opt-in layouts include snapshot cost but report zero hits.

Without reuse, public dumps remain byte-identical to the existing baseline.
With reuse, output must equal a fresh layout. Optimization counters are tested
separately; they must not be subtracted from correctness observations.

## Verification and implementation order

Build in reviewable steps within 5c, without enabling a new page scheduler:

1. Text-run retain/release and destruction tests, including overflow/failure,
   glyph eviction pressure and either release order. Keep the same API useful
   for other text consumers; no browser-specific ownership in libos64.
2. Snapshot history, fresh-structure correspondence and refusal reasons with
   reuse disabled. Prove repeated use of an older displayed tree and history
   loss, including independent cascade-reader advancement.
3. Ordinary-block graft and intrinsic memo behind the explicit entry. Exercise
   the whole/incremental oracle before Yonder opts in.
4. Wire Yonder's resource/state snapshots and both layout callers; validate
   scripted geometry and displayed updates, then measure on QEMU and the P5.

The oracle compares `flow_dump` and additional public observations after every
mutation, with the old source freed before querying the incremental result.
Hit-test/selection results, caret positions, link/control mapping, clipping,
scroll reach and paint operations supplement the dump where it omits them.
Both complete success and low-budget failure must satisfy existing ownership
and valid-prefix contracts; a failed attempt must not poison the source tree.
ASan, UBSan and leak detection stay enabled, including allocation sweeps over
new metadata, copy and ledger-growth paths.

Required adversarial cases:

| Case | What must be proved |
|---|---|
| Change one paragraph; grow/shrink the earlier sibling | Steady siblings reuse with correct translated y and margins |
| Equal sum but different positive/negative incoming margins; empty nested blocks | Cursor signature cannot conflate distinct collapse behavior |
| Text changes with identical styles; attribute changed back; moved nodes | Style equality and current ancestry cannot replace history |
| Old displayed A, measured B, measured C, failure, another read from A | Cumulative dirty state never forgets B; failures preserve ownership |
| History eviction, reader/index OOM, deleted/recreated nodes | Safe full rebuild; no stale identity or partial certificate |
| Insert an earlier link/control/list item; change `ol start` or `li value` | Fresh model indices and generated markers survive copying |
| Font arrival, image size/availability, typed form value, canvas resize | Resource/state changes miss even without a relevant HTML delta |
| Float begins intruding after a sibling shrinks; collapsed negative margins; nonzero Space offset; pending float/marker | The proposed float-query bound rejects influence, using the correct coordinate frame |
| Flex/grid stretch, table cells, absolute/fixed descendants, split inlines | Excluded geometry is rebuilt; ordinary safe descendants remain testable |
| Viewport/zoom/height/appearance/quirks changes; animation | Environment or motion mismatch cannot replay old geometry |
| Free source first; alternate source/destination destruction; repeated generations | Runs, bytes, styles and maps remain owned without an arena chain |
| Intrinsic-only hit followed by a different final width; incomplete source with `intrinsic_known` | Width memo does not install old placed height/lines or accept incomplete sources |

Use the original and companion mutation fixtures, the unchanged captured
article and a 2,000-item append fixture. For each, alternate the frozen 5b
control and 5c candidate three times on the P5. Keep the corpus-562 manual
maximization/address-bar protocol for the streamed article, and a fixed
matching viewport/settings for the mutation and append pages. Record first
opt-in layout time and peak memory beside the timings; declare which heap
owner and sampling point each memory number describes. Keep the frozen small
delivery cohort unchanged throughout the hardware test.

The append case must demonstrate fewer measurements: the new item's content
and necessary ancestor work, not the other 2,000 items. Measure marker work
separately where the current counter does not expose it. The original mutation
fixture should demonstrate geometry reuse on unaffected rows; initial-layout
and broad-fallback cases must not regress materially against the same-session
control. Preserve the actual samples and work counts, not just medians.

**The unchanged streamed article is a fallback control, not a positive reuse
fixture under this contract.** Its captured selectors include `:has()` and
`:dir()`, which force full cascade replacement. The chosen generation guard
then disables copied styles/geometry, and Yonder's `measured_fallback` releases
the old hidden tree. Requiring `reused > 0` from this exact workload would
require changing those architectural decisions, not merely refining the float
predicate. Do not remove broad rules from the existing control silently.

Approved positive float-reuse gate: add a separately named article-shaped
fixture with a grid content column, infobox/thumbnail floats and ordinary
paragraphs, using localized selector dependencies. Change a paragraph and
require fresh layout beside the float and `reused > 0` below it. Compare its
whole/incremental public output and alternating P5 timings. Include the
negative-margin counterexample as a refusal/oracle case. Label this fixture
as synthetic; it is not evidence of reuse on the unmodified Wikipedia capture.

## Architecture-read dispositions

Fable approved fresh pass 2, the local geometry exclusions, per-snapshot
cumulative history, resource revisions and text-run ownership. His requested
reader release on disqualification, source completion for intrinsic reuse,
translated origins, `laid` semantics and early refusal diagnostics are now
explicit above. His smaller cursor result record replaces the unnecessary
collapse-through variant.

Fable accepted both follow-ups on 2026-10-31, reading `bdadbad1`:

1. Amendment A's root-top comparison is superseded by the recorded query
   bound. The recorded lower bound on float-sensitive queries must
   cover collapsed negative margins and the Space coordinate frame. The
   [existing-engine probe](../../relayout-evidence/5c-review-probes.md)
   demonstrates the counterexample; no speculative production fix was made
   to produce it.
2. Amendment D's three-workload/append protocol is adopted. Fable withdrew
   its positive-reuse requirement on the unchanged article: the generation
   gate and hidden-tree release remain intact. The synthetic float fixture
   proves that safety predicate separately while preserving the real article
   as a fallback/no-regression gate. Removing the generation gate and retaining
   old hidden geometry across rebuilds would be a separate design change with
   memory and first-layout consequences.

Steps 1 through 4 are approved to build on this revised contract. The
separate ancestor/sibling selector optimization booked in
[RELAYOUT.md](RELAYOUT.md#booked-selector-invalidation-follow-up) is not a
prerequisite for 5c. Its corpus census informs a future scheduling decision;
it does not reopen this architecture approval.
