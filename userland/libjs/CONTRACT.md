# JavaScript runtime API contract

Status: R0/R2 contract with D7a's host tasks, D8's opt-in teardown and D10's
callback budget extensions, 2026-10-06.
`libjs.so` exports the embedding operations `js.h` and `js_engine.h` declare:
create (fatal or reporting teardown), context access, class-ID allocation, eval,
run, file execution, job draining, output/argument setup, cancellation, callback
budget checking, destruction, and the host-task entries (§ Host tasks). Their review/merge state
and Opus's C1 integration are tracked in JAVASCRIPT_TASKS.md. The default image
installs the runner, library, dependencies and QuickJS licence;
`js-runtime-test` builds an optional guest consumer.

## Creation and ownership

`os64_js_create` takes a configuration, `OS64_JS_ABI_ID`, an output runtime
pointer, and a caller-owned outcome. These pointers must be valid. It clears
`*out` before attempting creation and returns an independent runtime on success.
Input configuration, ABI, source/name/path and argument buffers must not overlap
output runtime-pointer or outcome storage. A null configuration, zero limits, or
unrepresentable deadline is a bad
argument. There is no unlimited sentinel; zero is rejected.
`os64_js_default_limits()` supplies the standalone profile: 64 MiB engine
memory, 256 KiB engine stack, 4 MiB source, 60,000 ms per turn and `UINT64_MAX`
jobs. This header helper initializes values; it grants no capabilities and
does not make a null configuration valid. Hosts may override individual fields.
The runner uses this profile; the job-count option supplies a practical
deterministic cap when wanted, while the default execution deadline bounds
ordinary Promise work. Native operations need their own bounded behavior.
The helper changes no struct layout, engine profile or exported symbol, so the
embedding ABI identifier remains unchanged. J2's guest measurements and the
profile's limits are recorded in VALIDATION.md.

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
OS64_JS_OUTPUT_CONSOLE grants the WHATWG Console Standard's `console`
namespace. Its levels (`log`, `info`, `debug`, `warn`, `error`, `trace`,
`dir`, `dirxml`, `table`) all print as `log` does, to the one handle;
`assert` prints `Assertion failed` (with its message) when its condition is
falsy; `group`/`groupCollapsed` print their label and indent later console
lines two spaces per level, `groupEnd` closes one and `clear` closes all;
`count`/`countReset` and `time`/`timeLog`/`timeEnd` keep per-label counts and
start times (microseconds, printed as milliseconds), and print the
standard's warnings for a missing or repeated label. Zero or unknown bits return
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
For console-only setup, an existing own data object is retained and its
members outside the namespace are untouched. An accessor or non-object console is a setup exception;
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
without a second source copy by the wrapper. At buffer boundaries, a one-byte
probe checks for EOF before growth; confirmed extra input is retained after
expansion. BUSY or invalid-path refusal does not open a file.
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
per-rejection browser events are later checkpoint work. A browser's tasks and
checkpoints use § Host tasks; the standalone profile does not implement the
HTML event loop.

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
of the configured memory limit. Engine arenas, source copies, rejection
bookkeeping and opt-in teardown ledger headers are charged through the same allocator. Native binding allocations
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
thread's actual stack extent. J2 measures the standalone 256 KiB profile and
the runner's 768 KiB option cap on os64's 1 MiB mapped thread stacks, including
a bounded 64 KiB native frame. These sampled workloads establish their own
headroom, not a guarantee for arbitrary native callbacks. Browser bindings
must measure their script-to-layout paths separately.

`os64_js_cancel` atomically latches cancellation and is the cross-thread
operation. The owner observes it before execution/job entry and through the
interrupt hook. Cancellation is sticky, with no reset/reuse operation. The
caller keeps the runtime alive until cancellation callers have finished. It is
not a signal-handler API and does not interrupt arbitrary blocking native code.

Destroy runs on the owner outside active callbacks, frees queued values and
contexts, and runs eligible native finalizers before releasing the engine.
NULL is a no-op. The wrapper holds its entry guard and closes context access
before running finalizers. Native state remains alive through destruction.
`os64_js_create` selects fatal teardown: a leaked object/weak reference or
engine invariant diagnoses and exits with `OS64_JS_FATAL_EXIT`.

### Opt-in reporting destruction

`os64_js_create_with_teardown` has the same creation arguments and outcomes,
plus `OS64_JS_TEARDOWN_FATAL` or `OS64_JS_TEARDOWN_RECLAIM`. Unknown policies
return BAD_ARGUMENT and publish no handle. This additive entry does not change
the existing config, outcome, limits or binding ABI layouts.

