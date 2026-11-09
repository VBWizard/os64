# JavaScript library and runner work plan

Status: running work checklist, updated 2026-10-04. The product decisions are in
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
plan, without assigning a new contributor or claiming work has started. "Partial"
records completed evidence while named acceptance work remains.
"Implemented; review pending" records a validated packet that still needs
independent review. "In progress" records an authorized slice being implemented.

| Work | Status on 2026-10-04 | Evidence and remaining work |
| --- | --- | --- |
| libos64 prerequisites | **Merged** | [PR #188](https://github.com/VBWizard/os64/pull/188), merge `6a6c08c1`: allocation-size query and four string/memory verbs, host coverage and guest test registration. |
| R0 runtime contract | **Merged** | [PR #189](https://github.com/VBWizard/os64/pull/189), merge `a426386d`: reviewed public header, lifecycle/capability contract, examples, pinned QuickJS foundation. R0 delivered the interface; R2 records implementation. |
| M1 maths library | **Merged** | [PR #191](https://github.com/VBWizard/os64/pull/191), merge `3b509e8b`. R1 now links against its real shared library. Numerical acceptance belongs to M1; the R2 core fixture supplies combined guest engine evidence. |
| R1 target adapter/core | **Merged** | [PR #192](https://github.com/VBWizard/os64/pull/192), merge `ef43d227`: strict core/userland builds, real libmath/libos64 linkage, target symbol/header/ELF/relink audits, 675 sanitized host checks and 437 cross-core host checks pass. R1 itself did not run a guest; the R2 core fixture now exercises the combined library. |
| R2 runtime implementation | **Merged** | Core [PR #194](https://github.com/VBWizard/os64/pull/194), merge `a6d45945`; file/output/argument helpers [PR #195](https://github.com/VBWizard/os64/pull/195), merge `a558f8fd`. All eleven embedding operations are implemented. Maintained suites pass 822 target-engine host, 3,014 sanitized-engine host and 395 os64/QEMU checks; evidence and review corrections are in libjs/VALIDATION.md. |
| C1 runner | **Merged; running on P5** | Opus's [PR #196](https://github.com/VBWizard/os64/pull/196), merge `a2e409a7`, delivers file/expression/stdin execution, copied arguments, selected output, diagnostics and exit mapping. Both `js` and `libjs.so` build by default for os64get delivery. Host and QEMU acceptance pass; Chris reports `js primes.js \| wc -l` produced `1229` on the P5. Standard-image delivery is recorded in I1. |
| V1 independent validation | **Merged** | [PR #197](https://github.com/VBWizard/os64/pull/197), merge `99df2636`: independently reviewed separate consumer suite passes 383 checks in each host engine profile and 375 in QEMU, with 55 upstream functions passing and four explicit host-feature skips. Capability/failure/native ownership, diagnostic exhaustion, reuse, ABI refusal and the separate fatal process are covered. V1 found and fixes an engine error lifetime defect under persistent allocation refusal. |
| I1 shared integration | **Merged** | [PR #198](https://github.com/VBWizard/os64/pull/198), merge `1b874edd`: the normal ext2 root and FAT rescue volume install `js`, `libjs.so` and `/etc/licenses/quickjs.txt` with their existing dependencies. Strict root/header/target checks and 18 image byte comparisons pass, including FreeType through libos64. Fresh ext2-root and FAT-root QEMU boots each pass the CLI smoke cases and V1's 375 checks, with 55 upstream functions passing and four explicit skips. Both prime pipelines produce `1229`; the separate fatal process returns the full JSFA badge. |
| D0 DOM design | **Merged** | Reviewed [DOM.md](DOM.md), delivered with [PR #190](https://github.com/VBWizard/os64/pull/190). Design completion is separate from browser scripting implementation. |
| D1 libhtml mutation core | **Merged** | [PR #190](https://github.com/VBWizard/os64/pull/190), merge `5c7d62ce`. Mutation/lifetime verbs and maintained tests; `parse_fragment` belongs to D2b. |
| D2a scripting-enabled parsing | **Merged** | [PR #199](https://github.com/VBWizard/os64/pull/199), merge `8016dd43`: stop/resume, the end of the input, abandon, and a tree read and changed between calls; evidence in DOM.md § D2a, as built. |
| D2b fragment parsing | **Merged** | [PR #212](https://github.com/VBWizard/os64/pull/212), merge `dca2fe46`: transactional contextual parsing and allocation-free serialization. All 192 fragment fixtures and failure/mutation proof pass; evidence, compatibility boundary and D6 storage handoff in DOM.md § D2b, as built. |
| D3 page rebuild/control state | **Merged** | [PR #211](https://github.com/VBWizard/os64/pull/211), merge `3f5fa28a`: shared node state, pinned models, STALE gates, script property APIs and transactional rebuilds. Evidence and D5/D6 handoffs in DOM.md § D3, as built. |
| D4 parser stream handoff | **Built; in review** | yonder parses on its window's thread from a body the worker streams through the navigation mailbox; libway's loader split into its pieces with `way_load` kept for wend; no script execution in this slice. Design in [DOM_D4.md](DOM_D4.md), record in DOM.md § D4, as built. |
| D5 DOM binding/first page fixture | **D5a and D5b merged** | [D5a PR #214](https://github.com/VBWizard/os64/pull/214), merge `72b2e710`, supplies libdom and state-aware mutation/clone transactions; evidence in DOM.md § D5a, as built. [D5b PR #216](https://github.com/VBWizard/os64/pull/216), merge `7acce890`, records the default-off settings switch and host/guest mutation, widget and queued-navigation proof; D4 is not a prerequisite. |
| D6 detached-subtree reclamation | **Implemented; review pending** | Counted holds, parser-reference protection and paired binding/state/model/browser ownership; 216,000 fragment refresh cycles stay flat under 64 MiB. [PR #217](https://github.com/VBWizard/os64/pull/217) now targets userland after D5b merged; five Fable findings are addressed and re-review is required. Evidence and retained weak-wrapper/collector debt in DOM.md § D6, as built. |
| D7 browser event loop | **Designed 2026-10-05 (DOM_D7.md); D7a and D7b merged (DOM.md § D7a and § D7b, as built); D7c the join with D8 and D10 merged (DOM.md § D7c, as built); D7d, the sheets before a script: built (DOM.md § D7d, as built), in review** | Tasks/checkpoints, timers/events, script order and J4 evidence. Resolve the execution-time default/range and consider a script-timeout Settings control; D5b's one-second fixture deadline does not settle ordinary-browsing policy (DOM.md). |
| D8 reporting runtime destroy | **Implemented; approved by Fable** | `codex/dom-d8`, based on `29641e20`: opt-in allocator-ledger reclamation, browser logging/counting, host and guest repeated-leak proof; [DOM_D8.md](DOM_D8.md). D7 integration requires a joined ownership audit and validation. |

The DOM slice definitions and detailed acceptance cases belong to
[DOM.md](DOM.md); this table tracks their place in the overall campaign.
The implementation and validation evidence for libjs belongs to
[VALIDATION.md](../../../userland/libjs/VALIDATION.md). The milestone criteria
belong to [JAVASCRIPT.md](JAVASCRIPT.md#delivery-and-validation):

| Milestone | Status on 2026-10-05 | Evidence and remaining work |
| --- | --- | --- |
| J0 reviewed foundation | **Complete** | Pinned QuickJS source/profile, maths selection, dependency inventory and reviewed runtime contract are delivered. |
| J1 library and runner on os64 | **Complete** | Merged M1/R1/R2/C1, strict target build/link, expected shared dependencies, script execution and exception output in QEMU. Chris's P5 prime-count pipeline adds a hardware smoke test. J1 does not require standard-image installation. |
| J2 lifecycle and failure acceptance | **Complete; merged** | [PR #200](https://github.com/VBWizard/os64/pull/200), merge `6fda4b79`: existing lifecycle/failure/cancellation/Promise/fatal and upstream evidence is supplemented by 105 guest measurement checks passing on one and eight CPUs. Twelve recursion/native-frame cases retain sampled headroom and permit reuse; two competing runtimes preserve XMM/x87/control state across yield/sleep with a deliberate-disturbance negative control. Six workloads fit the retained 64 MiB/256 KiB/4 MiB/60 s profile, now published by `os64_js_default_limits()` and shared by the runner. Both final boots also pass V1's 375 checks, CLI/default/768 KiB-cap cases and the fatal status; host runner suites pass 196/38 checks. |
| J3 first scripted Yonder fixture | **Complete; merged in PR #216** | D0/D1/D2a/D2b/D3/D5a are merged. D5b passes the visible mutation/rebuild/reference/form/navigation fixture and default-off Apply/Save switch; see DOM.md § D5b, as built. |
| J4 browser execution and events | **Pending** | D7's evidence: browser script order at the parser's stops, timers, events and their attributes, external scripts, the overrun policy, each with its fixture. D4, its prerequisite, is built and in review on `fable/dom-d4` (DOM.md § D4, as built); D6 is merged (PR #217). Origins and cookies move to the modern-web campaign. |
| J5 three real pages | **Pending** | With the switch turned on by hand, three real pages Chris chose work as a 1998 browser shows them: a `document.write` counter, a DHTML menu, a form validator. Needs D7, D9 and D10; D11 is driven by whatever these three still lack. Chris confirms on the P5. |
| J6 scripts on by default | **Pending** | D8's reporting destroy in, J5 passed, and Chris's ruling to change DOM.md ruling 1. The switch stays in Settings either way. |

Next steps:

1. Review and merge D4 (`fable/dom-d4`): the stream, DOM.md § D4, as built.
2. D7, Fable's: the loop over the parser D4 put on the window's thread.
   Browser function-call/checkpoint and audited runtime teardown retain
   their own acceptance before ordinary browsing runs scripts.

**The road from D7 to scripts on**, as laid out on 2026-10-04 and restored
here from Chris's copy after the original was lost. Each row is its own
slice with its own design or brief; the gates are the rows J6 waits on.

| Row | Owner | Why it is there |
| --- | --- | --- |
| D7 the loop: tasks, checkpoints, timers, events and their attributes, script order | Fable | J4's evidence; everything after it runs inside this loop |
| D8 reporting runtime destroy (DOM.md ruling 8, § A leak at teardown; CONTRACT.md's third browser extension) | Quinn | first gate before the default can change |
| D9 `document.write` (`os64_html_parser_write`, legal only while the parser is stopped at a script) — **designed 2026-10-05, DOM_D9.md; built on `fable/dom-d9`, DOM.md § D9, as built** | Fable designs and reviews, Opus builds | second gate; coupled to D7's stop semantics |
| D10 geometry (DOM.md § Geometry: a forced layout, charged to the script) | Quinn | third gate; menus and fit-to-window scripts |
| D11 the surface the old web calls | **Built; awaiting Fable review and P5 acceptance** | [DOM_D11.md](DOM_D11.md): Chris chose Lileks/Museum/Million Dollar Homepage and prioritized their visible widgets. Live named images/forms, inline styles, navigator, legacy event/function arguments, image maps and clip rectangles. Eager detached-image preloading deferred with Chris; document metadata remains outside these primary paths. J5 stays pending |
| The switch's default (DOM.md ruling 1) | Chris rules | after D8, D9, D10 and J5 |
| D12 weak wrappers and the collector (DOM.md § Booked, node reclamation beyond D6) | Quinn, design first | capacity for long sessions |
| D13 finer invalidation and incremental relayout (DOM.md § Booked, LAYOUT.md's row) | Opus | speed; belongs with the performance work |
| The modern web: `fetch`, origins, CORS, `document.cookie`, storage, `requestAnimationFrame`, frames | Fable as architect | its own campaign and design document |

## Agreed standalone scope

- Library-owned runtime behaviour, with a thin runner executable.
- Files and expressions as inputs, script arguments, output, exception
  diagnostics, and Promise processing.
- Explicit registration of host capabilities. The initial script capabilities
  are output and arguments, without filesystem, networking, or process APIs.
- A reusable maths library built from established numerical code.
- Finite memory and execution defaults, configurable through the library and
  runner, with distinct exception, limit, cancellation, and host-failure results.
- An interactive prompt, timers, and file-based module loading are deferred.
  C1 supports whole-script standard input through `js -`.
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
| C1 | Opus | Thin runner executable, usage documentation, command-line integration cases and os64get delivery; merged in PR #196. | R0, R2 and M1 are merged; standard-image installation belongs to I1. |
| V1 | Quinn implements the consumer suite; another reviewer accepts the independent gate | Consumer-level validation of capability boundaries, failure behaviour, and fixture coverage. | Based on merged M1/R2/C1; owns tools/js_acceptance and tools/test_js_acceptance_host.sh. R2's fixture files remain separate. |
| I1 | Runtime owner as integration coordinator | Shared build/image registration, final dependency checks, combined strict build and QEMU evidence. | M1, R2, C1, and V1 evidence. |
| D0 | Fable | DOM.md covering mutable libhtml, document/wrapper lifetime, retired storage, parser handoff, and browser event scheduling. | This design and the existing browser libraries. Design runs alongside M1/R1; reviewed completion gates J3, not the runner. |
| D1–D7 | Per [DOM_BRIEFS.md](DOM_BRIEFS.md): D2b, D3 and D5 Quinn with scoped subagents; D4 and D7 reserved for Fable; D6 Quinn with a scoped builder/tester team, PR #217 against userland; Opus on Yonder pile 3. Fable reviews the shared DOM slices | Mutable-document, parser, presentation, bindings and event-loop slices defined in DOM.md. | D0; detailed dependencies and acceptance belong to DOM.md, the builder's brief to DOM_BRIEFS.md. D1 and D2a are merged; later slices remain separate from the standalone milestone. |

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

The delivered forms are `js [options] FILE [ARG...]`,
`js [options] -e SOURCE [ARG...]` and `js [options] - [ARG...]`.
Chris ruled on the grammar and Quinn accepted it as runtime-contract owner;
the specification is [USAGE.md](../../../userland/apps/js/USAGE.md).
Arguments after the first script operand belong to the script; `scriptArgs[0]`
names the file, `-e` or `-`. Explicit limit options and a `--` delimiter are
supported. The library supplies the reusable loading, evaluation,
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

The core slice was delivered from `.worktrees/js-runtime-core`, branch
`codex/js-runtime-core`, based on merged R1 `ef43d227`. It exports create,
context access, class-ID allocation, eval, run, drain_jobs, cancel and destroy.
The guest fixture is an explicit build target and is installed only into a
validation image; libjs builds by default for os64get delivery but remains
outside the normal image population. Its
implementation also includes the tracked QuickJS allocation-failure cleanup guards.
The core merged in PR #194 as `a6d45945`.
The core slice passes 475 target-core host checks, 2,539 sanitized-engine host
checks and 280 checks in os64/QEMU; details and library hashes are in VALIDATION.md.

R2 was delivered in two implementation slices. The core
[PR #194](https://github.com/VBWizard/os64/pull/194) carried `45cae4fc`.
The helpers were delivered from `.worktrees/js-runtime-helpers`, branch
`codex/js-runtime-helpers`, stacked on that core commit for review.
[PR #195](https://github.com/VBWizard/os64/pull/195) was retargeted to `userland`
after the core merged, and merged as `a558f8fd` with corrections through
`9b5a795e`.

The second slice implements `run_file`, `install_output` and `install_args`,
including bounded input/close failure, output conversion/write failure, copied
arguments, installer flags and setup-state checks. The shared library exports
all eleven declared embedding operations. Both slices are merged; Opus's C1
runner uses them and is merged separately in PR #196.
The combined core/helper fixtures pass 822 target-engine host checks, 3,014
sanitized-engine host checks and 395 os64/QEMU checks, with zero failures.
The first helper review's combined-output transaction finding is corrected:
selected properties are staged/restored and the borrowed handle commits after
installation succeeds. The second review's unnecessary file-buffer growth at
EOF is corrected: an extra-byte probe precedes expansion, with tight-budget
boundary, probe failure/cancellation and owned-input cleanup coverage.
Both corrections are in the accepted helper merge; independent validation
remains the separate V1 packet.

Production limit defaults remain J2 work, rather than adopting the fixtures'
development budgets without measurements. D2-D4 can proceed independently; the D5 binding
join and later browser turn/reclamation APIs remain separate acceptance gates.

## C1 acceptance and P5 observation, 2026-10-02

[PR #196](https://github.com/VBWizard/os64/pull/196) merged as `a2e409a7`,
including Opus's runner, real-library integration and delivery changes through
`2610fc42`. `make` produces `js` and `libjs.so` in userland/bin for os64get;
the root image's app list still excludes js until I1 installs its library and
licence together. The GDB symbol map includes both.

Quinn's read-only C1 review covered published head `0d337421`, before the
delivery follow-up. The maintained suites passed 196 runner checks and 38
real-library checks. Temporary independent allocation-refusal probes raised
the runner suite to 202 checks, with no failures and leak checking enabled.
The strict root build passed, and existing app placements were unchanged.
Ten independent QEMU scenarios passed: file arguments, expression input,
piped stdin, thrown-exception diagnostics, jobs/time/memory/source budgets,
recursion at the 768K stack cap, and usage errors. Captured output and exit
statuses matched expectations; the returned js and libjs files matched the
review build byte for byte. This review found no code findings and accepted
the runner's grammar, argument convention and exit mapping.

Chris subsequently reported this hardware result on the P5:

```text
js primes.js | wc -l
    1229
```

This records Chris's reported hardware execution and pipeline output.
Independent language and numerical acceptance remain in V1/J2.

## V1 consumer acceptance handoff, 2026-10-02

Chris assigned V1, I1 and J2 in sequence. The V1 implementation is isolated in
`.worktrees/js-validation`, branch `codex/js-validation`, based on merged
`userland` `a2e409a7` plus task-status documentation commit `2b3ee4cb`.
Implementation `80f2d4f9` is published in
[PR #197](https://github.com/VBWizard/os64/pull/197), targeting `userland`.
The packet passed independent review and merged as `99df2636`.

`tools/js_acceptance` owns a consumer suite with no R2 fixture/private-runtime
includes, retained hash-checked upstream tests and their original licence.
`tools/test_js_acceptance_host.sh` runs the target core/M1 maths and a second
sanitized engine against those cases. Both pass 383 checks with zero failures;
diagnostic refusal ends with zero live allocations. Each reports 55 upstream functions passing
and four unsupported host-capability skips. QEMU passes 375 checks, the same
language selection, ordinary status zero and the expected separate JSFA fatal
status. Consumer/library byte comparisons, regression suites and precise
limitations are recorded in libjs/VALIDATION.md.

Persistent allocation refusal during diagnostic conversion exposed a borrowed
error freed while QuickJS assembled its backtrace. Manifest patch 0005 retains
the error through that operation and frees its early-exit buffer. The same
fixture that crashed before the patch now returns structured HOST_FAILURE and
reclaims its storage. No public API, image inventory or kernel behavior changes.

Quinn authored R2 as well as this suite; independent review of PR #197 closed
V1's acceptance gate. I1 image/licence installation and J2 stack/defaults/
scheduling measurements are separate packets.

## I1 standard-image handoff, 2026-10-02

Chris assigned I1 after accepting V1. The integration is isolated in
`.worktrees/js-integration`, branch `codex/js-integration`, based on merged
V1 `userland` `99df2636`. Implementation `3b986b84` is published in
[PR #198](https://github.com/VBWizard/os64/pull/198), targeting `userland`.
The packet passed review and merged as `1b874edd`.

The root GNUmakefile discovers `js` with the other applications and adds
`libjs.so` to the shared-library inventory used by both ext2 and FAT. Both
image recipes depend on and install the pinned `upstream/LICENSE` directly as
`/etc/licenses/quickjs.txt`; no duplicate licence source is introduced. The
obsolete image-exclusion debt and current installation claims are updated.
Runtime, runner, public ABI and kernel behavior are unchanged.

`python3 tools/test_js_image.py` audits the built runner's complete recursive
DT_NEEDED chain, verifies its and libjs's direct dependency sets, checks the
notice against the source manifest, and compares the actual payload on three
volumes. All 18 byte comparisons pass: runner, libjs, libmath, libos64,
libfreetype and notice on the standalone ext2 image, disk ext2 root and FAT
rescue volume. Scratch-image negative controls reject missing FreeType and
missing QuickJS notice on FAT. Strict root build, contract/header checks and
target symbol/dependency/relink audits pass.

Fresh QEMU boots use the normal ext2 root and, with a temporary boot-config
selection, the FAT root. Only the optional V1 consumer and CLI input files are
added to scratch `/home`; the root disk's payload is unchanged. Each boot passes
expression, arguments/Promise, piped stdin, exception/status and primes-pipeline
checks, plus 375 consumer checks with zero failures and the 55-pass/four-skip
upstream selection. The separate fatal fixture returns `0x4A534641`. The two
prime pipelines each produce `1229`. Delivered runner, libraries, notice and
consumer match the checkout's build byte for byte; pre/post/late boot checks
pass and both owned VMs are stopped. Read-only filesystem checks on both disks'
ext2 and home partitions pass. Exact results, hashes and evidence paths
are recorded in libjs/VALIDATION.md. No new P5 run is claimed.

At the I1 handoff, J2's stack/defaults/scheduling measurements were the next
packet; that evidence is recorded below. Browser scripting remains D2-D7.

## J2 standalone acceptance handoff, 2026-10-02

Chris assigned J2 after I1 merged. The packet is isolated in
`.worktrees/js-acceptance-limits`, branch `codex/js-acceptance-limits`, based on
`userland` `1b874edd`. Implementation `ec41a57f` and guest acceptance are
complete; [PR #200](https://github.com/VBWizard/os64/pull/200) is open against
`userland` for independent review.

`tools/js_measure` and the explicit `js-measure-test` target provide a maintained
guest consumer using public runtime/binding APIs and `/proc/self/maps`. At
128/256/768 KiB engine budgets, simple recursion, eight live locals, accessor
recursion and recursion with a bounded 64 KiB native frame all end as engine
exceptions and permit reuse. On the mapped 1 MiB native stack, the 256 KiB
default with the native frame retains
718,688 bytes sampled headroom; the 768 KiB cap retains 194,528 bytes. These
are sampled cases, not a universal per-frame cost or proof for arbitrary native
bindings. Yonder layout-call measurements remain in browser integration.

Two threads own separate runtimes and preserve sentinels in sixteen XMM and
two occupied x87 registers plus control/status fields across existing yield/
sleep syscalls. Distinct rounding controls, an intentional-disturbance negative
control, script numeric/formatting/Promise assertions, peer-progress counts and
proc switch reports establish the tested scheduling behavior. Final one-/eight-
CPU boots pass 105 checks with zero failures, with 259/141 recorded process
switches during the worker phase. No kernel or syscall change is introduced.

Six representative workloads cover primes, JSON, an 8 MiB typed array, BigInt,
100,000 Promise jobs and the 4 MiB source ceiling. The largest sampled engine
charge is 8,547,952 bytes; the longest observed case in these two runs is about
2.7 s. Retain 64 MiB memory, 256 KiB stack, 4 MiB source, 60,000 ms execution
and `UINT64_MAX` jobs as the standalone starting profile. The deadline bounds
ordinary Promise work; `--jobs` remains an explicit practical count cap.
`os64_js_default_limits()` publishes these values in the library header; the
runner and standalone example share it. The helper changes no struct layout or
exported symbol. All runner overrides remain available.

Strict root/header/target/image audits pass. The runner host suites pass 196
stand-in and 38 real-library checks with normal leak inspection. Each final
fresh boot also passes V1's 375 checks (55 upstream functions passing, four
explicit skips), CLI expression/arguments/Promise/stdin/exception/primes cases,
the runner's 768 KiB recursion cap and the separate actual JSFA fatal status.
All compared delivered artifacts match the build. Boot/late kernel checks and
post-stop ext2/home filesystem checks pass; both owned final VMs are stopped.
Exact workloads, tables, hashes and evidence paths are in libjs/VALIDATION.md.
The suite does not claim a full interrupt-coverage audit, arbitrary native-
callback safety, browser layout headroom or a new P5 run.

## D10 implementation evidence

Synchronous HTML geometry is implemented independently from D7 and D8 on
`codex/dom-d10`, based on `userland` `29641e20`. The provider interface,
forced-layout budget checks, status telemetry and combined JavaScript/native
stack evidence are recorded in [DOM_D10.md](DOM_D10.md), implementing
[DOM.md § Geometry](DOM.md#geometry). D7's loading-document provider and joined
dispatch validation remain integration work. This adds no dependency on D8's
teardown policy.
