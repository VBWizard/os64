# F.4b: trusted native microtasks and retained Streams continuations

Patch 0040 adds `JS_EnqueueJobNeutralSettings` for trusted native algorithms.
Its queue entry has no author owner, activity owner, incumbent or source record.
A scalar admission bit distinguishes this explicit request from ordinary raw
jobs. Execution saves and clears enclosing settings, admits actual execution
settings, then restores the prior state on return or failure. Native/script
calls supply current settings; script-bearing calls can supply incumbent.
There is no invented entry or native-only backup incumbent.

`JS_PeekJobCallbackSettings` reports admission with null role/report outputs.
The existing runtime job guard therefore admits checked queries without a new
wrapper API or configuration layout. Arguments and execution context use the
existing queue holds; budgets, installation groups, discard, failed-code checks
and teardown keep their established paths. The added flag fits the queue
record's padding. ABI 5 and bytecode version 6 remain unchanged.

The Streams callback opt-in uses this operation for private tee/pipe microtasks.
Its Promise reactions already have explicit neutral preparation. Marked
implementation code does not introduce an author record, and author callback
invocation still uses the checked boundary from
[the callback cut](IFRAME_F4_STREAM_CALLBACK_SETTINGS.md). Ordinary constructors
retain the raw job path. Internal abort algorithms remain synchronous native
algorithms, with nested callback preparation and restoration.

## Ordinary Promise adoption is a separate lifetime boundary

WebIDL's post-cleanup exception conversion creates a rejected Promise, then
converts it through a fresh Promise capability. Ordinary adoption follows the
observed `then` function's owner; retirement discards that job without settling
its Promise. Private microtask admission alone does not change this rule.

The [host capability milestone](IFRAME_F4_HOST_PROMISE_LIFETIME.md) supplies
explicit admission for native adoption and its private forwarding reactions.
The Streams callback opt-in uses it for return conversion, including the
post-cleanup thrown-callback path. A direct native intrinsic can complete after
implementation departure; author replacements, bound functions and proxies
retain ordinary ownership. Observable `then` and species lookups remain.

The current continuation fixtures assert completion for private native adoption
and retain ordinary discard controls. The validation counts below record the
original microtask cut; the host capability milestone records its updated proof.
This does not complete the remaining F.4b browser/security gates.

## Validation

`--neutral-native-jobs` runs shared host/guest runtime cases plus host allocation
refusals. It covers actual native/script settings, nested callback success/throw,
missing incumbent refusal, raw-job compatibility, nested raw drain restoration,
queue-context versus executable retirement, failed executable seal, terminal
cancellation, queued destruction and grouped discard. Both teardown policies
release their queue/value/context holds.

`--stream-continuations` covers retained default/byte tee, pipe completion, pipe
abort, encoding transforms, Response consumption and async iteration after D
retires. It adds the live/departed thrown-callback controls and mutable then,
foreign-native then, throwing getter and species observations described above.
The private capability milestone adds retained thrown-callback completion
across the callback families; ordinary author adoption remains a boundary.

ASan/UBSan and normal-exit LeakSanitizer remain enabled. The engine uses the
harness's ASan instrumentation. Target compilation is not guest/P5 acceptance.

Engine identity: `36d2ff8a0092e5ce32441ad2f6defef276a9aa2ec735600f247078cbbaf657d2`.
Focused runtime proof passes 1,199 shipping and 5,291 instrumented checks,
including 1 and 63 allocation-refusal points per teardown policy. The focused
DOM continuation/observation proof passes 3,232 checks on each engine. Queue
records remain 104 bytes in both parent and changed engine debug layouts.

The full shipping DOM suite passes 420,708 checks and 13,553 binding-allocation
sweeps; the instrumented callback composition passes 117,856 checks, including
1,142 allocation refusals. Both report zero failures. Target Yonder, js, runtime
guest consumer and shared libraries build; manifest/header checks, six preparer
tests and the 228 engine/84 runtime export/link audits pass.
The complete runtime suite passes 31,065 shipping and 101,381 instrumented
checks with zero failures and live allocations. Parser refusal sweeps and
expected invariant-failure exits also pass. The final F4/F15 stacks and current
userland each pass conflict checks; no combined-runtime acceptance is inferred.
