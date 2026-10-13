# E3 — Implementation packet for Sol

2026-10-13. Design owner: Quinn. Implementation is unstarted; intended
implementer is Sol when assigned by Chris. The governing contract is
[E03_DATA_VIEW.md](E03_DATA_VIEW.md); this file supplies execution order and
acceptance cases, not a competing behavior specification.

## Starting point

E2 is merged through PR #261 at `f39b0ae0` on `userland`. Use a fresh
implementation worktree based on that commit or a descendant plus this design
packet. Do not start from E1B's old branch, overwrite another worktree or assume
that design files exist on `userland` until their publication/merge is recorded.
Check [HANDOFF.md](HANDOFF.md) for the packet's publication status.

Your outcome is `/tests/actiongallery --details-page` backed by reusable
libos64 components. It must demonstrate 10,000 rows, real E2 icons, columns,
selection and scrolling. It is not a filesystem browser yet. No kernel, IPC,
new image decoder, desktop icon placement or existing listbox rewrite.

Read these implementation seams before editing:

- `userland/libos64/include/os64/ui.h`, `ui.c`: retained widget ownership,
  pointer grabs, context-hook routing, focus loss and gesture cancellation.
- `ui_list.c`, `ui_font.c`: visible-row staging, planned bounds and the
  prepare/commit/discard/destroy/reface distinction. Use the pattern, not the
  listbox's single-selection semantics or its paint-time text preparation.
- `ui_text.c`: `os64_ui_decode_key`, PS/2 bursts and HID input. E3 needs Ctrl
  selection modifiers; copying the listbox's Ctrl rejection would be wrong.
- `ui_action.c`, `ui_menu.c`, `ui_dispatcher.c`: menu precedence, repeat guards,
  bounded dispatch and callback lifetime. Do not add a private nested event loop.
- `os64/ui_icon.h`, `userland/libimage/include/image/icon_theme.h`: acquire
  during preparation, draw from leases, preserve the dependency direction.
- `os64/proc.h`: `os64_micros` for typeahead/double-click intervals and timings.
- `userland/tests/actiongallery/actiongallery.c`, `userland/GNUmakefile` and
  existing UI host scripts: integration, test linkage and packaging conventions.

Expected shared files: new `ui_data.c`, `ui_dataview.c` and their public headers,
the userland build, gallery source and focused host tests. Keep internal helper
splits modest and purposeful. If an existing toolkit seam genuinely needs a
change, identify its other consumers and cover their unchanged behavior.

## E3A — Identity, projection and selection

Implement copied immutable snapshots, ordered projections, typed sorting and
the detached selection state first. No font/window dependency in their tests.
This packet settles IDs, limits, memory ownership and exact state transitions.

Before implementing the widget, retain a short implementation map alongside
the evidence: each invariant below, the production entry points that can alter
it, the shared helper enforcing it, and the public test proving it. Fill in
real names as they exist. A map listing only the obvious event handlers is
incomplete: include replacement, reset, font/layout, failure, disposal and
programmatic setters.

| Invariant | Paths which must obey it |
| --- | --- |
| I1: selection/focus/anchor identify members of the current projection | Pointer/key/context changes; filter, sort, refresh, reset; detached and attached setters |
| I2: a failed candidate cannot partially publish | Construction, bind, sort, reveal, scroll, columns, font preparation; scrollbar callback restoration |
| I3: input intent belongs to its original revision and delivery sequence | Press/release, double click, projection replacement, geometry changes, blur/cover/INPUT_LOST, hide/disable/detach |
| I4: paint and hit-test use committed geometry/resources | Scroll/reveal, column mutations, resize, font changes, reface, layout failure/retry |
| I5: ownership survives failures and ends once | Partial construction, acquired icons, active/staged text, replacement, callbacks, generic font teardown and explicit disposal |
| I6: observable outcomes agree | Return status, error hook, change flags, activation target, revision, damage, sort arrow and scrollbars |
| I7: work and retained resources stay bounded | Paint, key navigation, sorting/filter publication, typeahead, repeated replacement and maximum-size inputs |

Concrete selection fixtures use IDs `A B C D E`, deliberately unrelated to
their indices. Check the entire tuple, not just a selected count:

