# F15 checked native tasks

Implementation PR **#660**. Private C1/C2/C4 milestone after scoped rejection
consumption (#657).
Implementation `abf2606a`; merged-base integration `fe375b1f`.
Baseline userland `5a67b444` includes the reviewed Proxy receiver correction
(#656, engine `bcc933c20d304c1d5be5b1cb0e703b9d0f198bc7dc59fdf374050f48f190ca5f`)
and Blob URL integration (#647). Display-frame integration #658
(`da7ca2c2`) is merged before final DOM validation; its animation signatures
and private layouts use fresh objects/fixtures. Libjs inputs and engine identity
are unchanged across that merge. C3, public activation, owner/provider
reclamation and slot reuse remain gated.

## Contract and coherent PR

Add a private `js_owner_runtime_task_run` driver using the existing
`JSOwnerScopedBody` invocation-only native contract. It resolves a ticket to
its original retained realm, acquires an independent loan, opens one ordinary
host task, invokes the body under the existing guard/settings, cleans its
returned value or exception, checkpoints ordinary microtasks to exhaustion,
closes that owned task, leaves and ends the loan.

This fills the native-task path without exporting a context/value or creating
persistent task handles. Existing source/scoped-run and public begin/call/end
APIs keep their behavior. The task driver requires an idle runtime: active
reentry, an open task, a previous turn or pending ordinary microtasks refuses.
A deferred FinalizationRegistry task is a separate lane and is not executed
by this ordinary checkpoint. Invalid lifecycle/ABI/name/body refusal publishes
no scope and polls no health. Known global/allocator failure precedes local
loan capacity. Metadata refusal precedes task budget setup.

After admission, one task owns its original realm, deadline, source and job
counters. Its body, nested setup, native coercions/finalizers and checkpoint
borrow them. Reentrant ticket close does not end the loan; promotion remains
explicit. The loan spans result/diagnostic cleanup, queued native execution,
unhandled-rejection cleanup and final leave. Callback exceptions retain the
first diagnostic while the task checkpoint continues through later ordinary
job exceptions as the existing task API does. Sticky failure wins and closes
the owned task without granting a fresh budget; the host can still destroy or
use existing supported recovery policy. Clock/preparation refusal leaves no
open task or loan.

The callback's scope/context and temporary values cannot escape into later
native storage or continuations without explicit compatibility export. Queued
engine values use engine tracing; an invocation pointer is not a job handle.
This driver's scope lives through its synchronous checkpoint; deferred tasks
outlive it and must not retain that scope. Legacy callbacks/raw operations
retain their compatibility veto. Zero counters and an unset veto grant no
release authority; retained foreign-code/native/provider graphs remain a
separate C3/reclamation obligation.

## Proof plan

One PR combines implementation, host/guest fixtures and failure controls.
Exercise real mapped child owners, settings on/off, reentrant close/promotion,
returned-value and exception finalizers, jobs that throw and enqueue more
work, first-diagnostic retention, final guard/loan cleanup, rejection and
deferred-task lane separation, stale/lifecycle/open-task refusal, job/deadline
limits, nested setup borrowing, allocator/clock/cancellation/metadata faults,
repeated native task cycles and exact teardown balance. Run both teardown
policies, target/instrumented engines with ASan/UBSan/LSan, named behavioral
mutants, actual QEMU child/parent/heap checks, affected DOM integration and
public export/layout/header/preparation checks. Freeze final qualified receipts
before requesting Codex review; Chris controls merging. Only F15's row changes.

## Implemented boundary and evidence

The production addition is one private driver in `runtime.c`, beside the
ordinary task checkpoint, and its private header declaration. It adds no
runtime fields and changes no existing execution/job dispatcher. The native
body uses the same scoped settings entry as scoped_run; it does not supply
captured browser callback settings or new reporting/security authority.

The wrapper remains **13,080 bytes** (target DWARF); the shared export set
matches #657 (**235 engine, 89 runtime** symbols). ABI 5 / bytecode 6 and
engine identity above stay unchanged. A scope loan pins its mapped original
owner metadata through synchronous work; it does not classify foreign code,
value roots or native/provider ownership as safely reclaimable.

Final proof commands and qualified results:

- `ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--owner-tasks bash tools/test_js_runtime_host.sh`:
  **522 target / 2,432 instrumented checks**, zero failures. The final driver
  and runtime were recompiled against the retained matching engine objects.
  Both teardown policies cover **15 / 206 allocation cuts** each, balancing
  native/engine/metadata storage without treating reclamation as leak success.
- Complete runtime, raw construction, compiler allocation sweeps and five
  intentional fatal modes: **40,936 / 140,006 checks**, zero failures and zero
  live allocations; compile sweeps pass and expected invariant exits retain
  the full fatal badge. All positive probes retain ASan/UBSan/LSan outside the
  sandbox with `ASAN_OPTIONS=detect_leaks=1`.
- `python3 tools/test_js_owner_task_mutations.py --artifacts ... --output ...`:
  **26 behavioral counterexamples rejected** on both engines. Wrong owner
  context, missing primary loan, early loan/guard/task release, omitted
  checkpoint/closure, lost first exception, skipped health, legacy settings
  export, renewed job counter and deferred-lane consumption trigger named
  assertions. The unmodified controls selector passes both engines. Negative
  runs stop on the first violated contract to avoid cascading invalid fixture
  handles; compiler failures, sanitizer errors and invariant exits do not count
  as detections or positive leak evidence.
- `python3 tools/test_js_owner_runtime_guest.py --base-image ... --boot-iso ... --output ...`:
  **1,619 shared checks**, target heap baseline, integrity and accounting pass;
  parent observes exact **0x4f520900**. The five-file executable/library closure
  and copied boot/disk inputs are frozen. All **28 source inputs** and four
  production runtime/core/ticket/issuer object hashes match after #658.
- Fresh post-#658 DOM objects and fixture: browser jobs **801 / 801**,
  callback reports **1,118 / 1,118**, report retirement **616 / 616**,
  recovery host **3,198 / 3,198**, Promise settings **246 / 246**, child
  installation engine **1,330 / 1,258**, timer settings **718 / 718**, and
  finalization settings **714 / 714**, zero failures, with leak detection
  enabled. The superseded pre-integration build was stopped and is not an
  acceptance cohort; objects from different private layouts are not mixed.
- Public contract headers, six engine-preparation tests, teardown audit,
  shipping CLI/shared build, export comparison, tracker validation,
  `git diff --check` and stale-reference checks pass.

The shared oracle uses actual mapped owners and queue-owned native arguments.
It covers close/promotion, body exception coercion before result cleanup,
continued checkpoint execution after later job errors, native finalizers under
the guard/loan, nested staged setup consuming the task's job budget, retirement
and cancellation, unhosted rejection cleanup during final leave, repeated
cycles, deferred-task separation and legacy export lifetimes. Host clocks and
actual metadata-capacity/issuer faults test preparation refusal, prior-turn
preservation, previously unobserved allocator-failure precedence and terminal
closure without a renewed deadline.

No P5 or visible-frame acceptance is claimed for this private entry point.
C3/public activation, graph migration and the reviewed reachability/native
teardown/slot-reuse exit remain open.
