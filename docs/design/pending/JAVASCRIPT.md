# JavaScript runtime and Yonder integration

Status: design plan, updated 2026-10-01. Chris has agreed on the delivery order:
build a reusable JavaScript library for os64, exercise it through a thin
command-line executable inside the OS, then integrate the same library with
Yonder. QuickJS is the engine selected for this porting investigation. The
runner scope, explicit capability registration, reusable maths library, and
configurable limits are also agreed. `libjs.so` and `js` are working names,
not a settled naming decision. The runtime operations, capability boundary,
and initial execution restrictions are agreed below; exact C declarations and
the browser DOM lifetime and scheduling details remain to be specified. The
recommended maths implementation and its porting contract are in [MATH.md](MATH.md); assignable work is in
[JAVASCRIPT_TASKS.md](JAVASCRIPT_TASKS.md).

Fable's review is in [JAVASCRIPT_REVIEW.md](JAVASCRIPT_REVIEW.md), with Quinn's
responses appended. Accepted changes are incorporated here; safe containment
of a teardown invariant failure remains an open browser design question.

The initial implementation slice now has [proposed public headers and a precise
runtime contract](../../../userland/libjs/CONTRACT.md), shared libos64 support
functions, and a durable pinned source import. Header examples and an engine-only
host probe are checks of that foundation; no libjs.so or os64 runtime execution
is established. JAVASCRIPT_TASKS.md records the implementation boundaries.

This document records the architecture and the initial dependency audit. It
does not establish that QuickJS builds or runs on os64. The inspected checkout
was `b55c37704212a8c03b9d9c8ab0bd128fb104d01e` on `userland`, with existing local
changes. The audit compiled and exercised an engine configuration on the Linux
host. No kernel change is proposed; a demonstrated kernel gap would need its
own discussion before implementation.

## Agreed direction

The library owns the JavaScript functionality. The runner is its first
consumer, and Yonder becomes another consumer of that same implementation.
Runtime creation, execution, value and exception ownership, limits, job
processing, and host integration must not be implemented privately in the
runner and extracted later.

The runner supplies command-line arguments, selects input and output, and
reports the library's outcome as an exit status. Reusable script loading,
diagnostic formatting, and any initial print facility belong in library code.
Tests may own their assertions and fixtures; production runtime behaviour
belongs below the executable boundary.

The first milestone is JavaScript executing correctly inside os64, including
predictable failures and teardown. Node compatibility, npm, a package manager,
and a general-purpose operating-system API for scripts are outside this plan.
A polished interactive shell is not a prerequisite for Yonder.

The agreed runner scope includes a script file or an expression, script
arguments, output, useful exception diagnostics, and Promise processing.
Interactive input, timers, and file-based module loading are deferred features.
The initial script capabilities are output and arguments; the library provides
an explicit registration mechanism for applications to add capabilities.
Scripts do not automatically acquire filesystem, network, or process access
because their host uses those services.

Memory and execution limits have finite defaults and runner overrides. Their
numeric values follow guest measurements. Exceptions, exhausted limits, and
cancellation are distinguishable library outcomes and produce a failing runner
exit status. Maths support is a reusable library rather than a private part of
the executable or JavaScript implementation.

Browser ownership and scheduling should be designed while the engine port is
being established. DOM implementation does not block the standalone runtime
milestone, but browser requirements must inform the library boundary.

## Architecture

| Component | Responsibility |
| --- | --- |
| JavaScript library | Pinned QuickJS core, os64 adaptation, runtime lifecycle, evaluation, exceptions, limits, pending jobs, and explicit host bindings. |
| Runner executable | Argument handling and invocation of the library, with output destinations and process exit status. |
| Yonder integration | Browser host configuration and connection to document, page, style, layout, navigation, and GUI services. Reusable browser semantics belong in libraries. |
| Existing os64 libraries | Allocation, time, files, networking, threading, and GUI notification primitives. |
| Maths library | Reusable floating-point functions required by the engine. The recommended musl source and first binary64 interface are specified in MATH.md. |

The engine library must be usable without linking the HTML parser, layout,
GUI, or network stack. Yonder's browser bindings add those dependencies above
the engine. Useful common support should be shared deliberately; copying
private compatibility functions out of other ports without reviewing their
contracts would create another maintenance obligation.