| Sequence | Expected state |
| --- | --- |
| Click B; Ctrl+click D | Selected {B,D}, focus D, anchor D |
| Then Shift+click B | Selected {B,C,D}, focus B, anchor D |
| Then Shift+click C | Selected {C,D}, focus C, anchor D |
| Click B; Ctrl+Down | Selected {B}, focus C, anchor B |
| Then Ctrl+Space | Selected {B,C}, focus C, anchor C |
| Then Ctrl+Space again after release | Selected {B}, focus C, anchor C |
| Click B; Ctrl+Shift+click D; Ctrl+Shift+click C | Selected {B,C,D}, focus C, anchor B (union does not subtract D) |
| Select B,D; reorder to E,D,C,B,A | Still {B,D}; focus/anchor D; enumeration D,B |
| Then filter out D | Selected {B}; fallback focus C at old position 1; anchor C |
| Then clear filter | Selected {B}; D is not resurrected; focus/anchor C |
| Select B,D; wheel until both are offscreen | Selection/focus/anchor unchanged |
| Focus D, anchor B; refresh removes B and D | No retained selection for removed IDs; focus E at clamped old position; anchor E |
| Replace with another domain reusing IDs | Empty selection, absent focus/anchor, top/horizontal zero |
| Replace with empty projection, then restore same domain | Empty selection and absent focus/anchor remain; no resurrected state |

Include independent views sharing one snapshot: sorting, filtering, selection,
scrolling and disposal in one must not change the other's state or lifetime.
For typed sort, use numeric keys 2 and 10 with display strings that would sort
differently; equal text keys; min/max signed/unsigned values; missing timestamps;
and at least two group ranks. Reverse direction without reversing groups or
moving missing values to the front.

## E3B — Details widget and diagnostic

Integrate with the font transaction and real drawing code. Build candidates
before publication; retain IDs rather than row pointers in gestures. Keep
selection mutation through the attached view so a keyboard reveal cannot
publish selection while leaving an old viewport after allocation failure.

Gallery entry:

```
/tests/actiongallery --details-page
```

Print a short startup explanation naming the page and its purpose. Label the
synthetic data. Include Name/Size/Type/Modified with folders-first sort ranks,
repeated prefixes, long/UTF-8 labels, missing values and E2 folder/file icons.
Show selected count, focus/anchor IDs and last activation in a diagnostic area;
these implementation details belong in the test app, not future Roam chrome.
Provide controls/menu actions to:

- Switch between 0, 1, 10,000 and the 100,000-row limit fixture; expose failure
  to construct the larger fixture without mislabeling the retained dataset.
- Apply/clear a deterministic subset filter and replace rows while retaining
  some IDs; also reset into another identity domain using reused numeric IDs.
- Create/show a second independent view of the same snapshot. Keep this opt-in
  so the normal Details demonstration is readable.
- Show/hide and move columns, retry failed layout, and report timing/memory
  diagnostics. Keep existing gallery pages and launch options working.

Menu commands use the shared action/menu system. A header menu includes column
visibility and ordering; a row context menu demonstrates explicit target
selection and reports IDs without opening or modifying real files. Route menu
events before body handling so Ctrl+A, Space, navigation and typeahead do not
affect the table underneath an active popup. Provider destruction follows view
disposal; missing icons keep readable labels. The app must follow Interface
font and light/dark appearance changes.

## Failure and interruption matrix

Fault-inject production allocations and resource acquisition through the
existing host seams or narrowly scoped new seams. A synthetic successful paint
callback is insufficient: test the actual widget renderer and font engine.
Verify between events, after the failure, after retry and after cleanup.

| Sequence | Required observation |
| --- | --- |
| Fail each allocation in snapshot/projection/sort creation | Typed failure, cleared output, caller inputs usable, no leaked candidate |
| Selected/scrolled view; fail replacement at successive preparation points | Old tuple, sort arrow, top row and pixels stay coherent; retry succeeds |
| Failure after some text runs and icon leases have been acquired | Partial resources released once; live resources remain; destroy is clean |
| Acquire icon fails while text succeeds | Row remains usable with reserved empty icon slot; successful leases release normally |
| Failed scroll followed by failed font adoption | Both failures leave old committed state; no stale scrollbar position or candidate runs |
| Resize delivered; fail layout, then fail Retry Layout, then succeed | Blocked state at current bounds, no old hit targets/row activation; persistent selection survives; success restores usable rows |
| Press row; sort/filter/refresh/reset before release | Stale release neither changes selection nor activates a replacement row |
| Press row or resize grip; blur/cover/INPUT_LOST before release | Gesture cancels; next fresh click/key works without needing an extra release |
| Decode partial PS/2 burst; lose focus; return and type | Old escape state cannot swallow new input; no burst bytes enter prefix |
| Hold Enter/Ctrl+Space; lose focus before key-up; return and press again | No repeated activation/toggle from hold; fresh press works |
| Double-click pair interrupted by scrolling, replacement or focus loss | No activation of a stale target; ordinary new clicks still select |
| Hide/disable/detach during a gesture, then restore | No pending press, prefix or repeat guard leaks into restored widget |
| Callback tries bind/scroll/dispose on its own view | BUSY, fully committed getters, no nested publication or freed storage |
| Tiny/zero viewport; extreme widths; last partial row; repeated hide/move | Clipped rendering, correct hit tests, clamped scrolling; required column stays available |
| Reface and generic font release, followed by explicit disposal | No duplicate release and no retained context/lease references |

