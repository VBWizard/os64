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

A callback throwing after implementation realm D retires exposes the distinction
between a private continuation and ordinary Promise work. WebIDL's post-cleanup
exception conversion creates a rejected Promise, then converts it through a
fresh Promise capability. That conversion reads `then`; a queued adoption using
D's intrinsic `then` is still D-owned under the existing runtime contract.
Retirement discards that job without settling its Promise. A live B callback can
therefore run once and throw correctly while the resulting stream operation
remains pending, with no runnable jobs. This is the current embedding's retirement
limitation, not a claim that WebIDL requires pending completion.

The proof checks both sides: without retirement the identical callback rejects
with its original error; with D retired it records one discarded job, no queued
work and no error misreported into the live caller. Changing `then` to a live E
function or E's native intrinsic follows E's ordinary job ownership and completes.
A departed D function or native intrinsic is discarded. The `then` getter is
read once after callback cleanup; getter exceptions and mutable species effects
remain observable. No private wrapper bypasses those properties, and no broad
neutrality is assigned to native Promise functions.

Full retained error-path completion requires a separately reviewed lifetime
policy. It remains an F.4 gate alongside the other settings/security and browser
job policies. This cut does not complete F.4b or authorize WindowProxy publication.
The underlying rules are in `userland/libjs/CONTRACT.md` (realm-owned jobs and
discard), WebIDL's [callback invocation](https://webidl.spec.whatwg.org/#invoke-a-callback-function)
and [Promise conversion](https://webidl.spec.whatwg.org/#js-promise).

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
These are seven completion families plus an explicit ordinary-adoption boundary,
not a claim that every retained operation completes.

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
