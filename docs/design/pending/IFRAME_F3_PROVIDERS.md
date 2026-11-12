# F.3b provider lifetime inventory

Work in progress, 2026-11-12. This is the implementation inventory required by
[the retirement brief](IFRAME_F3_RETIREMENT.md), not a claim that departure
finish is available. `d_provider_enter` refuses inactive document activity and
counts a native loan on both the binding and its cluster. `d_provider_leave`
ends the loan after consuming and releasing the provider's output. A sibling
can observe an outstanding loan through `d_provider_busy`.

The owner thread publishes retirement before releasing obsolete work. Provider
callbacks may mark retirement; they must not free opaque storage while a loan
is outstanding. The public finish operation still needs the complete inventory,
engine/dispatch/reaction barriers and cancellation cleanup. Internal counters
alone do not authorize freeing provider storage.

## Guarded rendering providers

| Entry sites in `userland/libdom` | Activity owner | Loan extent and inactive behavior |
|---|---|---|
| `geometry.c`: geometry read | Element's current document | Provider call; copied snapshot remains binding-owned. Script refuses inactive service, including retirement during the call. Foreign-namespace empty geometry needs no provider. |
| `view.c`: computed-style snapshot | Element's current document | Provider call and output into binding-owned storage; inactive provider query refuses. |
| `view.c`: viewport read, window and element scroll | Window service owner or element's current document | Each native call; no read/scroll after retirement. Script viewport queries and scroll services refuse; native view stepping stops. Pure replacement properties and no-op window focus remain ordinary operations. |
| `hit.c`: hit query, including expanded second buffer | Queried document or shadow root's current document | Each provider call; recheck before a second query. Inactive service refuses without calling the provider. |
| `range.c`: text rectangles, including expanded second buffer | Range's current document | Each provider call into caller-owned storage; stop before a second query if the first marks retirement. Range tree operations remain usable. |
| `svg_geometry.c`: fill hit query | SVG node's current document | Provider call including its bounded cancellation polling and scratch allocations; inactive service refuses. |
| `resize.c`: measurement batches | Each target's current document | Each native batch; discard retired-owner measurements and stop if the observer activity retires. Existing registrations remain. |
| `intersection.c`: measurement batches and viewport read | Explicit root/current measurement document | Each call; retirement during measurement prevents the subsequent viewport call and commit for that owner. |
| `canvas_image.c`: image acquire/release | Source image's current document | Captured producer and opaque remain borrowed through pixel/text consumption and matching release, including refused acquisitions. Retirement stops new queries but does not skip an owed release. Canvas-to-canvas pixels remain pure retained data. |
| `canvas_image.c`: image update/event polling | Queried document | Each update and image loan; stop native polling after callback-triggered retirement. |
| `canvas_text.c`: font layout | Canvas node's current document | From layout entry through retained-run inspection, outline use and run release, including refusal paths. Inactive font service refuses; pixel drawing without fonts remains available. |
| `canvas.c`: damage visitor | Current document whose damage is visited | Entire native walk, including visitor calls and borrowed node/damage arguments. Stop after retirement; retained bitmap storage is not drained. |

## Guarded synchronous services

