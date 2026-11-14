# F.4b — timer callback settings

Native hosts can create a DOM cluster with
`os64_dom_cluster_create_with_timer_settings`. This opts into checked host
entry scopes and timer callback capture/delivery. The ordinary constructor
and Yonder's configuration retain their existing behavior. F.4 remains open;
this interface does not publish frames or authorize cross-origin access.

## Distinct roles

| Role | Source |
|---|---|
| Task budget and timer activity | Timer queue's document |
| Conversion-time incumbent | Checked incumbent at callable argument conversion |
| Associated callback entry | `JS_GetCallbackRealm`, retained in the timer |
| Executable target eligibility | Existing `JS_GetFunctionRealm` check at delivery |
| Exception reporting | Existing callable script/report owner |
| Current execution | Engine execution tracking |

A registers C's native callback through B's `setTimeout`: B owns the task,
C is the associated entry and A is the captured incumbent. A proxy created
through B's Proxy constructor around that C callback instead enters B;
C remains its executable target/report owner. A script-bearing callback
supplies its own incumbent while executing. A native-only callback observes
the captured backup.

## Associated callable realm

[WebIDL's JavaScript environment](https://webidl.spec.whatwg.org/#js-environment)
does not fully specify associated realm for every object kind. This chapter
uses a concrete callable policy consistent with Chromium's
[callback relevant realm selection](https://chromium.googlesource.com/chromium/src/+/6560ef2cf35aa4ec1b721f7b8895e0bffa60c6f8/third_party/blink/renderer/platform/bindings/callback_function_base.cc)
and V8's [bound-function/proxy construction](https://github.com/v8/v8/blob/main/src/heap/factory.cc):
ordinary functions use their retained defining realm, bound functions inherit
the associated realm of their target object, and proxies use their constructor's
realm. That is distinct from recursively following proxy targets for executable
code. Mutable prototypes and proxy traps do not supply this identity.

Patch 0030 adds `JS_GetCallbackRealm`. It returns a borrowed context or NULL
for unsupported values/classes, without allocation, property lookup, traps or
exception mutation. Its bound-chain walk is iterative. Proxy records own one
additional context reference, accounted in their allocator size; GC marks the
context edge and finalization releases it. Revocation preserves the identity.
The query does not implement an arbitrary object-listener realm policy.
Public wrapper layouts and ABI ID remain unchanged; the engine identity changes.

## Registration, invocation and cleanup

Callable conversion captures before delay conversion. Missing prepared settings
refuse before `valueOf` can run. Delay conversion can reenter or retire realms;
registration rechecks target activity and captured/associated lifecycle before
publishing, including after the host clock callback. Failed conversion and
allocation clear the capture while preserving the pending exception.

The timer's captured record owns its callback reference; the legacy callback
field aliases it. Timeouts transfer ownership out of the table before delivery.
Intervals duplicate the invocation hold, so self-removal cannot invalidate the
running callback or arguments. Cancellation, drain, retirement and failed
registration release ownership. Captured-incumbent or callback-entry retirement
suppresses delivery and cancels intervals that cannot become eligible again.
Internal binding timers retain their existing direct-value convention.

`os64_js_callback_call` independently takes reporting and associated entry realms,
validates the capture and lifecycle, and holds the function through reentry.
Exception reporting follows the existing realm-call path. It neither opens a
new task nor renews a deadline, drains jobs or recovers a failed runtime.

`os64_js_enable_host_settings` must precede task/evaluation. Once enabled,
classic roots (source/file/bytecode), module roots and ordinary wrapper calls
prepare checked entry scopes inside their existing guard. Scope restoration
precedes host return and exception reporting. Promise jobs do not inherit a
root's settings scope. Timer registration from an unprepared job refuses;
Promise callback capture and invocation remain a separate F.4b gate.

## Validation and remaining scope

The focused host selector is `--timer-settings`; it runs both teardown policies
and both engine allocator profiles. It checks independently expected settings,
foreign native/bound/proxy callbacks, prototype mutation, arguments and receiver,
self-cancellation, nested/string timers, conversion failure, registration
allocation cuts, missing-scope conversion ordering, job refusal, revocation,
role-specific retirement, cancellation, timeout and balanced cleanup. The proxy
lifetime test releases its creation context while an external revoked callback
keeps it alive, then verifies collection of the context/proxy cycle after the
last external callback is released. Native data callbacks independently check
incoming entry, defining/current realm and captured incumbent.

Recorded host results (LeakSanitizer enabled):

- Final full runtime: 16,914 shipping-engine and 64,262 instrumented-engine
  checks; 40 parser allocation-failure checks per profile; invariant exit badge
  checks passed, with zero live allocations.
- Broad shipping-engine DOM: 322,740 checks and 12,941 native allocation cuts.
  This sweep preceded the final revoked-proxy error-context/rooting refinement;
  final timer, reporting, realm, Streams and departure selectors were rerun
  afterward on the final binaries.
- Final timer integration: 718 checks on each engine profile, both teardown
  policies. ASan/UBSan instrument the wrapper and DOM; the second profile also
  instruments the engine with ASan.
- Final instrumented DOM regression selectors: callback reports 1,118; realm
  routing 678; Streams realms 104; departure 353 checks, all passing.
- Seven deliberate defects were rejected: wrong captured incumbent, omitted
  host preparation, late capture, skipped incumbent retirement, proxy target
  substituted for associated entry, reporting context substituted for native
  data-call incoming context, and wrong revoked-proxy error realm.
- Target libjs/runtime test, libdom and Yonder build; contract headers, source
  preparation tests, teardown allocation audit, tracker and stale-reference
  checks passed. Target builds are not guest execution.

Object listeners and other callback families, Promise callbacks, operation-specific
origin checks, WindowProxy/Location exotics and browser publication remain open.
Host tests do not establish QEMU/P5 acceptance or F.4 completion.
