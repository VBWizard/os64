# JavaScript runtime API contract

Status: reviewed R0 contract with the two R2 implementation slices, 2026-10-02.
The explicit `js-library` target exports eleven embedding operations: create,
context access, class-ID allocation, eval, run, file execution, job draining,
output/argument setup, cancellation and destruction. Their review/merge state
and Opus's C1 integration are tracked in JAVASCRIPT_TASKS.md. The default image
does not install libjs; `js-runtime-test` builds an optional guest consumer.

## Creation and ownership

`os64_js_create` takes a configuration, `OS64_JS_ABI_ID`, an output runtime
pointer, and a caller-owned outcome. These pointers must be valid. It clears
`*out` before attempting creation and returns an independent runtime on success.
Input configuration, ABI, source/name/path and argument buffers must not overlap
output runtime-pointer or outcome storage. A null configuration, zero limits, or
unrepresentable deadline is a bad
argument. There is no unlimited sentinel; zero is rejected. Explicit development budgets appear
in the examples; production defaults wait for J2 guest measurements and can be
introduced through a configuration helper without changing this structure.

The ABI identifier covers the engine release, os64 wrapper revision, LP64
layout, 16-byte JSValue, 64-bit limbs, and Atomics configuration. Creation and
the context accessor compare each calling unit's identifier before exposing a
context. A binding built separately from the host must pass its own compiled-in
identifier, not one supplied by the host. Publish matching os64 and QuickJS
headers as one port profile. Relevant public configuration changes
require a new identifier. This detects accidental header/library disagreement;
it does not validate or isolate native code.

Creation installs language built-ins, including Promise and Date, but no host
output, arguments, file, network, process, worker, Atomics, or shared-buffer
capabilities. Disabling Atomics alone leaves shared-buffer exposure in the core.
This wrapper deletes the shared-buffer global
before publishing its context. No filesystem module loader is installed.

Creation binds the runtime to the creating thread. The same thread owns setup,
evaluation, jobs, diagnostic extraction, and destruction. This is a caller
obligation: os64 does not expose a current-thread identifier for a runtime to
compare. Calls on one runtime must not overlap. Wrapper evaluation/drain/setup
entry points reject active-call re-entry with BUSY without replacing the outer
call's result or budget. Native bindings are responsible for obeying the rule
when using the raw engine API. Independent runtimes may execute on different
threads; class IDs use `os64_js_class_id(&slot)`. The library serializes the
slot check and engine allocation together process-wide: a zero-initialized,
process-lifetime slot receives an ID once; later calls reuse it. Bindings access
shared slots through this function rather than reading/writing them separately.
NULL returns the invalid ID zero without entering the engine. The caller
registers the returned ID in each runtime with QuickJS's class API; allocation
and per-runtime registration are different operations.

`os64_js_context(runtime, OS64_JS_ABI_ID, outcome)` borrows the owned context.
It is available for idle setup and inside native callbacks executing in an
evaluation/job call; it is unavailable during destruction. Its caller-owned
outcome is required and must be separate from an active outer call's outcome.
It returns NULL with ABI_MISMATCH on header disagreement, BAD_ARGUMENT for a
null runtime/ABI argument, or FAILED_RUNTIME for a failed runtime, without
changing the runtime or active budgets. On success it initializes the outcome
with OK. Each translation unit using engine inline helpers checks through this
accessor before using the context. Consumers must
not free it, create additional contexts, migrate its runtime, or replace the
library's allocator, interrupt hook, Promise tracker, or loader configuration.
The runtime and context opaque slots both hold wrapper state. Custom bindings
must leave them unchanged and keep native state in their own objects and
function data. A raw engine runtime constructed outside this wrapper has its
ordinary engine configuration, including shared-buffer exposure; this contract
governs the wrapper-owned context.

## Capabilities and native bindings

