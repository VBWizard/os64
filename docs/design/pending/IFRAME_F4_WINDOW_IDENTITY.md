# F.4d — Private Window identity integration

The private `d_window_identity_prepare` host seam connects the native
[WindowProxy boundary](IFRAME_F4_WINDOW_PROXY.md) to the engine and DOM values
that expose a Window. Raw globals continue to own properties and document
listener records. This is preparation for the frame adapter, not public iframe
activation. [Window names and descriptors](IFRAME_F4_WINDOW_NAMES.md) adds
same-origin named lookup and private prototype/unforgeable integration.
F.4 remains in progress: mutable target names and complete security/lifetime acceptance
in [the F.4 brief](IFRAME_F4_SECURITY.md) remain gates. Cross-frame message
posting remains explicitly unsupported pending F.5; accepting a Window as a
synthetic MessageEvent source does not enable delivery.

## Preparation and publication

The trusted host prepares an origin-bearing binding under checked context
access before its first author script. The supplied engine context must own
that binding's raw global. An initial binding must be the context's active
Window; an unassociated replacement may prepare against its destination
context. Existing preparation is idempotent for that destination and refuses
another destination.

Preparation checks the own `globalThis` descriptor without invoking a getter.
It must be a writable, configurable data property holding the raw global.
The stable proxy is prepared and retained, the property is replaced, then the
allocation-free `JS_SetGlobalThis` hook sets the engine receiver. No fallible
work follows the property update. The binding records a borrowed pointer to
the cluster's proxy root. A refusal leaves the property, engine receiver and
binding marker unchanged; a lazily prepared context proxy may remain retained
if a later property update refuses. Engine allocation failure follows the
runtime's existing failure policy, rather than promising a retry after OOM.

A prepared replacement must not execute until native publication: its proxy
still targets the outgoing Window before that commit. This is a trusted host
sequencing requirement, not a new runtime eligibility state. Native replacement
checks preparation before retiring any document: an identity-enabled context
requires a prepared candidate, and a candidate prepared for another context
refuses. Creating a fresh native context from a prepared candidate also refuses.
Publication remains allocation-free. Replacement and discard preserve the
retained document's Window identity while property checks use the current
Window's origin.

The writable `globalThis`, `self` and `frames` properties are distinct from the
engine's internal receiver. Replacing or deleting them cannot replace that
receiver. Global declarations and ordinary property storage remain on the raw
global. No engine patch, public header, public export or browser opt-in is
added by this milestone.

## Exposure and ownership map

| Surface | Script value | Native ownership / authority |
|---|---|---|
| Global code, eval, Function and sloppy calls | Engine-configured proxy | Existing global storage and settings rules |
| `document.defaultView`, including borrowed getters | Receiver document's proxy; null for a windowless document | Retained document owner, not getter's defining binding |
| Window EventTarget receivers | Current proxy target | Same-cluster native brand resolution followed by member authority checks |
| Event target/currentTarget, Window path entry and function listener `this` | Proxy | Per-document raw Window listener record; object listeners keep their object receiver |
| Reflected Window handlers and `onerror` | Proxy | Original handler record and existing callback eligibility/settings |
| UI event view/Window relatedTarget; synthetic MessageEvent source | Native Window identity, including foreign Windows | Conversion performs no author lookup and grants no property authority |
| Timer receiver | Target Window proxy | Existing captured callback/settings and departure cancellation |
| rAF, idle and media callbacks | Captured paths retain undefined; sloppy conversion uses configured identity | Legacy explicit-Window fallback uses the same identity helper |
| Custom-element runner | Identity helper | Existing private runner and reaction ownership |

`d_binding_global` remains the storage helper. Bootstrap reads of Symbol and
Array remain raw storage reads. The standalone legacy Window getter and
standalone postMessage path retain their ordinary behavior; proxy-bearing
postMessage targets refuse before argument conversion. Frame element getters
and the production frame adapter remain outside this private integration.

The binding grows from 17,488 to **17,504 bytes** with alignment; its existing
charged allocation accounts for this. The borrowed identity pointer adds no
separate allocation or engine root. Native contexts remain 112 bytes and
DValue records 112 bytes. Rebuild DOM objects when integrating this layout.
Proxy roots, native contexts and retired bindings remain retained until cluster
teardown; this does not relax F.15/C3's reclamation or compatibility veto.

## Evidence

`tools/test_dom_host.sh --window-identity` prepares identities before running
scripts and tests both teardown policies. Trusted raw-global exchange provides
independent oracles; it is not a production exposure API. The focused fixture
covers script/direct and indirect eval/Function/sloppy/strict receivers,
property storage, borrowed document getters, synthetic and host event identity,
object listeners, body-reflected handlers, onerror, timer and rAF receivers,
foreign target refusal, forged Proxy conversion without traps, same-origin and
cross-origin replacement, outgoing listener isolation, discard and mutable
aliases. It also checks wrong-context preparation, locked/accessor globalThis
refusal, preparation destination checks and allocation-free publication.

Shipping-core and ASan-core profiles pass **412 / 476 checks**, with ASan,
UBSan and LeakSanitizer enabled. The preparation sweep covers two host
allocation cuts per policy in shipping core and four in ASan core, plus two
native DOM cuts per policy in both. Native allocation refusal is retryable;
engine OOM tests preserve the existing containment policy.

Adjacent suites cover WindowProxy, Location exotics, native contexts, Window
and binding authority, events, listener/handler/timer/scheduled/custom settings,
custom elements, callback reports and realm retirement in both host profiles.
Negative controls remove the engine hook, document-owner selection, listener
receiver conversion, event target/path conversion, synthetic source conversion,
UI Window conversion, timer receiver conversion and replacement preparation
requirement. Each must fail behavioral assertions with clean sanitizer teardown.
Yonder, js, dom-realms-test and js-runtime-test are built; source preparation,
contract headers, teardown audit, tracker and stale-reference checks accompany
the change. Guest execution, WPT and P5 acceptance are not claimed here.
