# F.3a engine validation

2026-11-12. Implements the engine half of
[the reviewed F.3 brief](IFRAME_F3_RETIREMENT.md), based on design amendment
`3a8ceb1c` and merged F.2b at `37ec284d`. PR #552 is stacked on design PR #546.
F.3b owns DOM departure, provider cleanup and failed binding installation;
F.3 remains in progress after this engine slice. Engine implementation is
`4a2b28dd`; this evidence closeout changes documentation/tracker metadata only.

The F.3a evidence below describes ABI 4. The F.3b quiescence addendum at the
end records the subsequent ABI 5 entry and its separate validation.

## Implemented boundary

Embedding ABI 4 adds allocation-free engine-headroom inspection/preflight,
guarded creation, irreversible realm retirement, and staged creation/setup/
activation. Retained slots count against the cumulative 65-slot limit.
Preflight accounts for the runtime wrapper's reservation and current engine
charges. Slot/headroom refusal does not allocate or poison siblings; creation
rechecks admission. Pre-existing allocator/cancellation/runtime failure is not
reported as a harmless local refusal. The caller chooses a positive measured
installation reserve plus sibling margin. The 256 KiB regression reserve is
for bare-engine fixtures, not a production DOM admission constant. F.3b must
measure installed/departed bindings and set that reserve for both profiles.

Retirement marks eligibility without allocating, clearing a failure, destroying
a context or mutating the queue under an active stack. Existing stacks unwind;
live scripts can call retained ordinary functions. New host entry checks the
realm and scheduled callback owner. Reporting, module providers and output
respect retirement; the DOM host still must stop its own activities and detach
provider storage at a safe point.

Patch 0019 separates native execution context from job callback/code ownership
and optional activity ownership. Promise reactions follow handlers, thenables
follow `then`, handlerless propagation is neutral, and custom capability
resolve/reject calls remain ordinary calls within an admitted job. Native data
functions, Promise resolvers, async continuations and generators retain their
actual realm. Bound/proxy traversal is iterative; revoked-proxy lookup falls
back without allocating or throwing during enqueue. FinalizationRegistry jobs
check registry and callback realms. Dynamic imports retain importing context.

Manual slices, task checkpoints and private setup drains inspect eligibility
before executing or charging a job. Discarded entries consume slice visits and
the original deadline, but not executed-job allowance. Dead heads can be
removed at the cap; the next live job still fails. Outcomes report executed
and discarded counts separately. Obsolete rejection/module reporting does not
surface in a live sibling checkpoint.

Patch 0020 gives candidate work a stable transaction group independent of its
execution owner. A second intrusive linkage selects candidate jobs in constant
time without scanning or reordering sibling jobs. Setup borrows the enclosing
budget/outcome, executes private bootstrap and its tagged descendants, and
requires explicit activation afterward. Ordinary entry/providers remain closed
while Installing. Local native refusal, an ordinary exception, or an unhandled
private rejection retires the candidate and discards its tagged work, including
neutral jobs. Engine resource failure remains sticky runtime-wide. Sibling
FinalizationRegistry cleanup triggered by allocation-time GC is not attached
to the candidate transaction. Native partial-state rollback is F.3b work.

## Runtime and allocation evidence

All host runs retain ASan/UBSan and LeakSanitizer. The sandbox denied LSan thread
attachment with EPERM and `TracerPid: 0`; leak-enabled runs used normal
execution outside that sandbox. Logs are bounded summaries outside tracked
source. The target engine uses its shipping allocator; the fully instrumented
engine bypasses small-object arenas, exposing additional allocation cutoffs.

- Full runtime: **12,550 target-engine checks** and **57,870 instrumented-engine
  checks**, zero failures and zero live fixture allocations. Compiler failure
  sweeps and fatal/invariant paths also pass.
- Final focused realm matrix: **2,921 target-engine checks**, **32,604 fully
  instrumented checks**, zero failures. These include the additional staged
  allocation/deadline/running-parent-job and retired-slot cases added after the full runtime
  suite compiled. The instrumented focused run reuses that suite's unchanged
  engine objects with the final focused runtime/test objects.
- Staged installation sweeps: **17 shipping allocation cutoffs** and **186
  instrumented cutoffs**, each under FATAL and RECLAIM. Every refused engine
  allocation retires the candidate, preserves shared failure, and releases
  native/engine holds at root teardown. Local refusal/ordinary exception cases
  separately prove healthy parent and sibling continuation.
- Eight retirement and five staging mutants compile and are caught against
  passing focused baselines. They break owner/activity selection, eligibility
  timing, discard accounting, callback entry, budget preservation, candidate
  tags, sibling drain/abort, activation ordering and retired-slot reuse. The earlier four admission
  mutants cover headroom, wrapper accounting, recheck and pending failure.
- Contract/header syntax, patch preparation tests and the allocation/global/
  visibility teardown audit pass. Target library, CLI and guest consumer link.

The shared host/guest cases cover exact surviving A/B/C job order, nested job
creation, retained ordinary functions, both Promise directions, neutral custom
capabilities, bound/proxy/revoked callbacks, async/await and generators, pending
modules, cross-owned finalizers, reporting hooks, both drain paths and limits.
Candidate cases cover standalone/nested success and abort, explicit activation,
private rejections, neutral descendants, sibling queue preservation, allocation
cutoffs, inherited deadlines and already-consumed parent job allowance.

Evidence: `staging-runtime.log`, `final-realms.log`,
`final-realms-instrumented.log`, `staging-mutants.log`,
`retirement-final-mutants.log`, `slot-mutant.log`, `admission-mutants.log`,
`staging-prepare-tests.log`, `staging-teardown-audit.log`,
`staging-headers.log` and `final-target-link.log`.

## DOM and guest consumers

