# F.4 — WindowProxy and Location security

Design brief, 2026-11-13. Inspection baseline: `6e25f323` (`userland`,
including F.3b #553). This is tracker slice **F.4**, within the F1 foundation
stage. Contracts below are proposed implementation decisions; they do not
claim that security or lifetime proofs have passed.

## Result and reviewable cuts

A trusted host can replace a context's active document while retaining its
script-visible window identity. Access decisions follow the executing operation
and the target's current state, including calls through retained methods and
descriptors. The host harness can model a frame tree without enabling Yonder
to load or expose frames. F.6 owns the Page-tree adapter; F.7 owns loading;
F.11 owns integrated enablement.

Use these implementation cuts under F.4, stacking only where needed:

| Cut | Change | Acceptance boundary |
|---|---|---|
| F.4a | Engine global-this value, distinct from global storage | Eval, cached scripts, sloppy functions and cross-realm calls agree; standalone defaults unchanged; no browser proxy installed |
| F.4b | Browser settings scopes and pre-WebIDL-call attribution | Current, entry, incumbent and relevant realm cases have independent oracles; callbacks/jobs restore scopes on failure |
| F.4c | Stable native browsing contexts, origin identity and atomic target publication | Three-context host proves replacement, refusal, detach and retained ownership; no script-facing cross-origin exposure |
| F.4d | WindowProxy and Location exotics plus binding integration | Full internal-method, borrowed-member, navigation-policy and lifetime matrix passes; explicit trusted-host opt-in |

Split F.4d further if review size warrants it; an incomplete proxy stays
private to the test harness. An API that publishes it is part of the final
security integration, not an earlier convenience. F.4a–c can land without
turning that API on. F.4 completion records the implementing PRs and proof;
a design merge or a global-this hook alone does not complete it.

F.4d's acceptance includes the security/lifetime read required by
[IFRAME.md's review table](IFRAME.md#the-cut). Record the
review dispositions beside validation. No engine change in this slice clears
the shared failure latch or reclaims departed realms: F.16 and F.15 own those
extensions.

## Source audit

Paths here are relative to `userland/`; engine symbols were checked in the
prepared F.3b engine as well as the tracked patch inputs.

| Source | Observation | Consequence |
|---|---|---|
| `libjs` prepared `quickjs.h`: `JSClassExoticMethods` | Contains property, prototype and extensibility hooks | Reuse native exotics; no speculative hook-table extension or ordinary JS Proxy implementation |
| Prepared `quickjs.c`: `OP_push_this`, `JS_Eval`, `JS_EvalFunction`, `js_global_eval` | Several paths select `ctx->global_obj` as a this-value | Audit global-this independently from global name/lexical storage; changing one opcode is insufficient |
| Prepared `quickjs.c`: `js_call_c_function`, C-function-data call path | Native functions use their defining realm | The `ctx` inside a borrowed binding is not proof of the security caller |
| `libjs/runtime/runtime.c`: `turn_owner`, task/setup guards | Outer task ownership governs accounting | Do not repurpose it as current or incumbent settings |
| `libdom/core.c`, `event.c`, `browsing.c` | Global object supplies window identity, events and related-window answers | Audit exported window values and event targets together; keep the event registry document-owned |
| `libdom/window.c`: `window_member`, `ask_url` | Location uses binding identity/URL, resolves through that binding, and queues an unattributed ask | Separate relevant global, current active target, source settings and queued navigation identity |
| `libdom/document.c`: `domain_set`; `libway/include/way/origin.h` | Domain mutation does not relax origin-keyed access; tuple/opaque origin representation already exists | Reuse comparison rules, supply real opaque identities, and stop deriving security identity from a URL string |
| `libdom/core.c`, `LIBDOM.md` | Retained departed bindings and safe-point provider detachment exist | Context targets can change without freeing old realm-backed objects |

The security audit must inspect both ordinary and optimized property paths,
as well as `Reflect`/`Object` helpers. A hook existing in the header is not
evidence that each relevant path reaches it with the required context.

## Global-this and settings contracts

F.4a adds a retained, GC-marked per-engine-context global-this value. It defaults
to the ordinary global object. The host configures it before author execution;
the value is an object in the same runtime, and changing it cannot change the
global environment's storage or lexical bindings. The setter must validate
its inputs before replacing a reference. Context construction failure and
destruction cover the extra hold. No teardown policy may depend on an engine
finalizer releasing native context records.

Audit script evaluation, compiled/read bytecode evaluation, indirect eval,
direct eval, Function constructors, arrows, strict/module code and sloppy
null/undefined receivers separately. Explicit receivers and strict semantics
stay intact. Global declarations stay on the real global. A mutable
`globalThis` property is not the authoritative internal this-value. The DOM
installs its public aliases when it opts into the complete proxy; a dormant
engine hook does not alter existing Yonder aliases.

F.4b keeps four explicit roles, following the
[HTML settings algorithms](https://html.spec.whatwg.org/multipage/webappapis.html#realms-settings-objects-global-objects):
current execution, outer entry, incumbent callback context and target relevant
settings. A WebIDL security check needs the context before the callee becomes
current. Callback capture/invocation needs its own scoped bookkeeping; it
cannot be reconstructed from a function's creation realm or timer owner.

Proposed bridge shape: native-only push/pop scopes with borrowed realm handles
whose backing is retained by the cluster; a read-only query names the role
requested, rather than a generic `caller`. Scope entry validates runtime and
lifecycle; exit restores previous state on exceptions, interruption and failed
setup. No allocation or author callback occurs in the query. Missing settings
at a security entry refuse access; trusted bootstrap is a separate path.

The F.4b implementing PR must specify exact signatures after its call-site
audit. It must cover bytecode and C functions, C-function-data wrappers,
bound/proxy calls, reflected calls, accessors, native coercion, module/eval
entry, Promise jobs and callback capture/invocation. Test three realms with
different roles in one call. Include a bound foreign native method invoked
by a timer with no author JS frame above it. Keep budget, callback eligibility
and settings ownership separate. F.5 consumes the incumbent bridge for
messaging, but does not supply a missing F.4 navigation security prerequisite.

## Native contexts, origins and publication

Add a cluster-owned context record distinct from `os64_dom_t` and from a
movable Page. Its stable identity owns the proxy handle, active-binding
reference, generation, parent/top relation, child ordering and target name.
Bindings retain their settings and back-reference after document departure.
Context records and failed reservations remain charged and retained until
root teardown in this slice. No pointer into a Page serves as context identity.

Origin input is an immutable copied `way_origin_t`, distinct from document
URL and API base URL. The host chooses an inherited origin explicitly for
initial blank/srcdoc and a fresh opaque identity for an opaque document.
F.4 fixtures exercise those inputs; F.6/F.7/F.12/F.13 supply actual creation
policy. Allocate nonzero opaque IDs from a checked monotonic runtime namespace,
without wrap/reuse. Zero is a denied/invalid identity, not a shared origin.
Do not compare serialized `"null"`, reconstruct origins after URL mutation,
or silently accept an uninitialized token. Parsing/admission failure refuses
publication. Domain assignment preserves the existing non-relaxation policy.

Use a cumulative context cap no greater than `OS64_JS_MAX_REALMS` and charge
record/name/cache storage through the cluster's existing native accounting.
F.4c measures limits and peak replacement cost under both teardown profiles.
F.8 sets the eventual aggregate browser ceilings; it does not excuse an
unbounded new F.4 registry. Generation and opaque-ID exhaustion refuse new
work before changing existing identity.

Publication is a native safe-point transaction:

1. Prepare the candidate settings/binding with F.3's staged installation.
   Validate cluster membership, origin, context relations and retained holds.
2. Preflight publication resources before departing the old binding. Refusal
   leaves the old target, generation and provider routing untouched.
3. With engine and native loans quiescent, mark the old binding departed and
   switch target/generation without allocation or script. Complete service
   detachment through the existing departure barrier.
4. Make the new state observable together. A pending navigation request or
   descriptor cache entry cannot pair an old generation with a new target.

This operation does not retire a document merely because an element moved.
The later host adapter supplies correct remove/reinsert semantics. A discarded
context remains a tombstone while proxies are retained; it has no usable Page
provider. Define detached answers through the internal-method algorithms and
test them rather than treating a null target as an ordinary empty JS object.

Location identity belongs to its relevant Window, rather than being a single
context-wide object reused on navigation. Its document-dependent operations
must resolve the appropriate active document through the context, including
additional method-level checks; an old Location cannot simply keep reading
the retired binding's URL. Preserve relevant-global identity for security and
descriptor caches. Cover old Location/new document and discarded-context cases.

## Exotics and binding integration

Implement against the linked algorithms, with a source-to-test matrix:

- [WindowProxy internal methods](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-windowproxy-exotic-object).
- [Location internal methods](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-location-interface).
- [Cross-origin and IDL security operations](https://html.spec.whatwg.org/multipage/nav-history-apis.html#security-windowproxy-location).

The matrix covers get/set/has/delete/define, own descriptors/keys, prototype
and extensibility, strict assignment, nonconfigurable properties, alternate
receivers, integer boundaries, symbols and `then`. Native proxies must support
the specified behavior without JS Proxy target-invariant restrictions.
The existing exotic-table null-prototype constraint applies.

Named access needs a correction to the broad design: the cross-origin
WindowProxy algorithm includes a restricted child-target-name case. Implement
the specified name set and duplicate ordering, not arbitrary ID/name lookup.
This does not authorize reading a returned child's document. Existing page
named access remains behind the appropriate gate.

Cross-origin descriptor functions are cached by accessing global, relevant
target global and key, with stable identity and charged roots. Navigation must
not turn a cached function into access under obsolete authority. Retained
methods/accessors perform their required invocation checks, including after
cross-origin replacement. Cache allocation failure grants no extra access.

Audit `window`, `self`, `frames`, `globalThis`, `top`, `parent`, `opener`,
`document.defaultView`, iframe access, event target/currentTarget/composedPath,
UI event view and callback receivers. Keep real-global storage private to the
binding implementation. Separate document-owned event records from the stable
public proxy identity so old listeners do not become new-document listeners.
Property replacement and unforgeable descriptors retain their required shapes.

Origin checking on a proxy does not replace the WebIDL member check when a
method/getter/setter is borrowed directly. Inspect constructors, `call`,
`apply`, `bind`, custom receivers and cross-origin descriptor functions. Read
access, write access and invocation permissions need distinct test cases.

## Navigation boundary

Replace unattributed frame asks with a bounded native request recording the
source settings identity, target context/generation, resolved URL, operation,
history behavior and captured activation/policy inputs. Relative URL handling
and source selection follow the relevant Location algorithm; do not resolve
everything against the target binding. Conversion may execute author code:
revalidate context state after conversion and before enqueue/commit.

The host policy seam receives explicit source and target, not a lone `_top`
string. Absent policy is refusal for the opt-in frame path. F.4's harness
proves refusal/permission, activation-sensitive requests and stale-target
rejection; F.7 implements loading/framing checks and F.13 sandbox enforcement.
No unsupported sandbox/srcdoc document can be installed in production to
exercise a permissive stub. Cross-origin property permission is not navigation
permission. Source retirement, target discard/replacement and rejected policy
must leave no request reaching a detached provider.

Preserve the existing no-new-popup and top-level-history rulings. F.5 provides
cross-realm message delivery; before it lands, F.4's opt-in test seam must not
quietly route a foreign `postMessage` through the old same-binding implementation.
Define the unavailable operation explicitly and keep production frame exposure
disabled. Messaging completion remains a separate dependency of F.11.

## Required proof and acceptance

| Area | Counterexample and observation |
|---|---|
| Global identity | Top-level/compiled/indirect-eval/sloppy this, strict/module distinctions, explicit receiver, globals and lexical declarations, separate realms, bytecode round trip, GC and construction failure |
| Attribution | A enters B which uses C's native method; borrowed accessor/Reflect; callback with captured incumbent differing from callable realm; exception/interruption restores scope without renewing budget |
| Origins | Default ports, distinct tuples, inherited blank token, equal versus distinct opaque IDs, serialized null collision, malformed/unset origin, domain mutation |
| Replacement | Same -> cross -> same origin in one context; proxy equality; old descriptor/method/Location/Document/node references; failed candidate leaves old page usable |
| Internal methods | Execute the full method matrix through language syntax and Reflect/Object helpers; rejected access has no getter/proxy/provider side effect |
| Child lookup | Indexed bounds and mutations; names colliding with built-ins, duplicate names with a disallowed first child, name versus element ID, child reordering and replacement |
| Navigation | Entry base differs from target base; incumbent differs from entry; conversion replaces/removes a target; borrowed href setter/replace; activation/policy denial; no stale dispatch |
| Ownership | Retain proxy, Location, descriptor functions and old DOM data through departure; poison detached providers; repeated navigation/cache churn hits measured bounds; root cleanup releases holds |
| Default behavior | Legacy single-document host, both modern settings, Window events/named properties/Location/history and existing realm/Streams tests retain their supported results |

Use target and fully instrumented engines, both teardown profiles, allocation
failure cuts, and ASan/UBSan/LSan. Diagnose sandbox attachment denial and rerun
leak-enabled outside it when supported. Mutants must catch wrong caller,
URL-derived origin, stale generation, absent member recheck, raw-global escape,
cache-key collapse, premature provider detach and budget renewal.

Record commit/engine identity and named test outcomes, not only totals.
Run contract headers, preparation/manifest tests, applicable runtime/DOM suites
and target links. F.4d adds a three-context guest with parent-observed exit and
heap checks. Compare affected pinned WPT named verdicts against the merged
baseline; keep exclusions and unchanged expected failures explicit. Host
emulation of a context tree is not evidence that browser frame WPT runs.

Prepare a small frozen P5 regression cohort if normal browsing changes. F.11
still owns mixed-frame hardware acceptance. Retain accepted bundles unchanged.
Update this brief's as-built companion, contracts and tracker with each cut;
prepare F.4's done flip only after the complete security/lifetime acceptance.

## F.16 interface coordination

F.16 tracks interrupted environments and prevents failed executable re-entry;
F.4 tracks settings roles and access permissions. Share immutable realm
identity where appropriate, but do not infer either decision from the other.
Keep new hooks additive/opt-in until their consumer proofs pass. Coordinate
patch ordering, public ABI and generated-engine identities before merging
parallel work. Each lane builds a coherent engine/header/native-object cohort.

The first proposed F.4 patch is `0022-browser-global-this.patch`. Its scope is
global-this storage and selection; it does not add failure recovery, an origin
gate or a generic current-caller query. F.4b's settings hook names and call-site
coverage require a separate reviewed interface note. A missing hook or proof
blocks that cut's publication API rather than licensing a guessed caller.
