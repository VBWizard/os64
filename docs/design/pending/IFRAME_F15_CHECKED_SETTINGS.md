# F15 checked settings invocation

Private C1/C2/C4 milestone after scoped settings observations (#665), on merged
userland `9e929a38` including WindowProxy methods (#664). Engine identity is
`038c999a4588e07b30691d4848af28df7d2c530e42fb8cb864bb551cccb696ce`,
embedding ABI 5 / bytecode 6. C3, public activation, reclamation, provider
release and slot reuse remain gated.

## Contract and small PR plan

`js_owner_runtime_with_settings` takes an entry ticket and an optional incumbent
ticket. It invokes the existing borrowed `JSOwnerScopedBody` contract under
explicit engine settings, using one independent bounded loan for each supplied
role, even when both name the same owner. No raw context or result is returned.
The body receives an invocation-only scope and context for the entry owner;
selected roles remain original retained realms, not F4 target/origin authority.

This call borrows an active wrapper guard or enters an already open host task.
It opens no task, checkpoints no jobs, renews no deadline or counters, and
selects no exception-reporting hook. It copies exception diagnostics and frees
the exception/result while the settings frame, guard and both loans are live.
Close during reentry prevents future ticket admission but does not end either
loan. Explicit promotion/export retain their existing contracts. Deferred and
ordinary queued work do not retain the invocation pointer.

A NULL incumbent supplies no replacement backup, preserving the engine's
existing nested scope and script-incumbent selection. It is not an instruction
to erase an enclosing backup or an active script frame. Explicit tickets name
original eligible owners, not the current document or an origin authority.

Existing ABI, setup, lifecycle, health and outcome-alias rules apply before
body entry. Metadata refusal exposes no scope; partial two-role admission
returns its first loan. Engine settings nesting refusal also unwinds both
loans without changing the enclosing task. Public raw settings/callback APIs,
legacy retention veto and Promise incumbent capture remain unchanged.

One PR contains the private header/driver, shared host/guest tests, host fault
and allocation probes, named mutation controls, this brief and F15's tracker
note. Exercise distinct roles, absent/same-owner incumbent, nested calls and
outcome aliases, close/promotion, result/exception finalizers under settings,
partial loan refusal, ineligible/stale/foreign tickets, unchanged task budget,
job deferral, nesting exhaustion, cancellation and allocator faults. Run both
engine and teardown profiles with ASan/UBSan/LSan, actual guest/parent/heap
checks, fresh merged-engine/DOM integration and public export/layout/header
audits. Freeze matching receipts, request Codex review, and leave merging to
Chris. No persistent callback/value holder or DOM migration is introduced.

Quinn confirmed the runtime/header work does not overlap her independent
private Window/document/event/callback identity integration. Both lanes use
fresh coherent engine and DOM objects after #664; no engine patch is planned.

## Implemented boundary and qualified evidence

The private driver follows merged `userland` `9e929a38` (#664/#665), engine
`038c999a4588e07b30691d4848af28df7d2c530e42fb8cb864bb551cccb696ce`,
embedding ABI 5 / bytecode 6. No engine patch or runtime field is added.
Target DWARF remains **13,080 bytes**; public exports match #665
(**235 engine, 89 runtime**). The shipping CLI/shared library cross-builds.
C3, public activation, provider/owner reclamation and slot reuse stay gated.

- `ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--owner-invocations bash tools/test_js_runtime_host.sh`:
  **284 target / 308 instrumented checks**, zero failures. Both teardown
  policies cover **1 / 5 native allocation cuts** each. Positive ASan/UBSan/LSan
  runs stay outside the sandbox with leak detection enabled. Final matching
  engine, runtime and fixture objects are retained; the merged #664 engine
  and DOM layouts are built coherently.
- Complete runtime, raw construction, compiler allocation sweeps and all five
  intentional fatal modes: **41,719 / 142,115 checks**, zero failures and zero
  live allocations. Compiler sweeps and full parent fatal badges pass. The
  final strengthened teardown oracle verifies clean reports with both policies;
  earlier receipts before those assertions are superseded.
- Host interception checks the actual entry and incumbent metadata rows each
  have a loan; a second loan against the entry cannot substitute for incumbent
  protection. Empty-body admission changes no allocation count even when the
  next host allocation is forced to fail. Clock, issuer, cancellation and
  expired-deadline refusals expose no scope. Native object/string allocation
  cuts free temporary values and both loans without leaked-root reclamation.
- `python3 tools/test_js_owner_invocation_mutations.py --artifacts ... --output ...`:
  **34 named behavioral counterexamples rejected**, including wrong selected
  contexts, wrong/missing/early loans, cleanup without settings/guard,
  skipped exception diagnostics, raw export, outcome clobber, lifecycle bypass,
  renewed counters/deadline, ignored nesting refusal and partial refusal unwind.
  The unmodified controls selector passes both engines. Compilation failures,
  sanitizer reports and invariant exits do not count as detections.
- `python3 tools/test_js_owner_runtime_guest.py --base-image ... --boot-iso ... --output ...`:
  **2,168 shared checks**, exact parent-observed **0x4f520c00**, heap baseline,
  integrity and accounting pass. The copied disk/boot inputs and five-file
  executable/library closure are preserved. All **31 source inputs** and four
  production runtime/core/ticket/issuer objects match the final source.
- Fresh post-#664 DOM objects/fixture, target and instrumented: browser jobs
  **801 / 801**, synchronous settings **33,662 / 66,254**, callback reports
  **1,118 / 1,118**, Promise settings **246 / 246**, import settings
  **1,378 / 1,378**, WindowProxy **1,426 / 3,726**, Location exotics
  **1,932 / 3,052**, child installation engine **1,330 / 1,258** and recovery
  host **3,198 / 3,198**, zero failures with leak checks enabled. The final
  runtime source independently produces the same tested DOM runtime object
  after its cleanup-comment clarification. No unmerged identity-layout objects
  from Quinn's next integration are mixed into this cohort.
- Public headers, six preparation tests, teardown inventory, production CLI,
  export/layout, tracker, diff and stale-reference checks pass.

The shared oracle exercises actual engine settings nesting exhaustion and
partial second-role metadata refusal, with later one-role admission proving
capacity was returned. Nested entry/incumbent swaps and NULL backup preserve
outer scopes; close of both holders still permits entry-loan promotion and
synchronous result/exception cleanup. Ordinary jobs run at the enclosing
checkpoint after the callback scope ends, with cumulative task job counters.
Scoped refusal cannot invent a task, use a foreign/stale/staged/retired owner,
publish during trusted setup or clear a legacy raw-getter veto.

This is a private invocation contract, not a persistent callback/root facility
or a DOM migration. Engine script-incumbent rules still apply within author
code; supplied original-owner tickets do not supply a current document or
security authority. No public checked profile, reachability/reclamation proof,
P5 or visible-frame acceptance is claimed.