Use an opaque runtime API in `os64/js.h` for lifecycle, limits, evaluation,
job processing, cancellation, and outcomes. The runner uses that API and the
library's binding installers without depending on QuickJS types. Native
binding authors use the pinned `quickjs.h`, deliberately published by libjs,
for values, classes, properties, exceptions, finalizers, and GC marking.
The runtime API provides a controlled accessor to the engine context. Do not
build a second argument/result/class API around the engine's existing one.

Before exposing a context, creation must check a caller-supplied compatibility
identifier from the installed header. It covers the upstream pin, the os64
port ABI, and configuration that affects the public layout or contract.
Reject a mismatch with an identifiable diagnostic before binding code uses
engine values. QuickJS inline functions make header/library agreement part
of memory safety. Exact declarations and the identifier format belong to R0.

## Existing browser foundations

Fable explicitly anticipated scripting in
[LIBPAGE.md](../../../LIBPAGE.md) and
[LAYOUT.md](../completed/LAYOUT.md). The relevant foundations are present,
with important limits:

- `userland/libpage/internal.h` stores form edits by source-node pointer.
  This permits preservation across a future model rebuild, provided node
  identity survives. `os64_page_rebuild` is specified as deferred work in
  LIBPAGE.md; it is not an implemented function.
- `flow_layout` builds a derived box tree from a document and page model.
  Incremental relayout is not required for an initial scripting milestone.
- Yonder's `page_lay_out` constructs a replacement layout before releasing
  the previous layout. Its present cascade refresh triggers are stylesheet
  and viewport changes; DOM changes need additional invalidation.
- Yonder has network workers, mailboxes, navigation generations, and GUI
  doorbells. Its ticker supplies deadline wakeups for existing browser work.
  These mechanisms are useful foundations, not implementations of the HTML
  event loop or JavaScript timers.
- libgarb has selector parsing and matching. DOM query APIs should reuse
  appropriate machinery while implementing their own specified scope,
  exception, and result semantics. Existing dynamic pseudo-class limitations
  also need consideration.
- libhtml exposes document-owned, read-only nodes. Its parser has no public
  mutation or fragment-parsing interface and uses scripting-disabled parsing.

The browser was prepared for this extension, but it does not already contain
a mutable DOM. The existing tree, model, and layout APIs share node pointers;
the ownership design must account for all three consumers.

## QuickJS source baseline

The initial audit used the official
[QuickJS 2026-06-04 archive](https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz).
Its downloaded SHA-256 was:

```text
b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a
```

This digest identifies the audited bytes; it is not an independently verified
upstream signature. Preserve upstream licensing, a source manifest, and a
reviewable patch series when vendoring. Follow `userland/libfreetype/`:
`upstream/`, `patches/`, `manifest.json`, `UPSTREAM_REVIEW.md`, `exports.map`,
and `port/`. The official project distributes QuickJS under the MIT licence.

The audited core consists of `quickjs.c`, `dtoa.c`, `libregexp.c`,
`libunicode.c`, and `cutils.c`, with their headers and generated tables.
The stock build also includes `quickjs-libc.c`, which supplies facilities for
its standalone host. That file, the upstream shell, and its `std` and `os`
modules are not required for our initial embedding.

For the audit, the `CONFIG_ATOMICS` definition was removed in a scratch copy
of `quickjs.c`. This is a feature restriction: JavaScript Atomics support is
absent in that configuration. A production configuration must express it in
a maintained patch or upstream-supported switch. Do not select an unrelated
platform macro to suppress dependencies; it can also change stack checking
and dispatch behaviour.

## Porting requirements

The following inventory describes the inspected configuration. The cross
compiler and final link may emit a different helper set; repeat the symbol
audit against the actual os64 objects.