The complete shipping DOM suite passes **44,928 checks**, including **3,968
binding allocation sweep cases**. The complete instrumented DOM suite passes
**309,810 checks**, including **36,642 binding allocation sweep cases**. Both
use the transaction-enabled engine; see `staging-dom.log`.
No DOM departure or binding rollback implementation is claimed by these runs.

QEMU uses q35, two CPUs, 2 GiB RAM, BIOS boot, no network, an existing kernel,
and a disposable FAT/ext2 disk. A frozen five-file cohort contains the runtime
consumer, parent runner and matching libjs/libmath/libos64, with SHA-256 hashes.
The cohort manifest SHA-256 is
`784fa801d1ea04bca57ea20ac48d8fa2f666676ca603a2bd13701bf726cbe80d`.
The parent captures bounded consumer output and observes its actual
`os64_wait` result: **1,879 checks, zero failures, exit `0x4a535254` (JSRT)**.
The common A/B/C retirement and staged/refused-candidate cases run under both
teardown profiles, alongside the existing guest heap checks.

Two disposable-image setup errors were corrected without changing realm code:
the libraries initially went to `/bin` instead of the loader's `/lib`, and the
reused image lacked `/home`, required by the existing file-helper tests. The
passing run includes those tests. Its unrelated network-disabled kernel test
`net_arp_pending_order` fails; this evidence claims the consumer, not a wholly
passing kernel suite. No P5 acceptance is needed for this engine foundation.

## Pinned WPT comparison

Both revisions use checkout `a699fb16767be39ba3e61a8e8b6bef60383f08ee` and unchanged
expectations. Compared with merged F.2b (`37ec284d`), every named verdict,
required/lost set, and summary matches, not just totals.

| Family | Pass | Fail | Timeout | Not run | Total | Pages |
|---|---:|---:|---:|---:|---:|---:|
| `html/webappapis/scripting` | 535 | 1,062 | 35 | 9 | 1,641 | 112 |
| `html/semantics/scripting-1/the-script-element/module` | 119 | 118 | 6 | 26 | 269 | 114 |

Scripting exits 1 on both revisions for the existing required/pass loss:
`events/event-handler-handleEvent-ignored.html` → `plain object "mouseenter"
handler`. Modules exit 0 on both. Exclusions remain 9/25 worker or shadow-realm
variants and one XML document per family. This comparison does not claim
unmodified iframe WPT passes. See `wpt-comparison.txt` and the four
`{baseline,staging}-wpt-{scripting,module}.log` evidence files.

## Interrupt source audit and counterexamples

`JS_ThrowInterrupted` creates an internal error and marks it uncatchable. The
interpreter exception path skips catch-offset handling for that marker.
Ordinary unwind closes variable references, releases local/stack values and
restores `current_stack_frame`. Async/generator frames have separately owned
state and release paths. These reference-lifetime mechanisms do not roll back
partially executed JavaScript or native operations.

The interrupt hook receives the runtime and opaque wrapper state. Neither
records the set of realms traversed by raw engine calls; `turn_owner` names the
outer turn, not the full interrupted stack. Pre-execution job ownership now
supports eligibility, but supplies no interrupted-stack attribution or proof
of per-realm application/native consistency after an uncatchable exception.

Host cases under both teardown profiles interrupt allocation-heavy child
loops, try/catch/finally, a child Promise job, native coercion, and nested
A → B → C calls. Each returns LIMIT/EXECUTION, executes neither catch nor
finally author hooks, blocks later sibling entry, and releases the heap/native
holds on root teardown. Retirement after interruption cannot clear the latch.
Job-cap cases likewise preserve the enclosing count through native setup,
retirement and candidate drains.

Verdict: no generic unwind corruption was observed, but safe child-local
recovery and affected-realm attribution remain unproved. Keep the whole-runtime
fallback. F.16 carries the missing recovery proof; clearing a latch is not one.

## Source qualification and next slice

Full runtime/DOM, WPT and initial guest evidence use prepared engine identity
`24e976fe1cae9be2b338c9ab4bbb5fe7bc27f9cf719111922660e9345ebeaec5`.
A final header-comment clarification distinguishes allocation-free inspection
from release/native-finalizer effects; it changes the manifest-derived identity
to `c1b9fceda7673420cae54717315a442f7b236a2c4297f9a92d30c42cb2815e77`
without changing engine behavior. Runtime prose likewise names all three drain
paths. Final target links and focused/guest confirmation use that identity, including
the added retired-slot regression. Exact patch whitespace is preserved through
`.gitattributes`; it is required context, not trailing source whitespace.

F.3b must integrate DOM departure and shared delivery eligibility, transactional
native installation rollback, provider lifetimes and measured admission bounds.
F.15 reclamation and F.16 recovery remain explicit F.11 gates. F.3a does not
free departed realms, enable frames, clear sticky overruns or mark F.3 done.

## F.3b addendum: engine quiescence

ABI 5 adds `os64_js_quiescent`, an allocation-free owner-thread snapshot for
native cleanup. It distinguishes unfinished engine work from a healthy idle
runtime and from failures requiring root teardown. It does not execute jobs,
change budgets, sample time, clear failures or detach providers. Native DOM
borrow and dispatch/reaction checks remain separate obligations.

The focused fixture passes 206 checks on both engine profiles. Existing realm
and lifecycle coverage passes 3,343 shipping and 33,026 instrumented checks,
with ASan/UBSan/LSan and allocation sweeps. Nine isolated query mutants are
caught. Fresh ABI 5 DOM validation and the malformed-queue fixture qualification
are recorded in [the F.3b evidence](IFRAME_F3_DOM_VALIDATION.md#engine-quiescence-boundary-checkpoint).
This wrapper API addition changes the binding ABI, not bytecode engine identity.
