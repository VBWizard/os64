# F.15 B1 — scalar retention diagnostics

Implementation PR: **#610**, source `8d9c67ef`.

Implementation baseline: merged `userland` `8aee6e2c`, including the approved
[F15 design #600](IFRAME_F15_RECLAMATION.md), observer settings #599,
custom-element settings #601 and the accepted media changes. Engine identity
remains `39063a8f7f5747a9737b4f15b67dddb87f6344021bfba9fe3ae79a12ef020fbf`;
embedding ABI 5 and existing runtime/DOM/callback record layouts are unchanged.

This is the first bounded diagnostic cut. It adds no roots, pins, collection,
slot reuse, admission decision or browser integration. Deep incoming-edge
counts, HTML/page borrower instrumentation and release-stage injection points
remain separate cuts. F15's identity/export contract and reclamation behavior
still depend on the reviewed F4c/d boundary.

## Public queries

`os64_js_retention_snapshot(runtime, caller_abi)` in `os64/js_retention.h`
returns a scalar `os64_js_retention_t` by value. It reads at most the occupied
65-slot wrapper table and existing scalar counters. Context-root counts are
the wrapper's own context references; `promise_then_roots` counts its cached
Promise intrinsic references. Neither is a complete engine reference count.
Retired/failed/staged counts overlap: an F16-failed slot is also retired.
`module_owner_slots` counts nonempty wrapper module lists, not module handles.
`rejection_records` is the wrapper's pending reporting ledger. `jobs_pending`
is queue presence, not a count or attribution of jobs or suspended work.

Heap bytes/peak are existing runtime-wide allocator charges, not per-realm
ownership. Active/task/turn/setup/settings flags report their wrapper state.
The wrapper's internal completed-checkpoint marker is not exported as an
active-drain flag. Tests inspect actual draining jobs through active/task/turn
instead of changing that existing marker's meaning.

Status OK means the observation succeeded. It does not mean the runtime is
healthy, quiescent or reclaimable. Latched status/limit are copied without
`observe`: pending cancellation and opaque nonzero allocator-failure flags
can be newer than the latch. Querying does not advance or clear it. Native
callback reads are allowed; destruction returns FAILED_RUNTIME with the
destroying flag before any engine-state access. A freed runtime is not a
valid argument. Bad arguments and a mismatched caller ABI refuse data.

`os64_dom_retention_snapshot(cluster, caller_abi)` in `dom/retention.h`
returns `os64_dom_retention_t` by value. Its member walk is bounded by the
cluster's 65-member admission limit. It reports occupied/closed/installing/
finished-departure members, retained-value records, the existing registry
census, HTML/page/DOM/kept/Canvas charges, provider/dispatch/departure-busy state,
borrowed layout generations and existing F16/Streams state. Registry census
totals are holder counters: aliases may count in several holders, and they
do not enumerate the graph. Departure-busy state is the existing guard, not a
complete native borrower census. Without a borrowed generation,
`oldest_layout_generation` is `UINT64_MAX`.

The native query reads no runtime or realm handle. It remains usable after
cluster drain and subsequent engine destruction, before cluster free. Zero
drained registry counters coexist with occupied native members and backing
charges. Neither drain nor F16's matching completed generation becomes a
slot-reuse certificate through this query.

Both entries run on the owner thread. They allocate no memory, invoke no
author/host callback, collect nothing, poll no clock and acknowledge no loan.
By-value results avoid writing into an outcome borrowed by an enclosing call.
The observations acquire no lifetime pins and are not a transactional
cross-layer snapshot or a token permitting later mutation.

## Explicitly unknown coverage

Both results retain their documented `UNKNOWN_ALL` mask even when visible
counters are zero. The masks identify unenumerated families, not failure of
the scalar measurements:

| Wrapper gaps | Native gaps |
|---|---|
| Incoming engine graph, job-role holds, callback context holds, trusted raw exports, native owners and exposure | Incoming engine graph, callback context holds, trusted raw exports, HTML/page borrowers, exposure and native incoming edges |

Pending Promise-reaction incumbents, callable/thenable continuations, future
F4 target/history state and suspended functions cannot be inferred absent
from these counters. A captured callback value and a stable borrowed realm
identity do not independently certify incumbent/entry context ownership.
Future coverage changes require their own reviewed semantics and proof.

## Implementation and invariants

Separate `retention.inc` units sit at EOF of runtime.c/core.c, using their
private types without altering existing storage or conversion paths. Runtime
inspection uses bounded slot reads, the rejection ledger count, allocator
scalars, an atomic cancellation read and `JS_IsJobPending`. It does not walk
unbounded module/rejection lists or call failure observation. Native inspection
uses the existing scalar census/ledger getters and departure-busy predicate;
it never follows a realm pointer after engine destruction.

The new headers compile independently for native and target callers. The
target export audit includes the added runtime query; the native query has
explicit public visibility. Existing symbols, constructors, callback settings,
F16 containment and cumulative admission limits keep their behavior. The CLI
consumer is built and tested alongside the public header/link checks.

## Proof cases and reproduction

The runtime `--retention` selector tests both root teardown policies:

- Real creation, staging and retirement retain their context/cache roots;
  two module handles in one realm report one owner, then clear on release.
- Setup, checked settings, a draining job and native callback reentry remain
  usable; inspection preserves an active outcome and an existing exception.
- A retained rejection is not consumed. A pending engine job remains pending
  and keeps its existing BUSY admission precedence over cancellation. The
  fixture explicitly discards that job before asking normal admission to
  observe cancellation; inspection itself never drains or renews its budget.
- Cancellation and an actual raw allocator failure are visible before their
  latch advances. Finalizer reentry during destruction refuses partial
  engine state. Both root policies finish without leaked allocations.

The DOM `--retention` selector adds real provider/layout loans, allocation
refusal seams, a stepping clock, physical ledger comparisons and ordinary
departed aliases that remain callable. The shared F16 host/guest consumer
also reads both queries during native provider reentry, after timeout, after
successful containment, after root drain and after engine destruction. It
checks unknown masks despite empty/drained counters, preserves original task
diagnostics and confirms occupied identities remain retained.

```sh
JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--retention ASAN_OPTIONS=detect_leaks=1 bash tools/test_js_runtime_host.sh
DOM_HOST_KEEP=1 ASAN_OPTIONS=detect_leaks=1 bash tools/test_dom_host.sh --retention
make -C userland -j8 js-runner dom-recovery-test
ASAN_OPTIONS=detect_leaks=1 bash tools/test_js_cli_host.sh
python3 tools/test_dom_recovery_guest.py --base-image BASE.img --boot-iso BOOT.iso --output NEW-DIRECTORY
bash tools/test_js_contract_headers.sh
python3 tools/test_js_port_target.py
```

The guest driver records hashes for the diagnostic headers/implementations as
well as the consumer and recursively matching libraries. The guest checks the
live heap returns to baseline after each case, allocator accounting holds and
its parent receives exact exit `0x446f0200`. No new P5 delivery cohort or
visible-frame browsing is enabled.
The reused NONET validation kernel retains the documented unrelated ARP
preboot-test failure; this receipt covers the child/parent consumer, not a
passing kernel-suite claim.

Validation on the coherent baseline with LeakSanitizer enabled:

- Focused runtime: **104 checks per engine profile**; focused DOM: **834 per
  profile**, both root teardown policies, zero failures and balanced native/
  engine allocation accounting.
- Full runtime: **22,118 shipping / 69,508 instrumented checks**, zero failures
  and live allocations. Parser-refusal and the canonical fatal invariant paths
  pass with their complete target exit badge. Native/wrapper code uses
  ASan/UBSan; the instrumented engine uses ASan as in the canonical runner.
- Eight isolated defects are rejected by named assertions in each profile:
  clearing either unknown mask, observing runtime health, ignoring destruction,
  hiding a layout loan, erasing drained identities, ignoring native ABI and
  polling the native clock. Mutation failures/aborts are not positive leak
  evidence; the unmodified completed runs provide that evidence.
- The shared guest's six cases under both policies pass with heap-baseline
  restoration and exact parent exit. The frozen cohort contains **19 matching
  files** with SHA-256 manifests and hashes of the diagnostic source files.
- F4/F16 recovery, settings and affected ownership/retirement/animation
  selectors pass on both profiles. `js-runner` builds; CLI suites pass
  **199 / 40 checks**. Public headers, manifest/preparer, teardown/import/link
  audits, suite inventory, tracker and stale-reference checks pass.

No new broad DOM or named WPT campaign is claimed for these additive queries;
the affected lifecycle/F4/F16 selectors supply their composition checks.