| Area | Evidence and required work |
| --- | --- |
| Allocation | os64 provides malloc, realloc, calloc, and free equivalents. QuickJS supports custom callbacks through `JS_NewRuntime2`. Adapt them with correct size accounting, overflow handling, failed-reallocation behaviour, and memory-limit enforcement. The default allocator references `malloc_usable_size`, which is not an os64 public API. Merely passing custom callbacks does not remove unresolved references in retained default-allocator code. |
| Memory and strings | libos64 supplies core memory primitives and string length. Add the required `memchr`, `strchr`, `strrchr`, and ordering `strcmp` semantics as public `os64_` verbs in a separately reviewed libos64 change. QuickJS uses a private rename header, not private function bodies. FreeType's private copies are precedents to check; their later consolidation is tracked in DEBTS.md under Shared libraries. |
| Maths | The audited engine references 32 floating-point maths functions, listed below. No general public maths library providing this set was found in the checkout. The musl dependency audit and porting recommendation are in MATH.md. Validate JavaScript-relevant special cases. Kernel floating-point context support already exists. |
| Clocks and dates | Engine calls to `gettimeofday` and `localtime_r`, including `tm_gmtoff`, require adaptation. os64 already exposes UTC epoch and subsecond phase, local calendar conversion with timezone/DST policy, and monotonic ticks. Check offset signs, historical dates, range limits, and failure behaviour. Use monotonic time for deadlines. |
| Formatting and diagnostics | `snprintf`, `vsnprintf`, and `fprintf` remain in the compiled dependency inventory. os64's formatter implements a subset of C formatting and omits floating-point formatting. Audit retained call sites before selecting adapters or extending shared formatting. Assertions and fatal diagnostics must have a defined failure path. |
| Compiler runtime | The host object references `__udivti3` and `__udivmodti4`. The cross toolchain supplies a `libgcc.a`; inspect and link the required target-compatible helpers without introducing a host-library dependency. |
| Threads and Atomics | The configured core has no unresolved pthread symbols. Ordinary evaluation can be owned by one thread. Shared-memory JavaScript and worker support are separate feature work; disabling Atomics does not itself guarantee that every shared-memory constructor is absent. |
| Packaging | Integrate the library with the existing shared-library address assignment, dependency declarations, strict build, and image inventory. The runner must load the same library implementation intended for Yonder. |

The maths symbols found in the host object were:

```text
acos acosh asin asinh atan atan2 atanh cbrt ceil cos cosh exp expm1
fabs floor fmax fmin fmod hypot log log10 log1p log2 lrint pow round
sin sinh sqrt tan tanh trunc
```

Check signed zero, NaN, infinities, subnormals, overflow, and rounding in
addition to ordinary results. Basic hardware floating-point arithmetic is
already available; implementing these library functions does not by itself
call for a kernel maths service.

Headers are not a dependency inventory. For example, included headers for
nonlocal jumps or floating-point environments did not produce corresponding
external calls in this configuration. Conversely, compiler-generated helpers
may be present even when the source names no such function.

## Runtime contracts

The six operations and initial execution restrictions in this section are
agreed. R0 in JAVASCRIPT_TASKS.md translates them into precise C declarations,
ownership rules, and failure semantics before consumers implement against them.

### Public operations

| Operation | Library responsibility | Consumer responsibility |
| --- | --- | --- |
| Create a runtime | Create an independent JavaScript environment with configured memory, stack, and execution limits. Creation grants no implicit filesystem, network, or process capabilities. | Supply configuration and retain the runtime handle. |
| Register capabilities | Supply installers for output and script arguments, and controlled context access for native bindings. | Choose capabilities; implement custom bindings with the published QuickJS API and honour its ownership rules. |
| Execute source | Evaluate a bounded source buffer with a diagnostic source name. A library file helper performs bounded loading and uses the same evaluation path. | Select a file or expression and supply its input or path. |
| Process pending jobs | Execute pending Promise work with limits and structured outcomes. Supply both controlled job processing and an execute-and-drain convenience operation. | Choose when to process jobs; the runner uses the convenience operation. |
| Inspect the outcome | Return structured status and diagnostics, including source location and stack trace when available. | Choose where to present diagnostics and how to map the outcome to application behaviour or exit status. |
| Destroy the runtime | Release script objects and queued work, and release registered capability resources according to the documented ownership and teardown order. | End use of the runtime and honour handle and host-context lifetime rules. |