`os64_js_install_output` borrows an already-open output handle and installs
only the names selected by its bitmask: OS64_JS_OUTPUT_PRINT grants `print`,
OS64_JS_OUTPUT_CONSOLE_LOG grants `console.log`. Zero or unknown bits return
BAD_ARGUMENT without installing anything. The runner selects both; a browser
can select only logging and retain ownership of the web's `print` function.
Unselected names are untouched. Selected properties and functions are staged
before publication. A reusable setup exception restores original data/accessor
descriptors and removes newly published names; the handle is borrowed only
after the complete installation commits. A staged callback cannot write before
that commit.
If a host property trap or allocation failure prevents restoration, the runtime
is retired instead of returning a reusable partial installation. Unrelated
side effects of host-supplied traps are outside the property rollback.
The host keeps a successfully installed handle valid until destruction;
the library does not close it. Values convert with QuickJS string conversion,
spaces separate arguments, and one newline terminates a call. Conversion may
execute script and remains subject to the active execution budget. A conversion
exception is a script exception. A write failure is HOST_FAILURE with its os64
error code. Partial writes are retried; zero progress is HOST_FAILURE with
`host_error` zero because no service code is available. Embedded NUL bytes in
converted values are written by length. Earlier argument bytes may already be
written when a later conversion/write fails; output is not atomic.
A blocking handle operation has the handle's ordinary blocking
semantics; the engine interrupt does not cancel a blocked native write. Hosts
needing bounded output must choose an appropriate handle or custom binding.

`os64_js_install_args` copies UTF-8 argument strings into `scriptArgs`; count
zero permits a null argument array. Other null entries are invalid. The caller
may release its array/strings when installation returns. The runner supplies
the filename as argument zero; the library does not invent it. Installers run
while idle, before the first evaluation, and are each installed once. Calling
one again, or attempting setup after evaluation starts, returns BAD_ARGUMENT.
The count must fit the engine array's uint32 index range. Both installers use
the runtime's memory budget and hold the re-entry guard. A setup operation has
its own execution budget, including property traps and exception diagnostics;
it does not mark the first source evaluation as started. If host-supplied traps
queue jobs, their setup turn must drain before other setup or source execution.
For console-only setup, an existing own data object is retained and its other
members are untouched. An accessor or non-object console is a setup exception;
the installer does not invoke a global console getter to discover the object.

Custom bindings use QuickJS values and lifetime rules directly. `examples/binding.c`
shows argument conversion, a thrown callback exception, and ownership on
property-install failure. Custom setup failure can leave partially installed objects;
the host destroys that runtime and must not proceed to evaluation. Engine
exceptions from custom setup belong to the host until consumed/freed through
QuickJS. The wrapper does not take ownership of arbitrary native host context.
Bindings define their own finalizers and GC marking, including cleanup of
native resources. Finalizers must not execute JavaScript.

## Source and job turns

`eval` executes a classic source buffer and discards its completion value after
checking for an exception. Callers needing script-visible results can expose a
binding or read a global with QuickJS. `run` evaluates then drains runnable jobs
through the same library machinery. Modules and top-level await are excluded.
The source pointer can be null only for length zero. The library copies bounded
source bytes and supplies QuickJS's required trailing NUL; source names are
borrowed during the call and copied/truncated for diagnostics. The source-size
limit is checked before copying, and retained source storage is included in
runtime memory accounting.

`run_file` loads a bounded file, closes its owned input handle, and uses `run`.
Reading the file grants no script-visible file capability. File read/close
errors are HOST_FAILURE, preserving the first failure's code; any nonzero close
verdict is an error. After a successful open, read/allocation/size/cancellation
failure still closes the owned input. Empty and exact-limit files are accepted;
one byte beyond the source ceiling produces LIMIT/SOURCE. The growing input
buffer is charged to the engine allocator and passed directly to evaluation,
without a second source copy. BUSY or invalid-path refusal does not open a file.
Non-file handles are outside this helper's contract.
The execution deadline starts immediately before compilation/evaluation, after
file loading. Input size and memory cap bound loading; the initial helper does
not promise an I/O deadline. Native bindings likewise supply their own bounds.

