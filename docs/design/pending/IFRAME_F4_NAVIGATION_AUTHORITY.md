# F.4d — private Location navigation authority

This milestone connects context generations to attributed Location requests.
For a binding attached to a native browsing context, Location URL operations
require an explicit host policy and enqueue a bounded native request. The
ordinary single-document path remains unchanged. No WindowProxy or browser
frame loading is exposed.

## Distinct settings roles

The existing IDL member check still observes the pre-native caller. Navigation
uses additional roles from the checked wrapper: the entry document supplies the
live URL base, and the incumbent document supplies the navigation source for
href, assign, replace and hash. Reload uses its relevant document as source.
This follows the separate URL-parsing and source-selection steps of
[HTML Location](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-location-interface).
These roles must not be substituted for one another or for the target binding.

The private request captures source, entry and target bindings, their native
contexts and generations, the policy generation, requested operation, effective
history behavior and resolved URL. Each participant must have active,
origin-bearing settings and a matching published context. Missing policy refuses
before conversion. After string conversion, the path rechecks those identities
and resolves the full URL against the entry document's then-current base. Assign,
hash and reload also enforce their entry-origin check. Conversion can mutate a
base element or retire a participant; neither bypasses revalidation.

Hash changes are encoded relative to the target URL and queued through policy;
an unchanged hash remains a no-op. Full URLs, including fragment-only navigation,
do not invoke the legacy immediate-history provider from this private path.
Reload is likewise queued. Unsupported Location component setters keep their
existing behavior; this does not claim full Location algorithm coverage or
retained old-Location behavior across replacement.

## Host policy and delivery

`navigation_authority.h` is private. The host configures or revokes a policy at
native/engine quiescence. The policy has a snapshot callback for trusted transient
activation, target load status and host restriction bits, and an authorization
callback invoked at enqueue and again at dispatch. No script property supplies
these facts. For non-reload requests, an unloaded target without captured
transient activation selects replacement history. The host authorizes with
explicit source/target identities and the effective operation; a cross-origin
IDL exception alone grants no navigation permission.

Callbacks run under source and target provider loans. Reentrant configuration,
context replacement/discard and nested navigation cannot change a request while
policy is evaluating it. Native retirement can still occur, so identities are
rechecked after callbacks. The enqueue path also checks the existing task budget;
it does not create or renew a budget. Policy code owns its native opaque storage
until successful replacement/revocation or root teardown, and must obey ordinary
host callback lifetime/exception contracts.

One reusable request slot belongs to each target context. The last successfully
authorized request replaces the previous one; conversion, policy or allocation
refusal leaves that predecessor intact. The slot and policy record are charged
to the cluster's DOM cap. Slots remain retained through discard and replacement
and are freed at grouped root teardown. They hold no JavaScript values and add
no GC roots. Policy generations are nonwrapping; policy changes invalidate queued
requests without traversing them.

The private dispatch API requires quiescence and consumes a pending request.
Before calling policy it rejects stale source, entry, target or policy identity.
After policy it rechecks quiescence and identity, then calls the native consumer
under provider loans. A dispatch denial drops that consumed request. The consumer
borrows the record for that callback; asynchronous loading must copy its identity
and generation evidence and revalidate before eventual commit. F.7 still owns
that loader integration. This is a dispatch-time guarantee, not a permanent
navigation capability or a lease on a document.

The legacy `os64_dom_take_navigation` consumer refuses asks from context-bound
bindings, so attributed requests cannot lose their source/generation metadata
through the old structure. Links, forms, history traversal and other navigation
surfaces are not migrated by this milestone and must be integrated before public
frame exposure. Real sandbox, activation and framing policy comes from the host;
this slice supplies no permissive default or production policy stub.

## Validation and limits

The host fixture gives each document a different base and distinguishes native
callback entry from captured incumbent. It exercises actual Location members,
conversion-time base changes, absent/revoked/denied policy, activation/history
selection, hash/reload, predecessor preservation, target replacement, source
retirement/discard, retirement inside policy, request allocation/cap refusal and
provider-loan exclusion. Both teardown policies run under both allocator profiles.
The existing default Window, authority, context, retirement and browser-job suites
accompany it. Exact results are recorded with the implementing PR.

Origin-bearing contexts are a private host seam. Property exotics, retained
Window/Location identity, raw-global escape closure, full navigation surfaces,
F.7 loading, F.13 sandbox enforcement and integrated browser activation remain
open. No F.4 completion, realm reclamation, full DOM campaign, guest execution or
P5 acceptance is claimed.

Both host profiles pass **1,792 navigation checks**. Default Window checks pass
**666 / 820**, native contexts **1,078 each**, Window authority **304 each**,
Location authority **526 each**, browser jobs **801 each**, retirement **9,770
each**, and realm routing **678 each**. ASan/UBSan and LeakSanitizer are enabled
outside the sandbox; the unchanged prepared engine retains its ASan-only profile.
Seven negative controls fail behavioral assertions: entry substituted for source,
target substituted for URL base, missing conversion revalidation, stale target
accepted, policy verdict ignored, missing request charge, and legacy-consumer
bypass.

The binding remains **17,392 bytes**; cluster storage grows **2,760 → 2,768**,
and context records **88 → 96 bytes plus name/terminator**, through their existing
base charges. Configuring policy allocates **40 charged bytes**; a context's first
accepted request allocates **8,296 charged bytes**, reused by later asks. Native
allocation and cap refusals leave no orphan slot. Shipping Yonder, JS CLI and the
DOM realm consumer compile. Public headers, wrapper ABI, prepared engine and
shared exports remain unchanged. Tracker and stale-reference checks pass.
Matching source, probes, consumers, recipes and logs are frozen with a verified
SHA-256 manifest.

Implementation: PR #655.
