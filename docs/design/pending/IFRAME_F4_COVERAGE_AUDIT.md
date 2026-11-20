# F4 — settings and associated-object coverage audit

Audit PR **#681**. F.4.36 reconciles the settings plan with merged consumers plus F.4.41. This is
an inventory and disposition, not a claim that F4 acceptance is complete.
The initial task list is: enumerate call/conversion families, distinguish their
associated/relevant owners, reconcile their evidence, and expose uncovered
work as child tasks. Public browser activation remains gated.

## Audit method and boundary

The audit reads native calls and constructors in `userland/libdom/*.c` and
`*.inc`, the callback capture/invocation helpers, engine job/host implementation
patches, and the matching `tools/test_dom_*settings.inc` fixtures. Reproduce the
entry inventory with:

```
rg -n 'JS_Call\(|JS_CallConstructor|os64_js_callback_' userland/libdom
rg -n 'GetCallbackRealm|GetObjectRealm|d_.*capture|d_.*invoke' userland/libdom
rg -n 'JS_Eval|PrepareHostImplementation' userland/libdom
```

A direct `JS_Call` is not automatically a missing WebIDL callback scope.
Saved intrinsics, iterator protocol methods, promise resolving functions,
self-hosted implementation helpers and converted author callbacks have
separate rules. Conversely, finding a shared helper does not prove every
consumer's capture timing, reentry checks, activity owner or cleanup.

## Conversion and delivery inventory

| Family and implementation | Associated/relevant owner and scope | Evidence / disposition |
|---|---|---|
| Timers, `timer.c` | Callable-associated entry, captured incumbent, scheduling binding activity and existing task budget; string handlers keep their separate source policy | `IFRAME_F4_TIMER_SETTINGS.md`, `test_dom_timer_settings.inc`; implemented private opt-in. |
| EventTarget listeners and HTML handlers, `event.c` | Callback object creation identity differs from executable method realm; `handleEvent` lookup stays inside the prepared scope. Handler slots/inline source retain their own compilation and report rules | Listener and handler settings briefs/fixtures; implemented private opt-ins. Inline event source is author code, not trusted implementation code. |
| rAF, idle and microtasks, `animation.c`, `idle.c`, `browsing.c` | Scheduled helper captures incumbent independently of associated callable entry; delivery uses activity and callback eligibility. `queueMicrotask` wraps capture in an engine-held job edge | Scheduled settings brief/fixture; implemented private opt-in. |
| MediaSession actions, `media_session.c` | Registration uses `d_scheduled_capture`, `d_scheduled_live` and script/report owner; action delivery uses the scheduled invocation helper | Shared infrastructure present; media behavior tests cover replacement/removal and exceptions, but not a role-separated cross-realm settings fixture. **ADDED F.4.42**. |
| Mutation, resize, intersection and performance observers | `d_observer_capture` records callable-associated entry and conversion incumbent. Delivery/lookup and target activity follow each observer algorithm | Observer settings brief/fixture; implemented. Observer queues and native registrations remain lifetime obligations, not context-release evidence. |
| Custom elements, `custom.c` | Constructor and lifecycle callback capture uses custom helper; constructor/prototype and observed-attribute iteration are conversion work, not additional callback-interface registrations | Custom settings brief/fixture; transactional publication, constructor/callback roles and rollback covered. `whenDefined` resolution uses promise capability functions. |
| NodeFilter, `walker.c` | Object-associated entry, including noncallable filter objects; `acceptNode` lookup, call and return conversion run inside `d_synchronous_invoke` | Synchronous settings brief/fixture. Creation owner, currently adopted document and filter executable realm remain separate. |
| URLSearchParams/FormData/Headers pair iteration, `url.c`, `blob.c`, `fetch.c` | `forEach` author callbacks use synchronous capture/invoke; ordinary sequence/iterator protocol conversion uses normal execution and exceptions | Synchronous settings brief/fixture. Iterable input methods are not retained callback registrations. |
| Streams, `streams.c`, `streams.js` | Engine-traced callback holder retains captured value; callback dictionary methods and return conversion use explicit prepared scope; implementation code is marked separately from author activations | Stream callback/continuation and host-promise lifetime briefs/fixtures. Engine cycles must stay traced; F15 C-root migration must not absorb them. |
| Promises / FinalizationRegistry, engine patches and runtime | Reaction/thenable and registry captures retain their specified settings/script records; separate finalization host tasks, eligibility, discard and reporting | Promise, neutral-Promise, finalization settings/tasks and browser-jobs briefs/fixtures. A neutral job is not an invented entry/incumbent context. |
| Adopted stylesheet array factory, `adopted.c` | `kFactory` is evaluated with ordinary `JS_Eval` and produces retained proxy/assignment closures; it can execute author iteration/getters | Unlike Streams, no trusted implementation preparation is present. **ADDED F.4.43** to establish provenance and reentry evidence before activation. |