One turn starts at evaluation and ends when its runnable job queue is empty.
It has one monotonic execution deadline and one total executed-job cap. Time
spent between manual job slices counts against that deadline; slicing does not
refresh either budget. `eval` can return OK with `jobs_pending` true.
`drain_jobs` executes up to positive `slice_jobs`, returning MORE_JOBS if the
queue remains nonempty. `run` drains until the queue empties or a non-success
outcome occurs.
No new top-level evaluation is accepted while a turn has runnable jobs; return
BUSY and preserve the turn. Reaching a slice boundary with an empty queue is OK;
if another job is runnable after the turn's total cap, the result is LIMIT/JOBS.

An empty queue does not establish that all promises resolved. Unresolved
promises without runnable work do not keep the runner alive or produce host
work. A script that creates one can complete successfully. The API reports
runnable pending jobs, not a count of every unresolved Promise.

The rejection tracker retains rejected/unhandled promises until they acquire
handlers or the turn reaches a checkpoint. Intermediate MORE_JOBS outcomes do
not report an unhandled rejection that a later job in the turn could handle.
At queue exhaustion, remaining unhandled rejections produce
UNHANDLED_REJECTION with a diagnostic for the first observed one. Their retained
values and bookkeeping count against the memory budget. At that checkpoint the
wrapper releases the reported bookkeeping and retained values; those rejections
are not reported again on the next turn. Engine notification of a later handler
for an already-reported rejection is harmless. If an exception is the primary
outcome at the same queue exhaustion, EXCEPTION takes precedence and checkpoint
bookkeeping is still released. The standalone outcome carries one diagnostic;
per-rejection browser events are later checkpoint work. Browser task/checkpoint
policy requires the extensions listed below; the standalone profile does not
implement the HTML event loop.

## Results, limits, and reuse

Outcomes use caller-owned fixed-capacity diagnostic arrays. Each operation
initializes its outcome, with NUL-terminated fields, even when formatting fails.
Truncation sets `diagnostic_truncated`; unavailable locations are zero. The
structured status and limit identify failure without parsing text.
`jobs_executed` is cumulative for the current turn, not just the last slice. Diagnostics
may use engine conversions, which can fail or invoke script; the original turn
budget and entry guard remain active during exception extraction, with a fixed
fallback when conversion fails. Rejection checkpoint diagnostics avoid executing
reason objects: primitive conversion or real Error own string data is used;
accessors, proxies and other objects retain the fallback. Locations remain zero
because the retained API does not expose structured line/column data; no error
message is parsed. Returned status equals the
outcome's status. There is no release operation for an outcome.

OK and MORE_JOBS keep the runtime usable. BAD_ARGUMENT and BUSY reject the
operation without changing its runtime. ABI_MISMATCH publishes no runtime at
creation and no context at access; an existing runtime stays unchanged.

EXCEPTION and UNHANDLED_REJECTION report script outcomes and keep the runtime
usable. An exception does not resume the failed script or job. Jobs already
queued remain queued: `jobs_pending` reports them, and the host drains them or
destroys the runtime. If jobs remain, the current turn and its original deadline
and executed-job count remain active; new top-level evaluation stays BUSY until
the queue empties. An exception with no runnable jobs ends the turn after the
outcome is captured. Capture the original exception before any later execution
can replace it. UNHANDLED_REJECTION is reported at queue exhaustion and ends that
turn. The runner stops on either outcome and destroys its runtime; other hosts
may continue with the documented draining rule.

LIMIT, CANCELLED, and HOST_FAILURE make the runtime failed: no further script or
jobs run, and pending work is discarded at destruction. An already-failed runtime
returns FAILED_RUNTIME. A browser can continue showing the document without its
script environment. Sticky limit/cancellation/host-failure signals take precedence
over an engine exception sentinel so they cannot become ordinary reusable errors.

