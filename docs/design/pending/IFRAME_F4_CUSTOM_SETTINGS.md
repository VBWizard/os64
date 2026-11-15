# F.4b — custom-element callback settings

`os64_dom_cluster_create_with_custom_settings` extends the observer-settings
opt-in to custom-element constructors and lifecycle callbacks. Earlier cluster
constructors retain their callback-family selection. Yonder does not enable
this opt-in; F.4 remains in progress.

## Definition and ownership

`define()` captures the constructor after name conversion and callable
validation, before options or prototype getters. Each lifecycle callback is
captured immediately after its getter returns, before reading the next one.
After the definition getters finish, publication rechecks the registry's
activity and the captured entry/incumbent identities. Throwing or retiring
getters release temporary captures and leave the name available for retry.
Form-associated callback getters receive the same conversion treatment;
ElementInternals and form-associated behavior are not added.

Definitions keep their existing owning `DValue` roots. Five pairs of borrowed
entry/incumbent identities add 80 bytes per definition on x86-64, accounted
under custom-element native memory. Invocation creates a borrowed callback
view over the registry root; the checked wrapper holds the callable across
reentry. These identities rely on stable wrapper realm storage until runtime
teardown. They introduce no independent context pin or reclamation guarantee.
F15 must account for this retained metadata alongside the existing roots.

Definition publication is transactional: if allocation or reporting metadata
fails, the newly retained root prefix is released, preserving older roots and
definitions. No author getter runs while that prefix is being published.
The defining flag is reset after a throwing getter so a healthy retry works.

Reporting provenance uses the existing callback-record helper. A callable
proxy already revoked when read remains a valid callback value; unavailable
executable provenance falls back to the registry, and invocation reports the
error using the callback's associated realm. This correction and publication
rollback also apply to legacy clusters; their invocation settings stay legacy.
A revoked constructor can still fail earlier when its prototype is read.

## Construction and reactions

Upgrade and synchronous `createElement()` construction use the nested prepared
callback adapter with `JS_CallConstructor`. Lifecycle reactions use its call
form. Both stay inside the admitted operation's existing turn and guard;
preparation does not renew its budget. Construction stacks, failed-element
fallback, initial attribute/connected ordering, nested CEReactions and backup
reaction queues retain their existing algorithms. Direct author `new C()`
keeps the author's calling settings rather than applying the definition's
captured backup.

Activity follows the element's current document. Registry ownership, report
ownership, captured incumbent, associated entry and executable realm remain
distinct. An adopted element may still invoke a healthy callback after its
registry realm retires. Retiring the activity document, captured incumbent,
associated realm or executable realm suppresses that delivery. Revoked-proxy
errors and callback exceptions preserve later healthy reactions; terminal
outcomes unwind through the enclosing guard.

Native callback fixtures expose the captured backup incumbent. Real JavaScript
class frames legitimately take precedence over that backup. Bound functions
follow their target's associated realm and preserve their explicit receiver;
callable proxies keep their creation realm independently of the target or
mutable prototype. Native ABI context selection remains as documented in
[the native context proof](IFRAME_F4_NATIVE_CONTEXT_VALIDATION.md): generic C
constructors receive their defining context, while CFunctionData callbacks
receive the incoming context. Neither dispatch fact substitutes for settings.

The engine identity remains
`39063a8f7f5747a9737b4f15b67dddb87f6344021bfba9fe3ae79a12ef020fbf`.
There is no engine patch, wrapper ABI change, recovery-policy change or new
native loan. Rebuild DOM objects together after the internal layout change.
F16's recovery constructor remains a separate opt-in.

## Source basis and proof

HTML's [definition algorithm](https://html.spec.whatwg.org/multipage/custom-elements.html#dom-customelementregistry-define)
provides getter order, definition publication and reactions. WebIDL
[callback conversion](https://webidl.spec.whatwg.org/#es-callback-function)
and [callback construction](https://webidl.spec.whatwg.org/#construct-a-callback-function)
provide capture and preparation. These contracts are separate from ordinary
ECMAScript calls made directly by author code.

`tools/test_dom_host.sh --custom-settings` names independent realms A (capture),
B (proxy association), C (execution), D (registry) and E (adopted activity).
It covers both teardown policies and engine profiles: construction, parser
upgrades, nested and backup reactions, four lifecycle families and their
payloads, independent role retirement, getter ordering/refusal, direct new,
bound functions, real classes, revoked callbacks/constructors, terminal
interruption, missing host/Promise settings and legacy opt-in selection.
Allocation sweeps check refusal, unchanged registry roots, preserved earlier
definitions, healthy retry, scope restoration and balanced final cleanup.

Final validation uses ASan/UBSan and LeakSanitizer on a coherent DOM build:

- Full shipping DOM: **356,633 checks, zero failures, 12,949 binding
  allocation sweeps**.
- Custom settings: **9,330 shipping-engine / 12,000 instrumented-engine
  checks**, zero failures, both teardown policies. Each policy tests 11 native
  allocation cuts, plus 11 shipping / 26 instrumented engine allocation cuts.
- Recovery: **2,532 / 6,672**; host recovery: **674 / 674**, all passing.
- The instrumented cohort also passes custom elements (298), observer settings
  (10,704), handler settings (5,526), listener settings (1,736), scheduled
  settings (6,066), timer settings (718), callback reporting (1,118), report
  retirement (616), realm retirement (9,770), realm routing (678), departure
  (353), child installation (1,306), shadow (135), Streams realms (104),
  events (1,020), reporting (4,523), window (820), mutation observers (32),
  resize (1,309), intersection (409) and performance observers (3,169).
- Twelve isolated defects are rejected by named assertions in both profiles:
  wrong entry/incumbent, unprepared upgrade/create/lifecycle invocation, late
  lifecycle capture, missing registry/settings rechecks, missing root rollback,
  wrong activity owner, wrong opt-in and eager revoked-callback refusal.
- Target libdom, Yonder and CLI builds pass; CLI suites pass **199 / 40** checks.
  Contract headers/manifest, teardown, CEReactions parser, suite inventory,
  tracker, local links and stale-reference audits pass. No new full-runtime
  campaign is claimed.

Promise reactions/thenables, the pre-member-check boundary and remaining
role-specific integration proofs remain F.4b gates. F.4c browsing contexts and
origins, F.4d WindowProxy/Location security and navigation, visible frames and
hardware acceptance remain separate. No WPT, QEMU or P5 acceptance is claimed.