The runner's convenience operation executes the initial script and finishes
its queued jobs under the configured limits. It does not turn an unresolved
Promise into an operating-system event source. R0 must specify the outcome
when no work is runnable, how unhandled rejections are reported, and what
happens to queued work after a failure.

Controlled job processing leaves scheduling policy with an embedding
application, while job execution remains library code. A future browser can
choose its microtask checkpoints without implementing another Promise pump.
The standalone convenience operation is not a browser event-loop contract.

### Capability boundary

An application deliberately supplies a capability; a script cannot grant
itself new native access through the registration mechanism. QuickJS's API
provides argument inspection, result construction, exception creation, and
association with native state. Creation installs no host capabilities; the
language's standard built-ins are distinct from host access.

The runner installs the library's output binding, such as `console.log`, with
a selected destination. Another application can register its own native
function without changing the engine or exposing the runner's facilities.
Browser objects are later bindings, not prerequisites of this mechanism.

Binding values and strings follow the pinned QuickJS ownership contract.
R0 documents ownership of the context accessor and library-owned diagnostics
and installers. Each custom binding documents its native-state ownership,
registration failure cleanup, and finalization/GC marking obligations. Native
bindings are trusted C code: raw engine access can bypass wrapper checks, so
this interface does not isolate a faulty or hostile native extension.

The first extensibility demonstration is a small test executable registering
a native function, invoking it from JavaScript, and checking both its result
and its error path. It uses the runtime API plus published `quickjs.h`. This
proves the registration boundary before the runner or browser relies on it.

### Outcomes and diagnostics

Distinguish success, script exception, exceeded limit, cancellation, and host
failure. Identify the exhausted resource where that information is available.
The header must define how outcomes and diagnostic storage are obtained and
released, including a usable fallback when allocation prevents detailed error
formatting. Source names, locations, and stack traces are diagnostic data, not
strings that a consumer must parse to determine success or failure.

A recoverable execution or host failure returns control to the caller. Library
reporting of those failures must not call `exit()` or terminate the application.
The runner maps the result to process status; another application chooses its
own response. Whether a runtime remains reusable after each failure class is
an explicit R0 contract and test requirement. R0 keeps ordinary script
exceptions and unhandled rejections reusable, with already-queued jobs retained
and the active turn's budget unchanged. Limits, cancellation, and host failures
retire the runtime. The runner stops and destroys on a non-success outcome.

### Engine invariant failures

Engine assertions and `abort()` indicate an engine or native-binding invariant
failure, distinct from a script exception, allocation failure, or cancellation.
The initial port must diagnose these and terminate the host process through
a defined fatal path; R0 specifies its diagnostic and exit status. Neither
consumer is promised recovery by catching an arbitrary abort. The runner
reports the fatal status through normal process supervision; the initial
in-process Yonder integration can also terminate on such a defect.

A deliberately leaked native-held value is a J2 fixture, run in a separate
test process. QuickJS 2026-06-04 asserts during `JS_FreeRuntime` when objects
remain. Destruction has already released resources and run GC by that point.
Returning from the assertion or escaping it does not establish safe recovery.

Browser survival after a specifically diagnosed teardown leak remains an
open design item. Any recovery implementation must identify a safe detection
point, disable the failed runtime and outstanding handles, account for native
resources and finalizers, and bound abandoned memory across the process's
lifetime. A per-runtime limit alone does not bound repeated abandoned runtimes.
Keep fatal handling unless that narrower recovery contract is implemented
and tested; this is not a prerequisite for the standalone runner.

### Ownership and execution

A runtime has an explicit owning thread. Evaluation, job execution, bindings,
and teardown are serialized on that owner. A network worker returns a result
through a queue; it does not call into the runtime concurrently. QuickJS does
not support concurrent execution within one runtime.

Class registration also needs a policy: the audited Atomics-disabled build
omits a mutex around process-global class ID allocation. Multiple runtimes
must not make that shared operation race. R0 serializes the check-and-allocation
of a binding's process-lifetime class-ID slot; it reuses that ID across runtimes rather than allocating per navigation.
Register the ID in each runtime separately.

