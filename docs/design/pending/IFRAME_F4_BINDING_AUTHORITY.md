# F.4b — private Location member authority

This milestone connects checked pre-native caller attribution to actual Location
members. It also supplies private origin-bearing DOM admission as groundwork
for F.4c. It does not publish WindowProxy objects, complete cross-origin property
exotics, or enable the policy in Yonder. Ordinary bindings retain their behavior.

## Immutable target settings

The private `binding_authority.h` constructor accepts a canonical `way_origin_t`
independently of the binding's URL and API base. Tuple origins round-trip through
libway before admission; malformed or noncanonical inputs refuse before grouped
ownership transfer. Parser allocation refusal also refuses admission. Opaque
origins use a nonzero, nonwrapping cluster-local issuer. The trusted host must
keep each token with its issuing cluster; copies within that namespace support
inherited origins. Tokens are not transferable capabilities or serialized `null`
strings. Exhaustion preserves the counter and output.

Admission copies and charges the origin before installation. Both the binding
and cluster caps include its allocation header. Early refusal leaves ownership
with the caller; installation failure follows the existing consumed-admission
and poisoned-cluster teardown contract. The copy remains until binding teardown;
there is no post-admission origin setter. This supplies document settings identity,
not F.4c's stable browsing context, replacement generation, or publication
transaction. `document.domain` retains the existing non-relaxation policy.

## Before conversion

[HTML's integration with IDL](https://html.spec.whatwg.org/multipage/nav-history-apis.html#integration-with-idl)
checks the current settings before the member's execution context is pushed.
The native implementation uses the wrapper's checked `NATIVE_CALLER` snapshot
at that boundary, not the data function's context argument, defining realm,
entry, incumbent, or task owner. It resolves that realm to a binding in the same
cluster and compares its immutable origin with the target's origin using libway.
Missing origin-bearing settings, unprepared raw entry and inactive sources
refuse with `SecurityError`. Targets without a native context must also remain
active. Context-bound Location lifetime follows the
[retained Location contract](IFRAME_F4_LOCATION_CONTEXT.md): original-global
identity survives replacement while document access resolves the current target.
Brand validation still precedes this check;
argument conversion follows it, so rejected access cannot execute coercion code.

The policy covers Location methods/accessors and the Window/Document `location`
forwarding path. The [Location IDL exceptions](https://html.spec.whatwg.org/multipage/nav-history-apis.html#crossoriginproperties)
for the `href` setter and `replace` skip same-origin comparison, while the private
host policy still requires usable source and target settings. Location acquisition
and forwarding likewise require those settings without a same-origin check.

This is an invocation boundary, not a secure cross-origin object membrane.
The harness exchanges raw globals and members solely to test the boundary.
Ordinary property lookup, descriptors, cached function identity and retained
WindowProxy/Location behavior across replacement remain F.4d work. The existing
navigation ask path is exercised after permitted IDL access; this milestone does
not add source-attributed navigation requests, sandbox/activation policy, or
reentrant navigation authorization. Those gates precede browser exposure.

## Evidence

The focused DOM fixture uses actual Location objects with document URLs that
deliberately disagree with their host-supplied origins. It distinguishes caller,
task entry, target, bound functions, proxies and foreign intrinsics; checks
borrowed getters/methods and rejection before coercion; and exercises opaque
inheritance, missing settings, retirement, allocation refusal and root teardown.
Both runtime teardown policies run under both allocator profiles. Focused
Location authority passes **526 checks per profile**, including shell/origin
allocation refusal and post-transfer installation failure. Existing Location
checks pass **666 / 820**, browser-job checks **801 each**, and realm routing
checks **678 each**. ASan, UBSan and LeakSanitizer remain enabled outside the
sandbox (the prepared engine follows the existing ASan-only instrumentation).

Shipping Yonder, CLI and the DOM realm consumer compile. The new private entry
points are hidden from shared-library exports. Public layouts, wrapper ABI 5,
bytecode 6 and the prepared engine are unchanged. The binding's private struct
remains **17,376 bytes** (the pointer uses existing padding); the cluster grows
from **2,736 to 2,744 bytes**, charged by its existing base admission. An admitted
origin adds **304 charged bytes** (288-byte copy plus 16-byte allocation header).
No full DOM suite, guest execution or P5 acceptance is claimed for this cut.

Four named negative controls are rejected by behavioral assertions: substitute
entry for pre-native caller, omit the origin comparison, equate distinct opaque
origins, and let IDL exceptions bypass missing settings. These controls run on
the shipping-engine host profile with sanitizers/leak detection retained.

Implementation: PR #651. Matching source, executed host probes, compiled
consumers, commands and logs are frozen with a verified SHA-256 manifest.
