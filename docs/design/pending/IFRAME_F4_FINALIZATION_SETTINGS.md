# F.4b — FinalizationRegistry callback settings

Registry cleanup can run after its constructor has returned and its creation
script has stopped executing. The finalization-settings opt-in retains the
constructor-time incumbent and exact ScriptOrModule, then restores them for
cleanup. It extends Promise settings; existing constructors and Yonder remain
on their existing configuration. F.4 is in progress.

## Roles and ownership

The registry owns a traced context reference and script-record value. Queueing
copies both into the job's existing owned fields. Registry collection, job
discard, completed invocation and runtime destruction release their respective
holds. Capture happens after constructor/prototype lookup and before the
registry joins the weak-reference list. Missing author settings refuses
construction without publishing a partial registry. Trusted private installation
keeps the existing bootstrap policy.

The fixture separates capture A, callback proxy creation B, executable C and
registry constructor D. Job entry follows the existing enqueue-time executable
realm query; registry D remains the activity gate. The wrapper checks captured
incumbent eligibility as well. Retiring A, C or D suppresses delivery; retiring
proxy creation B alone does not. Revoked callbacks retain the engine's existing
enqueue-time fallback and throw when invoked. This callable-target rule is an
explicit embedding policy, not a claim that proxy creation proves browser
callback entry authority.

Native cleanup runs with checked entry/current C. A restored source activation
can select incumbent A even when a foreign Function captured B; a null source
uses the captured backup. Nested callback capture and invocation use the same
checked bridge as Promise callbacks. Scope unwind preserves the surrounding
task, budget and failure state. Cleanup exceptions report through registry D's
script host after callback scopes unwind; the thrown value retains its own
identity and creation realm.

The source basis is the ECMAScript
[registry constructor](https://tc39.es/ecma262/multipage/managing-memory.html#sec-finalization-registry-constructor)
and HTML's
[cleanup host hook](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuefinalizationregistrycleanupjob).
Construction captures callback state; cleanup prepares callback entry and routes
errors to the registry global. Callback restoration follows the shared
HostCallJobCallback implementation documented in the active-script chapter.

## Interfaces and integration boundary

`os64_js_enable_finalization_settings` extends the Promise opt-in before author
execution. `os64_dom_cluster_create_with_finalization_settings` composes it with
the existing DOM callback settings. Public wrapper layouts and ABI 5 are
unchanged. Patch 0035 adds an independent raw capture hook and the borrowed
`JS_PeekJobCallbackSettings` query, including optional exception-report realm.
`JS_PeekPromiseJobSettings` remains a compatibility spelling without that output.
Internal callback helpers now use job-wide names because finalization shares
them with Promise callbacks. The prepared engine identity is
`1a60b7a86e45e2e82bc5f359c3295ad68333a2f18f1ea79f3f710fd9cea9b598`.

Each registry gains an incumbent pointer and JSValue record; the runtime gains
a capture hook and opaque pointer. The allocator charges this storage. These
new retained edges do not make F15 graph diagnostics complete, reclaim retired
realm slots, or establish recovery eligibility. Rebuild engine consumers with
matching prepared headers and objects; bytecode format remains 6.

Cleanup continues to use the bounded engine queue and existing one-held-value
jobs. GC never invokes author cleanup directly. Queue allocation refusal does
not throw recursively during GC; the wrapper allocator's sticky failure prevents
further author execution and is reported at the next checked boundary. This
settings opt-in does not select HTML's separate global task source or guarantee
GC notification timing. The [task opt-in](IFRAME_F4_FINALIZATION_TASKS.md) adds
separate host dispatch and registry-global task association; the opt-in alone must not enable visible frames.
The [import-settings opt-in](IFRAME_F4_IMPORT_SETTINGS.md) supplies null-referrer
base fallback. Realm-less Promise capabilities, remaining WebIDL conversions
and Window/Location security gates remain open.

## Validation

`userland/libjs/tests/finalization_settings.h` runs on the host and compiles into
the guest runtime consumer. It covers native/bound/proxy callbacks, distinct
source and executable realms, null source, nested capture and reentry, role
retirement, revocation before/after enqueue, pending and queued destruction,
abandoned registries, exceptions, cancellation and late/raw configuration
refusal under both teardown policies. Cyclic targets use two explicit GC passes:
this engine processes weak targets before collecting cycles.

Host allocation sweeps independently cover construction, registration and GC
queueing. The shipping allocator can satisfy queue storage from an existing
arena; the instrumented allocator exposes that individual failure point.
DOM integration requires native cleanup to define a custom element and later
invoke its constructor and lifecycle callbacks with the retained incumbent.
The combined opt-in also runs a Promise continuation.

The final focused suite passes **2,005 / 3,027 checks** in shipping/instrumented
profiles; allocation inventories cover **4 / 34 injected failures** across both
teardown policies. DOM cleanup integration passes **234 checks per profile**.
Four negative controls fail in both profiles: wrong captured incumbent, omitted
cleanup restoration, omitted script-record tracing and omitted incumbent-context
tracing. The first two fail assertions; either missing GC edge trips the engine's
unreleased-object invariant. Target runtime/DOM/Yonder/js builds, header/manifest
checks and **224 engine / 81 runtime export** audits pass.

The full runtime suite passes **26,443 / 79,099 checks**, with zero failures or
live allocations; parser refusal sweeps and fatal-invariant paths pass. Full
DOM passes **373,648 checks**, zero failures, including **13,596 binding
allocation-sweep cases**. Sanitizer runs keep
ASan/UBSan and LeakSanitizer enabled. Host execution and guest compilation do
not establish P5 behavior or browser task-source conformance.