The initial API prohibits native callbacks from starting another top-level
evaluation or draining jobs in that same runtime. Enforce this at the os64
evaluation/drain entry points; binding authors must not bypass it through raw
QuickJS calls. Ordinary property access and value conversion can themselves
invoke JavaScript, so these operations must retain the active budgets and
valid host state. Runtime destruction occurs outside active evaluations and
callbacks. Bindings follow QuickJS's reference counting and cycle tracing,
including its prohibition on executing JavaScript from finalizers.

Bindings must use the R0 class-registration synchronization policy even though
the raw engine function is exported. The context accessor does not grant a
second thread permission to use the runtime or replace its allocator,
interrupt handler, or other library-owned configuration.

### Resource limits

Each runtime has configured memory, stack, and execution limits. Allocator
callbacks must maintain the accounting required by the pinned QuickJS version;
a call to `JS_SetMemoryLimit` alone does not validate a custom allocator.
Browser-owned DOM and resource allocations need budgets in addition to the
engine's heap budget.

Chris chose a public allocation-size query in libos64 on 2026-10-01, as
recorded in Fable's review reply. Add it as a separately reviewed R1
prerequisite and use it for QuickJS allocation accounting; do not duplicate
size metadata in a private adapter header or inspect libos64's private header
directly. Specify the query's payload-size semantics and valid-pointer contract,
plus the adapter's charged bytes, overflow checks, and realloc failure handling.
The size query supports accounting; the adapter still enforces the memory limit.

QuickJS's default stack allowance is 1 MiB. The inspected os64 user-thread
stack is also 1 MiB (`kernel/include/thread.h`). Configure a smaller engine
allowance with room for the embedding code and native calls, and test it on
the thread that actually evaluates scripts. The host probe's 256 KiB setting
was an experiment. The standalone production profile now uses 256 KiB,
published by `os64_js_default_limits()` with 64 MiB memory, 4 MiB source,
60,000 ms execution and `UINT64_MAX` jobs. The runner shares that profile and
keeps its 768 KiB stack-option cap. A lower job count provides an explicit
deterministic cap; the default deadline bounds ordinary Promise work.

J2 records recursion depth and sampled stack use for representative frames and
a bounded native callback; report the workload rather than claiming a universal
stack cost per frame. Browser validation adds script-to-layout calls on that same stack.
Configurable thread stacks are deferred in DEBTS.md, triggered by measured
insufficient headroom; no kernel API change is authorized by this plan.

Use the interrupt hook with a monotonic deadline and cancellation state.
Interruption aborts execution; it is not a resumable time slice. Native host
operations need their own bounded or cancellable behaviour. Audit cancellation
coverage in compilation and regular-expression work rather than assuming that
an interrupted infinite bytecode loop proves all paths are bounded.

Pending Promise jobs must be processed explicitly. Bound runaway chains and
define what happens when the budget is exhausted. A browser integration must
respect microtask checkpoints; arbitrarily interleaving GUI events between
Promise callbacks is not a substitute for that scheduling contract.

### Initial host capabilities

The first runner evaluates a classic script from a bounded file or expression,
provides library-owned output and argument bindings, reports exceptions,
processes Promise jobs, and terminates with a defined status. Module loading,
timers, and interactive input are deferred features.

Host bindings are selected when configuring the runtime. Reading the runner's
input file does not imply giving scripts unrestricted file access. The browser
profile does not acquire command-line host facilities by accident. Shared
buffers and workers should not be exposed in the first profile without their
ownership and synchronization contracts.

Capability installation is available with the first library release. The
library's output and argument bindings use the same published QuickJS binding
surface as custom bindings. Test a custom capability as well as the absence of
unregistered ones; the runner calls installers rather than implementing them.

## Browser integration requirements

### Mutable document ownership

Use libhtml's tree as the shared mutable document. Add library mutation verbs
for node creation, insertion/removal, attributes, text, and context-sensitive
fragment parsing. The public structs remain read-only views for consumers;
public mutations go through the verbs so invalidation has a defined entry
point. Parser-internal changes must participate in the publication/invalidation
contract when the partially parsed tree is exposed to consumers.

Preserve node identity when unrelated content changes. A node removed from the
visible document may still be referenced by JavaScript or later reinserted.
Replacing the document with a fresh parse after each edit would invalidate
existing pointers and lose that identity.

