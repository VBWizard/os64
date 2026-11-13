# F.4b — settings attribution interfaces

Source baseline: `7f647d4e`, including F.4a (#568 delivered to `userland`
through #570). F.4b is split into native call facts, settings bookkeeping,
and callback/job integration. This first implementation supplies **native
call facts only**. It does not supply a browser permission check or a complete
current/entry/incumbent query. F.4 remains in progress.

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

## Implemented raw interface

Patch `0025-native-call-contexts.patch` adds:

```c
typedef struct JSNativeCallContexts {
    JSContext *incoming;
    JSContext *defining;
} JSNativeCallContexts;
JS_BOOL JS_GetNativeCallContexts(JSRuntime *rt, JSNativeCallContexts *out);
```

The query copies the innermost dynamically active generic/data native
invocation's context pair. It returns false and clears a non-null output when
there is no such invocation or `rt` is null. A null output is refused. It does
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
The tracking is intrinsic to these two dispatchers, with no script-visible
switch, caller substitution or browser exposure.

## Remaining F.4b interfaces and acceptance gates

Before publishing browser settings queries, implement and prove:

- A bounded native script-entry stack independent of `turn_owner`, with
  runtime/lifecycle validation and restoration across nested host entry,
  setup, exceptions and interruption. Relevant settings come from the target
  binding; they cannot be inferred from the active call pair.
- Explicit callback capture and invocation state, preserving the incumbent
  captured at WebIDL conversion rather than deriving it from the callable,
  event target, timer owner or task owner. Engine script-bearing state and
  callback suppression must be represented; a native-call stack alone cannot
  supply them.
- Promise reaction and thenable capture/invocation, including a bound foreign
  native method with no author frame above it, plus module/eval and suspended
  async/generator cases. Queue ownership and cancellation eligibility remain
  separate from settings provenance.
- A role-specific wrapper query and a pre-member-check boundary whose missing
  settings refuse access. Specify the exact wrapper signatures after the
  callback registry/entry-map audit, then test three distinct roles in one call.
  A trusted bootstrap path must not become a guessed fallback caller.

These are required before F.4b completion and F.4d's browser publication API.
This raw primitive is not a substitute for any of them. No security gate may
use `incoming` or `defining` without the operation-specific attribution proof.

## Focused proof

Shared host/guest fixture `userland/libjs/tests/native_call_contexts.h` runs
through both teardown policies; the host selector is `--native-call-contexts`.
It uses independent A/B/C context identities and compares direct, borrowed,
reflected, spread, bound, proxy, getter/setter and constructor calls. It also
covers raw host invocation, nested JS/native calls, eval, generated functions,
generator/async execution, Promise jobs, exception preservation and cancellation.

The full runtime suites must retain their existing verdicts. Run the selector
against both target and fully instrumented engines, with leak checking enabled;
record counts, mutations, engine identity and limitations in the validation
companion. Guest compilation is not guest execution. Default browser behavior
is unchanged; no WindowProxy security, WPT or P5 acceptance is claimed here.
