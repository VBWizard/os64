# F4d — Window names and descriptor integration

Private origin-bearing bindings expose live Window named properties through a
`WindowProperties` object between `Window.prototype` and
`EventTarget.prototype`. The raw global remains the ordinary storage object;
the stable WindowProxy follows its current document's prototype chain. This
implements F.4.34 and the existing-member descriptor/prototype obligations of
F.4.35. Ordinary Yonder bindings keep their existing behavior. Browser frame
publication, F4 closeout and C3 remain gated.

## Names and visibility

[HTML named access](https://html.spec.whatwg.org/multipage/nav-history-apis.html#named-access-on-the-window-object)
combines child target names with element IDs and the `name` attributes of HTML
embed/form/img/object elements. IDs include non-HTML elements. Empty names,
other elements' name attributes, detached nodes and shadow-tree descendants
do not qualify. Each element appears once even if its ID and name both match.

For duplicate child names, the first child's current origin decides whether
that child name is supported. A later same-origin duplicate does not bypass
an earlier foreign child. An independently supported DOM name can still
select that first child's proxy, as HTML's value-selection algorithm requires.
Without a matching child, one matching element returns its wrapper; multiple
elements produce a live HTMLCollection in document tree order. That collection
uses the same filter and follows additions, removals, attribute changes and
adoption. Repeated collection lookup does not promise object identity.

The [WebIDL visibility algorithm](https://webidl.spec.whatwg.org/#dfn-named-property-visibility)
lets global own properties and non-named-properties prototypes shadow these
names. Descriptor inspection does not invoke getters. The implementation skips
named-properties objects when inspecting the chain, avoiding recursion, and
re-resolves membership after potentially reentrant prototype inspection. Native
tree/prototype walks periodically observe the existing task budget.

Child names/order come from F.4c's retained native context records. This is a
trusted host model, not the F.6 frame-container adapter. Context replacement
changes the proxy's target and its name lookup; retained named-properties
objects and collections continue to refer to their original document.
Discard removes a child from the parent's ordered live list. A retained name
object does not acquire cross-origin WindowProxy property authority.

## Descriptors and ownership

[WebIDL's named properties object](https://webidl.spec.whatwg.org/#named-properties-object)
reports virtual name descriptors as writable, enumerable and configurable.
Ordinary own-key enumeration contains its class-string symbol, not virtual
names. Consequently those names do not appear in Window's own keys or `for-in`.
Assignment through Window can create a global own property; deleting that
shadow reveals the live named value again. Indexed WindowProxy properties
retain their separate precedence and write restrictions.

The named object refuses definitions, deletions and prevention of extensions,
including mutation of its class-string property. Its prototype is immutable.
The private Window and EventTarget interface prototypes also have immutable
prototype links while retaining ordinary property mutation. An empty,
nonextensible engine guard supplies definition refusal with the correct
throw/strict/sloppy behavior; returning false directly from a native define
hook is insufficient for QuickJS's throwing callers.

The private Window's `location` accessor keeps its PutForwards setter and
becomes nonconfigurable, joining the existing unforgeable `window`, `document`
and `top` members. Borrowed members still validate the actual receiver before
conversion. Cross-origin method descriptor caching retains HTML's distinction
between a callable original property, which gets the native IDL wrapper, and
a noncallable original value; this milestone does not replace that algorithm.

The named object borrows one binding-owned DValue containing its ordinary
class-string backing object. Both the DValue and document shell survive through
grouped teardown. The exotic has no own engine slots, so QuickJS's ordinary
slot fast paths cannot bypass its definition/deletion rules. Its prototype
edge is engine-traced; its backing root follows existing binding accounting
and drain. No new native finalizer, owner ticket, context slot or public
structure field is introduced. Engine identity and public ABI stay unchanged.

## Added obligation

**ADDED 2026-11-19 — F.4.41:** this audit found no `Window.name` member or
native context rename path. F.4c explicitly admits immutable names with a
255-byte cap. Script-facing mutable names need their own charged ownership,
conversion/security, rollback and duplicate-lookup proofs. They remain a
visible task required by integrated F4 acceptance; F.6 owns synchronization
with frame-container attributes and order. This milestone does not silently
extend the immutable admission contract.

## Validation boundary

The focused `tools/test_dom_window_names.inc` host fixture uses independent raw
globals, native context records and DOM queries. It covers prototype/descriptor
reflection, supported-name filters, collisions, live collections, indexed and
duplicate-child precedence, discard/replacement/adoption, retained objects,
unforgeable members and borrowed receivers. Engine and native allocation cuts
exercise prototype construction and first multi-element lookup under both
root teardown policies, requiring no partial publication and balanced cleanup.
Native lookup refusal also retries after the allocation fault is removed.

Host tests and matching target builds do not claim guest, WPT or P5 execution.
F.4.38–40 retain integrated security/lifetime review, acceptance and publication.

Focused validation passes **1,862 shipping-engine / 3,716 instrumented-engine
checks**, zero failures, under both fatal and reporting teardown policies.
ASan/UBSan and outside-sandbox LeakSanitizer remain enabled. Allocation cuts
cover 1/10 engine allocations at prototype construction and 3/7 at first
collection lookup in the two profiles, plus 1/3 native allocations respectively.

Sixteen adjacent suites pass in each profile: Window identity/proxy/authority,
Location exotic/context, navigation authority, binding authority, native
contexts, classic DOM/methods, events, shadow trees, ordinary Window,
custom-element settings, listener settings and scheduled settings. Four
independent faulty variants fail at the intended behavioral assertions in
both profiles: accepting arbitrary HTML name attributes, skipping prototype
visibility, retargeting a retained name object, and leaving location
configurable. Their failures are not compilation errors or sanitizer faults.

Matching target libdom, Yonder, js and DOM realm-fixture builds pass, as do
source manifest/header, target allocator/global/visibility, tracker and
stale-reference checks. The dynamic function export set remains 153 public
DOM functions; the two linker base/slot symbols are not API functions.
