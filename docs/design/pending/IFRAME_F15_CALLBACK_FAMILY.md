# F15.21 — bounded private C-root callback family

## Baseline task list — 2026-11-19

- [x] Select private interfaces and charge a finite record pool before runtime publication.
- [x] Implement actual-incumbent capture, stale-safe numeric keys and scalar counts.
- [x] Compose scoped callback invocation with existing entry/incumbent settings loans once.
- [x] Implement guarded close bookkeeping and pre-context managed-root shutdown.
- [x] Prove roles, rollback, stale identity, reentry and bounded cleanup with shared host/guest cases.
- [x] Run both engine/root profiles, leak-enabled failure controls, affected integration and audits.
- [x] Freeze matching evidence and publish #683 for Codex review; address findings without merging.

Implementation PR **#683**. Based on merged userland `4602ef25` and reviewed contract #675
([IFRAME_F15_CALLBACK_CAPTURE.md](IFRAME_F15_CALLBACK_CAPTURE.md)). The child
F.15.21 is independent of the later F4 C3 input consumed by F.15.22; no C3
representation, public activation, DOM migration, physical owner release or
realm/resource-slot reuse is selected here. Additional discoveries are recorded
as dated **ADDED** notes below, separately from the baseline list.

## Interface and implementation choices

The explicitly selected private constructor reserves callback cells in the
charged owner-metadata arena. Callback keys contain runtime incarnation and a
fresh record serial. Capture takes a live checked scope and retains the actual
mapped incumbent owner independently of the admitting owner. The callback
object is a C root; its associated-entry holder remains the host's separate
obligation. Numeric summaries distinguish reserved/live/closing cells and
temporary invocation roots; they grant no release or security authority.

Invocation lends the callback object to a new private native body under the
same settings dispatcher as #667. Entry and incumbent each consume one loan;
no duplicate dispatcher layer, raw result, reporting hook, checkpoint or new
budget is introduced. Clear invalidates the key and keeps incumbent/storage
through finalizers. Root shutdown drains managed records before contexts and
engine teardown without hiding unrelated legacy raw roots.

## ADDED implementation findings

- **ADDED 2026-11-19 — bounded recursive cleanup:** native finalizers may close
  another record. Clearing records therefore stay reserved, nested closes queue
  invalidated records, and one guarded loop releases them. Capture/invocation
  refuses while that cleanup is active, so the loop cannot chase newly created
  managed roots or require a recursively growing native cleanup stack. This
  implements the baseline bounded close/drain obligation.

## Private boundary

`js_owner_runtime_create_with_callbacks` explicitly reserves a positive finite
callback capacity, alongside owner/holder/loan capacities. Checked arithmetic
and the actual allocator charge are admitted before engine construction.
Other constructors select no cells. Runtime wrapper layout and public symbols
match the merged base; the selected private metadata header grows. Zero and
overflow capacities refuse. Full cells/holders refuse without a published key
or value root. Fresh serials may be consumed by a refused reservation; issuers
never rewind or wrap.

`js_owner_callback_capture` accepts a trusted live same-engine object, without
property access, clock polling or exception consumption. It snapshots the
actual mapped incumbent, independently retains that owner's ticket and owns
one callback-object reference. No incumbent means refusal; the admitting or
primary owner cannot substitute. The host keeps associated entry separately.
Neither capture nor invocation removes a prior raw compatibility veto.

`js_owner_callback_invoke` resolves entry and captured incumbent, admits both
loans through the shared settings dispatcher, and duplicates the callback
before native reentry. The native body performs callback type/receiver/getter
and return conversion policy. Result/exception diagnostics and value cleanup
remain inside the selected settings frame. Closing and reusing the cell
cannot change the admitted snapshot. The caller's guard, task deadline, job
counter and reporting policy retain their existing meanings.

