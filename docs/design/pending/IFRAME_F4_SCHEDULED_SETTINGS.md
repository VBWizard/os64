# F.4b — scheduled callback settings

`os64_dom_cluster_create_with_scheduled_settings` extends the listener/timer
opt-in to requestAnimationFrame, requestIdleCallback (idle and timeout routes),
and queueMicrotask. Yonder and the ordinary constructors do not enable it.
The engine identity and wrapper ABI 5 are unchanged.

## Attribution and invocation

A schedules B's proxy around C's native function through D's scheduling API.
The callback captures incumbent A, enters associated realm B, executes code
in C, and uses D's activity and existing task budget. Reporting retains the
existing callable script/report owner. New script-bearing activations select
their own incumbent; native-only code uses the captured backup. This is
callback preparation, not an operation-specific origin check.

The [animation frame](https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#run-the-animation-frame-callbacks),
[idle callback](https://w3c.github.io/requestidlecallback/#invoke-idle-callbacks-algorithm)
and [microtask](https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#microtask-queuing)
algorithms invoke WebIDL callbacks. [WebIDL invocation](https://webidl.spec.whatwg.org/#invoke-a-callback-function)
prepares the associated realm and captured incumbent and defaults the callback
receiver to undefined. The opt-in frame/idle paths therefore pass undefined;
the legacy paths preserve their window receiver. Strict functions see undefined,
while sloppy functions perform the engine's normal global-this substitution.

Callable conversion captures before idle timeout getters or numeric conversion.
Registration checks target activity and captured/associated lifecycle after
conversion and after a host clock callback. A rejected conversion, unavailable
scope, native allocation or late retirement releases the temporary capture.
A revoked proxy retains its associated creation realm; the callback fails at
invocation, with its TypeError created in that entry realm.

## Queue ownership and cleanup

`DScheduledCallback` owns one callback reference. Its value aliases the checked
capture in settings mode; legacy records own a direct value. Its clear helper
resets the holder before releasing ownership. Frame and idle queues transfer
that record out before removal and invocation. Cancellation, array compaction,
period turnover, timeout delivery and root drain preserve single ownership.
The inline record adds 40 bytes per frame/idle entry on x86-64, including the
legacy mode, and uses the existing native allocation accounting.

The frame batch keeps its existing handle snapshot, shared deadline and
between-callback checkpoints. Idle periods keep their existing deadlines and
one-callback tasks; a timeout's internal binding timer prepares the author
callback inside its existing guard. Preparation does not start a new task,
renew a budget or create an extra checkpoint.

An opt-in microtask queues a private engine holder containing its callback
record, retained activity/report binding pointers and script identity. The
holder traces its callback edge once and clears it when execution or discard
releases the job. Holder/object allocations and failed queue publication use
the engine allocator. The existing job execution/code/activity metadata remains
in place, preserving FIFO with Promise jobs and early retired-code/activity
discard. A captured-incumbent or associated-entry retirement is checked inside
the prepared invocation as a separate gate.

Native microtask reentry can capture the same incumbent for another scheduled
callback. Ordinary Promise jobs have no automatic settings preparation and
cannot register callbacks through these opt-ins. This does not implement
Promise reaction or thenable capture. Terminal failures retain their pending
exception and uncatchable state; the microtask report metadata is native, so
recording it does not perform a failing context lookup that replaces an error.

## Proof and remaining scope

The focused selector is `--scheduled-settings`. Four named realms distinguish
incumbent, proxy entry, native executable and queue/activity ownership. Both
teardown policies and both engine profiles cover frame/idle/timeout/microtask
delivery, undefined receiver and arguments, nested native microtasks and FIFO,
independent role retirement, cancellation and root release, conversion/clock
retirement, missing scopes, unprepared Promise refusal, native allocation cuts,
revoked callback errors followed by healthy work, interruption and balanced
cleanup. Recorded host evidence (ASan/UBSan and LeakSanitizer enabled):

- Full shipping-engine DOM suite: 330,123 checks, zero failures, including
  12,952 binding allocation sweeps.
- Scheduled settings: 5,570 shipping-engine and 6,066 instrumented-engine
  checks, both teardown policies. The isolated microtask enqueue uses existing
  shipping allocator arenas (zero host allocation cuts); the instrumented
  engine exposes four allocations, each refused per teardown policy.
- Seven deliberate defects are rejected by named assertions: wrong associated
  entry, wrong incumbent, omitted retirement admission, unprepared microtask,
  unprepared idle timeout, frame window receiver and late idle capture.
- Instrumented regressions pass: animation 106, animation routing 564,
  animation retirement 1,196, window 820, timer settings 718, listener settings
  1,736, callback reports 1,118, realm routing 678, Streams realms 104,
  departure 353, report retirement 616 and child installation 1,306 checks.
- The additional instrumented `--p2b` run has three pre-existing clone-recursion
  failures, reproduced with the frozen merged #591 binary. The first is stack
  overflow during nested structuredClone getters; its missing messages/order
  cause the other two assertions. This selector is not claimed as passing.
- Target libdom/Yonder/runtime build, contract-header and teardown audits pass.
  No engine/runtime implementation changed in this chapter.

HTML event-handler slots, observers/custom-element callback families, Promise
preparation and security/WindowProxy integration remain F.4 work. F16's native
and checked-settings exclusion rules are unchanged; this chapter does not
certify child recovery. Target compilation and host tests are not QEMU/P5 or
WPT acceptance. F.4 remains in progress.
