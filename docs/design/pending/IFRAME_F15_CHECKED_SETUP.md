# F15 — checked staged native installation

Implementation PR: **#654**, source `d4a8c335`; merged-base integration `7abd1f2a`.

Proof baseline: merged `userland` `9ae58ef4` (#649–#653), embedding ABI 5,
bytecode ABI 6, engine digest
`3e9a81ec0428aaf6456af21c9d8cf5b152793b747ebb503bd9cc86ff5a91afd4`.
This follows [scoped native access](IFRAME_F15_SCOPED_ACCESS.md) and the
[export-lifetime proposal](IFRAME_F15_EXPORT_LIFETIMES.md). F15 stays in progress.

## Implemented private boundary

`js_owner_runtime_stage` uses checked owner admission for an actual retained
child slot, marks it staged and initializes its existing setup group before
returning its ticket. It returns no raw realm/context. Normal entry refuses a
staged ticket. Headroom, owner/holder capacity, actual charges, failed reservations
and cumulative slot retention keep the existing admission rules.

`js_owner_runtime_setup` resolves the original live ticket and invokes a trusted
native installer with an invocation-only scope/context. The installer frees
temporary values before returning OK or ADMISSION_REFUSED without an exception,
or EXCEPTION with its pending exception for wrapper diagnostics. Other native
statuses follow existing setup rejection rules. No persistent value root or
untracked native borrower is introduced. Explicit scoped promotion/export keep
their existing contracts; public raw paths have no ambient exemption.

An independent bounded loan begins after existing entry guards and before
setup preparation or transaction selection. Capacity/issuer refusal unwinds the
unprepared guard without a new deadline, view or candidate retirement. A known
raw allocator failure refuses as FAILED_RUNTIME before loan admission, preserving
the cleanup-required signal even when native code caught its engine exception.
An unrepresentable deadline unwinds the loan before installation; terminal clock,
allocator, cancellation and budget failures keep their existing global policy.

Once prepared, the common F3 path preserves native installation, exception/value
diagnostics, candidate microtask/rejection handling and failure retirement. The
loan spans diagnostic finalizers, both-lane rollback, group restoration and setup
unwind, including reentrant close of the admitting ticket. Rollback keeps the
group selected and discards microtasks and deferred registry tasks until both
lanes are empty, including work queued by native finalizers during discard.
It does not checkpoint unrelated sibling work.

Installers keep F3's private-bootstrap restrictions on sibling invocation,
external provider calls and candidate publication. An explicit compatibility
export preserves a C lifetime; activation/publication obligations still apply.

A successful setup can retain deferred FinalizationRegistry tasks under the
existing opt-in policy. Those tasks wait for activation and host selection; they
do not extend the native scope's lifetime. A scope/context must not be captured
for later use without an explicit compatibility export and its longer promise.
The public setup comments and contract are corrected to distinguish candidate
microtasks from deferred cleanup; the prior wording that all queued work drained
before success was false. This corrects prose about existing behavior.

`js_owner_runtime_activate` applies existing lifecycle, setup-complete, pending
candidate-microtask and health checks through the original ticket. It publishes
no raw pointer and opens no task/deadline. A closed/stale/foreign holder cannot
activate another owner; a promoted live holder can activate its original completed
candidate after the admitting holder closes. Activation enables native execution
admission, not F4 target publication, origin permission or security authority.

## Compatibility and gates

Legacy stage/setup/activate use the same common transaction and preserve ABI 5
lifetimes and compatibility vetoes. The new callback contract is explicit private
bootstrap, not a shorter interpretation of an old native signature. Local setup
failure retires the consumed slot; ticket close or cleanup releases no owner,
context, graph/native resource or quota. Retired candidates stay charged/mapped.
Zero holders and an unset compatibility veto remain insufficient for release.

No public profile, DOM ownership migration, engine patch, C3 representation,
per-owner reclamation or slot reuse is selected. Public declarations, ABI,
exports and default budgets are unchanged; header changes are comments only.
The opaque scope type moves within `runtime.c` without adding wrapper fields.
The wrapper remains **13,080 bytes**. Quinn's merged #651–#653 DOM/native-context changes are included in the baseline;
this milestone changes no context/publication contract.

## Validation

- Focused final host: **341 shipping / 1,631 instrumented checks**, zero failures.
  Actual installer allocation sweeps cover **18 / 147 cuts per root policy**;
  a duplicate holder preserves original identity after reentrant admitting-holder
  close, and native/engine cleanup balances under both teardown policies.
- Shared cases cover ticket-only staging, premature entry/activation, close and
  promotion, BUSY/outcome aliases, candidate jobs, diagnostic native finalizers,
  local refusal, deferred cleanup rollback and native-finalizer-enqueued work.
  Nested setup spends the enclosing two-job budget; a forbidden sibling job
  remains unexecuted. Invalid headroom/ABI/installer/default-profile and cumulative
  owner capacity refuse without partial publication or slot recycling.
- Real loan-capacity/issuer refusal preserves a retryable staged candidate without
  clock sampling or deadline preparation. Deadline representation and clock
  failures unwind correctly. A raw-allocation-plus-full-loan regression produced
  **two named failures** before the targeted admission guard; it passes in both
  final profiles. The caught allocation failure still terminates the outer runtime.
- **22 named behavioral controls** reject missing staging/loans, accidental raw
  getters, lost failed retirement, incomplete two-lane rollback, premature
  activation, wrong installer context, parent-budget renewal, premature loan end
  and removed allocator precedence. Compile errors/invariant aborts do not count.
- Final full runtime: **39,511 shipping / 134,451 instrumented checks**, zero
  failures/live allocations, plus compile-failure and fatal-invariant regressions.
  Runtime sources/headers are unchanged by the subsequent DOM-only #652/#653 merges.
- Actual cross-built Os64 consumer: **840 checks**, target heap integrity/balance,
  exact parent-observed **`0x4f520700`**, complete PASS. Matching production objects,
  five-file library closure, source/build-input hashes, commands and serial output
  are frozen. Source disk/boot images and accepted P5 bundles are retained.
- At the combined merged baseline, DOM child-installation engine/budget cases
  pass **1,330 shipping / 1,258 instrumented checks**. Both profiles pass child
  installation **58**, retirement **9,770**, finalization settings **714** and
  Window authority **304** and native browsing contexts **1,078** checks each. Default CLI **199 + 40**, shared
  test262 builder **4/4**, public headers/link (**235 engine / 89 runtime exports**),
  source manifest, teardown inventory and wrapper layout pass. ASan/UBSan and
  LeakSanitizer remain enabled outside the sandbox.

Final header comment corrections have identical preprocessed public/private
interfaces before/after. Earlier pre-guard positives and the intentional allocator
reproducer are retained separately from final passing evidence. Native API/queue
proofs do not establish DOM authority or browser/P5 acceptance. Tracker remains
`code`; no F15 completion flip is prepared.

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 \
    JS_RUNTIME_FOCUSED_SELECTOR=--owner-setup bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_setup_mutations.py \
    --artifacts <retained-runtime-artifacts> --output <fresh-output-directory>
```
