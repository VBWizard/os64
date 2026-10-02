# JavaScript library and runner work plan

Status: running work checklist, updated 2026-10-02. The product decisions are in
[JAVASCRIPT.md](JAVASCRIPT.md). This plan separates work so Chris can assign
packets to Opus and Fable without overlapping ownership. The status table records
completed work, review dependencies, and remaining acceptance gates. It is updated
at implementation, review, merge, and validation handoffs; statuses are a dated
snapshot rather than a live GitHub feed. `libjs`, `js`, and `libmath` are working
names.

The standalone milestone has no dependency on a browser DOM implementation.
Keep Yonder integration in its later campaign. A proven need for a kernel
change is discussed separately rather than being absorbed into these tasks.

## Progress checklist

"Merged" records an accepted deliverable on `userland`; it does not imply that
later consumers or guest acceptance gates have passed. "In review" records an
implemented slice in an open PR. Pending rows record work still needed by this
plan, without assigning a new contributor or claiming work has started.

| Work | Status on 2026-10-02 | Evidence and remaining work |
| --- | --- | --- |
| libos64 prerequisites | **Merged** | [PR #188](https://github.com/VBWizard/os64/pull/188), merge `6a6c08c1`: allocation-size query and four string/memory verbs, host coverage and guest test registration. |
| R0 runtime contract | **Merged** | [PR #189](https://github.com/VBWizard/os64/pull/189), merge `a426386d`: reviewed public header, lifecycle/capability contract, examples, pinned QuickJS foundation. R0 delivered the interface; R2 records implementation. |
| M1 maths library | **Merged** | [PR #191](https://github.com/VBWizard/os64/pull/191), merge `3b509e8b`. R1 now links against its real shared library. Numerical acceptance belongs to M1; the R2 core fixture supplies combined guest engine evidence. |
| R1 target adapter/core | **Merged** | [PR #192](https://github.com/VBWizard/os64/pull/192), merge `ef43d227`: strict core/userland builds, real libmath/libos64 linkage, target symbol/header/ELF/relink audits, 675 sanitized host checks and 437 cross-core host checks pass. R1 itself did not run a guest; the R2 core fixture now exercises the combined library. |
| R2 runtime implementation | **In review** | Two slices: core [PR #194](https://github.com/VBWizard/os64/pull/194), `codex/js-runtime-core`; file/output/argument helpers [PR #195](https://github.com/VBWizard/os64/pull/195), stacked `codex/js-runtime-helpers`. The eleven-operation embedding interface is implemented; validation is recorded in libjs/VALIDATION.md. Both slices need review and merge. |
| C1 runner | **In progress** | Opus owns the runner and split its work in two. Chris reports the groundwork is complete; final integration is waiting for R2. Command grammar/usage, diagnostics and exit mapping belong to C1; end-to-end acceptance still needs R2 and M1. |
| V1 independent validation | **Pending** | Consumer capability/failure/ownership tests and selected upstream cases; execution needs M1/R2/C1. |
| I1 shared integration | **Pending** | Real shared-library dependency/link audits, build/image and licence installation, combined strict build and QEMU acceptance. Depends on M1/R2/C1 and V1 evidence. |
| D0 DOM design | **Merged** | Reviewed [DOM.md](DOM.md), delivered with [PR #190](https://github.com/VBWizard/os64/pull/190). Design completion is separate from browser scripting implementation. |
| D1 libhtml mutation core | **Merged** | [PR #190](https://github.com/VBWizard/os64/pull/190), merge `5c7d62ce`. Mutation/lifetime verbs and maintained tests; `parse_fragment` belongs to D2. |
| D2 scripting-enabled parsing | **Pending** | Stop/resume/abandon, fragment parsing and serialization; acceptance in DOM.md. |
| D3 page rebuild/control state | **Pending** | Stale-model gate, durable form edits, script setters and failure-safe rebuilds. |
| D4 parser stream handoff | **Pending** | Yonder parser-thread handoff and responsive streaming; no script execution in this slice. |
| D5 DOM binding/first page fixture | **Pending** | Bindings and J3: visible text change, stable references/form edits and safe navigation teardown. Needs the runtime and earlier DOM slices. |
| D6 detached-subtree reclamation | **Pending** | Reclaim unheld detached trees and prove bounded long-running churn. |
| D7 browser event loop | **Pending** | Tasks/checkpoints, timers/events, script order and J4 evidence. |

The DOM slice definitions and detailed acceptance cases belong to
[DOM.md](DOM.md); this table tracks their place in the overall campaign.
The implementation and validation evidence for libjs belongs to
[VALIDATION.md](../../../userland/libjs/VALIDATION.md). The library fixtures
do not complete J1/J2: those gates also require accepted R2 slices, the runner,
independent validation and shared integration.

Next steps:

1. Accept R2 against the reviewed contract; R0, R1 and M1 are merged.
   Review and merge the core and stacked bounded-file/output/argument helpers
   in that order. Promise processing and cancellation belong to the core because
   queued jobs retain the original evaluation budget. Both slices use the real
   maths library for target/guest evidence.
2. Hand the complete R2 interface to Opus for C1 integration, then deliver
   independent V1 validation and I1 integration to establish J1/J2.
3. Track Fable's D2 onward alongside the standalone work; J3/J4 require their
   own browser acceptance evidence.

## Agreed standalone scope

- Library-owned runtime behaviour, with a thin runner executable.
- Files and expressions as inputs, script arguments, output, exception
  diagnostics, and Promise processing.
- Explicit registration of host capabilities. The initial script capabilities
  are output and arguments, without filesystem, networking, or process APIs.
- A reusable maths library built from established numerical code.
- Finite memory and execution defaults, configurable through the library and
  runner, with distinct exception, limit, cancellation, and host-failure results.
- Interactive input, timers, and file-based module loading are deferred.
- Six public operations: create, register capabilities, execute source, process
  jobs, inspect outcomes, and destroy. The runner uses `os64/js.h` and library
  installers; native bindings use the published, pinned `quickjs.h`.
- One execution owner per runtime; callbacks cannot start evaluation or drain
  jobs recursively in that runtime. Recoverable failures return to the host;
  engine invariant failures use the defined fatal path.
- Library-owned execute-and-drain convenience for the runner, with controlled
  job processing available to other embedding applications.

## Packet ownership and dependencies

| Packet | Proposed owner | Owned deliverable | Depends on |
| --- | --- | --- | --- |
| R0 | Runtime owner, reviewed by Fable | Public runtime and capability contract, working header, diagnostic/status contract, initial profile specification. | This design. |
| M1 | Opus | Maths source import, private compatibility layer, shared library, maths tests, and port documentation. | MATH.md interface confirmed with runtime owner. Can run alongside R0 and R1. |
| R1 | Runtime owner | Pinned QuickJS import and private C adaptation for allocation, strings, dates, formatting, compiler support, and selected features. | R0 for public integration. Compile work can proceed before M1; production guest link needs M1. |
| R2 | Runtime owner | Runtime lifecycle, limits, job processing, library-owned input/output helpers, and capability registration implementation. | R0 and R1; M1 for guest validation. |
| C1 | Opus | Thin runner executable, usage documentation, and command-line integration cases. Groundwork is complete per Chris; final integration waits for R2. | R0 contract frozen. End-to-end completion needs R2 and M1. |
| V1 | Fable or another independent reviewer | Consumer-level validation of capability boundaries, failure behaviour, and fixture coverage. | R0 for test design; M1/R2/C1 for execution. Coordinate test-file ownership before writing. |
| I1 | Runtime owner as integration coordinator | Shared build/image registration, final dependency checks, combined strict build and QEMU evidence. | M1, R2, C1, and V1 evidence. |
| D0 | Fable | DOM.md covering mutable libhtml, document/wrapper lifetime, retired storage, parser handoff, and browser event scheduling. | This design and the existing browser libraries. Design runs alongside M1/R1; reviewed completion gates J3, not the runner. |
| D1–D7 | Browser owner; coordinate with Fable | Mutable-document, parser, presentation, bindings and event-loop slices defined in DOM.md. | D0; detailed dependencies and acceptance belong to DOM.md. D1 is merged; later slices remain separate from the standalone milestone. |

The runtime owner is the coordinating implementer working with Chris in this
thread. Review of a packet is separate from ownership of its implementation.
Assignments are suggested so Chris can dispatch them; they are not an implicit
request to launch agents or send messages.

## R0 runtime contract

The conceptual contract is agreed in JAVASCRIPT.md under Runtime contracts.
R0's interface was reviewed by Fable and merged in PR #189; runtime
implementation and behavioral evidence remain R2 work. Declarations, ownership rules, and
failure semantics are recorded in [the runtime contract](../../../userland/libjs/CONTRACT.md)
and [public header](../../../userland/libjs/include/os64/js.h). Fable's
follow-up review accepted the revised contract, including reusable script failures and continuous turn budgets. The runner requires
no QuickJS types; binding examples use the pinned engine API. Include:

- Creation/destruction, caller-supplied limits, evaluation of a bounded source
  buffer with a source name, and library helpers for bounded file loading.
- Evaluation outcomes and explicit ownership of diagnostics and result data,
  including reporting when the engine cannot allocate an exception string.
  Separate status from diagnostic text and specify runtime reuse after each
  recoverable failure class. Define the fatal diagnostic/status for engine
  assertions and aborts separately; recovery from teardown leaks is not a
  standalone guarantee.
- Controlled pending-job processing and an execute-and-drain convenience
  operation, unhandled-rejection reporting, and termination behaviour when a
  Promise remains unresolved but no work is runnable. Define queued-job
  disposition and budget accounting across evaluation and draining.
- Controlled access to the engine context, plus examples using QuickJS's
  argument/result/exception and ownership rules. Document the accessor's
  lifetime, registration failure cleanup, and native-state teardown; do not
  duplicate the engine's value or class API.
- Caller-header compatibility checking before exposing an engine context,
  covering the upstream pin, port ABI, and relevant configuration. Creation
  checks the host; the context accessor separately checks each binding unit
  with a caller-owned outcome. Test both mismatches and useful diagnostics.
- Output and script-argument bindings implemented through library machinery;
  callers choose output destinations and `print`/`console.log` names rather
  than duplicating those bindings. Test console-only installation, invalid
  flag masks, and preservation of unselected globals.
- One owning thread per runtime, permitted cross-thread cancellation signalling,
  serialized check-and-allocation of process-lifetime class-ID slots, and
  rejection of evaluation or job-drain re-entry from callbacks into the same runtime through the os64 wrappers.
  Native bindings must not bypass these restrictions or replace library-owned
  engine configuration. Document conversions/property access that can invoke
  script, and preservation of active budgets. Destruction occurs outside
  active evaluations and callbacks.
- Source-size, engine-memory, stack, execution, and job-chain budgets, including
  which phase a timeout covers. Default numbers remain measured engineering
  choices, not numbers inferred from the research probe.

Acceptance: a reviewable header and contract with example call sequences for
file execution, expression execution, controlled job processing, custom
capability registration, errors, and teardown. Include ownership of arguments,
returned values by reference to QuickJS, and explicit rules for diagnostics and
application context, plus registration failure cleanup. An interface review
checks that the runner needs no engine code and no second event/job
implementation. Browser-specific objects are
excluded. Agreement on these concepts alone does not mark R0 complete.

## M1 maths handoff

Give the proposed port owner [MATH.md](MATH.md). It contains the source archive
and digest, the 32 public functions, the revised source inventory with SSE2
`sqrt`, the standard-name ABI, actual cross-compilation evidence, scoped
upstream warning exceptions, and test requirements. The port is independently usable and does not wait for QuickJS.

Owned paths: `userland/libmath/`, `userland/tests/mathtest/`, dedicated
`tools/test_math_*` files, a maths licence notice, and updates to MATH.md.
Private source adaptations stay inside that port. Leave the existing libpage
numeric implementation and its fixtures unchanged.

## R1 and R2 runtime implementation

Owned paths: `userland/libjs/`, runtime-focused guest tests, dedicated
`tools/test_js_runtime_*` files, and the QuickJS licence notice. Exact fixture
paths are settled at R0 so C1 and V1 do not edit the same files.

Follow FreeType's `upstream/`, `patches/`, `manifest.json`, `UPSTREAM_REVIEW.md`,
`exports.map`, and `port/` layout. Preserve unmodified originals and explicit
patches. The private compatibility layer may translate conventional C names to os64 facilities,
but must match behaviour as well as spelling. Audit the final target symbols
and retained code; source-level callback configuration alone does not prove
unneeded dependencies were discarded.

Add the four missing string/memory operations to libos64 as `os64_` verbs in
a separately reviewed prerequisite change. QuickJS maps the C names through
a rename header; it does not add duplicate bodies. Chris chose the public
libos64 allocation-size query on 2026-10-01. Include that query as a separately
reviewed R1 prerequisite, settle its payload-size and valid-pointer contract,
and use it in the adapter's limit accounting instead of a private size header.
FreeType/libtls shim consolidation is tracked separately in DEBTS.md.

Acceptance: direct library tests cover repeated lifecycle, exceptions,
allocation failure, memory limits, deep recursion, cancellation, Promise work,
unhandled rejection, callback failure, and independent runtimes. Include reuse
after an exception/rejection, jobs retained after a thrown script/job, original
budgets preserved while draining after failure, and class-ID reuse across
repeated runtime creation and simultaneous slot requests. Confirm that
unregistered capabilities are absent and prohibited callback re-entry returns
the documented error. The first capability demonstration is a small test
executable that registers a native function and checks its successful result
and error conversion using the os64 runtime API and published `quickjs.h`.
Provide representative guest evidence before considering the CLI a validation of the
engine.

Measure stack use and supported recursion for representative scripts and native
callbacks in J2; reserve browser layout-call measurements for integration.
Run a deliberately leaked-value fixture as a separate process and check its
fatal diagnostic/status. Do not convert an assertion into a success return.

The core owner defines library file/output helpers and the status contract.
The runner owner must report a missing interface instead of implementing a
private substitute. Changes to shared libos64 functions are coordinated and
reviewed as separate changes rather than made concurrently by both ports.

## C1 runner handoff

Owned paths: `userland/apps/js/`, a runner usage guide, and dedicated
`tools/test_js_cli_*` cases. Implement argument parsing, library calls, output
destination setup, and mapping of library outcomes to exit status.

Proposed command forms are `js file.js [arguments...]` and `js -e expression`,
with explicit limit options and a `--` delimiter. R0 and the runner owner settle
the precise grammar, argument binding, output conventions, and status numbers
before implementation. The library supplies the reusable loading, evaluation,
output binding, diagnostics, and pending-job machinery. The runner uses the
library's execute-and-drain convenience operation instead of carrying its own
loop around QuickJS's pending-job API.

Acceptance: execute a file and expression on os64; pass arguments; report a
missing file, malformed script, thrown exception, and exceeded limit clearly;
preserve ordinary shell redirection; return the documented status. An imported
module or other deferred feature must not be advertised as supported.

## V1 independent checks

The validation owner checks observable contracts without rewriting engine
internals. Include cases for a registered capability and an absent capability,
a throwing callback, prohibited evaluation and job-drain re-entry, output
failure, a runaway Promise chain, an unhandled rejection, exhaustion during
error reporting, and teardown after cancellation. Check that a recoverable
failure returns control to the host and that reuse follows the stated contract.
Test the fatal invariant path in a separate process and verify header/library
compatibility rejection. A passing recoverable-error fixture does not establish
that an engine invariant failure can be contained within the host process.

Use selected upstream language tests with explicit pass/fail/skip reporting.
Identify unsupported host features separately from failed supported features.
Maths correctness is M1's responsibility; the runtime suite checks JavaScript's
own numerical semantics on top of that library. Do not replace independent
expected results with values computed by the same implementation under test.

## D0 browser design handoff

Fable's owned deliverable is `docs/design/pending/DOM.md`. Define the libhtml
mutation verbs and invalidation contract, stable node identity, document and
wrapper lifetime, retirement of borrowed text/attributes, and budgeted failure
behaviour. Specify how `os64_page_rebuild` preserves surviving form edits and
how failed model/layout replacement affects presentation and interaction.

Evaluate UI-thread script execution and a cancellable parser-worker mailbox
handoff against libway and Yonder. Settle exclusive access to partially parsed
documents, navigation generations, synchronous geometry/layout, event tasks,
and Promise checkpoints. This packet is a design deliverable; implementing
browser APIs remains J3/J4 work. The standalone contract reserves budgeted
function-call turns and checkpoints inside a host task for this browser design. Known-teardown-leak reporting and
reclamation, selected at creation, needs its separate audited contract and
repeated-leak evidence before replacing the fatal policy.

## Shared file and integration rules

Use isolated worktrees for implementation packets. The integration coordinator
owns changes to shared files such as root and userland `GNUmakefile`, shared
test dispatchers, and image/library inventories. Each port provides its local
`shared.mk` and the exact common-file changes it needs. Coordinate a small
registration commit or prerequisite branch before guest validation; a task is
not complete merely because it cannot run until someone registers its library.

The coordinator also owns the main JavaScript design and task document so
parallel contributors do not rewrite the same plan. Contributors update their
component documents and report any proposed contract change before consumers
depend on it. Use explicit branch dependencies when testing work before merge.

After each handoff, record the commit, changed paths, dependencies, validation
commands and results, skipped checks, and remaining limitations. Shared
integration verifies symbol visibility, the shipped library identity, licence
installation, the strict build, and guest execution. Pushes and merges follow
Chris's explicit authorization; this plan does not authorize either.

## Foundation slice 2026-10-01

The runtime owner began in `.worktrees/js-runtime-foundation` on branch
`codex/js-runtime-foundation`, based on `b55c3770`. The agreed design/review
documents were copied into that checkout without changing the main checkout.

- R0 has concrete proposed declarations, a lifecycle/job/error contract, and
  syntax-checked runner, expression/job, and custom-binding examples. The
  round-one revisions keep ordinary exceptions/rejections reusable, make
  class-ID slots and binding ABI checks explicit, and select output names.
  Fable accepted the revised interface and it was merged in PR #189; C1
  can depend on the reviewed contract, with execution waiting on R2. No runtime
  symbol is implemented by these headers.
- The separate libos64 prerequisite change supplies `os64_malloc_size`,
  `os64_memchr`, `os64_strchr`, `os64_strrchr`, and `os64_strcmp`, with host
  regression coverage and `/tests/jssupporttest`, registered in `testrun` with
  its JSUP pass badge.
- R1 source preparation retains the pinned original core and hashes, notices,
  a scoped Atomics patch, and a repeatable engine-only host probe. The profile
  preserves stack checks. The target-adapter slice below follows this foundation;
  default image registration remains open. This baseline probe uses host
  libc/libm and does not establish os64 execution.

Validation and remaining work are recorded in `userland/libjs/VALIDATION.md`.
M1, R2, C1, V1, and the DOM slices are separate work packets. Current statuses
are recorded in the progress checklist above. The library prerequisite
commit is on `codex/js-support`; the interface and source preparation are stacked
above it on `codex/js-runtime-foundation`. PRs #188 and #189 merged these
foundation branches; they are retained for reference.

## R1 target adaptation slice 2026-10-01

Worktree `.worktrees/js-target-port`, branch `codex/js-target-port`, starts at
merged foundation `a426386d`; its review follow-up merges `userland` at
`d5eecf1b`, including M1. The runtime owner supplies the private C headers,
allocation callbacks using `os64_malloc_size`, wall-clock/timezone adaptation,
diagnostic formatting, compiler memory veneers, generated patch preparation,
core build and shared-link recipe. Upstream originals remain unchanged.

`make -C userland js-core` cross-compiles and partially links the retained core
and required libgcc helpers. `js-library` is an explicit shared target requiring
the real `libmath.so`; it is excluded from the default image. Normal installation
is I1 work. The shared placement population reserves
both library names; image registration remains I1 work.

The maintained target/host fixtures are `tools/test_js_port_target.*`,
`tools/test_js_port_host.*`, and `tools/test_js_port_calendar.c`. They belong to
R1, alongside its own component docs. Exact evidence and limitations live in
`userland/libjs/VALIDATION.md`. The R1 deliverable does not implement R2, C1, or
D1 and its original validation
does not establish guest JavaScript or numerical conformance.

[PR #192](https://github.com/VBWizard/os64/pull/192) merged as `ef43d227`,
including implementation `e53bde19` and its review corrections through `b9e6122b`.

The 2026-10-02 review follow-up removes the duplicate maths header, restores
fatal handling of zero-byte allocations while preserving NULL/zero resize,
tracks shared-recipe and placement-assigner dependencies, and clarifies the
ptrace requirement for normal-exit leak checks. Target evidence now includes
the production shared link against the real merged M1 library.

## R2 implementation slices

The core slice is isolated in `.worktrees/js-runtime-core`, branch
`codex/js-runtime-core`, based on merged R1 `ef43d227`. It exports create,
context access, class-ID allocation, eval, run, drain_jobs, cancel and destroy.
The guest fixture is an explicit build target and is installed only into a
validation image; libjs remains outside the normal image population. Its
implementation also includes the tracked QuickJS allocation-failure cleanup guards.
Review and merge are separate from implementation/validation completion.
The core slice passes 475 target-core host checks, 2,539 sanitized-engine host
checks and 280 checks in os64/QEMU; details and library hashes are in VALIDATION.md.

There are two R2 implementation slices. The core is published in
[PR #194](https://github.com/VBWizard/os64/pull/194) at `45cae4fc`. The helpers
are isolated in `.worktrees/js-runtime-helpers`, branch
`codex/js-runtime-helpers`, based directly on that core commit. Their PR base
is the core branch so its review diff contains the helper changes.
The helper slice is published in [PR #195](https://github.com/VBWizard/os64/pull/195)
at `6344e1d7`. Merge the core first, then retarget the helper PR to `userland`.

The second slice implements `run_file`, `install_output` and `install_args`,
including bounded input/close failure, output conversion/write failure, copied
arguments, installer flags and setup-state checks. The shared library exports
all eleven declared embedding operations. Opus can integrate C1 against this
stack; both R2 slices require review and merge before acceptance. C1 is not
implemented or modified by the runtime slice.
The combined core/helper fixtures pass 698 target-engine host checks, 2,890
sanitized-engine host checks and 365 os64/QEMU checks, with zero failures.
The first helper review's combined-output transaction finding is corrected:
selected properties are staged/restored and the borrowed handle commits after
installation succeeds. The updated PR requires re-review; core/helper review
acceptance remains separate from the recorded implementation evidence.

Production limit defaults remain J2 work, rather than adopting the fixtures'
development budgets without measurements. D2-D4 can proceed independently; the D5 binding
join and later browser turn/reclamation APIs remain separate acceptance gates.
