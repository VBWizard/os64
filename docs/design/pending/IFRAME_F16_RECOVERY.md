# F.16 — child-local overrun recovery

Implementation brief and proposed PR cuts, 2026-11-13. Source audit baseline:
`6e25f3232aa1238b7a4f1570cfa0f79f8b229dde`, merged F.3b (#553), embedding
ABI 5, prepared engine identity
`73ef22d5921da4ccb6d9482de6edfc6d9787dbccd17cc09de8205f2cfd70c280`.
**Design proposal: recovery is not implemented or proved.** Existing
runtime-wide failures remain the shipping contract. This brief owns F.16;
WindowProxy/security is F.4, reclamation is F.15, and frame loading remains
in the integration slices. No visible frames or slot reuse are enabled here.

## Intended result and limits

An opted-in host can stop a child whose execution deadline expires, contain
the environments abandoned by that interruption, and subsequently start an
independent task in an unaffected sibling. It cannot resume the abandoned
script, undo its mutations, or reuse its realm slot. Ordinary departure still
permits direct retained-function calls; failure containment is stronger.

The first supported candidate is deliberately narrow: an execution timeout
with a complete affected set of child environments, a closed outer task,
proved engine/native unwind, and no surviving engine jobs. A deadline inside
a child-owned Promise job may qualify under the same rules. Pending native
sibling timers/events can survive and retain their order. A native host must
certify its invariants before recovery; arbitrary embedders do not acquire
that certification merely by enabling an option.

Shared **JOBS exhaustion retains the global fallback** in this proposal.
The next queue head did not spend the exhausted allowance; a neutral job or
healthy sibling cannot be blamed for being next. Likewise, surviving healthy
engine jobs cannot silently become a new checkpoint with a fresh deadline.
Broadening either case needs a reviewed job-provenance/accounting extension.
These restrictions remain visible in F.11 acceptance and the F.16 completion
record. If the supported subset cannot satisfy the invariants below, ship the
fallback and record the missing proof rather than claiming recovery.

## Audited source and constraints

Locations are relative to the repository; symbols identify the baseline
more reliably than line offsets after the planned engine patches.

| Location | Observed behavior | F.16 consequence |
|---|---|---|
| `userland/libjs/runtime/runtime.c`: `latch`, `observe`, `interrupt` | First failure wins; clock polling stops after a latch; interrupt receives runtime/opaque | Separate the task's original verdict from sticky global health; observe global signals after a local timeout |
| Same: `step_job_group`, `leave`, `checkpoint`, `os64_js_task_end` | Job cap belongs to the outer turn; sticky task end closes without checkpointing | Preserve counts and close the abandoned task without granting its work another budget |
| Same: `os64_js_quiescent` | Rejects failure, open work and any pending job | Do not weaken this API to manufacture a recovery safe point |
| Prepared `userland/obj/js/upstream/quickjs.c`: `JS_CallInternal` | Polls before installing the incoming frame; bytecode and generator resumes choose code realm internally | Stack enumeration alone misses the about-to-enter environment; guard/capture dispatch and resume separately |
| Same: `js_call_c_function`, class call dispatch | Native function context differs from raw caller context; non-bytecode classes dispatch separately | A single wrapper around public `JS_Call` misses executable paths and native scopes |
| Same: `JS_ThrowInterrupted`, interpreter `exception`, `async_func_resume` | Uncatchable unwind skips author catch/finally; closes ordinary locals/var refs; suspended state has separate cleanup | Heap teardown success does not prove that native algorithms or retained resumptions are safe |
| Same: `JS_GetFunctionRealm` / `js_function_realm` | Follows bound/proxy target, with distinct revoked/neutral job behavior | Useful creation-realm query; not a complete execution stack or F.4 security caller |
| Same: `JS_ExecuteJobGroup` | Unlinks the job before running it, retains execution/callback/activity contexts | Snapshot current job provenance before entry; inspect pending jobs without executing them |
| `userland/libdom/core.c`: `departure_busy`, `os64_dom_depart_finish` | Checks native walks/provider loans, settles storage/cancels requests, then detaches | Reuse audited cleanup components behind a distinct recovery barrier, preserving public departure semantics |
| `userland/libdom/streams.c`: `foreign_stream_slot`, `internal_reaction_call`, `internal_microtask_run`; `streams.js` | Retained cross-realm brands call private JS probes; neutral continuations execute self-hosted JS | Neutral scheduling is not immunity from failed-code containment; retained stream algorithms require a separate consistency proof |
| `userland/apps/yonder/scripts.c`: `sticky`, `judged`, `retire`, `drain_after_exception` | LIMIT/CANCELLED/HOST_FAILURE/FAILED_RUNTIME ultimately retire the whole runtime | A recovered outcome must be routed explicitly; changing only libjs cannot preserve a browser sibling |
| `userland/apps/js/js.c`; `libjs/examples/{runner,jobs}.c` | Standalone execution/draining assumes existing terminal failure contract | Default behavior remains global; rebuild consumers on ABI change |

The prepared engine was generated in this worktree with `tools/js_prepare.py`;
it is audit output, not patch source. Manifest patches 0018–0021 supply realm
and job ownership. Engine changes belong in new hash-checked patches and the
manifest. F.4 and F.16 must agree numbering/order before either reserves new
shared interfaces. Source preparation is not an execution/unwind test.

Existing evidence is in `IFRAME_F3_ENGINE_VALIDATION.md` (interrupt audit),
`IFRAME_F3_DOM_VALIDATION.md` (departure, Streams and installation checkpoints)
and `IFRAME_F3_PROVIDERS.md`. Its verdict remains: local recovery unproved.

## Failure classification and precedence

Keep two independent records: the immutable original task outcome/counters,
and runtime health. A local candidate does not suppress later global failure.
Global health is sticky and dominates recovery eligibility even if its cause
arrived after EXECUTION. Preserve the first global cause for diagnostics;
retain the original timeout separately. Recheck cancellation and allocator
flags during unwind/cleanup and immediately before committing recovery. A
negative clock read or native cleanup failure also escalates globally.

| Signal/context | Proposed handling |
|---|---|
| EXECUTION in completely attributed child execution or child job | Recovery candidate; still LIMIT/EXECUTION for the abandoned task |
| Root script affected, foreign/unclassified scope, attribution overflow or incomplete entry coverage | Global fallback |
| JOBS, including idle/neutral/shared head and mixed ownership | Global fallback; no reassignment of cap consumption to next job |
| MEMORY/allocator OOM, cancellation, host/clock failure, destruction | Sticky global; may invalidate a pending recovery candidate |
| SOURCE limit or any other unsupported limit | Existing global handling |
| Ordinary exception or engine stack overflow without reliable STACK signal | Existing EXCEPTION behavior; do not parse exception text |
| Nested candidate installation/setup or unresolved native/Streams invariants | Global fallback for overrun; ordinary local installation refusals retain F.3 semantics |

Root cancellation remains available throughout. Cancellation racing commit
must still prevent subsequent entry through the existing atomic health check.
No code clears allocator flags or cancellation to recover a child.

## Affected environments and executable containment

Capture an allocation-free set of stable realm slots before interrupted
frames disappear. Capacity is 65, including the primary; use a fixed bitmap
or equivalent bounded storage, never a single 64-bit mask. Capture completeness
and a recovery generation alongside it. Unknown contexts fail closed.

The affected set includes **all live author/code environments whose frames or
native algorithm scopes are abandoned**, not only the innermost realm, the
job owner, or `turn_owner`. In A → B → C where all three author frames unwind,
all three are failed; if A is primary the first rollout falls back globally.
A closed earlier A task is not affected merely because its timer later ran B.
A top-level child B task with idle A/C can contain B and preserve A/C. A
native C service defined in A does not itself imply abandoned A author code;
its unwind/invariant obligation must instead be explicitly classified. Unknown
native scopes make the case unsupported. Include the pending callee when the
pre-entry interrupt fires; active-frame enumeration alone is insufficient.
Capture at the first wrapper/native budget observation too, before that
native scope returns; waiting for a later engine interrupt can lose evidence.

Use explicit engine execution scopes at bytecode dispatch, native/class calls,
eval/module entry and async/generator resume. Snapshot current job code,
execution and activity ownership as separate facts. Getters, coercions and
constructors reach these scopes through raw native and JS-to-JS paths.
Suspended pre-existing async/generator state need not be on the interrupted
stack, but cannot later resume code in a failed environment.

Failed state is irreversible and distinct from retirement. Publish executable
ineligibility before releasing jobs or native records. Refuse entry before any
failed body, proxy trap, accessor or constructor side effect runs. Cover:

- Ordinary functions, closures, bound targets, proxy traps and callable targets;
  resolving only a proxy's final function realm misses its handler code.
- Getter/setter lookup, coercion, species/constructor lookup, eval and retained
  bytecode/module handles, including top-level await and dynamic import.
- Generator next/throw/return, async continuation/resolver functions, Promise
  handlers/thenables/custom capabilities, and native queued continuations.

Keep identity, primitive values and data operations that provably execute no
failed code. A retained object is not certified safe merely because a property
read looks like data: proxies, accessors and prototypes can invoke author code.
Native retained DOM/Canvas/Blob operations need their existing owner/service
rules plus a consistency proof. Do not drain/free failed backing as a shortcut.

Blocked aliases after completed recovery should produce a distinct local
failed-realm refusal at wrapper entries and a catchable engine error inside a
healthy task, without executing failed code or poisoning the healthy caller.
During the original abort, preserve uncatchable propagation and prevent native
callbacks from replacing it with a reusable ordinary exception. Test both
modes independently. Trusted native cleanup must not require entering failed
JS. Self-hosted Streams code is not exempt merely because a job is neutral;
if its private state cannot be safely contained, keep global fallback for that
case. Ordinary departed-function tests must continue to pass unchanged.

## Recovery boundary and native protocol

Proposed state sequence: healthy → abort pending → engine unwound → native
containment prepared → recovered, or sticky global failure at any step.
These are design states, not existing enum values. No recovery inside an
outer task, setup transaction, provider loan, dispatch, observer/reaction walk,
module callback or native operation still borrowing affected data.

1. Record LIMIT/EXECUTION, original deadline, executed/discarded counts and
   affected set. Stop task work without running author catch/finally or reports.
2. Return through the outer wrapper and host native scopes. Close an explicit
   task using the failure-close path without checkpointing; implicit turns
   must also be closed. The task outcome stays failed after successful recovery.
3. Under a distinct recovery-only barrier, mark failed realms and inspect
   queued work without executing it. Initially permit discarding entries only
   when proved wholly obsolete for failed realms. Foreign/mixed/neutral jobs
   with unproved settlement obligations, or any surviving healthy engine job,
   retain global fallback. Preserve native sibling tasks/order. If a discard
   finalizer enqueues work, inspect it again within bounded cleanup; inability
   to establish completion falls back rather than renewing the task budget.
4. The DOM host checks cluster-wide native exclusion and prepares affected
   activity shutdown. Settle/cancel with original providers still alive; do
   not dispatch author error/storage/abort events from recovery cleanup.
   Request cancellation, storage settlement, observer/CE shared queues,
   animation/Canvas borrowers, module handles and Streams must each have a
   named consistency oracle. Reuse departure helpers where the proof applies;
   its current public finish API cannot run beneath the timeout latch.
5. Recheck global health and completeness. Detach providers only after their
   matching release and native cleanup have finished. Commit recovery without
   entering script, allocating, running jobs or restarting a budget. The next
   independent host task may arm its own deadline through normal task begin.

BUSY never grants permission to free provider opaque storage. A generation
checked recovery token prevents stale affected-set acknowledgements after a
later failure. Native cleanup failure requires root teardown after unwind;
failed realms and their charges survive until that teardown. Native blocking
syscalls retain bounded-I/O/check-budget obligations: engine interruption
cannot roll back an arbitrary blocked syscall or external side effect.

## Interface proposal and F.4 coordination

Use additive, explicit opt-in on an idle runtime before author execution;
legacy constructors and standalone consumers retain global failure semantics.
The API must distinguish original task failure, recovery pending, completed
recovery and global failure. Do not teach hosts that LIMIT alone means a
sibling may continue. Proposed operations (names/layouts subject to review):

- Engine: non-allocating execution-scope snapshot with complete/unknown status;
  irreversible context executable-failure mark; guards covering the entry map
  above; query proving no active execution scope. Engine scopes expose facts,
  not WindowProxy security policy.
- Libjs: enable the supported policy; inspect recovery record/set/generation;
  recovery-only cleanup barrier; commit or escalate after host preparation.
  No public general-purpose failure-reset operation.
- Libdom: prepare/finish containment for the entire affected set at once, with
  native loans/walks excluded; return a generation-bound completion result to
  libjs. Factor shared departure cleanup without weakening its public query.
- Host: preserve failed task diagnostics, route affected bindings to containment,
  and start independent sibling tasks only after commit. Yonder stays opt-out
  until a coherent cluster consumer exists; its primary-only path still retires.

F.4 requires caller/entry/incumbent attribution and global-this/security
behavior. F.16 requires abandoned code/native scopes and failed-entry guards.
Function creation realm and outer task owner do not substitute for either.
Share a scope hook only after tests prove both semantics, including native
callbacks and setup. Coordinate `runtime.c`, `js.h`, `js_engine.h`, manifest,
patch order and ABI revision before implementation. Use the next available
ABI at landing, regenerate engine identity, rebuild dependent units and update
CONTRACT/TEARDOWN/LIBDOM in the implementing cuts. No ABI number is reserved
by this brief. F.4's separate branch is not rebased or rewritten by F.16.

## Small PR sequence and exit checks

Keep F.16 in progress across the following cuts; subordinate names describe
PR scope, not additional completed tracker rows.

| Cut | Reviewable result | Required exit before next dependent cut |
|---|---|---|
| F16a: this brief | Source audit, supported-case contract, interface map and acceptance plan; shipping unchanged | Review decisions on supported subset, queue boundary and native certification; F.4 interface agreement |
| F16b: dormant engine scopes | Attribution and failed-entry primitives, disabled recovery; engine/header/manifest identity and focused tests | Raw/JS/native entry map, completeness failures, retained alias guards, suspended state and allocation-free capture proved on both engine profiles |
| F16c: opt-in libjs protocol | Separate task/global failure records and recovery state machine for trusted native harness; default remains global | Late-global precedence, exact accounting, queue restrictions, no early/recursive commit, repeated cycles and teardown proved |
| F16d: DOM containment and host acceptance | Native invariant/cleanup integration, three-realm consumer, coherent host routing and contracts | Provider/registry/Streams proof, shipping and instrumented integration, guest/WPT evidence, hardware regression decision where shared browsing changes |

Split F16b by entry family if needed, keeping primitives dormant until complete.
Do not combine an incomplete entry guard with enabled recovery. F16d may split
native preparation from host adoption; neither alone finishes the slice.
No automatic merge or reviewer-comment loop is part of these cuts.

## Validation planned before implementation

Use deterministic injected clock/interrupt positions and three realms. Assert
values, forbidden effects, exact order and resource counts, under FATAL and
RECLAIM with shipping and fully instrumented engines:

- Allocation-heavy loops; A/B/C raw calls and native reentry; getters,
  coercions and constructors; modules; Promise jobs; async/generator resumes;
  catch/finally skipped on abort; failed closures retain identity but cannot run.
- A/C sibling native timers/events survive B recovery in order. An unaffected
  engine job pending at abort forces fallback in this rollout. JOBS at a child,
  mixed or neutral head remains global, with original counts preserved.
- Native scopes or primary author frames caught in unwind force fallback unless
  expressly proved supported. Nested setup remains global on overrun. Retained
  ordinary departure functions and the existing install/refusal matrix pass.
- Alias tests cover the entry list above, including healthy proxy with failed
  handler, failed proxy target with healthy trap, bound chains, revoked proxies,
  species getters, borrowed methods, custom Promise capabilities and module handles.
- Cancellation, allocator limit/OOM, clock and host failure before/during/after
  timeout dominate commit. Inject them in diagnostics, finalizers and native
  cancellation. Unknown attribution, new queued cleanup work and stale tokens
  fail closed. No allocation failure may be disguised as harmless local refusal.
- Poison provider opaque storage after certified finish; exercise late callbacks,
  original-producer cancellation, shared observer/CE order, retained DOM data,
  borrowed Canvas/effects, Streams brand/probe/settlement and native loan release.
- Repeat overruns to admission refusal; retained slots/bytes remain charged.
  Check no live native/engine roots after final teardown; RECLAIM releasing a
  leak is a failing ownership test, not acceptance.
- Mutants: wrong/missing realm, pre-entry capture omission, raw/resume bypass,
  proxy-handler bypass, neutral exemption, early reset, renewed task budget,
  lost later-global signal, stale generation and premature provider detach.

Baseline entry points: `tools/test_js_contract_headers.sh`,
`tools/test_js_prepare.py`, `tools/test_js_runtime_host.sh`,
`tools/test_js_realms_host.sh`, `tools/test_dom_host.sh` and
`tools/test_js_teardown_audit.py`. Add focused selectors for attribution,
failed aliases and recovery. The realms-only shell script exits after its
shipping-engine probe; instrumented evidence needs its own rebuilt run.
Check actual flag forwarding before selecting DOM fixtures.

For integration, run full runtime and shipping DOM plus affected instrumented
realm/provider/observer/CE/Streams/install suites. Keep ASan/UBSan and LSan
where supported by each engine build; the existing fully instrumented engine
build disables engine UBSan while its wrapper retains it. Diagnose any LSan
attachment failure with verbosity/thread logs and TracerPid; rerun outside the
sandbox with leak detection enabled when supported. Record source commit,
prepared engine identity, exact commands, profile and bounded evidence logs.

Rebuild affected target libraries/consumers, then run a three-realm QEMU
consumer with parent-observed exit and heap checks. Compare named affected WPT
results to the merged baseline at verified pin
`a699fb16767be39ba3e61a8e8b6bef60383f08ee`, with unchanged expectations. If shared
normal browsing changes, agree a small frozen P5 cohort; preserve the accepted
F.3b delivery. Do not repeat its completed acceptance campaign.

Completion requires reviewed evidence for the supported recovery contract and
explicit retained fallback limits, matching contracts/debt/divergence records,
and the owned F.16 tracker done flip with implementing PR numbers. Engine
instrumentation or one sibling eval after latch clearing does not satisfy it.
