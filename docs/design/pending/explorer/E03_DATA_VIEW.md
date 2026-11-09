# E3 — Reusable Details view and selection

## Status and ownership

- **State:** E3A/E3B implemented on `codex/explorer-e3-implementation`.
  [Evidence](evidence/e03/README.md) records host/native scope; P5 acceptance
  and independent review remain pending. Design dated 2026-10-13.
- **Design and implementation owner:** Quinn; Chris assigned E3 on 2026-10-13.
- **Design branch/worktree:** `codex/explorer-e3-design`,
  `.worktrees/explorer-e3`, based on `userland` at `f39b0ae0` (E2 merge).
  Publication and the implementation resume point are in [HANDOFF.md](HANDOFF.md).
- **Prerequisites present:** E1A actions/status, E1K native popups, E1B menus,
  E1C production consumers and E2 icons. E2 PR #261 merged as `f39b0ae0`.
- **Governing requirements:** D002/D011/D017 in [DECISIONS.md](DECISIONS.md),
  [umbrella browsing requirements](../EXPLORER.md#browsing-and-selection).
- **Fable review:** Not required: userland model/widget work, no syscall or
  kernel changes. A discovered platform prerequisite gets a separate packet.
- **Implementation instructions/evidence plan:** [E03_HANDOFF.md](E03_HANDOFF.md).

## Outcome and boundary

A shared, virtualized Details table with columns, icons, scrolling, keyboard
navigation, type-to-select and multiple selection. `/tests/actiongallery
--details-page` demonstrates 10,000 synthetic entries and a second independent
view over the same data. This is a diagnostic, not Roam or a Control Center app.
E4 supplies the real directory browser; E5 supplies Scribe's picker consumer.

Keep the existing single-selection listbox and its consumers compatible.
Use focused public headers `os64/ui_data.h` and `os64/ui_dataview.h` in libos64.
No dependency from libos64 to libimage: the app lends the E2 icon provider.
No filesystem calls, path interpretation, directory loading, rename, clipboard,
drag/drop, thumbnail jobs, tabs or persisted preferences in E3.

The division of responsibility is explicit:

| Component | Owns |
| --- | --- |
| Immutable data snapshot | Copied row IDs, display cells, typed sort keys, group ranks, search labels and optional semantic icon keys |
| Immutable projection | An ordered subset of a particular snapshot's IDs and its sort descriptor |
| Reusable selection state | Selected IDs, keyboard focus ID and range anchor ID for one projection |
| Details widget | Column presentation, viewport, prepared text/icon resources, input gestures and damage |
| E4 directory producer | Filesystem metadata, ID assignment/matching, filter predicates, sort-key meanings, request generations and history |

Selection is separate from presentation so a later E14 view can reuse it.
E3 supports one attached widget per selection state; two independently usable
views share a snapshot, not mutable selection or scrolling. Switching renderers
is a later integration, not permission to implement several renderers now.

## Data contract and ownership

### Identity and immutable inputs

`os64_ui_data_id_t` is a nonzero `uint64_t`; zero means no item. A snapshot
has a nonzero `uint64_t` identity-domain token supplied by its producer. IDs
must be unique within it. Replacing rows in the same domain asserts that reused
IDs refer to matching logical entries. A different domain clears selection,
focus, anchor, typeahead and scroll. E4 uses a new domain for navigation and
may match names on refresh within a location; this is not stable filesystem
identity and must not authorize a destructive action without revalidation.

Snapshot construction copies the input. Each row has an ID, unsigned group
rank, a search label, an optional icon key, and one cell per schema column.
Each cell has display text and an optional typed sort key: text, unsigned
64-bit or signed 64-bit. A column declares one key type; mismatched values are
invalid. Numeric/date sorting must not compare formatted display strings.
Missing metadata is a missing key, not zero. Column IDs are nonzero, unique
32-bit values; columns supply title, alignment, default width, sortable flag
and a required-visible flag. At least one column must be required-visible.
The gallery's Name column is required-visible and is the search/icon column.

Text uses explicit byte lengths, is copied and NUL-terminated internally.
Embedded NUL and LF are invalid in these single-line fields; other bytes follow
the existing text engine's display profile. A producer with such filename bytes
must supply an escaped display/search label while keeping its original name
outside the widget. Do not introduce a new Unicode normalizer. Labels never borrow a
directory worker's buffers. IDs have no pointer or array-index interpretation.

Limits: 100,000 rows, 16 columns, 4,096 bytes per individual text field and
64 MiB total snapshot-owned allocations, including index tables and copied
strings. All sums/products are checked before allocation. Overflow or an
exceeded bound returns LIMIT, never a truncated successful snapshot. Empty
snapshots are valid. These are toolkit bounds; E4 may select smaller listing
bounds and must label its own partial results truthfully.

Snapshot and projection are opaque, retained/released objects. A projection
retains its snapshot; selection retains its current projection; an attached
view borrows its app-owned selection state and icon provider. The provider
outlives all view leases. Public create functions clear their output first;
failure publishes nothing and releases the incomplete candidate. Successful
construction permits the caller to release source buffers immediately.

Use one UI/lifecycle thread for live objects, references, callbacks and input.
An unpublished snapshot may be built on a worker from worker-owned inputs and
transferred with exclusive ownership; no reference-count operation races the
handoff. Projection/selection/widget operations run on the UI thread. E4 owns
the completion-generation check before a new snapshot reaches the view.

### Projection, filtering and ordering

Projection creation accepts a snapshot and an explicit ordered list of IDs.
Unknown or repeated IDs are refused. An identity projection includes the whole
snapshot in source order. This API lets E4 compute its own predicate without
embedding filenames or filter text in the widget. Keep an ID-to-source lookup
and an ID-to-projection-position lookup; range and focus operations cannot
repeatedly scan the dataset to locate a known ID.

Provide a sort helper over a projection. Compare group rank ascending first,
independent of direction; then present keys before missing keys, independent
of direction; then compare the chosen column's typed key in the requested
direction. Text comparison folds ASCII A–Z only and compares other bytes
unsigned. Equal keys retain snapshot source order, independent of direction.
Use source position as the final tie-breaker, including when the input
projection already has a different order. Use a bounded O(n log n) algorithm,
without subtracting 64-bit keys or
negating an arbitrary comparator result. E4 assigns directories a lower group
rank than files, giving folders-first sorting in both directions.

Sorting produces a new projection and descriptor; a header click requests
that projection and binds it transactionally. The sort arrow changes only on
success. Clicking the active column toggles direction; a new column starts
ascending. Unsortable columns do not show a sort affordance. A filtered
projection's sort sees only its included IDs.

Per-view metadata, including selection, maps, projection and transaction
scratch owned for that view, has a 16 MiB budget (the retained snapshot is
accounted separately). Candidate plus live storage counts against it. Reusing
an immutable object does not duplicate its allocation accounting. Exhaustion
returns LIMIT or NO_MEMORY with the old view intact. Publish concrete byte
accounting in the implementation evidence rather than assuming virtualization
means the metadata is free.

## Selection contract

Persistent state is `(projection, selected IDs, focus ID, anchor ID)`. Focus
identifies keyboard navigation, selection identifies command targets, anchor
defines a range. Each nonzero ID belongs to the current projection. Focus and
anchor need not be selected. The window's widget-focus flag is separate again.

On same-domain replacement, intersect selection with the new projection.
Preserve focus/anchor IDs when present. If focus disappears, choose the row at
its old projection position clamped to the new last row; if there was no focus,
leave it absent. If anchor disappears, set it to the resulting focus. An empty
projection clears all three. The replacement does not select the fallback
focus automatically. RESET replacement and a new domain clear all three.

**Filtering removes selections outside the projection. Clearing a filter does
not resurrect them. Scrolling does not change selection.** Command enumeration
returns selected IDs in current projection order, including offscreen rows.
Painting enumerates viewport rows, not the selected set. Sorting preserves IDs
and reinterprets future ranges in the new order; it never keeps old row numbers
as identities. A successful projection replacement ends any pointer gesture,
double-click sequence and typeahead prefix, even if all IDs survived.

Ranges include both endpoints. Without an anchor, use the old focus if present,
otherwise the destination. Shift replaces selection with the current range;
Ctrl+Shift unions that range into existing selection. Shrinking a Ctrl+Shift
range does not subtract earlier additions. Ctrl-only movement leaves the
anchor intact. Ctrl+click/Space makes its target the new anchor, including when
the target becomes unselected. These rules apply in both model tests and UI.

| Input over body | Result |
| --- | --- |
| Plain left click on row | Sole selection, focus and anchor become that row |
| Ctrl+click | Toggle target; move focus and anchor there |
| Shift+click / Ctrl+Shift+click | Replace / union inclusive anchor-to-target range; move focus |
| Plain click on blank body | Clear selection, focus and anchor |
| Modified click on blank body | No selection change |
| Up/Down, Home/End, Page Up/Down | Move focus, sole-select destination and set anchor; reveal it |
| Ctrl + navigation | Move/reveal focus only |
| Shift / Ctrl+Shift + navigation | Move/reveal focus and apply replace/union range |
| Ctrl+A | Select the whole projection; retain existing focus/anchor; if absent and nonempty, use first row |
| Space / Ctrl+Space | Sole-select focused row / toggle it; establish anchor |
| Shift+Space / Ctrl+Shift+Space | Replace / union anchor-to-focus range |
| Enter | One activation notification for focused ID, if any; no automatic selection rewrite |
| Double left click | Activate that row after ordinary click selection rules; require unmodified input |
| Wheel / scrollbar | Move viewport; preserve selection, focus and anchor |
| Escape | Clear typeahead and cancel in-progress pointer/header gesture; keep selection |
| Tab / Shift+Tab | Existing toolkit traversal; never select a row as a side effect |

With no focus, forward navigation starts at the first row, backward navigation
at the last; Home/End select their named endpoint. Page step is at least one
row and otherwise the number of fully visible rows. Empty input is a no-op.
Left/Right scroll horizontally by a fixed bounded step (32 px); modifiers
do not turn horizontal movement into selection. Alt combinations and F2 stay
available to the application/menu routing; E3 does not implement rename.

During a left-button grab, defer ordinary keyboard selection/typeahead; Escape
cancels the grab through toolkit gesture cancellation. Commit pointer selection
on a matching left release, not on press. Retain the
pressed ID, projection revision and press modifiers; release must hit that ID
in that revision. Movement beyond 4 px on either axis cancels click activation.
Two successful unmodified clicks on the same ID within 500 ms form a double
click, with no intervening model, viewport or focus change. Activation clears
the pair, so a triple click does not emit a second activation.

Right-button delivery already goes through `on_context` before widget events.
Do not change that global contract. Expose an explicit data-view context-target
helper which the app calls from its hook: pointer on a selected row preserves
the group and sets focus there; pointer on an unselected row sole-selects it;
blank body clears selection/focus/anchor; keyboard invocation preserves state
and targets focus. The helper clears typeahead and click state. Only this
explicit call changes selection; generic context bubbling remains unchanged.
E4 decides menu actions, and revalidates the saved ID/domain before invoking.

### Type-to-select and event cancellation

Use the existing key decoder for PS/2 escape bursts and HID input; raw burst
bytes must not become search characters. Recognized navigation repeats. Guard
Enter/Space activation/toggling against held-key repeats and reset guards on
focus loss, INPUT_LOST, hiding, disabling and detachment. A lost release must
not swallow the next fresh press after returning to the view.

E3 typeahead accepts printable ASCII from the current GUI key stream, with
ASCII-insensitive prefix matching against search labels. UTF-8 labels still
render; this is not a promise of Unicode text input or locale collation.
Allow 64 prefix bytes plus a terminator and a 1,000 ms idle expiry from `os64_micros`, with an
injectable host clock. No wakeup timer is needed: check expiry on the next key.
If the clock is unavailable, treat each printable key as a fresh single-letter
search; keep navigation functional and expose this in diagnostics.

A fresh prefix searches after focus, wrapping once. Extension first tests the
current row, then searches following rows once. Repeating the same single
letter cycles matches instead of building `aaa`; a longer prefix can contain
repeated letters. No match preserves focus/selection but retains the prefix
for Backspace correction. Backspace removes one ASCII byte and retries a
nonempty prefix from current focus; an empty prefix does not change selection.
At capacity ignore another character, without corrupting the buffer. A match
sole-selects, focuses, anchors and reveals its ID. Ctrl/Alt chords do not enter
typeahead; Space retains its selection meaning and is not a prefix character.
If revealing a match fails, preserve the prior prefix and persistent state and
report the failure. A no-match search is successful input, not a layout failure. Navigation, pointer selection, context invocation, successful
replacement and input cancellation clear it; wheel scrolling alone does not.

## Columns, viewport and rendering

The initial gallery has Name, Size, Type and Modified. Default widths in pixels
are 260/100/140/180, minimum 48, maximum 4,096. Titles and cells use Interface
font metrics. Row pitch is `max(font line height, icon slot) + 8`; choose the
icon bucket with the E2 helper. Header pitch is font line height + 12.
One-line cells are clipped at their own column boundary; no auto-sizing scan
over all rows, wrapping, or glyph drawing into adjacent cells. Keep full text
in the snapshot. Numeric columns align right; text aligns left. Clipping is
sufficient for E3; do not add an ellipsis engine as a prerequisite.

Column presentation belongs to each view: order, widths and visibility.
Required-visible columns cannot be hidden. Columns may move across the Name
column; it is not frozen. Header boundary dragging previews a resize guide and
commits width at release. Cancelled or failed resizing preserves the old width.
Header right-click uses the shared menu with Show/Hide and Move Left/Right for
the target column; disable impossible actions. This satisfies reordering
without implementing header drag-and-drop. Keyboard invocation of the header
menu must be available through a gallery View > Columns action. No persistent
column settings or auto-size feature is part of E3.

Reserve separate vertical and horizontal scrollbar strips, using the existing
scrollbar widget with its `horizontal` flag. Keeping strips present avoids
recursive layout decisions. Exclude header and strips from body hit testing.
Vertical units are rows, horizontal units pixels. Paint the partially visible
last row, clipped; page size counts full rows. Clamp top to
`max(0, count - max(1, full_rows))` for a nonempty viewport; a zero-height body
does not access rows. Clamp horizontal position to content width minus viewport
width. Use wide checked arithmetic before narrowing to screen coordinates.

On reorder/refresh/filter, preserve the old top row's ID if it survives,
otherwise its old position clamped to the new range. RESET/new domain starts
at top and horizontal zero. Preserve horizontal position on other replacements,
clamped after column/layout changes. Replacement does not force surviving
focus onscreen. Keyboard navigation/typeahead explicitly reveal focus.

Retain text runs and icon leases for visible rows plus at most one partial row,
and for the header. Bound run slots to 16,384; excess layout returns LIMIT.
The existing font-context budget additionally bounds engine allocations; report
its high-water use separately from snapshot/view metadata. Reuse compatible
prepared resources where useful, but key reuse by snapshot/projection revision,
row ID, column, text and font generation rather than viewport slot alone.
Icon acquisition happens during preparation. Any icon failure leaves its slot
empty and the text usable, as in menus; release successful leases exactly once.
Icon failures do not fail a text-complete view transaction; record diagnostics.
Painting performs no allocation, model mutation, icon acquisition or I/O.

Selected rows use theme selection colors; focused ID has a distinct outline
even when not selected. Window blur removes active focus decoration without
erasing selection. A keyboard invocation with no focused row has no activation
target; it may still offer app-level context commands. Damage the old and new decorations, changed header, body
and scrollbars as applicable. Full-widget damage is acceptable in E3 because
painting still visits only the viewport; O(total rows) work on paint is not.

## Transactions and failure behavior

There is one live revision, at most one unpublished candidate, and transient
input state. Use common candidate preparation and publish/discard paths for
replacement, sorting, scrolling and column changes. Publish is allocation-free.
Do not install selection or scrollbar positions before preparing the rows that
will represent them. Operations which need no new resources may update in place
after validation; notification and damage still belong to that operation.

| Transition | Prepare / success | Failure / interruption |
| --- | --- | --- |
| Initial bind | Validate schema/domain/IDs; allocate selection/indexes; prepare visible text; publish | Remain empty and usable; report typed failure |
| Same-domain replacement | Reconcile IDs, top and schema; prepare replacement resources; publish together | Old snapshot, selection, header, viewport and resources remain live |
| RESET/new domain | Clear state in candidate; prepare first viewport; publish | Old location stays live; caller must not label it as the new location |
| Scroll/reveal/sort/columns | Prepare new viewport/geometry and optional selection changes | Old state stays live; restore companion scrollbar to committed position |
| Font adoption | Use candidate metrics and planned bounds in class prepare; swap in commit; free staging in discard | Existing whole-window transaction keeps old font, metrics and state |
| External resize | Apply new native bounds; prepare resources for them | Enter layout-blocked state below; never hit-test stale rectangles |
| Input loss/blur/hide/disable | Cancel pressed/header targets, decoder, repeat guards, prefix and double-click state | Preserve persistent selection and live resources; no callback activation |
| Destroy/detach | Cancel gestures, detach selection, free active/staged runs and leases | No fallible native windows to destroy; no retry obligation for freed resources |

Same-domain replacement requires an identical schema (column IDs in schema
order, types, titles, alignment, defaults, sortable and required-visible/search
roles); incompatible schema returns INVALID. RESET may
adopt a new schema and its defaults. Replacing just row data preserves compatible
column presentation. A candidate records the revision it was built against;
a stale explicit candidate returns STALE rather than overwriting newer input.
There is no background pending view commit in E3.

Native resize is already delivered when a widget learns its bounds, so it
cannot promise rollback of the window itself. The application calls a typed
`dataview_layout` operation after assigning bounds and before dispatch/paint.
If preparation fails, mark the view **layout-blocked**, cancel gestures, retain
the model/selection and old resources solely for cleanup/retry, and paint a
background/border inside current bounds. Do not paint old rows or accept body,
header or scrollbar actions in this state. The app displays the error via its
status area and offers Retry Layout. A later layout attempt or resize retries;
there is no per-paint or busy-loop retry. A bounds mismatch discovered by paint
or event also blocks safely; it does not allocate. Getters expose this state so
actions cannot offer invisible row activation. Model replacement may proceed
only by successfully preparing against current bounds, clearing the block.

Class font hooks must follow `ui_font.c`'s prepare/commit/discard contract;
`metrics` and `commit` cannot allocate. `reface` invalidates runs and blocks
layout until explicit preparation, rather than borrowing runs from a retired
font. The `destroy` hook and explicit disposal must be idempotent: generic
font teardown already calls widget destruction. Do not conflate releasing
font resources with destroying the app-owned selection/snapshot.

Use `os64_ui_status_code_t`: INVALID for malformed inputs, DUPLICATE for IDs,
LIMIT for checked bounds, NO_MEMORY for allocation, STALE for revision mismatch,
BUSY for reentrant mutation, ERROR for an unexpected preparation failure.
Map font statuses deliberately. Model/sort failure never silently falls back
to an empty table or a different order. Keep the last operation/status available
without allocating a new message in the error path.

Public programmatic mutations return status and do not synthesize user change
callbacks. Widget gestures notify once after publication with revision and
flags for selection/focus/viewport/columns; activation additionally supplies
domain + ID. Failure invokes an error hook, without a success notification.
All getters in a callback see the complete committed state. Callbacks may read
or queue app work; mutations/disposal of the same view/selection during its
callback return BUSY. App/window destruction is deferred until dispatch returns.
Never access a row pointer after a callback; notifications carry values, not
borrowed transient cells. Increment the revision for persistent changes; refuse
wrap with LIMIT. Do not recycle revision/domain tokens across stale gestures.

## Public surface to implement

The headers should express these operations; spelling changes are permissible
if the contract and this section are updated together, not silently weakened.

| Family | Required operations |
| --- | --- |
| `os64_ui_data_*` | Create copied snapshot; retain/release; immutable schema/row lookup by ID; domain/count/budget queries |
| `os64_ui_data_projection_*` | Create explicit ordered subset / identity; sort; retain/release; ID-at-position and position-of-ID |
| `os64_ui_selection_*` | Init/dispose; bind with PRESERVE or RESET; focus/anchor/selection queries; gesture operations; ordered selection enumeration |
| `os64_ui_dataview_*` | Init/dispose; attach selection/provider; transactional bind/layout/scroll/columns; selection commands through the attached view; context-target helper; revision/layout/status queries; callbacks |

An attached selection cannot be mutated through a model-only bypass: return
BUSY and require its view's transactional path. Detached selection operations
are host-testable without fonts/windows and support a future renderer switch.
Getters are borrowed for the call; retain a snapshot/projection explicitly if
using its contents beyond a successful replacement. Iteration must detect a
changed revision or operate on a retained projection plus copied selected IDs.
Expose enough state to assert contracts without including private C files in
tests or making tests drive internal helper functions in place of public APIs.

## Implementation and acceptance

E3A (snapshot/projection/selection) and E3B (widget/gallery) are sequential logical
packets within E3. They can share a PR; this is not a request for arbitrary tiny
PRs. The [handoff](E03_HANDOFF.md) names files, examples, failure combinations,
performance evidence and hardware walkthrough. A green host suite alone does
not complete the slice; the real font/drawing and native input paths need proof.

- [x] Reconcile the design with the merged toolkit and E2 boundary.
- [x] Settle identity, filtering, range, failure and repaint contracts.
- [x] Implement E3A and its meaningful behavioral tests.
- [x] Implement E3B and the diagnostic consumer.
- [ ] Record host, QEMU and P5 evidence separately.
- [ ] Complete ordinary review; record implementation/merge commits.

Implementation and host evidence are recorded in [E3 evidence](evidence/e03/README.md).
P5 acceptance and independent review remain outstanding; no IPC or kernel
prerequisite was added.