## Other native call sites

The remaining direct calls are classified by the operation they perform:

- `abort.c`: internal abort algorithm hooks, not a new author callback IDL type.
- `blob.c`, `url.c`, `custom.c`: iterator methods during sequence conversion;
  author reentry is ordinary execution, with the enclosing algorithm's cleanup.
- `crypto.c`, `encoding.c`, `image_data.c`, `controls.c`, `validity.c`, `clone.c`:
  captured built-in constructors/getters/collection operations and clone's
  native enumerator. These require their existing object/receiver and lifecycle
  checks; they do not acquire callback incumbent capture merely by calling C.
- `adopted.c`, `blob.c`, `custom.c`, `fetch.c`, `media.c`, `screen.c`, `subtle.c`,
  `webanimation.c`: promise capabilities or captured promise intrinsics.
  Promise reaction execution belongs to the engine's prepared job policy;
  provider/DOM activity and settlement cleanup remain each producer's duty.
- `report.c`: callback adapters plus saved error/filename/WeakSet intrinsics;
  exception/report owner must not be inferred from callback entry.
- `window_proxy.inc` and `location_exotic.inc`: descriptor getter/setter
  invocation after the relevant exotic's authority check. These use ordinary
  member execution, not a retained WebIDL callback registration.

This classification does not certify every reentrant conversion's behavioral
correctness. Integrated F.4.38 must test conversion-induced retirement and
publication changes at authority/commit boundaries, with independent role
oracles and negative controls. F.4.41's setter checks both caller and target
after string conversion; that local result is not a blanket proof for siblings.

## Associated objects and security

`node.c` distinguishes allocation/creation ownership (`d_creation_owner`) from
the node's current document (`d_owner`). Adoption cannot replace the creation
realm/prototype owner. Event targets and retained documents similarly keep
relevant identity independently of the current WindowProxy target. These are
F15 lifetime inputs, not evidence that the old owner can be reclaimed.

Window IDL members use actual receiver selection and NATIVE_CALLER. Property
exotics use CURRENT with canonical origin identity. Location retains its
original relevant global while document selection can follow the current
context. Navigation separately captures ENTRY for URL resolution and INCUMBENT
for source authorization, then checks source/entry/target/policy generations
at dispatch. None of these roles is interchangeable with the outer task owner.
See the binding-authority, Window-authority, native-context, navigation,
Location-context/exotic, WindowProxy/identity/names and Window.name briefs.

## Added tasks and acceptance

**ADDED 2026-11-19 — F.4.42:** prove MediaSession's cross-realm action capture,
associated entry, incumbent/current/report roles, conversion-time retirement,
handler replacement/removal during invocation and departed activity. Reuse the
shared helper, but supply consumer-level independent assertions and fixes if
those assertions expose a defect.

**ADDED 2026-11-19 — F.4.43:** audit and implement adoptedStyleSheets' trusted
implementation provenance, including iterator/getter reentry, incumbent/source
record preservation, exceptions and retained closures after retirement. Verify
that dynamically supplied author code retains author provenance. Do not mark
inline event handlers or arbitrary evaluated strings as trusted code.

F.4.36 is complete when this inventory and these explicit dispositions are
reviewed. F.4.38 depends on the two added tasks as well as C3 and the existing
Window integration. F.4.39–40 retain full acceptance and publication. F15 owns
checked-lifetime migration and release proofs; this audit supplies no release
permission, guest result, WPT result or hardware acceptance.