`js_owner_callback_close` invalidates the key before releasing its root. A
clearing cell and incumbent holder survive synchronous finalizers. Nested
closes queue already invalidated cells; one bounded outer loop drains them,
without new bookkeeping allocation, loans or clock reads. Value finalizers
still run. Ordinary wrapper execution and new capture/invocation refuse
during cleanup. Destruction invalidates and drains managed roots before the
first wrapper context release, under both teardown policies. Unrelated legacy
raw-root leaks still produce the existing leak report.

This family represents runtime-owned C roots. It does not replace engine-traced
edges, classify Streams cycles or migrate DOM callback consumers. Scalar zero
counts and callback-cell churn are not owner collection or realm-slot reuse.

## Evidence and reproduction

The engine is `0e34fab233ca0a3e3dfc4f8fabd76774231359899c8134892a4318e8232ac1a0`,
embedding ABI 5 / bytecode 6. Target wrapper DWARF remains **13,128 bytes**;
the **98** runtime-object public symbols match `4602ef25`. The shared library
has **335** exported symbols, with no private `js_owner_` symbol. No engine
patch or public header is changed.

- `ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--owner-callbacks bash tools/test_js_runtime_host.sh`:
  final **1,053 shipping / 1,125 instrumented checks**, zero failures. Both
  root policies exercise native object/string allocation cuts: **1 / 5**
  per capture/invocation phase. Clock, cancellation, expired deadline, partial
  loan admission, actual callback/holder issuer exhaustion and three initial
  constructor allocation cuts refuse without lost roots or capacity.
- The complete runtime suite passes **42,794 / 143,262 checks**, zero failures
  and zero live allocations. Raw construction, compiler allocation sweeps and
  five intentional invariant modes preserve their full target exit badges.
  A separately selected deliberate raw-root leak fixture reports the leak;
  it is negative detector evidence, not a positive leak-free teardown run.
- `--owner-callbacks-controls` passes unmodified in both engines.
  `python3 tools/test_js_owner_callback_mutations.py --artifacts ... --output ...`
  rejects **36 named behavioral counterexamples**: wrong roles/loans, borrowed
  roots, stale serial reuse/wrap, new/cleared raw veto, early cleanup, missing
  settings/cleanup guard, incomplete drain and renewed deadlines. Compile,
  sanitizer and invariant failures do not count as behavioral detections.
- `python3 tools/test_js_owner_runtime_guest.py --base-image ... --boot-iso ... --output ...`:
  **3,010 shared checks**, exact parent-observed **0x4f520d00**, target heap
  baseline/integrity/accounting pass. Source inputs and production objects
  are recorded with the copied guest executable/library closure.
- Fresh coherent DOM objects pass browser jobs **801 / 801**, synchronous
  settings **33,662 / 66,254**, callback reports **1,118 / 1,118**, Promise
  settings **246 / 246**, import settings **1,378 / 1,378**, WindowProxy
  **1,426 / 3,726**, Location exotics **1,932 / 3,052**, child installation
  engine **1,330 / 1,258** and recovery host **3,198 / 3,198**. The final runtime
  source independently reproduces the tested DOM runtime object.
- Header contracts, six preparation tests, teardown allocation/visibility
  inventory, production CLI (**199 / 40 checks**), tracker, diff and stale
  reference audits pass. Positive sanitizer runs retain ASan/UBSan and
  LeakSanitizer outside the sandbox with `detect_leaks=1`.

Independent native probes inspect the actual entry and incumbent loan rows,
check holder/cell survival in finalizers, and require an empty managed family
before wrapper context release. A reusable host wrapper slab places two
successive runtimes at the same actual address: copied old keys still refuse
the new incarnation. This is an allocator-controlled identity test, not a
promise to validate arbitrary freed pointers. The shared tests cover callback
interface getters/conversion, Proxy executable/current roles, self-close,
cell reuse, exceptions, retirement, staged setup, nested finalizer close and
128 bounded capture/close cycles. No P5 or public browser acceptance is claimed.