Nodes initially live until document destruction, including detached nodes.
Charge them to the document budget; exhaustion reaches script as an exception.
Reclamation of unreachable detached nodes is deferred in DEBTS.md, triggered
by legitimate page workloads exhausting that budget through node churn.

Replaced text and attributes retain storage while consumers can borrow it.
Retired storage may be reclaimed after replacement model/layout publication
and release by other borrowers; a layout swap by itself proves neither.
libhtml's current permanent arenas mix stable nodes and strings, so DOM.md
must specify how replaceable storage becomes independently reclaimable.
Failed rebuilds must retain the data borrowed by the old presentation and
define which interactions remain valid while presentation lags the tree.

Fable owns the proposed DOM.md design packet. It specifies document and wrapper
lifetimes, retired-storage accounting, mutation failure atomicity, invalidation,
and the parser/event-loop handoff before J3. The bindings must prevent a live
wrapper from accessing a freed document. This does not make libpage a mutation
engine; `os64_page_rebuild` consumes the changed tree and preserves live edits.

### Page model and rendering

Implement model rebuilding or a defined mutation update path that preserves
surviving control state. Attribute defaults and live form properties have
different semantics; resetting or changing a value must use the shared form
rules rather than a second implementation in JavaScript bindings.

Changes to text, attributes, tree structure, styles, and resource URLs must
invalidate the appropriate model, cascade, layout, and resource state. Batch
work where permitted. Geometry reads may require pending layout to be resolved
synchronously. Define focus, selection, scroll anchoring, and hit-test behaviour
when nodes disappear.

### Parsing and script execution

Yonder currently performs fetch, parse, and model construction on a worker,
then transfers the completed page to the UI for layout. Parser-blocking scripts
need access to the partially constructed document. Define how parsing pauses,
script execution runs on the owner, and parsing resumes without two threads
mutating the document.

The design starting point is script on the UI thread, with the parser worker
posting a script request and parking through Yonder's mailbox/doorbell pattern.
DOM.md must validate exclusive document access, navigation generations,
cancellable waits, partial-tree ownership, and synchronous layout before
adopting that mechanism. These are design proposals, not implemented parser
capabilities.

Add the appropriate scripting-enabled parser behaviour, including `noscript`,
while retaining a scripting-disabled mode. Define classic script ordering,
external script fetching, async and defer handling, lifecycle events, and later
module execution. Fragment parsing is needed for APIs such as `innerHTML` and
must follow context-sensitive parsing rules. `document.write` and parser
re-entry require an explicit compatibility decision.

An early fixture may deliberately execute a script against a finished document
to prove mutation and redraw. Label it as an integration milestone; it does
not establish browser-compatible script execution order.

### Events and asynchronous work

Implement DOM event targets, listener ownership, propagation, cancellation,
and default actions. Connect GUI input, timers, network completion, and Promise
jobs through a defined browser event loop. QuickJS's pending-job API does not
supply these browser semantics.

Extend the existing generation and cancellation mechanisms so late results
cannot enter a discarded runtime. Specify teardown ordering for pending jobs,
callbacks, timers, requests, wrappers, document data, and rendered data.

### Browser access rules

Script-visible networking and cookies require origin and credential rules,
CORS where applicable, and script-aware cookie access. The current jar records
HttpOnly and SameSite attributes, but its request-header API is not a
`document.cookie` implementation. Browser transport reuse does not establish
permission for a script to read a response.

Storage, frames, workers, internationalization, and broader web APIs are
separate compatibility work. QuickJS's ECMAScript support does not imply that
an arbitrary modern website can run in Yonder.

## Delivery and validation

| Milestone | Deliverable | Evidence required before proceeding |
| --- | --- | --- |
| J0 | Pinned source, maths selection, dependency inventory, and reviewed runtime boundary. | Target compiler and ABI requirements identified; unresolved design choices recorded. |
| J1 | Engine library and thin runner built for os64. | Strict target build and link; expected shared dependencies; script execution and exception output inside QEMU. |
| J2 | Runtime lifecycle and failure handling established. | Guest tests for repeated create/destroy, GC cycles, allocation failures, memory limits, measured recursion/native-callback stack use, cancellation, Promise chains, dates, numeric edge cases, and floating-point behaviour across scheduling. A deliberately leaked value verifies the fatal teardown contract in a separate process. Selected upstream tests with explicit pass/fail/skip reporting. |
| J3 | Reviewed mutable-document design and first Yonder integration fixture. | Script changes visible text; layout updates; references and form edits survive relevant changes; navigation during pending work tears down safely. |
| J4 | Browser execution, event, and resource slices. | Relevant web-platform tests and guest fixtures for each implemented contract, including script order, parser mode, timers, events, origins, and cookie access. |

