# F4d — mutable Window target names

Implementation PR **#680**. F.4.41 implements mutable names for private origin-bearing Windows. Baseline:
merged userland `4602ef25`, including #676. Ordinary Yonder bindings retain
existing behavior; frame-container synchronization belongs to F.6.

## Initial task list

- IDL getter/setter, receiver checks, conversion and post-conversion authority.
- Charged native storage, exact name lookup and failure atomicity.
- Replacement/reset rules and independent native/engine regression evidence.

## Representation and authority

A context reserves 256 name bytes plus an explicit length in its charged
record at admission. The existing private limit remains 255 encoded bytes;
oversized script assignments throw `QuotaExceededError` without truncation.
This bounded implementation policy is not a claim that HTML imposes that limit.
UTF-8 encoding preserves unmatched UTF-16 surrogates; explicit lengths preserve
embedded NULs. Native child lookup and cross-origin atom lookup compare the
complete string. DOM element matching refuses embedded-NUL names rather than
matching a truncated prefix. Empty names do not supply named properties.

The configurable, enumerable Window own accessor implements the
[HTML name member](https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-name).
The actual Window receiver and native caller's same-origin authority are
validated before WebIDL string conversion. Conversion may reenter; the setter
rechecks caller and target eligibility, then commits only to the original
context generation and active binding. A valid live Window without an
associated context returns an empty string and converts but ignores writes.
An explicitly borrowed member with a departed raw Window receiver retains the
private member layer's `SecurityError` policy. Ordinary raw-global property
access uses the engine's embedding receiver (the proxy), just as a retained
WindowProxy selects the current target on each access.

Native commit performs no allocation, script entry or provider callback. It
changes bytes and length without changing publication generation. Recursive
renames during conversion are allowed; a successful outer assignment writes
last. Conversion exceptions, oversized values and stale/retired targets cannot
partially write storage. Engine allocation failure follows the runtime's
failure policy; preserving old bytes does not claim runtime recovery.

## Publication

Same-origin replacement preserves the name. Origin-changing replacement of a
root clears it; private roots have no opener. Nested contexts preserve names
across origin changes. This follows HTML's
[cross-document navigation finalization](https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalise-a-cross-document-navigation)
condition for non-auxiliary top-level contexts. The comparison uses canonical
origin identity, not URLs. Validation/refusal precedes the reset. F.7 owns
session-history integration and restoration.

The borrowed native name pointer remains context-owned until grouped teardown,
but its contents can change at rename or publication. Callers use the length
query, not C-string assumptions. Reserved bytes remain charged on discard;
this task grants no slot reuse or reclamation permission.

## Evidence boundary

`tools/test_dom_window_name.inc` checks native bytes and generation separately
from JavaScript reads and parent lookup. It exercises borrowed/forged receivers,
security-before-conversion, recursive and throwing conversion, symbols, NUL,
Unicode/surrogates, byte limits, duplicates, caller/target retirement, live
unassociated Windows, nested replacement and root resets. Engine allocation
cuts cover conversion with old-byte and teardown checks under both policies.
Existing context admission tests check the full reserved record charge.

Validation results are recorded with the implementation PR. No guest, WPT or
P5 result is claimed; public iframe publication remains gated by F4 acceptance.

Focused validation passes **582 shipping / 686 instrumented checks** under
both teardown policies. Conversion uses existing shipping allocator arenas
(zero intercepted allocations); the instrumented profile exposes one allocation
and refuses it without changing native bytes. Thirteen adjacent suites pass in
each profile: contexts, Window names/identity/proxy/authority, Location
exotic/context, navigation and binding authority, classic DOM/methods, ordinary
Window and scheduled settings. ASan/UBSan and outside-sandbox LeakSanitizer are
enabled. Two faulty variants (omitted caller recheck and NUL truncation) fail
the intended assertions in both profiles without sanitizer faults.

Target libdom, Yonder, js and dom-realms-test builds pass, as do manifest/header,
allocator/global/visibility, tracker and stale-reference checks. Public DOM
function exports remain 153 (plus the two linker symbols); ABI/engine identity
are unchanged.
