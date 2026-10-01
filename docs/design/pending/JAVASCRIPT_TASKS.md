# JavaScript library and runner work plan

Status: proposed work assignments and foundation progress, updated 2026-10-01. The product decisions are in
[JAVASCRIPT.md](JAVASCRIPT.md). This plan separates work so Chris can assign
packets to Opus and Fable without overlapping ownership. No collaborator has
been contacted or started by this plan. `libjs`, `js`, and `libmath` are working
names; paths below are proposed implementation destinations.

The standalone milestone has no dependency on a browser DOM implementation.
Keep Yonder integration in its later campaign. A proven need for a kernel
change is discussed separately rather than being absorbed into these tasks.

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
| C1 | Chris, if he wants it | Thin runner executable, usage documentation, and command-line integration cases. | R0 contract frozen. End-to-end completion needs R2 and M1. |
| V1 | Fable or another independent reviewer | Consumer-level validation of capability boundaries, failure behaviour, and fixture coverage. | R0 for test design; M1/R2/C1 for execution. Coordinate test-file ownership before writing. |
| I1 | Runtime owner as integration coordinator | Shared build/image registration, final dependency checks, combined strict build and QEMU evidence. | M1, R2, C1, and V1 evidence. |
| D0 | Fable | DOM.md covering mutable libhtml, document/wrapper lifetime, retired storage, parser handoff, and browser event scheduling. | This design and the existing browser libraries. Design runs alongside M1/R1; reviewed completion gates J3, not the runner. |

The runtime owner is the coordinating implementer working with Chris in this
thread. Review of a packet is separate from ownership of its implementation.
Assignments are suggested so Chris can dispatch them; they are not an implicit
request to launch agents or send messages.

## R0 runtime contract

The conceptual contract is agreed in JAVASCRIPT.md under Runtime contracts.
R0 remains open for interface review: declarations, ownership rules, and
failure semantics are proposed in [the runtime contract](../../../userland/libjs/CONTRACT.md)
and [public header](../../../userland/libjs/include/os64/js.h), awaiting Fable's
interface review and implementation evidence. The runner requires
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
  covering the upstream pin, port ABI, and relevant configuration. Test a
  mismatched caller and a useful diagnostic, not only a matching build.
- Output and script-argument bindings implemented through library machinery;
  callers choose output destinations rather than duplicating those bindings.
- One owning thread per runtime, permitted cross-thread cancellation signalling,
  class-registration synchronization, and rejection of evaluation or job-drain
  re-entry from callbacks into the same runtime through the os64 wrappers.
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
unhandled rejection, callback failure, and independent runtimes. Confirm that
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
browser APIs remains J3/J4 work. Safe recovery from a known teardown leak, if
proposed, needs a separate audited contract including cumulative memory bounds.

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

## First actions

1. Confirm the proposed maths ABI between M1 and the runtime owner. Opus can
   then start the maths port from MATH.md without browser work.
2. Turn the agreed six-operation API into R0's runtime header and precise
   ownership and failure contracts, with Fable reviewing the consumer boundary.
   Use QuickJS directly for bindings. This runs alongside the maths port.
3. Begin R1 after source and adaptation decisions are recorded. C1 starts when
   the R0 contract is stable, and V1 can design fixtures against that contract.
4. Fable develops D0 alongside M1/R1. Integrate guest evidence and review DOM.md
   before advancing to Yonder's mutable-document implementation campaign.

## Foundation slice 2026-10-01

The runtime owner has begun in `.worktrees/js-runtime-foundation` on branch
`codex/js-runtime-foundation`, based on `b55c3770`. The agreed design/review
documents were copied into that checkout without changing the main checkout.

- R0 has concrete proposed declarations, a lifecycle/job/error contract, and
  syntax-checked runner, expression/job, and custom-binding examples. The
  conservative failed-runtime policy and continuous deadline across manual
  job slices require interface review before C1 depends on them. No runtime
  symbol is implemented by these headers.
- The separate libos64 prerequisite change supplies `os64_malloc_size`,
  `os64_memchr`, `os64_strchr`, `os64_strrchr`, and `os64_strcmp`, with host
  regression coverage and a guest fixture at `/tests/jssupporttest`.
- R1 source preparation retains the pinned original core and hashes, notices,
  a scoped Atomics patch, and a repeatable engine-only host probe. The profile
  preserves stack checks. Target adaptation and libjs.so registration remain
  open; the host probe uses host libc/libm and does not establish os64 execution.

Validation and remaining work are recorded in `userland/libjs/VALIDATION.md`.
M1, R2, C1, V1, and D0 remain separate work packets. The library prerequisite
commit is on `codex/js-support`; the interface and source preparation are stacked
above it on `codex/js-runtime-foundation` for review before consumer work begins.
