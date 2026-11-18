# F15 checked native tasks

Private C1/C2/C4 milestone after scoped rejection consumption (#657).
Baseline userland `5a67b444` includes the reviewed Proxy receiver correction
(#656, engine `bcc933c20d304c1d5be5b1cb0e703b9d0f198bc7dc59fdf374050f48f190ca5f`)
and Blob URL integration (#647). C3, public activation, owner/provider
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