Limit classification must be reliable when the wrapper detects it: SOURCE,
MEMORY, EXECUTION, and JOBS are wrapper-owned counters/flags. STACK is reported
only when the engine exposes a reliable signal; do not recognize it by matching
exception text. Otherwise stack overflow remains EXCEPTION. Memory accounting
uses `os64_malloc_size` plus documented metadata charges; realloc failures keep
old allocation/accounting intact. The wrapper reserves its rounded allocation
capacity before constructing the engine; its allocator ceiling is the remainder
of the configured memory limit. Engine arenas, source copies and rejection
bookkeeping are charged through the same allocator. Native binding allocations
outside the engine require their host's own budget. DOM allocations have their
separate budget. `host_error` preserves negative monotonic-clock service codes;
file and output operations preserve their service verdicts too;
allocation refusal has no service error code and reports zero rather than an
invented errno. The target adapter's fatal wall-clock invariant remains distinct
from recoverable monotonic-clock failure.

The stack budget must fit the caller's native thread stack, leaving room for
native callbacks and host frames. Creation rejects zero, values above PTRDIFF_MAX,
and values above half its local stack address to keep engine subtraction away
from unsigned underflow. This representability guard does not discover the
thread's actual stack extent. The fixture's 256 KiB engine budget fits os64's
1 MiB native thread stack; J2 must measure room for the runner's own calls.

`os64_js_cancel` atomically latches cancellation and is the cross-thread
operation. The owner observes it before execution/job entry and through the
interrupt hook. Cancellation is sticky, with no reset/reuse operation. The
caller keeps the runtime alive until cancellation callers have finished. It is
not a signal-handler API and does not interrupt arbitrary blocking native code.

Destroy runs on the owner outside active callbacks, frees queued values and
contexts, and runs native finalizers before releasing the engine. NULL is a
no-op. The wrapper holds its entry guard and closes context access before
running finalizers. The host must end access to its native state after finalization, not
before. A leaked value or engine invariant violation diagnoses and exits the
host with `OS64_JS_FATAL_EXIT`; it is not an outcome. Teardown-leak containment
for Yonder remains D0 work under the conditions in JAVASCRIPT.md.

## Browser extensions reserved for later work

DOM.md's event-loop and teardown design asks for three reviewed extensions,
outside R0 implementation:

- Start a budgeted turn by calling a function value (timer or listener), with
  the same outcomes and ownership rules. A raw JS_Call outside a wrapper turn
  does not arm its deadline and is not a browser dispatch mechanism.
- Drain to a microtask checkpoint inside a host task, including between
  listeners. The task's deadline and total job cap continue across checkpoints;
  each checkpoint judges unhandled rejections without opening a fresh budget.
  R2 must separate budget lifetime from queue exhaustion so this can be added.
- Select a known-teardown-leak reporting/reclamation policy at creation. This
  requires a reviewed detection patch, dead runtime/handle guarantees, native
  resource/finalizer rules, allocator-ledger and global-reference audits, and
  repeated-leak tests proving no cumulative growth. R0 destruction stays fatal
  until that narrower contract is implemented and validated; unrelated engine
  invariant failures remain fatal. An outcome cannot make unsafe cleanup safe.

## Review and implementation gates

Fable's first review approved the interface shape and continuous budget across
manual slices. The reviewed interface includes reusable script exceptions,
serialized class-ID slots, per-binding ABI checks and selected output names.
R0's header and examples have syntax checks; R2's maintained host/guest fixtures
provide implementation evidence in VALIDATION.md.
R1 supplies target C adaptation and compatibility-header publication without
bringing host libc into the exported binding header. R2 implements and tests
the lifecycle, limits, installers, error fallback, job tracking, cancellation,
header mismatch, custom callbacks, and fatal teardown contracts. J2 selects
production defaults using guest evidence. M1 supplies the maths library.
