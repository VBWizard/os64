# F.3b DOM validation — shared-delivery checkpoint

2026-11-12. Work in progress against the
[reviewed retirement brief](IFRAME_F3_RETIREMENT.md). This branch starts from
`e696277f`, the merge of F.3a PR #552 into design PR #546. The F.3a review
was clean; the six design findings on #546 have evidence replies and are
resolved. F.3 remains `code`, not complete.

## Implemented boundary

Mutation-observer notification and the custom-element backup queue are
agent work. Their engine jobs have neutral callback/activity ownership, so
retirement of the first enqueuing realm does not discard live siblings'
deliveries. The retained execution context still identifies the shared DOM
agent. Engine staging continues to tag these jobs with the current candidate
transaction; neutral ownership does not bypass candidate abort.

Delivery checks the operation's activity document and the function's realm.
Observer activity belongs to its registration owner. A custom-element
reaction or upgrade belongs to the element's **current** document, including
after adoption; the retained definition's creator is not an activity owner.
Bound functions and callable proxies resolve to their function realm. A
revoked proxy follows the ordinary exception-reporting path. Synchronous
`createElement` constructors use the same eligibility check as queued upgrades.

An ineligible observer loses queued records and transient registrations
without allocating a MutationRecord array. A skipped custom reaction releases
its queued payload. Slot signals for inactive documents are consumed without
dispatch. Eligibility is checked at each delivery, so an earlier callback
can retire a later recipient without suppressing unrelated work.

Storage validation (`d_context`) remains separate. Retained DOM wrappers,
observer methods, definitions and ordinary exported functions remain usable;
this checkpoint does not free a binding or introduce a departure API.

## Evidence

`tools/test_dom_host.sh --realm-retirement` runs a three-binding fixture under
both FATAL and RECLAIM teardown policies. The fixture uses trusted engine
retirement; it does **not** claim to test DOM departure cleanup.

The new regression failed against unmodified F.3a: a job first queued by B
was discarded on B's retirement, stranding A/C observer deliveries. Final
tests cover both shared queues, callback/activity disagreement, bound and
proxy callbacks, revoked-proxy handling, queued and synchronous constructors,
slotchange suppression, an adopted element retaining B's definition with a
live C callback, later queue reuse, retirement during notification, retained
object access and balanced native teardown.

| Final focused suite | Shipping engine | Instrumented engine |
|---|---:|---:|
| Retirement queues | 90 checks | 90 checks |
| Cross-realm binding routing | 678 checks | 678 checks |
| Mutation observers | 32 checks | 32 checks |
| Custom elements, including allocation failures | 298 checks | 298 checks |

These runs retain ASan/UBSan and LeakSanitizer, outside the sandbox that denied
LSan thread attachment during F.3a. Unchanged host/engine objects from the
final F.3a run were reused; `core.c`, `observer.c`, `custom.c` and the host
test translation unit were rebuilt. The native `dom-library` target also
builds successfully from this worktree. No full-suite, WPT, QEMU or P5
acceptance is claimed for this intermediate checkpoint.

Nine isolated source mutants were compiled and rejected by named assertions:
restoring first-enqueuer ownership for either shared queue; ignoring callback
realm or activity eligibility; treating a definition's creator as the current
document; bypassing queued-upgrade, synchronous-constructor or slotchange
eligibility; and retaining discarded observer records. Mutants used temporary
source copies and did not modify the implementation worktree.

## Remaining F.3b work

- Publish the allocation-free departure mark and idempotent safe-point
  finish protocol, with reentrant dispatch/reaction/provider-borrow proofs.
- Audit and gate the other host delivery paths, report routing and provider
  entry points. Inventory and detach provider storage only after borrowers
  finish; preserve retained DOM/Canvas/WebAnimation identities.
- Cancel binding-owned work by its own request tokens, consume late results
  safely, and release scheduled payloads without closing shared connections.
- Install a child DOM transactionally with staged engine setup and native
  rollback, preserving siblings and accounting for retained failed shells.
- Measure installation peak and retained active/departed charges under both
  allocator profiles, set the production reserve/margin, and prove repeated
  admission refuses locally before sticky runtime failure.
- Complete the brief's integration, allocation, budget, mutant, guest and
  pinned-WPT evidence before marking F.3 done or requesting final review.

Frame enablement, independent realm reclamation and child-local overrun
recovery remain outside F.3b. No P5 handoff is required for this checkpoint.
