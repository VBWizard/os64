# Quinn review — F4 checkpoint 1

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
