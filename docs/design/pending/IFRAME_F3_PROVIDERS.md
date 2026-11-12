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

## Entries still to integrate

These are open implementation obligations. They must be handled before a
successful public finish can promise that opaque storage is no longer borrowed.

| Provider family | Entry sites | Required treatment |
|---|---|---|
| Host clocks and navigation timing | `timer.c`, `idle.c`, `animation.c`, `performance.c`, `event.c`, `message.c`, `xhr.c`, `storage.c`, `intersection.c`, `webanimation.c` | Guard native clock reads; distinguish inactive script requests from pure stored timestamps and scheduler no-work answers. |
| Alert, parser write, activation and script connection | `core.c`, `window.c`, `validity.c` | Check service owner after conversions; guard each native call and stop repeated writes/delivery after retirement. |
| Focus and zoom | `document.c`, `screen.c` | Resolve current document, apply inactive-document behavior, and borrow through returned-node consumption. |
| Entropy | `crypto.c` | Guard entropy request after validating output; preserve pure cryptographic operations that use no provider. |
| Broken/failed diagnostics, global miss, script/module/report hosts and console | `attr.c`, `report.c`, `classic.c`, libjs host installation | The global-miss bridge already gates activity; add loan accounting and detach report/loader/output routes without deleting siblings' state. |
| User agent and cookies | `classic.c` | Borrow through copying returned text; guard retrying reads and setters. |
| Storage identity/load/commit and walkers | `storage.c` | Guard normal service entry; settle owed changes once while provider remains valid. Cleanup must also work during root drain, which has set `closed`. |
| History length/update/fragment | `window.c` | Guard after URL/state conversions and through host completion; retained serialized data is separate from navigation activity. |
| Fetch/XHR transport | `fetch.c`, `xhr.c` | Guard start/more/run; keep synchronous response loans through release. Cancel by binding request token, unlink before cancellation reentry, and discard late answers without author abort events. Cleanup release/cancel must remain callable while departing. |
| CSS animation hosts and effect borrowers | `webanimation.c` | Guard count/at/version/drive; stop timeline scheduling while retaining effect identity and tree generations until acknowledgements. Do not reuse destructive animation drain. |
| Other synchronous native visitors | `importmap.c`, `pointer.c`, storage walkers | Count callback extent, stop later callbacks on departure, and retain records while a visitor is running. |

Provider replacement contracts and departure cleanup must agree: clearing a
provider cannot erase the release operation for an existing loan. Image loans
capture the producer explicitly. The owner-thread finish barrier must protect
other borrowed output and the shared registry walks before clearing callbacks.

Validation and remaining limits are recorded in
[IFRAME_F3_DOM_VALIDATION.md](IFRAME_F3_DOM_VALIDATION.md).
