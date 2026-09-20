# Quinn review — F4 checkpoint 1

**Latest disposition: C1 boundary accepted at f76093c.** The
[fourth review](#fourth-review-of-f76093c) closes the remaining R2b and R6
findings and reports no new findings in the reviewed scope. Opus can proceed
with C2/C3 on this boundary. This is not full F4 completion or merge approval.
Earlier rounds remain below with their original commit references and results.

2026-09-17. Reviewed implementation **d11608b** and review brief **5a33242**
on `opus/font-widgets`, based on F2.5
`788de9900282e941a7a93aa110ffa69154daa066`.

This answers [F4-C1-REPORT.md](F4-C1-REPORT.md), including all four boundary
questions. It is an API/ownership/transaction review before C2/C3, not F4
completion or merge approval. No implementation changes were made during review.

**Disposition: keep the general approach, but correct R1–R4 before building the
editor checkpoints on this boundary.** The deferred textfield/textview/Scribe
migration and bounded long-line work are not findings against C1.

## Findings

### R1 — P1: retain the allocator owner while context destruction is BUSY

Location: `userland/libos64/ui_font.c`, `os64_ui_font_release`, lines 530–541;
allocator callback `binding_free`, lines 85–89. Line numbers refer to d11608b.

The release function clears `ui->font`, drops its set, ignores the result of
`os64_text_destroy`, and frees the binding. A caller-owned candidate or retained
run can keep the context BUSY. That surviving context still uses the binding
as its allocator callback context, so freeing it makes subsequent release unsafe.

Reproduction: obtain the UI context, prepare a candidate, bind it (which retains
its own reference), release the UI while retaining the caller's candidate
reference, then release the candidate. ASan reports heap-use-after-free in
`binding_free` at line 88; the binding was freed at line 541.

Required correction: expose BUSY/refusal while keeping the owner valid, or
implement explicit deferred/reference-counted context ownership. Do not free the
allocator context while fonts, sets or runs can still call it. This matters to
F5 candidate lifetime and C2 retained editor runs, not just the fixture.

Regression: exercise both retained sets and runs across attempted UI teardown,
then release them and finish teardown without leaks or invalid accesses.

### R2 — P2: propagate preparation errors instead of substituting bitmap metrics

Locations: `ui_font.c`, `faces_resolve` lines 130–159, `role_run` lines 235–250,
`os64_ui_text_width` lines 260–274, and `os64_ui_draw_text` lines 308–342.

An unsuccessful F2 layout becomes a NULL run. Width and paint then silently use
the legacy bitmap path, even when the installed or candidate face is an outline
font. The application planner receives an ordinary integer width, so it cannot
propagate the allocation failure to the coordinator. Space measurement also
silently substitutes a fixed tab interval when preparation fails.

Reproduction with DejaVu Sans 24px: deny allocation during planner measurement of
`WWWW`. Adoption returns OK with a staged width of **32px**; the adopted face
actually needs **96px**. Separately, deny allocation during painting after a
successful bind: the rendered pen becomes **32px** for the same 96px string.

Required correction: use status-bearing measurement/preparation or a checked
transaction error mechanism, and propagate failures through the adoption result.
A failure cannot produce a successful plan measured in another font. Prepare and
retain the runs/resources needed for coherent initial painting before commit;
per-draw layout allocation followed by bitmap substitution does not satisfy the
F2.5 adoption contract. Caption mutation needs an explicit invalidation/update
strategy, not an assumption that raster-mask caching makes layout infallible.
The deliberate builtin startup fallback is a separate policy from failure while
preparing or painting an adopted outline face.

Regression: inject allocation failures into space/tab resolution, planner width
measurement, and adopted-caption preparation; verify old identity/layout remain
usable on refusal. Verify commit and the first coherent paint need no new layout
allocations. Exercise caption changes through the chosen invalidation mechanism.

### R3 — P2: make planner registration report failure

Location: `ui_font.c`, `os64_ui_font_planner`, lines 402–415; declaration in
`os64/ui.h`, lines 345–361.

Registration calls `binding_ensure`, which allocates, but returns void. If that
allocation fails, registration silently disappears. A later successful context
creation/adoption has no planner and can install a face without application
layout preparation.

Reproduction: fail the binding allocation at registration, allow subsequent
allocations, prepare a candidate and adopt it. Result: **OK, zero planner calls**.
The caller has no result to check when installing its required layout adapter.

Required correction: return an explicit status (and check it in clients), or make
registration allocation-free with reliable storage in the UI. Validate callback
combinations: an owned plan needs commit and discard paths; all-NULL can disable
the planner. Define who frees partial application staging when prepare fails.

Regression: failed registration must be observable and must not silently allow
unplanned adoption. Exercise successful preparation followed by another consumer
or barrier refusing, checking that discard runs once and old state remains intact.

### R4 — P2: intersect drawing with the primary row clip

Location: `ui_font.c`, `os64_ui_draw_text`, particularly line 340.

The helper resolves primary row metrics but passes the caller's entire clip to
`os64_text_draw`. A caller clip can cover a widget or surface larger than the
primary line. A taller fallback/missing marker then paints outside that line.
F2.5 explicitly keeps primary row pitch fixed and requires row clipping.

Reproduction: DejaVu Sans 8px, an unsupported W1 scalar (the missing marker),
text top Y=20, and a surface-sized clip. The primary row is **[20,30)**, but
painted pixels span **Y=16–31**, including **24 pixels outside the row**.

Required correction: intersect the incoming clip with the primary row's vertical
bounds before drawing, while retaining the caller's horizontal clip/overhang
policy. Apply the same rule wherever fallback painting is permitted.

Regression: a small primary with a taller fallback/marker must leave adjacent
rows unchanged, including with a caller clip larger than one row.

## Answers to the four boundary questions

### Q1 — registered application planner: yes, with checked failure/ownership

The registered plan/commit/discard seam is reasonable and can remain on the UI.
It keeps application layout inside the same consumer transaction as widgets and
editor state. Candidate measurement must remain separate from installed bounds.
R2/R3 are prerequisites: registration and measurement failures must reach callers,
and successful preparation must own sufficient staged state for infallible commit.
Define cleanup ownership on failed preparation and abort. Require a complete
callback trio when a plan owns resources; all-NULL is the no-planner case.

This approves the direction, not proof that Scribe's eventual plan is complete.
C2 must still demonstrate document/help state, source offsets, focus, selection,
scrolling, and application layout changing together as specified in the handoff.

### Q2 — class metrics callback: yes, with a narrow responsibility

A class hook for font-dependent metrics and a cached list pitch are sensible
while theme `font.w/h` remain frozen. Keeping `os64_ui_listbox_rows(list, theme)`
is acceptable. Derivation/application that occurs at commit must not allocate or
fail. Any fallible work belongs to preparation.

The added public struct field requires rebuilding affected clients/classes.
The initializer adjustments are reasonable consequences of that source/API
change; final integration must deploy matching library and client builds.
The callback should provide natural/minimum metrics, rather than own the final
allocated rectangle as proposed in Q3.

### Q3 — class ownership of bounds.h: change this before C2

The class should own natural/minimum height; application/layout should own the
allocated bounds. Add an explicit automatic-size policy or equivalent distinct
state. Do not infer application intent from zero versus a value previously
written by layout, and do not require applications to repeatedly repair their
chosen height after attach/font changes.

The failed natural-height experiment demonstrates that `os64_ui_stack_vertical`
must consume that policy correctly; it does not establish that overwriting
allocated height is the right boundary. Stage the candidate natural/minimum
metrics and the resulting layout together. A candidate that cannot fit should
be refused before changing active rectangles.

Test automatic and explicitly allocated heights across attach, relayout and
font replacement. Include a padded/taller control so the test is not satisfied
merely by the builtin font reproducing a 16px label.

### Q4 — context ownership: support explicit application-owned sharing

A shared context with separate immutable sets already permits independent
previews. A context is not an active-font singleton. Recommend one explicitly
owned context per serialized application UI group/thread as the usual choice,
with multiple UI bindings able to share it and hold distinct active sets.
Keep isolated contexts possible where budgets or lifetimes call for them.

The public boundary should accept/retain a shared owner (or otherwise explicitly
borrow an application-owned context with enforced lifetime), rather than force
context ownership to equal one UI binding. Resolve R1 as part of this ownership
change. Whole adoption groups must use a compatible context. Reject a mismatched
candidate instead of silently measuring through an unrelated context.

Additional policy probe: binding a candidate from another UI's context currently
returns OK, but its 96px `WWWW` measures as 32px because F2 rejects the context
mismatch and the wrapper takes bitmap fallback. The report instructs callers to
use the binding's own context, so this is not ranked as an additional finding;
it demonstrates why shared-context support and rejection rules must be explicit.

## Verification and reproducibility

- Read the complete C1 diff and report against the pinned F2.5 boundary.
- Independently reran the supplied real-backend host suite: **85 checks, zero
  failures**, ASan+UBSan, default O2. LeakSanitizer disabled.
- Built targeted probes against the actual C1 sources and the pinned backend;
  observations and the ASan failure above were reproduced.
- Diff whitespace check passed; the implementation worktree was clean at review.
- Did **not** independently repeat the full cross-build or QEMU evidence. Their
  reported results are not presented here as reviewer-run validation. No hardware
  validation, completeness acceptance, push or merge was performed.

All handoff artifacts live in this repository, not in a session scratchpad:

- [Observed results](f4-evidence/c1-review/observations.txt)
- [ASan teardown trace](f4-evidence/c1-review/release-asan.txt)
- [Baseline host result](f4-evidence/c1-review/baseline-host.txt)
- [Targeted probe source](f4-evidence/c1-review/repro.c)
- [Portable runner](f4-evidence/c1-review/run.py)

From the worktree root:

```sh
python3 docs/fonts/f4-evidence/c1-review/run.py --output /tmp/f4-c1-review-reproduction
```

The runner first rebuilds the supplied real-backend suite, then builds/runs the
probes. `release` is expected to exit nonzero under ASan on the reviewed commit;
other probes print the observed behavior and do not assert correctness through
their exit code. Compare with `observations.txt`. The committed observations and
trace came from the original review; the runner packages that source for reuse.


## Follow-up on 5357245

2026-09-17. Reviewed **5357245**, including the implementation delta from
5a33242, the report's **After the review** reply, revised tests and original
probe adaptations. This remains a C1 boundary review. No production code,
shared F0/F2.5 contract, or C2/C3 work was changed by this review.

### Disposition of the first round

| Item | Result |
| --- | --- |
| R1: allocator-owner use-after-free | Closed for the original defect. Retained sets/runs no longer call a freed binding. See the teardown policy observation below and new R5. |
| R2: failure propagation and prepared paint state | Partially fixed; remains open as R2a–R2c below. Status-bearing measurement and direct-draw refusal now work. |
| R3: planner registration | Closed. Registration allocates nothing and rejects an incomplete callback trio. |
| R4: primary-row clipping | Closed. The original small-primary/large-marker probe paints zero pixels outside the row. |
| Q1: application planner | Direction accepted; R2c demonstrates a remaining ordering/data-flow gap between application geometry and widget preparation. |
| Q2: class hooks | Prepare/commit/discard plus metrics are a reasonable split. Add a complete lifetime path for class-owned resources (R5). |
| Q3: natural versus allocated height | Accepted direction. `natural_h`, `auto_h`, and the fixed-height setter separate the responsibilities; the supplied height-policy checks pass. |
| Q4: shared context | Accepted direction. Explicit borrowing and rejection of a foreign candidate are implemented; the supplied sharing checks pass. |

### R2a — P2: propagate failure while resolving the space/tab interval

Location at 5357245: `userland/libos64/ui_font.c:130–159`, called without a
status check from `consumer_prepare:688` (also used by startup binding).

`faces_resolve` still returns void. Failure to lay out the space still becomes
`8 * OS64_FONT_GLYPH_W * OS64_FONT_UNIT`, after which adoption can return OK.
This is the space/tab path explicitly included in original R2, independently
of the now-correct public measurement function.

Reproduction: DejaVu Sans **16px**, deny one allocation during adoption, then
allow allocations and measure a space and tab. Denials **2 through 13** each
produce **adoption OK, space 5px, tab 64px**, although eight spaces require
**40px**. The wrong interval persists in the installed faces after memory is
available again. The 24px face used in the earlier width probe can conceal
this error because its eight-space interval matches the bitmap interval.

Required correction: make face/interval resolution report failure and carry
that status through preparation with complete cleanup. Distinguish a defined
policy for a valid zero-width space from an allocation/layout error. Cover
all callers, including startup binding; live replacement must retain the old
identity and usable state when resolution fails.

Regression: walk denials through each role's space resolution using a face
whose interval differs from 64px; require refusal or the exact successfully
resolved interval, never a successful substitution.

### R2b — P2: paint button alignment from its retained run

Location: `userland/libos64/ui.c:549–562`, `button_paint`.

The caption is retained during adoption, but the actual button painter calls
`os64_ui_text_measure` again and ignores its status. This performs a new
layout allocation on each paint. On refusal `tw` remains zero, while
`os64_ui_draw_text` successfully paints the already-retained caption. The
comment that failed measurement implies no painting is false for this path.

Reproduction: adopt DejaVu Sans 24px on a 200px-wide button labeled `WWWW`.
The first call to the **actual button class painter** makes **five allocations**.
With allocations refused, the existing run still paints, but its leftmost ink
moves from **X=52 to X=100**; **876 pixels differ**. The window has already
committed the new font, so there is no preparation refusal left to return.

Required correction: obtain the alignment width from the same retained/staged
run used for painting. Resolve a changed caption once through a coherent
update path; failed remeasurement must not pair a zero width with an old or
successfully retained run. Review sibling painters for independent measure
and draw work.

Regression: after adoption, deny allocations and invoke the real button
painter, not just `os64_ui_draw_text`. Require identical placement/pixels and
no layout allocation for unchanged content.

### R2c — P2: stage list rows against the application's candidate bounds

Locations: `userland/libos64/ui_list.c:45–55,153–157` and
`userland/libos64/ui_font.c:695–704`.

List preparation uses the candidate row pitch but **live `w->bounds.h`**.
`stage_tree_runs` runs before the application planner, so it cannot account
for a candidate layout that gives the list more height. After commit, newly
visible rows have no retained slots and must allocate in their first paint.
The application cannot fix this through the current hook by writing live
bounds during prepare: the shared transaction contract forbids that mutation.

Reproduction: a three-item list, DejaVu Sans 8px, live height **22px**; the
application planner stages height **58px** and applies it only at commit.
Adoption returns OK with **three visible rows but one retained row run**.
The first paint attempts **two refused allocations**, leaving the other two
labels blank; with memory available it makes **ten allocations**. The two
surfaces differ by **296 pixels**. This uses unchanged labels and no event or
resize between preparation and painting.

Required correction: make candidate application geometry available to widget
resource preparation before the transaction succeeds. A planning/preparation
split or an explicit staged-geometry view can do that without modifying live
bounds. Include the candidate visible range/scroll position as applicable.
Do not solve this by moving fallible work into commit or by staging every
item in an unbounded list.

Regression: application layout grows and shrinks a list as part of adoption;
first paint of every candidate-visible row must use prepared resources under
allocation denial. This ordering matters directly to C2 textview windows.

### R5 — P2: release class-owned list runs during window teardown

Locations: `userland/libos64/ui_font.c:779–801` and
`userland/libos64/ui_list.c:84–98`.

`release_tree_runs` releases only the generic widget `run` and `run_staged`.
The new listbox stores its active runs in `row_runs` and its pending runs in
`row_runs_staged`; teardown reaches neither array. List commit/discard cover
replacement/abort, but provide no normal destruction path for the active
array. The owned text context remains BUSY solely because of libui's own
list resources.

Reproduction: adopt a list with **one visible retained row**, release the
caller's candidate, retain no external run, then release the UI. Both the
initial release and retry return **BUSY**. The list retains its run and the
binding retains **810,587 bytes**. The probe then manually releases the
list's internal run/array, at which point teardown returns OK and the host
allocator ledger reaches zero. That manual internal cleanup is diagnostic,
not a proposed application responsibility.

Required correction: give class-owned resources an explicit teardown path
and invoke it for the tree, including active and staged arrays as applicable.
Cover borrowed contexts as well: they must not retain window-owned runs after
the UI is gone. Preserve the allocator owner for genuinely external survivors.
A class destroy hook or another ownership mechanism is fine; generic caption
cleanup alone is insufficient for C2's larger retained state too.

Regression: successful list adoption and teardown with no external handles
must complete without BUSY/leaks, for owned and borrowed contexts. Also test
aborted staging and an external survivor that genuinely requires BUSY.

### Teardown policy observation for R1/Q4

The original lifetime fix works, but `BUSY` currently means **partial teardown
with the allocator owner retained**, not the unchanged usable window promised
by the header/report. With a caller-held candidate, `busy-state` observes
BUSY, a cleared active-set identity, and `WWWW` changing from **96px to 32px**
on subsequent measurement. The release implementation drops runs/set before
attempting context destruction and permits builtin reinitialization afterward.

This is recorded as a boundary-policy observation, not a second version of the
closed use-after-free finding. A teardown-only retry API can be valid, but C2/F5
callers must not rely on an unchanged usable UI after it. My recommendation is
to make that lifecycle explicit if retaining destructive teardown; if unchanged
refusal is the intended guarantee, the implementation needs to preserve the
active state. R5 requires a code fix under either choice.

### Follow-up evidence and handoff

Independently reran the corrected real-backend host suite: **197 checks,
zero failures**, O2, ASan+UBSan. Independently rebuilt/reran all **six original
probes** against the new API; their results match the corrected observations.
The follow-up probes link the real `ui.c`, `ui_controls.c`, **`ui_list.c`**,
font adapter, F2/provider, and pinned FreeType backend. Their harness is O1
with ASan+UBSan and uses the freshly built O2 backend objects. LeakSanitizer
was disabled; the probes separately count allocator blocks and finish at zero
(after the expressly identified manual cleanup for R5).

The supplied first-paint test calls the draw helper directly, so it does not
exercise button remeasurement. The supplied host runner does not link
`ui_list.c`, so its 197 passing checks cannot establish list preparation or
teardown correctness. The new probes cover those concrete missing paths.

Repository handoff artifacts:

- [Follow-up probe source](f4-evidence/c1-review/r2-repro.c)
- [Follow-up runner](f4-evidence/c1-review/r2-run.py)
- [Observed follow-up results](f4-evidence/c1-review/r2-observations.txt)
- [Independently rerun baseline](f4-evidence/c1-review/r2-baseline-host.txt)
- [Independently rerun original probes](f4-evidence/c1-review/r2-original-probes.txt)

From this worktree:

```sh
python3 docs/fonts/f4-evidence/c1-review/r2-run.py --output /tmp/f4-c1-r2-review
```

The default command rebuilds the baseline and original probes before the new
ones. `--baseline <existing-baseline-dir>` can reuse compatible objects from
this checkout. Probes print observations rather than treating exit zero as
approval; compare the recorded results and the required behavior above.

No independent full cross-build, QEMU rerun, or hardware validation was
performed for this follow-up. Those claims in Opus's report remain his evidence.
No implementation edits, commit, push, merge, or C1 completion approval were made.


## Third review of 0cfb485

2026-09-17. Reviewed **0cfb485**, its complete production delta from 5357245,
the new tests, fixture changes, and the report's **After the second review**
reply. This is still C1 boundary review, not F4 completeness or merge approval.

### Dispositions

| Item | Result |
| --- | --- |
| R1, R3, R4 | Remain closed for their original defects; the original probes rerun successfully. |
| R2a: space/tab failure | Closed. Resolution now reports errors; binding resolves before replacing active state; adoption cleans up and refuses. The denial probe finds zero successful adoptions with a substituted interval. |
| R2b: button measurement/paint | The unchanged-caption case is fixed: zero allocations and identical pixels. The changed-caption failure path below remains open. |
| R2c: candidate list geometry | Original case closed: three visible rows now have three prepared runs; both paints allocate zero and match. New helper issue R6 below affects staged parent layout. |
| R5: class-owned teardown | Closed for the reproduced leak. The list destroy hook releases both arrays; release and retry return OK with zero retained rows and zero binding bytes. |
| Teardown policy | Accepted as destructive teardown with BUSY meaning incomplete, as now stated in the public header. This was an allowed lifecycle choice, not a demand for nondestructive release. |

The shared-context, class lifecycle, natural-height, and staged-geometry API
directions remain reasonable. The two implementation issues below do not
require reopening F0/F2.5 or expanding this checkpoint into C2/C3.

### R2b follow-up — P2: stop painting when the caption's width preparation fails

Location at 0cfb485: `userland/libos64/ui.c:574–585`, `button_paint`;
`userland/libos64/ui_font.c`, `slot_run` and `os64_ui_run_width`.

The painter still ignores the status returned by its width helper. The new
shared slot fixes unchanged text, but a caption change requires a new layout.
If that layout fails, `tw` stays zero and the old slot remains. The subsequent
`os64_ui_draw_text` retries layout for the new bytes; if this allocation now
succeeds, it paints the new run at the position calculated from zero width.

Reproduction: adopt a 200px button with `iiii`, change its caption to `WWWW`,
and deny **only the next allocation**. The actual class painter makes **14
allocation attempts** and puts the ink at **X=100**. Its next unrestricted
paint puts the same caption at **X=52**; **876 pixels differ**. Both paints
use the same installed 24px face. This is ordinary caption mutation, explicitly
supported by the slot's byte comparison, not a second font adoption.

Required correction: check the width/preparation status before proceeding to
caption paint. On failure, preserve a coherent update policy and retry on a
later paint; do not perform a successful second layout using placement from
a failed first one. A successfully prepared run should supply both width
and ink. No change to the agreed unbound startup fallback is needed.

Regression: invoke the real button painter after a caption change, with one
allocation denied and subsequent allocations available. A failed update may
omit the new caption under the chosen failure policy, but must not paint it
at a zero-width-derived position. Keep the unchanged-caption zero-allocation
regression too.

### R6 — P2: use the staged parent's rectangle when stacking staged children

Location: `userland/libos64/ui.c:457–462`, `stack_vertical_into`.

The staging helper reads a child's planned height, but computes X, Y, and
width from **`parent->bounds`** in both modes. A planner that stages a moved
or resized panel and then stacks its children commits the panel's new
rectangle together with child rectangles derived from its old one. This also
breaks nested staged stacks: an outer stack stages a container before an
inner stack lays out its children.

Reproduction: stage a parent from `(0,0,200,240)` to `(40,20,160,220)`, then
call `os64_ui_stack_vertical_staged` with 6px padding during adoption.
Adoption succeeds with the new parent, but the child becomes
**`(6,6,188,29)`**. Its correct X/Y/width are **`46,26,148`**. The child is
outside the parent's new content rectangle and may be clipped or overlap
adjacent UI. Live bounds were not changed during preparation in this probe.

Required correction: take the parent's planned rectangle when staging and
its live rectangle for immediate layout. Derive all three coordinates from
that selected rectangle; preserve the existing child-height policy.

Regression: stage a parent move/resize and a nested container stack, then
check committed child rectangles against candidate parents. Also abort a
staged layout and verify live rectangles are unchanged. The current list
regression stages the list's own height directly, so it cannot catch this
parent-to-child propagation error.

### Evidence recovery and fresh verification

The overwritten round-2 raw capture survived in my original temporary output.
I restored `r2-observations.txt` **byte-for-byte from that capture**, replacing
the reconstructed copy. Its observed results match Opus's reconstruction.
`baseline-host.txt` matches the tracked original at 5357245. I also restored
the previous review's two linked baseline/original-probe receipts, which were
absent from the submitted commit, from their surviving original captures.
See [the provenance receipt](f4-evidence/c1-review/r3-provenance.md) for exact
sources and a SHA-256 of the restored raw output. No historical result was
relabelled as a fresh run.

Independent current validation:

- Real-backend host suite: **269 checks, zero failures**, O2, ASan+UBSan.
- Six original probes: rebuilt and rerun successfully against the current API.
- Five round-2 probes: rebuilt and rerun after repairing a probe-local
  `list_label` name collision with the newly expanded included host suite.
  As submitted, that runner did not compile; the repair renames only its
  callback and call site. Production code is unchanged.
- Two new probes: actual button painter with a changed caption and one-shot
  allocation denial; staged-parent stack through a complete adoption. Both
  reproduce the findings above under ASan+UBSan and finish teardown with OK.
- `git diff --check`: clean.

New probe harnesses use O1 and the freshly compiled O2 pinned-backend objects.
LeakSanitizer is disabled. No independent full cross-build, QEMU repeat, or
hardware run was performed; Opus's reported guest results remain his evidence.

Fresh artifacts (kept separate from the recovered historical captures):

- [269-check host receipt](f4-evidence/c1-review/r3-baseline-host.txt)
- [Round-2 probes against 0cfb485](f4-evidence/c1-review/r3-prior-probes.txt)
- [New probe source](f4-evidence/c1-review/r3-repro.c)
- [New probe runner](f4-evidence/c1-review/r3-run.py)
- [New observed results](f4-evidence/c1-review/r3-observations.txt)

```sh
python3 docs/fonts/f4-evidence/c1-review/r3-run.py --output /tmp/f4-c1-r3-review
```

This rebuilds the baseline and six original probes before the new cases.
The repaired `r2-run.py` separately reruns the five round-2 cases. Both runners
accept `--baseline` to reuse compatible objects from this checkout. Probe
exit zero means execution completed, not that the behavior is correct.

Only review documents and evidence files were changed. No production fixes,
commit, push, merge, or C1 completion approval were made.


## Fourth review of f76093c

2026-09-17. Reviewed **f76093c**, including the complete production/test delta
from 0cfb485 and the report's **After the third review** reply.

**No new findings in this review. C1 boundary accepted for C2/C3.**

- **R2b follow-up closed:** `button_paint` checks the width preparation status
  and skips the caption when preparation fails. The changed-caption probe
  makes one refused allocation and paints no misplaced text. A subsequent
  paint places the caption correctly. A sweep across 24 one-shot allocation
  denial positions produces either the correct pixels or no caption; it never
  paints a new caption using a failed measurement's position.
- **R6 closed:** staged stacking selects the parent's planned rectangle,
  while immediate layout selects its live rectangle. The original probe now
  commits child `(46,26,148,29)`, matching the new parent and padding. A nested
  stack also places the grandchild from its candidate parent. Refusal at a
  barrier after successful preparation preserves every live rectangle and
  discards staged bounds/runs; a subsequent successful adoption works.
- **Earlier closures stand:** reran the original six, round-2 five and round-3
  two probes against this head. Tab-resolution refusal, allocation-free first
  painting, candidate list-row coverage, teardown, registration and clipping
  behave as recorded in their corrected dispositions. Destructive BUSY
  teardown remains the accepted lifecycle policy.

The callback-name changes in the host harness compile with the supplied
review probes without reviewer repairs this round. Historical captures remain
separate from fresh evidence; the recovered `r2-observations.txt` still has
SHA-256 `00101efe7405f8209add83f45d3456198a27a11f8b7841f4bd8f4af46bc250b9`.

### Independent verification

- Supplied real-backend host suite: **293 checks, zero failures**, O2,
  ASan+UBSan.
- **All 13 earlier probes** rebuilt and rerun. Their exit status alone is not
  a correctness assertion; the recorded outputs were checked against the
  required behavior.
- Supplemental review checks: **134 checks, zero failures**, covering 24
  caption-allocation denial positions, a nested staged stack, barrier refusal
  after preparation, cleanup, retry, and agreement with immediate layout.
- Supplemental harness is O1 with ASan+UBSan, linking the freshly compiled O2
  pinned-backend objects. LeakSanitizer is disabled.
- `git diff --check`: clean. No production source was changed in this review.

Fresh evidence:

- [293-check suite receipt](f4-evidence/c1-review/r4-baseline-host.txt)
- [Thirteen earlier probes on f76093c](f4-evidence/c1-review/r4-prior-probes.txt)
- [Supplemental check source](f4-evidence/c1-review/r4-check.c)
- [Supplemental runner](f4-evidence/c1-review/r4-run.py)
- [134-check supplemental receipt](f4-evidence/c1-review/r4-supplemental.txt)

```sh
python3 docs/fonts/f4-evidence/c1-review/r4-run.py --output /tmp/f4-c1-r4-review
```

The default runner rebuilds the baseline and original six probes before the
supplemental checks. The existing `r2-run.py` and `r3-run.py` reproduce the
later probes; each accepts `--baseline <dir>` for compatible backend objects
from this checkout.

### Acceptance scope

The four boundary answers now stand with the implemented lifetime,
status-bearing preparation, staged geometry, class cleanup and shared-context
mechanisms. C2 must still demonstrate the document/help model, unsaved bytes,
source selections, caret/scroll behavior and Scribe's complete adoption plan;
C3 must still demonstrate its bounded long-line strategy. Those are upcoming
checkpoint obligations, not reopened findings against C1.

This review did not independently repeat the full cross-build, QEMU or hardware
validation. Opus's reported build/guest results remain his evidence. Acceptance
here is the requested C1 API/ownership/transaction boundary decision, not F4
completion or authorization to merge. No commit, push or merge was performed.