The DOM design can be reviewed during J0 through J2; J3 depends on it. Broader
website compatibility is measured separately from language-engine conformance.
Production defaults and budgets should follow guest measurements, not the
host probe's convenient values.

Use focused host tests for pure adapters and library logic, the repository's
strict build and applicable checks for implementation changes, and QEMU for
runtime claims. P5 validation can follow the controlled guest milestone. A
successful Linux build is not evidence that os64's allocator, clock adaptation,
loader, or scheduling behaviour is correct.

## Research evidence and limits

The research probe compiled the five engine source files with Atomics disabled,
using host C headers and these material flags:

```text
-O2 -fPIC -ffreestanding -fno-builtin -fno-stack-protector
-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D_GNU_SOURCE
-DCONFIG_VERSION="2026-06-04"
```

The objects were combined with `ld -r` and inspected with `nm -u`. A small
host executable linked them with host libc and `-lm`, without `quickjs-libc.c`
or explicit pthread linkage. Its checks passed for arithmetic, BigInt,
regular expressions, JSON, UTC date conversion, a current-date call, a queued
Promise callback, and interruption of an infinite loop. The configured runtime
limits were 16 MiB of memory and 256 KiB of stack. The Promise check executed
one pending job; the interrupt test stopped after ten hook polls.

This demonstrates a viable engine-only host configuration. It does not test
memory exhaustion, prove leak freedom, validate timezone edge cases, run
Test262, or establish a cross-linked os64 binary. The temporary research files
are not a durable regression suite; J1 and J2 must add maintained tests using
the actual port.

A follow-up host probe on 2026-10-01 created an object, deliberately retained
its native reference, freed the context, and destroyed the runtime. It exited
through SIGABRT at `JS_FreeRuntime`'s `gc_obj_list` assertion. This confirms
the teardown hazard, not a recovery mechanism or an os64 fatal-path test.

## Decisions to resolve before implementation

- Library and runner names, exact public C declarations, ABI/configuration
  compatibility checks, and host-context/diagnostic ownership. Native values
  use QuickJS's published ownership rules within the six-operation design.
  The reusable file-loading and output helpers belong to the library.
- Confirm the engineering interface in MATH.md between the maths and runtime
  owners; preserve the source pin and notices in the maintained port.
- Exact runtime profiles, source limits, numeric execution budgets, and status
  codes within the agreed capability and failure policy, including runtime
  reuse after failure and rejection of prohibited callback re-entry.
- Scope of the first maintained upstream test subset and declared feature
  exclusions, including Atomics and module loading.
- Mutable libhtml verbs, document/wrapper lifetime, reclaimable retired storage,
  and interaction with borrowed page and layout data in Fable's DOM.md packet.
- Browser execution thread, parser handoff, re-entry rules, and initial web API
  scope, plus whether safe teardown-leak containment can be provided. Any
  requirement for new kernel primitives needs separate evidence.

The standalone work does not wait for the last two browser design items.
JAVASCRIPT_TASKS.md separates the work that can proceed independently from
the integration dependencies.

## References

- [QuickJS project](https://bellard.org/quickjs/) and
  [embedding documentation](https://bellard.org/quickjs/quickjs.html).
- [DOM Standard](https://dom.spec.whatwg.org/).
- [HTML event loops](https://html.spec.whatwg.org/multipage/webappapis.html#event-loops)
  and [HTML parsing](https://html.spec.whatwg.org/multipage/parsing.html).
- [libhtml contract](../../../LIBHTML.md),
  [libpage contract](../../../LIBPAGE.md),
  [Yonder design](../completed/YONDER.md), and
  [layout design](../completed/LAYOUT.md).
