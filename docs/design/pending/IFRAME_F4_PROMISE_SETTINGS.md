# F.4b — Promise callback settings

`os64_js_enable_promise_settings` adds Promise reaction and thenable settings
under the existing runtime guard. `os64_dom_cluster_create_with_promise_settings`
extends the custom-element settings constructor with that opt-in. Configuration
precedes author execution. Earlier constructors keep their existing selection;
Yonder does not enable this path. F.4 remains in progress.

## Capture, entry and lifetime

Callable fulfillment/rejection handlers capture the incumbent when `.then`
registers them, including already-settled promises. Thenable resolution captures
immediately after its `then` getter returns. The capture is independent of
callable-proxy creation, mutable prototypes, executable realm and settlement.
Bound functions retain their receiver and executable target.

Pending reactions retain real engine context references and trace those edges
with the promise. Enqueue retains a separate context reference before the
reaction releases its own. Execution, discard and runtime destruction release
the queue reference. A failed capture publishes no reaction pair; allocation
failure releases the initialized prefix. These are engine roots, unlike the
borrowed wrapper identities used by the DOM callback records. F15 retention
coverage must account for them; this change does not claim physical realm
reclamation or completeness of diagnostic counters.

At enqueue, job entry follows `GetFunctionRealm` of the handler or `then`
function. A proxy created in B around executable C therefore enters C; the
WebIDL associated-realm rule would instead select B. Revocation before enqueue
uses the engine's current-context fallback, while revocation after enqueue
preserves the recorded job entry. The wrapper checks executable and captured
incumbent eligibility independently. Retirement of a settlement or proxy
creation realm alone does not suppress an otherwise eligible job.

## Invocation and continuation work

The engine prepares job entry under the checkpoint's existing guard and budget.
The wrapper marks this entry as admitted so native bindings can use checked
settings queries and register DOM callbacks from Promise handlers. The handler
call temporarily prepares its captured incumbent; script-bearing frames still
take precedence. Nested calls restore both scopes on normal, exceptional and
terminal returns. No checkpoint or deadline is renewed by preparation.

An admitted job also supplies its entry as a backup for engine continuation
work outside the handler call. In particular, resolving the handler's result
can discover a returned thenable and create another callback after the previous
captured scope has ended. That capture uses the job backup, not the previous
handler's incumbent. This is an explicit embedding policy for the otherwise
empty-stack boundary, comparable to V8's documented
[entered/microtask-context fallback](https://chromium.googlesource.com/v8/v8/+/d587f67af51533944731424f334e04495f4dcdf5/src/execution/isolate.cc).
The regression distinguishes initial capture A from continuation entry C and
checks the incumbent of the second thenable invocation.

Raw host registration without admitted settings refuses. Private staged
interface installation is explicitly trusted bootstrap; neutral internal
continuations and empty handlers keep their existing queue policy. Ordinary
noncallable-handler value forwarding remains covered. This chapter does not
assign browser authority to arbitrary raw jobs or change generic/finalization
job policy.

## Boundaries and source basis

HTML's [HostMakeJobCallback](https://html.spec.whatwg.org/multipage/webappapis.html#hostmakejobcallback),
[HostCallJobCallback](https://html.spec.whatwg.org/multipage/webappapis.html#hostcalljobcallback)
and [HostEnqueuePromiseJob](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob)
separate capture, invocation and job entry. Exact active ScriptOrModule
restoration is still required for full HTML integration, including delayed
`eval` and dynamic-import attribution. Realm-less jobs with custom capability
callbacks, finalization jobs, remaining WebIDL conversions and pre-member
security checks retain separate acceptance gates. Do not treat this opt-in as
a complete HostMakeJobCallback implementation or publish WindowProxy authority
on its strength.

Patch `0033-promise-settings.patch` adds internal runtime/reaction/job fields
and additive engine APIs. `JSJobInfo`, public wrapper layouts and wrapper ABI 5
are unchanged. Rebuild engine consumers with matching prepared headers and
objects. The prepared engine identity is
`31bcaae4fa1778113d0c3cebc4de4435ab7d34f9ef8306e334e89774c06fa631`.
`JS_PeekPromiseJobSettings` is a read-only borrowed query for the selected
queue head; it supplies no pin, origin permission or recovery authorization.

## Validation

The shared host/guest fixture `userland/libjs/tests/promise_settings.h` uses
four independent realms: A capture, B proxy creation, C execution, D settlement.
The host selector is `--promise-settings`; host-only allocation sweeps live in
`tools/test_js_promise_failures.inc`. Both teardown policies cover pending and
settled reactions, rejection, thenables, bound/native/script handlers,
revocation timing, role retirement, nested checked calls, missing preparation,
GC, pending/queued destruction, cancellation and aggregate job caps.

`tools/test_dom_host.sh --promise-settings` exercises native Promise-to-custom
callback conversion, later construction and lifecycle delivery, listeners,
observers, Streams and chained jobs. It checks scope restoration and allocation
balance. Both target-core and instrumented-engine profiles retain ASan/UBSan
and leak checking. Host tests do not establish guest/P5 acceptance.

The final host runtime suite passes 23,951 checks on the shipping engine and
75,108 on the instrumented engine, with zero failures/live allocations. The
focused Promise suites pass 1,923 and 5,705 checks. Allocation refusal sweeps
cover 6 shipping cuts and 128 instrumented cuts across both teardown policies.
Compile-failure sweeps and fatal-invariant contracts also pass. The DOM Promise
suite passes 242 checks in each profile. The full shipping DOM suite passes
361,893 checks with zero failures and 13,246 binding allocation-sweep cases.
CLI stand-in/real-library suites pass
199/40 checks. Manifest/header, target allocation/visibility audit, target
runtime consumer, libdom, Yonder and js builds pass.

Four negative controls fail in both engine profiles: capture from the incoming
context, proxy-associated job entry, omitted incumbent retirement gate and
omitted reaction GC marking. The first three hit role/callback assertions;
the GC mutation hits the engine's unreleased-object invariant at teardown.
Positive and mutant binaries use separate frozen cohorts. The returned-thenable
regression also fails against the earlier engine without continuation backup.