Fail two *successive* operations in the matrix; do not simulate this by
pretending cleanup itself can refuse. E3 does not own native popup destruction;
that menu contract remains E1B's responsibility. If a finding exposes a broken
invariant, search all paths above before resubmitting. Add the meaningful
regression, not merely an assertion mirroring a changed helper.

## Evidence and acceptance

Create `evidence/e03/README.md` with exact commands, commits, host environment,
fixture parameters, native installed hashes and linked logs/screenshots.
Keep “not run” visible until there is evidence; Chris's P5 report has its own
scope and revision, separate from later review fixes.

| Check | Expected evidence | Current result |
| --- | --- | --- |
| Core and real-font host behavior | ASan/UBSan/LeakSanitizer; state fixtures, allocation sweep, interrupted sequences and two independent views | Not run |
| Production rendering/damage | Instrument row visits and allocation/I/O calls during paint; image/pixel checks for stale highlights, focus, header and row clipping | Not run |
| Existing consumer regression | Affected UI/menu/font suites; existing listbox and gallery modes remain usable | Not run |
| Cross-build and deployment | libos64 plus gallery; normal image installation and os64get deployment include the same tested library/test executable | Not run |
| QEMU | Native Details walkthrough with PS/2 input, real icon set, Interface font changes and tiny/large windows | Not run |
| P5 | Chris's walkthrough with HID input, appearance, selection, sort/filter/scroll, resize and activation | Not run |
| Resource/performance bounds | Measurements below, repeated create/replace/dispose reaches baseline | Not run |

Leak detection stays enabled. If the sandbox denies LSan thread attachment,
diagnose and rerun the same binaries outside it through the normal permission
mechanism; do not turn leak checking off to obtain a green result.

For 10,000 rows, record snapshot bytes, live/peak view metadata, font-engine
bytes, run count, icon leases/cache bytes and construction/sort time. Measure
100 ordinary navigation/scroll events after warmup, reporting median, p95 and
maximum event-to-paint duration on host and native environments separately.
Target p95 under 50 ms on P5 for these ordinary events; report a miss candidly
and investigate before calling performance accepted. Typeahead searches and
sort/replacement can scan the projection; time them separately, including a
no-match search. Do not label host/QEMU measurements as P5 latency.

At fixed viewport dimensions, instrument painting for 100 versus 10,000 rows:
visited/prepared-on-paint row count must be bounded by viewport capacity, not
dataset or selected count. Paint must allocate nothing and acquire no icons.
Initial construction, sort and publication may be O(n) / O(n log n); ordinary
known-ID lookup is not a full scan. Check the 100,000-row fixture and reject a
100,001-row request explicitly. Repeated replacement must not grow caches
without bound. Explain shared font/icon cache retention separately from leaks.

P5 walkthrough, after deploying both library and gallery:

1. Start the Details page; sort Size both ways and confirm folders stay first.
2. Click, Ctrl+click, Shift+click, Ctrl+arrows/Space and Ctrl+A; scroll away and
   back. Focus outline and selected rows should agree with the diagnostic IDs.
3. Filter out a selected row and clear the filter. Its selection must stay gone.
   Refresh/delete the anchor fixture, then sort while several items are selected.
4. Type repeated initials and a longer prefix, pause a second, then type again.
   Test Enter/double-click activation and returning from another window.
5. Resize/move/hide columns; restore them through View > Columns. Shrink the
   window; inspect both scrollbars and the last row. Change Interface font and
   light/dark theme, then continue selecting and scrolling.
6. Open the second view; change one view and close it while the other stays
   usable. Confirm existing gallery pages still work.

## Review and handoff discipline

No E3 implementation or independent review exists yet. E1B's six finding-bearing
rounds are the reason for explicit transitions and intermediate observations;
large assertion counts did not by themselves prove those contracts. Its
[invariant audit](E01B_INVARIANTS.md) is a method to reuse, not menu behavior to
copy: file selection may remain offscreen.

Before submission, read comments and names against the behavior they describe,
run the repository stale-reference check, inspect every result and update this
packet's implementation status, the board and session log. Review both inline
threads and PR conversation comments when collecting findings. Record fixes by
invariant and audit sibling paths. If an invariant is reported a second time,
revisit its transition map before another local patch/resubmission.

E3A and E3B can be one coherent PR against `userland`; do not merge without the
project's ordinary approval. E4 follows accepted E3. No need to wait for E6 or
request Fable's approval for this userland slice.
