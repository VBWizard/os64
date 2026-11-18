# F.4b — private Window member authority

This milestone extends the private origin-bearing admission from
[Location authority](IFRAME_F4_BINDING_AUTHORITY.md) to Window member invocation.
Ordinary bindings and Yonder's default policy remain unchanged. The host proof
exchanges raw globals to reach native members; it does not expose frames.

## Receiver and caller are separate

The Window member factories record immutable policy in native function-data
magic. No JavaScript wrapper, nested call, or settings scope is inserted. The
callback resolves the actual Window receiver by object identity in the defining
binding's cluster, then checks the wrapper's pre-native `NATIVE_CALLER` settings
against that receiver's immutable origin. A plain object, primitive or Proxy
receiver fails the Window brand without invoking a trap. Null and undefined use
the function's defining Window, as specified by
[Web IDL operations](https://webidl.spec.whatwg.org/#es-operations).

A borrowed member can therefore operate on a same-origin receiver even if the
function was created in a different-origin Window. Conversely, a same-origin
function cannot authorize a different-origin receiver. Departed source or target,
missing source/target origin settings and unprepared host invocation refuse.
The defining binding's retained function need not be an active document when the
source and actual receiver remain usable. Native state is still retained until
the grouped teardown; this adds no independent reclamation path.

[HTML's IDL check](https://html.spec.whatwg.org/multipage/nav-history-apis.html#integration-with-idl)
runs before argument conversion. Its
[CrossOriginProperties](https://html.spec.whatwg.org/multipage/nav-history-apis.html#crossoriginproperties)
exceptions are recorded separately for methods, getters and setters: close,
focus, blur and postMessage; the window/self/location/closed/frames/length/top/
opener/parent getters; and the location setter. Exemptions skip origin equality,
not the private live-settings requirement. Renaming or copying a function does
not change its policy. Fetch converts a boundary error into a rejected promise,
following Web IDL's promise-returning operation rule.

## Covered installation paths

The common method and accessor factories annotate Window installations. Their
native callbacks use the receiver resolver before operation-specific code. This
covers dialog, timer, frame/idle/microtask, report, clone, messaging, fetch,
base64, style/media/view, selection, browsing, storage, performance, screen,
crypto, navigator, status and property-handler entry points. EventTarget's
inherited methods additionally check a Window target after resolving its entry;
body handlers that reflect to Window use the same check. Non-Window EventTargets
keep their existing path.

The direct window/top getters and held document/history/customElements getters
also use the boundary. Private self/frames are accessors with Replaceable
setters. Retained getters return the receiver's object; Replaceable setters
write that Window rather than the incoming execution context's global. The
original data roots still keep held values alive. Policy bits fit the engine's
signed 16-bit function-data magic alongside the existing setter bit. No new binding-native allocation, public structure, runtime/engine change or
shared-library export is introduced by this member policy. The private alias
accessor functions use the existing engine allocation and budget path.

A new Window callback installed through these factories must call
`d_member_callback` before inspecting magic or converting arguments. A new held
Window getter needs an explicit receiver mapping; installation refuses unknown
mappings. Cross-origin exceptions are centralized in `d_window_member_magic`.

## Boundaries still open

This is invocation authority, not a property membrane. Raw global data properties,
constructors, retained associated objects and legacy-originless functions are
not secured by this factory annotation. Cross-origin property access/descriptors,
WindowProxy identity, escape closure and replacement remain F.4d gates. F.4c must
supply stable browsing contexts and publication; current root-only context
answers remain. Public origin/context answers and postMessage source/target
semantics are not upgraded here. Location assignment still reaches the existing
navigation ask; source-attributed navigation, sandbox/activation checks and
reentrant authorization remain required before browser exposure. F.4 stays open.

## Validation

`tools/test_dom_window_authority.inc` exercises actual installed members under
both teardown policies and both host allocator profiles. It distinguishes
receiver, defining owner, task entry and pre-native source; tests borrowed held
values and Replaceable setters, callable proxies, bound functions, foreign
intrinsics, missing/retired settings, inherited events, promise rejection and
refusal before conversion. The existing Location authority and default binding
regressions accompany it. Exact results and negative controls are recorded with
the implementing PR's frozen evidence.

Both profiles pass the focused Window fixture (**304 checks each**) and existing
Location authority (**526 each**), Window (**666 / 820**), browser jobs (**801
each**), realm routing (**678 each**), listeners (**1,736 each**) and handlers
(**5,526 each**). ASan, UBSan and LeakSanitizer are enabled outside the sandbox;
the prepared engine uses its established ASan-only instrumentation. Five
shipping-engine negative controls fail behavioral assertions: defining owner
instead of receiver, entry instead of pre-native caller, setter exemption
leakage, caller global instead of target global, and omitted EventTarget guard.

Shipping Yonder, JS CLI and the DOM realm consumer compile. The shared-library
export list matches the predecessor, public headers are unchanged, and tracker
and stale-reference checks pass. No full DOM suite, guest execution or P5
acceptance is claimed. Executed probes, compiled consumers, mutation recipes,
source and logs are frozen with a verified SHA-256 manifest.

Implementation: PR #652.