| Entry sites in `userland/libdom` | Activity owner | Loan extent and inactive behavior |
|---|---|---|
| `core.c`: alert | Dialog's window | After message conversion, through native notice. Inactive provider entry throws; an already delivered notice may finish unwinding. Constant confirm/prompt fallback results use no provider. |
| `window.c`: parser write/open | Document's window | Native parser call after argument conversion. An inactive write answers the existing InvalidStateError path; no parser call occurs. |
| `window.c`, `validity.c`: activation and validation reporting | Node's current document | Native notice and borrowed node. Inactive focus/blur/reset/report notices are omitted, including retirement in a preceding invalid event. Validation data remains usable. |
| `window.c`: connected-script notices | Script's current document | Mark-started and native notice under the loan. Eligibility is checked for each script; a provider that marks retirement prevents later notices in the same tree change. Pure tree mutation remains available. |
| `crypto.c`: entropy | Crypto service's window | Entropy call through scratch consumption and erasure. Refuse inactive service; if the provider marks retirement, erase scratch and leave the caller's buffer unchanged. Empty fills and cryptography that needs no entropy remain usable. |
| `document.c`: focus | Queried document | Native call and returned-node validation. Departed window documents answer null activeElement and false hasFocus without querying the host. |
| `classic.c`: user-agent text | Navigator's window | Provider call through string interpretation/copy. Inactive service refuses; constant Navigator members do not query the provider. |
| `classic.c`: cookies | Document or Navigator window | Each get/set/enabled call. Setters check after conversion; reads recheck after a response, before retrying with a larger buffer. Inactive cookie service refuses. |
| `classic.c`, `attr.c`: missing-global and broken-attribute diagnostics | Diagnostic binding | Native callback. Inactive diagnostics are omitted; retained functions and detached Attr records remain usable. |
| `core.c`: shared millisecond/microsecond reads; callers in timers, idle, animation, events, messages, storage, XHR, intersection and Web Animations | Clock's document | Each host read; inactive reads use binding-owned saved samples. Timer/idle/frame creation and message posting recheck after clock callbacks before publishing work or detaching transfers. Storage settlement rechecks clocks; transport cleanup remains an obligation below. |
| `performance.c`: current time and navigation timing | Performance object's window | Native clock/timing call through snapshot copy. Live `now()` and implicit mark/measure timestamps refuse retirement; explicit timestamps and previously sampled navigation timing remain usable. |
| `screen.c`, `view.c`: zoom and media-query sampling | Screen or media-query window | Each zoom call. A media environment uses one sample; post-call retirement refuses script query results and stops native media delivery. Stored screen metadata remains available. |
| `window.c`: history length/update/fragment | History or Location service's document | Each native call; recheck after argument/state conversion and host completion. Inactive History operations use SecurityError, while exported state objects remain usable. Retirement prevents local completion and fragment fallback even when the host accepted an entry. Pending navigation holds are discarded when taken after retirement; late native moves and history events do no work. |
| `storage.c`: identity/load/commit and cleanup | Storage receiver's document | Each native call; commit holds a loan through serialization and response handling. Inactive normal access refuses, including cached/memory-only areas. Clock, conversion, load retry and quota-driven commit paths recheck activity. Quiescent native cleanup may borrow inactive providers for one final attempt per owed area, then drops pending changes and detaches callbacks; root drain remains supported after `closed`. Reentrant settlement/forget/replacement/drain cannot disturb borrowed maps. |

`os64_dom_storage_walk` and `os64_dom_storage_merge` operate on caller-owned
bytes, without a binding or DOM-owned visitor. They do not create a document
activity loan of their own. A walk invoked from a storage provider callback is
inside that callback's loan. The native storage cleanup entry refuses while
provider borrowers remain; the public finish operation must also establish
the engine/dispatch/reaction barriers before calling it.

## Entries still to integrate

These are open implementation obligations. They must be handled before a
successful public finish can promise that opaque storage is no longer borrowed.

| Provider family | Entry sites | Required treatment |
|---|---|---|
| Failed diagnostics, script/module/report hosts and console | `report.c`, libjs host installation | Detach report/loader/output routes without deleting siblings' state. Missing-global and broken-attribute loans are covered above. |
| Fetch/XHR transport | `fetch.c`, `xhr.c` | Guard start/more/run; keep synchronous response loans through release. Cancel by binding request token, unlink before cancellation reentry, and discard late answers without author abort events. Cleanup release/cancel must remain callable while departing. |
| CSS animation hosts and effect borrowers | `webanimation.c` | Guard count/at/version/drive; stop timeline scheduling while retaining effect identity and tree generations until acknowledgements. Do not reuse destructive animation drain. |
| Other synchronous native visitors | `importmap.c`, `pointer.c` | Count callback extent, stop later callbacks on departure, and retain records while a visitor is running. |

Provider replacement contracts and departure cleanup must agree: clearing a
provider cannot erase the release operation for an existing loan. Image loans
capture the producer explicitly. The owner-thread finish barrier must protect
other borrowed output and the shared registry walks before clearing callbacks.

Validation and remaining limits are recorded in
[IFRAME_F3_DOM_VALIDATION.md](IFRAME_F3_DOM_VALIDATION.md).
