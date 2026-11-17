# F.4b — host-delivered FinalizationRegistry tasks

The finalization-task opt-in separates cleanup from the Promise microtask queue.
A host checkpoint leaves cleanup pending. The host selects one cleanup task
between ordinary tasks; that dispatch prepares the registry's task budget,
invokes one eligible cleanup callback and checkpoints its microtasks before
returning. Existing constructors keep their scheduling policy. F.4 remains in
progress and Yonder's default configuration does not enable this opt-in.

## Queue, cells and lifetime

Patch 0042 provides a separate runtime queue with the existing owned job-entry
machinery. Ordinary peek/execute/discard APIs select microtasks. The explicit
finalization APIs select cleanup tasks. Transaction groups retain their second
linkage across both queues; rollback discards both lanes without touching
sibling work. Successful private installation drains its microtasks, leaves
cleanup deferred, and can activate with that cleanup pending. Host delivery
refuses an unpublished candidate.

GC clears each dead target and queues a task holding its registry. Cells remain
in that registry until delivery: `unregister(token)` can cancel a collected cell
between collection and host dispatch. Each task chooses at most one cleared
cell. If intervening unregister calls removed the cells, the task does nothing.
Removing the cell before calling author code makes recursive registration,
unregistration and collection independent of the active invocation. The held
value stays rooted through the callback and is released under its guard.

A registry's native data has a borrowed object back-pointer, valid while the
registry data remains on the weak-reference list. Weak processing precedes
cycle collection in this engine. Enqueue refuses to resurrect a zero-reference
registry awaiting finalization; successful enqueue owns a normal strong value
root. There is no untraced strong back-edge. Registry tracing accounts for the
held values, callback, incumbent, source record and registry realm. Completion,
discard and runtime destruction release task roots through the common job
machinery. Destruction keeps discarding both queues until empty, because a
native finalizer released from either lane can enqueue in the other. Queue allocation does not throw recursively during GC; the wrapper
allocator preserves terminal allocation failure for the next checked boundary.

This one-cell cleanup policy uses the latitude in ECMAScript's
[CleanupFinalizationRegistry](https://tc39.es/ecma262/multipage/executable-code-and-execution-contexts.html#sec-cleanup-finalization-registry).
The distinct task and registry-global association follow HTML's
[cleanup host hook](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuefinalizationregistrycleanupjob).
Neither specification promises prompt garbage collection or notification.

## Host contract

`os64_js_enable_finalization_tasks` composes constructor capture and callback
settings before execution. `os64_js_finalization_task_pending` is an
allocation-free owner-thread presence query. `os64_js_run_finalization_task`
visits one task and performs its microtask checkpoint; an empty queue is OK.
Dispatch refuses active execution, an open host task or an unfinished microtask
turn. An intervening ordinary task is permitted while cleanup is pending.
The host controls fairness between task sources; this API does not create a
thread, timer or wake notification.

Registry activity supplies the task owner. Callback executable realm supplies
settings entry, and captured incumbent/source restoration remains independent.
Retired or failed executable, activity or incumbent suppress delivery. Cleanup
exceptions report to the registry's script host after scopes unwind. A cleanup
and its subsequent microtasks share the deadline and jobs-per-turn limit;
recursive dispatch cannot renew them. An ineligible task consumes no execution
count, but its native finalizers and any jobs they enqueue still run inside the
bounded dispatch task. Global cancellation and allocation failure remain sticky.

Ordinary execution outcomes' `jobs_pending` continues to describe microtasks.
Recovery inspection and global-fallback outcomes conservatively include both
queues. The separate
presence query describes deferred cleanup. Quiescence and recovery bookkeeping
include both queues: separating tasks cannot make a runtime with retained work
eligible for child recovery. This adds no recovery or reclamation permission.

`os64_dom_cluster_create_with_finalization_tasks` composes Streams callback
settings with this scheduler. Hosts use the runtime poll/dispatch APIs at their
event-loop task selection point. Existing DOM constructors and Yonder retain
their current policy. Browser publication still requires the remaining F.4b
attribution/security proofs and F.4c/F.4d integration.

## Storage and compatibility

The prepared x86-64 layout has a 1,656-byte runtime (24 additional bytes), a
96-byte registry record (8 additional bytes), an unchanged 104-byte job header
and a 64-byte registry cell. A cleanup task owns one 16-byte registry argument;
cleared cells remain allocated until unregister, delivery or registry release.
The runtime allocator charges these retained objects and queue entries. Wrapper
ABI 5 and bytecode format 6 are unchanged; prepared engine identity changes and
matching engine headers/objects must be rebuilt together. No reclamation or
complete F15 graph-accounting claim follows from these holds.

## Validation

Shared host/guest runtime cases cover separate ordering, two cleanup dispatches,
microtasks between them, unregister after collection, queued registry release,
role retirement, callback revocation, exception routing, cancellation, reentry,
ordinary task and microtask refusal, pending teardown, candidate activation and
rollback, and native-finalizer enqueue during ineligible discard. Host tests
also inject construction, registration and GC enqueue allocation failures under
both teardown policies. A child overrun with pending finalization work exercises
the conservative recovery fallback. DOM integration converts a custom-element
callback from cleanup, then invokes its constructor and reactions after cleanup
has ended, preserving the captured source and incumbent.

Validation counts and shipping build evidence are recorded after the final run.
ASan, UBSan and LeakSanitizer remain enabled. Host execution and guest compilation
do not constitute P5 acceptance or enable visible frames.
