# F.4c — native context identity and document publication

This milestone gives the trusted host a stable, cluster-owned context record
whose active document can change. It builds on private origin-bearing Window
and Location admission. It does not create a WindowProxy, change script-visible
parent/top/frame answers, or enable frame loading in Yonder.

## Identity and ownership

`libdom/browsing_context.h` is private. A context contains an active binding,
nonzero generation, immutable parent, ordered child links and a copied target
name. Multiple roots may share a cluster. Duplicate names retain separate
identities and insertion order; name-based script lookup is a later gate.
Names are bounded to 255 bytes, with an explicit refusal for larger names.

Records and their names are charged to the cluster's DOM cap. The cumulative
record count is bounded by `D_REALMS`, statically checked against the runtime's
realm cap. Discarded identities are retained tombstones and do not release a
slot. Bindings retain their context back-reference through departure. A binding
cannot publish into a second context, including after its first context was
discarded. Context storage is released during grouped root teardown after the
bindings and engine no longer use it; no finalizer owns native context lifetime.

A generation-qualified target query returns a binding only if the generation
still matches and the binding remains eligible. It grants no lease across
reentry. Parent/top/name queries describe retained identity, including tombstone
ancestry; they do not authorize script access or provider activity. Child queries
return the live ordered links and return no children for a discarded context.

## Preparation and publication

`d_create_child_with_origin` reuses staged child installation with a canonical,
copied origin present before native interfaces are installed. The host supplies
installation clock/timing snapshots; setup does not call document providers.
Native preflight includes the binding shell and the origin's allocation header
in aggregate admission. Early refusal preserves caller ownership. After transfer,
failed candidates retain their shell, backing and charges under the existing
child-installation contract. Ordinary child creation uses the same path without
origin authority, preserving its public API and behavior.

Successful preparation returns an activated but host-private candidate. The host
must not expose it to author code before publication. Preparation does not retire
the old document or reserve a context generation. On local installation failure,
the old target and its eligibility remain intact. A shared engine failure still
requires root cleanup: retaining native identity does not make a failed runtime
usable or roll back its allocator failure. The API reports that distinction using
the existing child result states.

Create, replace and discard require wrapper quiescence plus the cluster's native
provider, dispatch, reaction, animation, pointer and departure barriers. Pending
microtasks or finalization tasks also make publication busy. Quiescence observes
rather than drains work, and protects outcomes borrowed by an enclosing call.

Replacement validates the expected generation, candidate membership/origin/
eligibility, unique binding association and generation headroom for the target
and its live descendants. A stale request or exhausted generation refuses before
retirement. A provider callback or engine entry cannot intervene after these
checks. Retirement uses the wrapper's allocation-free mark with a fresh outcome;
refusal at that point would violate the embedding invariants and is fatal rather
than returning a misleading rollback result.

The commit retires the old document and its descendant documents, turns those
descendants into tombstones, and switches the active binding and generation.
The replaced context keeps its identity, name, parent and sibling position. An
explicit discard retires the target too, unlinks it from its parent, and leaves
an unusable tombstone. Descendant generations cannot wrap. Existing siblings and
other roots remain untouched. These bounded native commits allocate no storage,
enter no engine code and invoke no host callbacks.

Provider cleanup follows publication through `os64_dom_depart_finish` for each
retired binding. Retirement already disables old provider delivery. Cleanup may
invoke host callbacks and may need retry; its failure is not reported as a failed
publication. The host retains provider storage until that barrier succeeds, or
until root cleanup completes. This ordering prevents a callback from observing a
new target with an old generation, while avoiding a false claim that cleanup is
an infallible part of the commit. Moving an element is not a context-discard API;
F.6 supplies remove/reinsert policy.

## Remaining integration

Context records contain no proxy handle or cross-origin descriptor cache yet.
F.4d must attach charged script identities, route retained Location operations to
the appropriate active document, and close real-global escape paths. Public
context/origin answers, source-attributed navigation and policy authorization
remain separate gates. Target names are immutable in this milestone; host/IDL
rename policy and named lookup are not introduced. No realm is reclaimed, no
failed state is reset, and no public browser policy is enabled.

The native mechanism covers F.4c's identity, tree, origin-bearing preparation,
replacement and tombstone foundation. It does not mark F.4 complete or substitute
for the final security/lifetime integration review.

## Validation

`tools/test_dom_browsing_context.inc` exercises both teardown policies and both
host allocator profiles. It checks ordered duplicate-name children, copied names,
record/quota refusal, cumulative capacity, stale generations, provider/task/
departure exclusion, target and descendant exhaustion, replacement, subtree/root
discard, retained identity and grouped teardown. Staged origin-bearing preparation
runs sampled native allocation cuts on both sides of ownership transfer and
checks that the original page still executes after a local refusal. Peak native
replacement charges are recorded before preparation, at publication and after
provider cleanup. Existing child-installation and authority suites cover the
shared paths. Exact receipts accompany the implementing PR.

Both profiles pass **1,078 context checks**. Existing child installation passes
**58 each**, child engine/refusal/budget checks **1,330 / 1,258**, retirement
**9,770 each**, Window authority **304 each**, Location authority **526 each**,
realm routing **678 each**, and browser jobs **801 each**. ASan/UBSan and
LeakSanitizer run outside the sandbox; the prepared engine retains its existing
ASan-only profile. Six shipping-engine negative controls fail behavioral
assertions: stale generation accepted, old document not retired, descendants
left live, provider loan ignored, context charge omitted, and origin allocation
header omitted from child admission.

The private binding grows from **17,376 to 17,392 bytes**, and the cluster from
**2,744 to 2,760 bytes**, through their existing base charges. Each context
allocates **88 bytes plus the name and terminator**. The replacement fixture's
cluster charge is **296,211 bytes before preparation**, **354,643 at publication**
and **354,643 after provider cleanup**, in both teardown policies. These are
fixture measurements, not browser workload ceilings. Generation and context-count
boundary tests place private counters at exhaustion; the child-budget regression
separately exercises retained realm capacity. No reclamation is implied.

Shipping Yonder, JS CLI and the DOM realm consumer compile. Shared exports,
public headers, wrapper ABI and prepared engine are unchanged. Tracker and
stale-reference checks pass. No full DOM suite, guest run or P5 acceptance is
claimed. Matching source, probes, consumers, mutation recipes and logs are
retained with a SHA-256 manifest.

Implementation: PR #653.
