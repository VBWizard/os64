# F.3a engine validation — in progress

2026-11-12. Implements the first admission boundary from
[the reviewed F.3 brief](IFRAME_F3_RETIREMENT.md), based on design amendment
`3a8ceb1c` and the merged F.2b implementation at `37ec284d`.
This is an implementation checkpoint, not F.3a completion or review readiness.

## Implemented boundary

Embedding ABI 4 adds an allocation-free engine-headroom query, realm
preflight and creation with a caller-specified headroom requirement.
The calculation subtracts the runtime wrapper's reserved storage as well as
current engine charges. Slot refusal and headroom refusal are non-sticky;
creation rechecks a preflight snapshot before attempting allocation.
Pre-existing allocator failure, cancellation and runtime failure cannot be
reported as harmless admission refusal. The normal creation path still
observes the enclosing deadline and keeps the original sticky failure rules.

The host chooses a positive reserve using its measured peak installation
cost and sibling operating margin. Preflight does not reserve memory and
cannot guarantee arbitrary future allocations. The 256 KiB reserve in the
regressions is a fixture value for bare engine realms, not a DOM admission
constant or a measurement of installed/departed bindings. F.3b supplies that
measurement and production reserve for both teardown/allocator profiles.

## Evidence scope

The common host/guest realm matrix covers exact headroom and one-byte excess,
wrapper accounting, stale snapshots, ABI rejection, independent slot limits,
small-heap exhaustion, both teardown policies, parent/sibling survival and
state preservation. A parent queues Promise jobs, refuses a child, admits a
child, then verifies exact queue order without an early checkpoint. A second
case admits during a running job and verifies the enclosing executed-job cap.

Host-specific checks arm allocation refusal and verify preflight attempts no
allocation, preserve a raw allocator failure before wrapper observation, and
verify admitted creation cannot renew an expired parent deadline. The guest
consumer builds these common cases; execution on a guest is not yet claimed.

LeakSanitizer's sandbox diagnostic reported thread attachment `errno 1`
(EPERM), with `TracerPid: 0`. Leak-enabled tests run outside the sandbox;
ASan and UBSan remain enabled. Evidence logs are bounded test summaries,
retained outside the tracked source tree.

Results for this admission implementation:

- Full runtime suite: target engine **11,140 checks**, fully instrumented
  engine **56,453 checks**; zero failures and zero live allocations. Compiler
  allocation sweeps and fatal/invariant-path checks also pass. The two final
  extra admission budget cases were added after this suite compiled and are
  included in the focused results below.
- Final focused realm matrix: target engine **1,211 checks**, fully
  instrumented engine **28,866 checks**, zero failures, with leak detection.
  The instrumented run reuses the full suite's unchanged engine objects and
  the final focused run's runtime/test objects.
- Four admission mutants compile and are caught: ignored headroom,
  unaccounted wrapper storage, creation without the required recheck, and
  masking an unobserved allocator failure. The harness restores source bytes.
- Contract/source-manifest and header syntax checks pass. Target runtime,
  shared library and CLI link; no guest execution or P5 delivery is claimed.
- Tracker, diff whitespace and local documentation links pass. The stale-reference
  scan reports the existing retirement-brief references, verified as live links.
  F.3 remains in `code`; no implementation completion flip.

Evidence filenames: `admission-runtime.log`, `admission-realms-final.log`,
`admission-realms-instrumented.log`, `admission-mutants.log`,
`admission-target-link.log`, and `admission-lsan-diagnosis.log`.

## Initial interrupt source audit

Prepared engine identity:
`6f29c6b01f7a9a4150b608e1cc8054c6746ab116c0b102adec7d25bf9246731a`.
This checkpoint does not change the engine patch series.

- `JS_ThrowInterrupted` creates an internal error and marks the exception
  uncatchable. The interpreter exception path skips catch-offset handling
  for that marker. Ordinary function unwind closes variable references,
  releases locals/stack values and restores `current_stack_frame`; generator
  frames have a distinct saved-stack path requiring separate scrutiny.
- The interrupt callback receives `JSRuntime` and opaque runtime state. The
  wrapper's `interrupt` calls `observe`; neither records the set of realms
  entered through raw engine calls. `turn_owner` identifies the outer turn,
  not the realms affected by an interrupted cross-realm stack.
- Both manual drains and host checkpoints currently call
  `JS_ExecutePendingJob`, which reveals the context after execution. This
  does not supply pre-execution eligibility or interrupted-stack attribution.
- EXECUTION/JOBS/MEMORY limit reasons already distinguish causes. There is
  no demonstrated generic engine unwind corruption in this inspection, but
  ordinary reference cleanup does not prove script/native invariants suitable
  for continuing sibling execution after an uncatchable interrupt.

Initial verdict: interrupted-realm attribution is absent from the embedding
boundary; safe child-local recovery is unproved, rather than known impossible.
Keep the runtime-wide fallback. The F.3a completion audit must add the planned
allocation-heavy, cross-realm, catch/finally, Promise and native-coercion cases
and document their unwind/teardown observations. Clearing a latch is not proof.

## Remaining F.3a work

Realm lifecycle and entry refusal; staged setup with stable transaction tags;
pre-execution job ownership/filtering and candidate-only setup drains;
Promise/thenable/async/module ownership cases; discard accounting and queue
bounds; reporting/service eligibility; the complete interrupt audit and
mutant/guest evidence. F.3b then implements DOM departure and isolated failed
installation. Neither F.3a nor F.3 is marked done by this checkpoint.