RECLAIM is restricted to audited hosts: C-held values are in registries drained
before destruction; native cleanup does not depend on an engine finalizer; no
process-global reference retains a runtime handle; cross-thread cancellation
users have stopped. External ArrayBuffer backing stores, custom finalizers
owning host allocations/handles and shared-buffer callbacks violate this
profile unless their host releases those resources independently. Built-in
engine allocations belong to the runtime's allocator ledger.

`os64_js_destroy_report(runtime, report)` consumes the runtime under its selected
policy and writes an allocation-free report. A NULL report discards it;
`os64_js_destroy` calls this entry with NULL. A NULL runtime clears a supplied
report. A clean tracked result has `leaked == false` and zero reclaimed counts.
FATAL retains the object/weakref assertions and has no allocator ledger or
raw-buffer/string leak reporting; if it returns, its report is zero.
In RECLAIM mode the engine returns at its object/weakref assertions if objects
remain. The wrapper then frees the allocator ledger without engine entry or
additional finalizers. After clean engine destruction, remaining raw buffers
or ordinary strings are also detected and reclaimed through that ledger.
`leaked == true` is a binding bug, not successful reference ownership.
`reclaimed_blocks/bytes` describe storage freed by bulk reclamation, including
ledger metadata; they do not count leaked JavaScript values. Hosts log/count
leaks and fixtures fail on unexpected counts. Both entries invalidate every
context, value and buffer from that runtime, including leaked handles.

Ledger headers are aligned and charged to the memory budget. Fatal-profile
allocations retain their original header-free adapter. Reallocation preserves
links, payload and charge on refusal, with the existing temporary-copy budget
semantics. Other engine aborts, allocator corruption and destroy during active
calls remain fatal in both policies. Reclamation is not safe continuation of a
failed engine. Detection, allocation/global audits, native ownership and
acceptance evidence are recorded in [TEARDOWN.md](TEARDOWN.md).

## Host tasks

A browser runs script in TASKS (a timer's callback, the listeners of one
event), and a task is more than one entry into the engine: each listener is
a call, and HTML puts a microtask checkpoint after each. D7a builds the two
extensions DOM.md reserved for that, declared in `js_engine.h` because they
take engine values:

- `os64_js_task_begin(runtime, abi, name, outcome)` opens a task: it arms one
  execution deadline (`now + execution_ms`) and zeroes the job count. The
  name is the outcome's source name for everything the task reports.
- `os64_js_call(runtime, abi, function, this, argc, argv, returned_false,
  outcome)` calls a function value inside the open task, with eval's
  outcomes. The host owns the function, `this` and the arguments; the return
  value is discarded, except that a strict `false` is reported through
  `returned_false`.
- `os64_js_checkpoint(runtime, abi, outcome)` drains runnable jobs to
  exhaustion and judges unhandled rejections.
- `os64_js_task_end(runtime, abi, outcome)` is a final checkpoint, then closes
  the task.
- `os64_js_set_execution_ms(runtime, ms, outcome)` replaces the execution
  limit for tasks and turns started from then on.

The rules:

