# F.4b — observer callback settings

`os64_dom_cluster_create_with_observer_settings` extends the handler settings
opt-in to MutationObserver, ResizeObserver, IntersectionObserver and
PerformanceObserver. Existing constructors retain their selection of callback
families. Yonder does not opt in; F.4 remains in progress.

## Capture and ownership

Construction captures the caller's incumbent and the callback object's
associated realm before reading constructor options or `newTarget.prototype`.
A callable proxy retains its creation realm independently of its target and
mutable prototype. After those getters return, publication checks the activity
binding and captured settings again. Refusal releases the capture together
with any converted options; no observer is published.

Each observer owns one callback reference through the shared scheduled-callback
holder. Its engine object traces that value once; the holder's other fields
are retained realm identities, not additional engine edges. Registration keeps
its existing observer-object root. The finalizer empties and releases the
capture. Delivery pins the observer object so disconnecting, dropping author
references or collecting during a callback cannot invalidate its capture.
The holder adds 40 bytes per observer on x86-64; PerformanceObserver also keeps
an 8-byte binding pointer for cleanup. These records retain the existing native
or engine allocation accounting, respectively.

## Delivery

Activity, captured incumbent, associated entry and executable realm remain
separate gates. Inactive records are consumed without entering author code.
The executable-realm lookup creates revoked-proxy errors in the associated
realm; reporting retains the previously recorded script/report owner.

Mutation notifications and the PerformanceObserver timer use the nested
prepared callback adapter inside their existing wrapper guard. Resize and
intersection callbacks use the host prepared-call adapter. Their existing
queue order, resize depth loop, checkpoints, reporting and terminal outcomes
remain in place. In particular, PerformanceObserver callbacks still share one
task and checkpoint at its end (the existing debt); this chapter does not
change that scheduling contract.

All four callbacks receive their observer as `this` and second argument;
PerformanceObserver retains its third options argument. Their entries keep
the existing construction realm. Preparation selects entry B and captured
incumbent A even when code executes in C and activity belongs to D.

The engine identity remains
`39063a8f7f5747a9737b4f15b67dddb87f6344021bfba9fe3ae79a12ef020fbf`.
No engine patch, wrapper ABI/layout change, recovery-policy change or native
loan change is introduced. F16's recovery constructor remains separate.
Rebuild DOM objects together after the internal record changes.

## Source basis and proof

WebIDL [callback conversion](https://webidl.spec.whatwg.org/#es-callback-function)
and [invocation](https://webidl.spec.whatwg.org/#invoke-a-callback-function)
provide capture and preparation. The receiver and argument contracts come from
[DOM mutation notifications](https://dom.spec.whatwg.org/#notify-mutation-observers),
[resize broadcast](https://drafts.csswg.org/resize-observer-1/#broadcast-active-resize-observations),
[intersection notifications](https://w3c.github.io/IntersectionObserver/#notify-intersection-observers-algo)
and [PerformanceObserver tasks](https://w3c.github.io/performance-timeline/#queue-the-performanceobserver-task).

`tools/test_dom_host.sh --observer-settings` exercises independently named
A/B/C/D realms under both teardown policies and engine profiles. It covers
capture and delivery, receiver/record identity, independent role retirement,
constructor getter exceptions and retirement, unprepared host/Promise refusal,
legacy constructor selection, disconnect/GC during invocation, callback-root
release, native and engine allocation refusal, revoked-proxy continuation and
terminal interruption. Each case checks scope restoration and balanced cleanup.

Custom elements, Promise reactions/thenables and the pre-member-check boundary
remain F.4b work. Browsing-context/origin ownership (F.4c), WindowProxy/Location
security and navigation (F.4d), visible frames and hardware acceptance remain
separate. This is not a complete HTML/WebIDL conformance claim.
