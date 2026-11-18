# F15 checked rejection consumption

Private C1/C2/C4 milestone after checked setup (#654), based on userland
15f050e5. C3, public checked activation, graph migration, reclamation and
slot reuse remain gated. Quinn owns engine patch 0043; this milestone changes
no engine patch, manifest, DOM layout or job-dispatch policy.

## Contract and small PR plan

One coherent implementation PR adds ticket-based script-host registration and
consumption of one retained rejection, with shared host/guest failure proofs.
Both entry points remain private in `runtime/owner_runtime.h`, with no public
exports or wrapper fields. The old raw take and public host APIs keep ABI 5.

Registration resolves a live holder and delegates the existing status-only
setter. It creates no raw export. Native host opaque storage lives until
successful detach or root shutdown; keep a holder available for detach.
Closing a ticket does not detach a host. Detach after failure/retirement keeps
the old cleanup semantics. Raw rejected/handled/exception/router callbacks
still record the compatibility-retention veto when invoked; selecting an
empty callback table keeps rejection records without exporting raw views.

The consumer requires a valid ticket, hosted active realm, callback/name and
distinct caller-owned `consumed`/outcome storage. Pending ordinary microtasks
refuse consumption. A healthy empty poll returns OK/false with no callback,
loan or fresh turn. Refusal before claim preserves the record and consumed
output. Known allocator/global failure takes precedence over local capacity.

For a record, acquire an independent owner loan before engine/settings
admission. Select and detach the oldest matching record inside the admitted
adapter, so no selected node crosses preparation. Publish `consumed=true`
only after claim; later exception/failure cannot undo consumption. The callback
borrows the original promise, reason and host tag and returns one owned value
or JS_EXCEPTION. No scope/view/value escapes persistently without explicit
compatibility export; this is not a persistent value-root ticket facility.

The detached record owns both roots through callback, returned-value cleanup
and exception diagnostics. Release them under the active guard and original
loan before leave; reentrant holder close, promotion, retirement and queue
mutation cannot free this detached node. Fresh idle consumption owns a turn
and drains its microtasks before loan end. Between explicit-task checkpoints,
consumption borrows the original task, source, deadline and counters; it leaves
queued work for that task's checkpoint/end. It cannot renew a task budget or
recover a failed task. Normal nested execution stays BUSY.

Shared execution factoring moves only invocation/result diagnostics into a
helper used by fresh turns and task borrowers; ordinary source/scoped setup
semantics remain. Empty counts and an unset compatibility veto grant no owner,
realm, provider or native graph release authority. Freeing a queue's own roots
is ordinary value cleanup under existing retained-realm policy.

## Proof obligations

Cover both teardown policies with actual mapped primary/children; FIFO and
realm filtering; empty/refused polls; tag preservation; close/promotion;
recursive outcome protection; callback result/exception/rejection finalizers
under guard+loan; cancellation and allocation cuts; queued work and task-budget
inheritance; legacy raw take/host delivery veto; failed/retired detach; repeated
queue cleanup and exact native/engine balance. Host tests run target and
instrumented engines with ASan/UBSan/LSan enabled. Shared cases run in an actual
QEMU child with an exact parent-observed badge and target heap checks. Named
mutants must fail behavioral assertions. Recheck affected script-host/FR DOM
integration and public headers/exports/layout. Final evidence is recorded
against the implementing commit and prepared engine identity before review.

## Implementation and validation

`runtime.c` factors invocation/result diagnostics into `invoke_body` without
changing ordinary source/scoped-run budget setup. The checked consumer keeps
queue ownership in its stack call after unlink, frees both values and the node
before leave, and ends its independent loan after fresh-turn draining. A task
borrower bypasses fresh-turn setup and preserves the task owner even when the
consuming host belongs to a child. The legacy raw-take implementation is
unchanged. The tracker comment now describes host claims through either path.

The runtime wrapper remains **13,080 bytes** (target DWARF). The shared export
set matches #654: **235 engine and 89 runtime symbols**, with no new public
checked entry points. ABI 5 / bytecode 6 and prepared engine
`3e9a81ec0428aaf6456af21c9d8cf5b152793b747ebb503bd9cc86ff5a91afd4`
are unchanged. This cohort includes merged navigation #655. Separately
coordinated engine receiver PR #656 retains a configured global's embedding
context; that edge belongs in the future reachability census and is not a
collection receipt or an engine change in this milestone.

Final commands and results (target-core / instrumented-core where paired):

- `ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--owner-rejections bash tools/test_js_runtime_host.sh`:
  **785 / 3,005 checks**, zero failures. The final pass reused matching engine
  objects and recompiled the driver/runtime after fixture strengthening.
  Both teardown policies cover **8 / 119 consumer allocation cuts** each,
  with native/engine/metadata accounting and non-reclaiming leak verdicts.
- Complete runtime probes, raw construction, compile allocation sweeps and the
  five intentional invariant modes: **40,295 / 137,455 checks**, zero failures,
  zero live native allocations; compile sweeps pass and invariant exits retain
  the exact full fatal badge. ASan/UBSan/LSan stay enabled outside the sandbox.
- `python3 tools/test_js_owner_rejection_mutations.py --artifacts ... --output ...`:
  **24 behavioral counterexamples rejected**, both engines. Wrong queue,
  reason/tag, early consumed output, replaced task owner, renewed task budget,
  early loan/guard release, missing primary-owner loan, skipped health,
  empty-poll admission and missing raw-take veto trigger named assertions.
  Sanitizer reports, crashes and invariant exits do not count as detections.
- `python3 tools/test_js_owner_runtime_guest.py --base-image ... --boot-iso ... --output ...`:
  **1,332 shared checks**, target heap integrity/accounting/baseline pass;
  parent observes exact **0x4f520800** and prints the completion verdict.
  The driver clones the preserved disk/boot inputs and freezes the actual
  consumer, runtime/core/ticket/issuer hashes and five-file executable/library closure.
- Fresh DOM cohort, then final runtime relink: browser jobs **801 / 801**,
  callback reports **1,118 / 1,118**, report retirement **616 / 616**, recovery
  host **3,198 / 3,198**, Promise settings **246 / 246**, and child installation
  engine **1,330 / 1,258** checks, zero failures, with ASan/UBSan/LSan enabled.
- `bash tools/test_js_contract_headers.sh`, `python3 tools/test_js_prepare.py`
  (six tests), `python3 tools/test_js_teardown_audit.py`, shipping CLI/shared
  cross-build, export comparison, tracker validator, `git diff --check` and
  `bash tools/stale_refs.sh` pass. The stale-reference report has no retired
  prose names or new superlative claims.

The shared oracle uses real queue-owned native reason objects, not fabricated
values. It checks FIFO across two owners, returned-value and diagnostic cleanup
before reason release, reentrant handling of another queued promise, sixteen
repeat cycles, close/promotion, recursive outcome protection, task-owned queued
jobs and legacy rejected/handled/exception/raw-take retention. Host-only clocks
prove a half-spent task cannot gain a new deadline; capacity, issuer, clock,
cancellation and previously unobserved allocator faults preserve preclaim
ownership/output. An empty poll does not allocate, acquire a loan or consult a
fresh deadline.

This is private checked export groundwork. The engine/native graph is not fully
classified, hosts/opaque storage retain their established native contracts,
and foreign value/code ownership is not certified by the consuming owner's
loan. No owner/provider backing is released, no realm slot is reused, no
browser reporting policy changes, and no new P5/visible-frame acceptance is
claimed. C3 and F15's reachability/teardown/reuse exit remain open.
