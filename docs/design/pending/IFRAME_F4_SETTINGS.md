# F.4b — settings attribution interfaces

F.4b supplies raw native call facts (#571) and explicit engine entry/incumbent
scopes. The wrapper adds lifecycle-checked queries and scopes, with native
callback ownership and current/pre-native-entry attribution. Browser callback
integration, job integration and operation-specific security checks remain
acceptance gates. These primitives
have no production DOM consumer; F.4 remains in progress.

## Roles and source findings

The [HTML settings algorithms](https://html.spec.whatwg.org/multipage/webappapis.html#realms-settings-objects-global-objects)
distinguish current execution, entry, incumbent and an object's relevant
settings. Entry follows the most recent host script-entry scope, including
reentrant callback invocation; it is not necessarily the outer budget owner.
Incumbent selection requires script-bearing execution contexts plus callback
backup state. A captured callback context can differ from its function realm.

The [Window/Location security algorithm](https://html.spec.whatwg.org/multipage/nav-history-apis.html#security-windowproxy-location)
uses operation-specific checks. A native dispatcher context is an input fact,
not proof of the source authority for every operation. The eventual binding
must apply the relevant algorithm at the correct boundary.

The prepared engine has two distinct native dispatch conventions:

| Dispatcher | Incoming argument | Context delivered to native body | Retained defining context |
|---|---|---|---|
| `js_call_c_function` | Calling engine path's `ctx` | `p->u.cfunc.realm` | `p->u.cfunc.realm` |
| `js_c_function_data_call` | Calling engine path's `ctx` | Incoming `ctx` | `s->realm` |

The data function retains a creation realm for lifetime/function ownership,
but does not switch its callback argument to that realm. Changing this
convention would affect existing bindings; this cut records both facts and
preserves delivery.

Native wrappers, foreign intrinsics and raw host calls make a generic `caller`
label misleading. A method created in C, directly called from A, receives the
pair `(A,C)`. Calling it through C's own `Function.prototype.call` gives the
pair `(C,C)` at the method's dispatcher. `Reflect.apply` from A and spread
syntax from A give `(A,C)`. These are engine facts, not interchangeable origin
or incumbent identities. A raw host can choose the incoming context explicitly.
A proxy trap that executes in B can create a different incoming context from
an empty proxy around the same native target.

## Native invocation facts

Patch `0025-native-call-contexts.patch` adds:

```c
typedef struct JSNativeCallContexts {
    JSContext *incoming;
    JSContext *defining;
} JSNativeCallContexts;
JS_BOOL JS_GetNativeCallContexts(JSRuntime *rt, JSNativeCallContexts *out);
```

The query copies the innermost dynamically active C-function/data invocation's
context pair, including optimized `iterator_next` dispatch via patch 0027.
Optimized iterator callbacks receive the incoming context; their defining
context remains independently observable. It returns false and clears a non-null
output when there is no such invocation or `rt` is null. A null output is refused. It does
not allocate, change reference counts, invoke script, or consume an exception.

Both pointers are borrowed for the duration of that native invocation, on the
runtime's owner thread. The caller's existing context lifetime obligation and
the executing function's defining-context hold supply the backing. This API
adds no retained roots, context slots, finalizers, or ownership transfer.
Runtime bookkeeping is one pointer; each active native dispatcher uses an
automatic context pair and saves the previous pointer. Nested calls restore
that pointer on normal return, exception, constructor refusal and interruption.

A pair remains observable while its native body reenters JavaScript. Therefore
it is deliberately **not** a query for the currently executing JS realm. A
non-native class call or provider callback does not acquire a new native pair
merely because it runs under an existing native invocation. Callers must not
mistake an enclosing pair for such a callback's own boundary. The production
DOM and Yonder do not consume this API.

Public wrapper layouts and `OS64_JS_ABI_ID` remain unchanged. The generated
engine identity changes; engine objects and headers must be rebuilt together.
The query neither modifies nor interprets F.16 execution scopes/failure state.
The tracking is intrinsic to native function dispatch, with no script-visible
switch, caller substitution or browser exposure.

## Explicit entry and incumbent scopes

Patch `0026-settings-scopes.patch` adds:

```c
#define JS_SETTINGS_SCOPE_LIMIT 64
typedef JSValue JSSettingsBody(JSContext *entry, void *opaque);
JS_BOOL JS_RunWithSettings(JSContext *entry, JSContext *incumbent,
                          JSSettingsBody *body, void *opaque, JSValue *out);
JSContext *JS_GetEntryContext(JSRuntime *rt);
JSContext *JS_GetIncumbentContext(JSRuntime *rt);
```

This is a trusted owner-thread raw interface. The host keeps both contexts
alive through the body and supplies contexts belonging to the same runtime.
It validates browser lifecycle and execution eligibility before entering; the
raw primitive does not authorize execution or certify quiescence. It does not
open a task, reset a budget, clear a failure/cancellation latch, or drain jobs.
Public wrapper ABI layouts and `OS64_JS_ABI_ID` are unchanged.

A null `incumbent` opens an ordinary host entry. A non-null `incumbent` also
prepares a callback: it supplies the captured backup and increments the top
script-bearing activation's suppression counter. A newly executing script
activation can then supply its own incumbent. A native-only callback instead
observes the captured backup. Ordinary nested entries preserve that backup
and suppression state. Returning from the native body restores entry, backup
and suppression before transferring its result to `out`, including when the
result is `JS_EXCEPTION`. Bodies must return normally in C; nonlocal jumps and
runtime destruction while the body is active violate the raw contract.

Null entry/body/output, a foreign-runtime incumbent, and nesting beyond 64
are refused before the body runs. Refusal returns false and preserves output
and the pending exception; it does not manufacture a JS exception. True means
the scope was admitted, not that its body succeeded. The scope bookkeeping
allocates nothing and adds no retained roots. Each nested call saves its state
on the native stack. Runtime depth remains nonzero throughout the native
body, including intervals without JS frames; F.16 quiescence must account for
this state before recovery can commit.

The entry getter returns the most recent explicit host entry. The incumbent
getter selects the top script-bearing activation when its suppression count
is zero, otherwise the current callback backup. Both getters return null
without an explicit entry, and the incumbent getter can also return null
inside an entry with neither a script-bearing activation nor a backup. They
do not substitute the native defining context, incoming context, task owner,
or target binding. They borrow their results and do not touch exceptions.

### Script provenance and suspension

The engine records the **presence** of ScriptOrModule on each bytecode closure,
not the identity of a Script Record or Module Record. Script/module roots
establish that presence. Ordinary closures, including dynamic functions,
inherit presence from the active script-bearing chain when instantiated.
Direct eval inherits its immediate calling frame's presence; indirect eval's
root has none. A function created inside indirect eval can still inherit the
active outer script's presence. This distinction follows
[PerformEval](https://tc39.es/ecma262/multipage/global-object.html#sec-performeval)
and [OrdinaryFunctionCreate](https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-ordinaryfunctioncreate).

The marker uses a spare bit in the object header and is initialized with the
object. It does not retain a context or record, and it is derived again when
cached bytecode is instantiated. Each ordinary bytecode call or generator/
async resume creates an automatic activation record with its execution realm
and a zero suppression count. Suspension, normal return and exception unwind
restore the previous chain; no pointer to a suspended native stack survives.
Null-provenance code does not mask an older script-bearing activation.
Tracking runs even outside opt-in scopes so closures created there remain
correct when later invoked inside one. Queries still require explicit entry.

This presence marker is sufficient for this incumbent-selection primitive.
It is not an active-script-record API or the Promise HostMakeJobCallback
capture record. Queue ownership from the existing job API remains separate.

## Checked wrapper bridge

`os64_js_realm_with_settings` admits an explicit host entry and optional
captured incumbent inside an existing turn/task. It validates wrapper
membership, ABI, published/non-retired lifecycle, runtime health and budget.
Private setup cannot use this path to enter published settings. Its native
body retains the wrapper guard, with separate outcomes for nested scopes;
no checkpoint or new budget is introduced. The engine restores attribution
before the body result transfers to the caller, including abrupt returns.

`os64_js_settings_realm` exposes ENTRY and INCUMBENT as checked borrowed realm
handles only inside such a scope. It refuses missing/unregistered settings,
retired/staged realms and raw-only entry, without sampling time or touching
the pending exception. These are opt-in host interfaces with no DOM consumer.
Full signatures, ownership and status rules are in
[libjs CONTRACT.md](../../../userland/libjs/CONTRACT.md).

The wrapper's allocation includes a native scope-outcome chain; the bridge
itself allocates nothing. The #575 bridge cut changed neither existing public
layouts/ABI ID nor the engine identity. The wrapper state checks do not
constitute F.16 recovery or a browser origin check.
Evidence: [IFRAME_F4_SETTINGS_BRIDGE_VALIDATION.md](IFRAME_F4_SETTINGS_BRIDGE_VALIDATION.md).

## Native callback capture and ownership

`os64_js_callback_t` in `os64/js_callbacks.h` owns a callback object reference
and its conversion-time incumbent. Capture uses the checked incumbent query;
it does not derive attribution from the callback's code or registration
binding. The record can be a C root or a traced edge of an engine object;
explicit clear and mark operations support both lifetimes and cycle collection.
The holder owns and accounts for the inline storage. Cleanup remains available
after cancellation, retirement and failure.

Invocation uses the checked bridge with a separately proven associated entry
realm. A native adapter performs the binding's lookup, conversion and call
work inside that scope. The value and incumbent survive registration removal
or replacement during delivery, with final reference release inside the guard.
The helper neither identifies the associated realm nor implements WebIDL type
validation, callback activity checks or origin permissions.

[WebIDL's callback function and interface rules](https://webidl.spec.whatwg.org/#es-callback-function)
separate conversion-time incumbent capture from invocation-time preparation.
Associated object realm is a separate requirement
([§3.1](https://webidl.spec.whatwg.org/#js-environment)); a callable-target query
is not a general proof for bound functions, proxies or callback objects.
This cut leaves that proof at the adapter boundary and adds no DOM consumer.
Evidence: [IFRAME_F4_CALLBACK_CAPTURE_VALIDATION.md](IFRAME_F4_CALLBACK_CAPTURE_VALIDATION.md).

## Current execution and native entry

Patch `0028-current-realm.patch` adds borrowed raw queries:

```c
JSContext *JS_GetCurrentContext(JSRuntime *rt);
JSContext *JS_GetNativeCallerContext(JSRuntime *rt);
```

Both require explicit host settings preparation. CURRENT follows the executing
realm, including bytecode without ScriptOrModule presence, native functions,
constructors, generator/async resumes and known built-in call classes. Host
preparation supplies its realm execution context during the native body.
Ordinary property exotics preserve the executing context. Bound/proxy calls
preserve current until a trap/target enters; unknown custom call classes mask
it rather than inheriting an unproved identity.

NativeCaller snapshots current before C-function/data or optimized-iterator
entry. It stays attached to that native invocation across reentry; CURRENT
instead follows the reentrant code. In a C body calling a data function with
an explicitly chosen incoming A argument, native caller can still be C.
Defining/current, incoming, incumbent, entry and task owner are separate facts.
The existing incoming/defining query and native argument conventions remain
unchanged. The snapshot is not a generic source-authority decision.

The checked wrapper exposes additive CURRENT and NATIVE_CALLER roles using the
same scope, runtime and lifecycle admission as ENTRY/INCUMBENT. An absent,
unregistered, retired or installing realm refuses; queries do not allocate,
sample time or consume exceptions. The engine retains no new roots and saves
scope state on the native stack. Public layouts/ABI 5 remain unchanged; the
prepared engine identity changes, requiring coherent headers and objects.

The source basis is [HTML current settings and host preparation](https://html.spec.whatwg.org/multipage/webappapis.html#current)
and [ECMAScript function execution contexts](https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-builtincallorconstruct).
[HTML's platform-object check](https://html.spec.whatwg.org/multipage/nav-history-apis.html#isplatformobjectsameorigin)
requires the context at the appropriate operation boundary. A future binding
must perform its pre-member check before conversions or reentry and obtain
relevant settings from the target. This cut supplies attribution, not that
binding/security integration. Native jobs still require host preparation;
Promise incumbent capture and exact script/module identity remain separate.
Evidence: [IFRAME_F4_CURRENT_REALM_VALIDATION.md](IFRAME_F4_CURRENT_REALM_VALIDATION.md).

## Remaining F.4b interfaces and acceptance gates

Before publishing browser settings queries, implement and prove:

- DOM integration of checked entry/lifecycle scopes and role-specific
  attribution, including relevant settings from the target binding.
- Integrate native capture records at WebIDL conversion and invocation sites,
  proving associated-object entry realm and callback activity/security checks.
  Preserve captured incumbent independently of callable realm and task owner.
- Promise reaction and thenable capture/invocation, including bound foreign
  native methods. Supply host scopes per invocation rather than treating a
  checkpoint's queue context as the captured callback incumbent. Exact active
  script/module records needed by those algorithms are separate work.
- A pre-member-check boundary that refuses missing settings, with no guessed
  bootstrap caller. Security/lifetime review precedes browser publication.

These remain F.4b completion gates and prerequisites for F.4d. The raw APIs
are not security checks. No binding may substitute native dispatch facts or
an entry/incumbent context for the operation-specific attribution algorithm.

## Proof and limits

[Native context evidence](IFRAME_F4_NATIVE_CONTEXT_VALIDATION.md) records #571.
[Settings scope evidence](IFRAME_F4_SETTINGS_SCOPE_VALIDATION.md) records the
entry/incumbent cut. Shared host/guest fixtures are `native_call_contexts.h`
and `settings_scopes.h`; focused host selectors are `--native-call-contexts`
and `--settings-scopes`. Both teardown policies run with independently
identified A/B/C contexts.

The settings fixture distinguishes host entry A, author code B and native
binding C; callbacks with and without author frames; repeated suppression;
ordinary nested entry; foreign call/Reflect/proxy paths; eval and dynamic
functions with and without provenance; cached script/module roots; generator
return/finally; async/module resumes; explicit job scopes; exception identity;
scope overflow; and cancellation. The native Promise fixture uses an explicit
test-host backup, not automatic callback capture. It proves scope selection
and restoration, not browser Promise integration.

Full runtime regressions, focused tests, and fault-injected variants run on
host engines with leak checks. Guest compilation is not guest execution.
No WindowProxy security, WPT, QEMU or P5 acceptance is claimed for this cut.
