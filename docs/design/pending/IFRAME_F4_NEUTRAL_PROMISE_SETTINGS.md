# F.4b — empty-handler Promise capability settings

An absent `.then()` handler does not imply that its reaction executes no author
code. A custom species can supply executable `resolve` and `reject` capability
functions. The neutral-Promise opt-in lets that execution provide checked
current and script incumbent settings, without manufacturing a job entry realm.

## Policy and ownership

[NewPromiseReactionJob](https://tc39.es/ecma262/2026/multipage/control-abstraction-objects.html#sec-newpromisereactionjob)
assigns a null job realm when its handler is absent, then calls the selected
capability. HTML's [HostEnqueuePromiseJob](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob)
omits realm-based preparation for that case. The engine therefore runs these
reactions in a bounded settings scope with no entry or backup incumbent. It
clears enclosing host settings for the reaction and restores them on return,
including exceptions and terminal failures.

| Role | Empty-handler reaction |
|---|---|
| Entry | Absent; checked query refuses |
| Current | Actual executing script/native function; checked lifecycle applies |
| Incumbent | Eligible script-bearing activation, otherwise absent |
| Callback capture | Requires an eligible incumbent; no settlement/creator fallback |
| Nested WebIDL callback | Explicit associated entry and captured incumbent; restores neutral scope |

The species constructor, source promise, settlement context and capability
execution context can all differ. A native-only capability has current settings
but no invented incumbent. Callback capture there refuses. Script-bearing
capabilities can register callbacks, including callbacks whose associated realm
is different from their executable realm. Retirement does not seal executable
code; checked settings still refuse retired realms. F16's independent executable
failure seal blocks both script and native capabilities.

The scope adds no context/value roots or queue fields. Existing reaction and
queue values keep their capability functions alive. Settings pointers are
borrowed from the running execution; stack-local state restores the prior scope.
Pending/queued teardown and allocation refusal retain the existing ownership
rules. The scope shares the original job budget, cancellation and wrapper guard.
It does not expand F16's supported recovery subset.

Abrupt custom capabilities retain the embedding's existing queue-context
exception diagnostic fallback. The thrown value keeps its original identity.
This is not a claim to implement an HTML reporting-global rule: that case is
explicitly unresolved in [HTML issue 10526](https://github.com/whatwg/html/issues/10526).
The separate [browser job opt-in](IFRAME_F4_BROWSER_JOBS.md) chooses outcome
diagnostics without a guessed window event for neutral jobs. This constructor
retains its original fallback; browser publication still requires the remaining
binding/security gates.

## Interfaces and compatibility

`os64_js_enable_neutral_promise_settings` composes the import, finalization,
Promise and ordinary host-settings opt-ins before execution.
`os64_dom_cluster_create_with_neutral_promise_settings` composes the DOM callback
consumers with it. Existing constructors do not enable it; Yonder's normal
browsing configuration is unchanged.

Patch `0037-neutral-promise-settings.patch` adds
`JS_SetPromiseNeutralSettings`. `JS_PeekJobCallbackSettings` and its compatibility
spelling return true for an opted-in empty-handler reaction, with null entry,
incumbent and report outputs. This means the checked-query scope is admitted,
not that every role exists. Generic raw jobs remain unprepared. Raw current and
incumbent queries recognize the explicit neutral scope; queries outside admitted
settings scopes remain null.

Wrapper ABI 5, bytecode version 6 and public layouts are unchanged. The engine
identity is `944d3d773802c9c4fdaa4f0c53f84eecae8eea75f58860a4618388295518b9b5`.
Rebuild consumers with matching prepared engine headers and objects.

## Validation

`userland/libjs/tests/neutral_promise_settings.h` supplies shared host/guest
fixtures; host selectors use `--neutral-promise-settings`. Four-realm cases
cover fulfillment/rejection, pending/settled registration, native/script/bound/
proxy capabilities, nested callback success/throw, absent settings, legacy opt-in,
raw sibling jobs, raw nested host scope restoration, retired realms, sealed
script/native capabilities, queued/pending destruction, cancellation, aggregate
job caps and thrown-value identity with independent diagnostic routing.

`tools/test_js_neutral_failures.inc` injects each allocation failure across
registration, settlement and allocating capability execution. The target-core
profile reaches one allocation cut per teardown policy; the instrumented engine
reaches 75. Focused runs pass 1,797 and 6,829 checks respectively.

`tools/test_dom_neutral_promise.inc` proves nested custom-element upgrades restore
missing entry and actual C settings, and that later listener/observer delivery
uses associated B with captured C in document D after independent E settlement.
Both profiles pass 480 checks. All sanitizer runs retain ASan/UBSan and leak
detection.

The complete runtime suite passes 29,477 target-core and 95,701 instrumented
checks with zero failures and live allocations, plus compile-failure sweeps and
expected invariant-failure exits. Target runtime-test, libdom, Yonder and js
builds pass. Manifest/header checks and 226 engine / 84 runtime export audits
pass. Adjacent instrumented DOM suites pass: Promise 246, finalization 234,
import 1,378, custom elements 12,000, callback reports 1,118, recovery 6,672,
departure 353 and Window 820. The full target-core DOM suite passes 376,735
checks and 13,625 binding-allocation sweeps with zero failures. Two instrumented
negative controls fail the focused proof as expected: disabling neutral
preparation and inventing entry/backup from the queue context. Host execution
and target compilation do not establish guest or P5 acceptance.

The [settings interface](IFRAME_F4_SETTINGS.md) retains the remaining F.4b gates:
remaining WebIDL conversions and role-specific lifecycle/security checks.
The browser job opt-in supplies task selection and an explicit neutral
exception-reporting policy. F.4c contexts/origins and F.4d WindowProxy/Location remain
separate implementation and acceptance work.