- **One task at a time.** `task_begin` is BUSY while a task is open, or while
  an earlier turn still has runnable jobs (eval's own rule). Inside an open
  task, eval, run, run_file, drain_jobs and the installers are BUSY: the
  task's checkpoints are what drain its jobs.
- **One budget per task.** The deadline and the job cap armed by
  `task_begin` hold across every call and checkpoint until `task_end`.
  Neither a call nor a checkpoint opens a fresh budget, and a checkpoint is
  not a turn's end: the task keeps its turn until it is closed.
- **Rejections are judged at checkpoints.** A call that leaves a rejected
  promise unhandled reports nothing; the next checkpoint (or `task_end`'s)
  reports UNHANDLED_REJECTION, with eval's diagnostic rules, and releases
  the bookkeeping once.
- **A checkpoint never stops early for an exception.** A Promise reaction
  that throws is a rejection, not a job exception; what a job can throw is
  a host job's own error. The first such exception is the outcome and the
  checkpoint drains the rest, so a task never ends with runnable jobs it
  could not run.
- **A sticky failure ends the task's work.** LIMIT, CANCELLED and
  HOST_FAILURE inside any entry make every later entry of the task answer
  FAILED_RUNTIME, except `task_end`, which closes the task and reports the
  sticky status. After that, as before, nothing is accepted but destroy.
- **Calls are not re-entrant.** `os64_js_call` and `task_begin` are refused
  from inside a callback, as eval is. A native binding that must run script
  synchronously under a call (a script's `dispatchEvent`) uses the raw
  engine call: it runs inside the task, so the interrupt hook bounds it by
  the task's deadline. A raw call OUTSIDE an open task or turn is not
  bounded by anything; that is what the task bracket exists to prevent.
- **A new limit applies to the next task.** `set_execution_ms` is refused
  during a call and accepted between them; the task that is open keeps the
  deadline it was armed with. Zero or an unrepresentable value is
  BAD_ARGUMENT, as creation refuses.

`os64_js_run` is unchanged: a script element is one eval-and-drain turn, the
same budget a one-call task would have.

`tools/test_js_runtime_host.c`'s task cases hold each rule: refusals outside
and inside a task, a throwing callback followed by a call that still runs,
`returned_false`, a rejection left for the checkpoint, a throwing host job,
the job cap spanning checkpoints, a nested raw call overrunning, every later
entry answering FAILED_RUNTIME, and a changed limit arming the next task
only.

## Reserved for later work

Nothing. The three extensions DOM.md reserved are built: the host tasks
above (D7a), the callback budget check (D10, § Native callback budget
checking) and the creation-selected reporting destruction (D8, § Opt-in
reporting destruction). Each was reviewed as its own slice.

## Review and implementation gates

Fable's first review approved the interface shape and continuous budget across
manual slices. The reviewed interface includes reusable script exceptions,
serialized class-ID slots, per-binding ABI checks and selected output names.
R0's header and examples have syntax checks; R2's maintained host/guest fixtures
provide implementation evidence in VALIDATION.md.
R1 supplies target C adaptation and compatibility-header publication without
bringing host libc into the exported binding header. R2 implements and tests
the lifecycle, limits, installers, error fallback, job tracking, cancellation,
header mismatch, custom callbacks, and fatal teardown contracts. J2 records
the standalone production profile's guest evidence. M1 supplies the maths library.

## Native callback budget checking

`os64_js_check_budget` accepts the runtime, caller ABI and a separate outcome.
It requires an active turn and observes its existing deadline, cancellation and
sticky failure state without entering JavaScript, draining jobs or restarting
the clock. Passing the outer active outcome is refused as BUSY without changing
it. Hosts can check before and after synchronous native work; elapsed native
work belongs to that turn, even when JavaScript catches the resulting exception.
A native operation still needs its own bounded behavior because this check
cannot interrupt it midway.

Yonder selects a 128 KiB engine stack for synchronous geometry callbacks. The
standalone default remains 256 KiB. Combined native/engine guest evidence and
the integration boundary are recorded in [DOM_D10.md](../../../docs/design/pending/DOM_D10.md).

### Explicit browser legacy arguments helper

The patched binding header exports `JS_GetLegacyFunctionArguments`. A browser
may install an accessor which calls it; standalone context construction leaves
QuickJS's throwing `Function.prototype.arguments` unchanged. The helper accepts
an ordinary non-strict function from the calling context, copies its innermost
active frame into an unmapped arguments object, and returns null if inactive.
Strict, arrow, native and other unsupported function kinds throw TypeError.
The snapshot owns its values and does not expose or alias stack storage. The
calling binding charges this synchronous work to its active task. D11's
consumer and regression evidence is in
[DOM_D11.md](../../../docs/design/pending/DOM_D11.md).

### Explicit browser global-miss handler

The patched binding header exports `JS_SetGlobalMissHandler(ctx, handler,
opaque)`. While a handler is installed, it hears each string name that a
lookup on the context's global object runs out of prototype chain for. The
lookup can end in either of the engine's two places that answer "not
found": the general property read, or the interpreter's inline field read.
A symbol or an integer index is not heard, and neither is an `in`,
`hasOwnProperty` or descriptor query, because none of those is a lookup.
The handler runs before the lookup answers, and the answer is the engine's
own (undefined, or the ReferenceError). The name is in the engine's stack
buffer, truncated to its atom-print size, so hearing it allocates nothing.
The handler must not enter the engine. A standalone context installs none.
libdom's `os64_dom_set_global_miss` is the browser's door to it, and
yonder's page file is the consumer
([YONDER_DIAGNOSTICS.md](../../../docs/design/pending/YONDER_DIAGNOSTICS.md)).

### Named "not a function"

Patch 0009 adds no export. When a call's method was fetched (`obj.m(...)`)
and found not callable, the call's TypeError reads `'m' is not a function`
instead of `not a function`. The context keeps one record: the name (a
duplicated atom), the frame, stack slot and value the method was pushed
as, and where the fetch's bytecode ends. A method call names it only when
it can prove it is that fetch's call: same frame, slot and value;
straight-line bytecode from the fetch to the call (no jump, call, return,
throw, handler, iteration, suspension or other method fetch between), so
the only way to the call is through the fetch; and the fetched value
never popped on the way (each op's pops and pushes followed, as
compute_stack_size does), sitting under exactly the call's arguments.
Anything else keeps the plain message: a call whose arguments call or
branch (`o.m(f())`), a bare call (`x()`), and every case Quinn found on
#239 (a skipped optional call, a failing or abandoned argument, a `with`
lookup, a private getter, an increment's fetch consumed before a `super`
or private call). The record is replaced by the next such fetch, dropped
at the call that reads it, and released at teardown.
