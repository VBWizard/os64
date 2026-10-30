# JavaScript runtime API contract

Status: R0/R2 contract with D7a's host tasks, D8's opt-in teardown and D10's
callback budget extensions, 2026-10-06, and P2a's script host, 2026-10-23.
`libjs.so` exports the embedding operations `js.h` and `js_engine.h` declare:
create (fatal or reporting teardown), context access, class-ID allocation, eval,
run, file execution, job draining, output/argument setup, cancellation, callback
budget checking, destruction, the host-task entries (§ Host tasks) and the
script host's (§ A script host). Their review/merge state
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

`run_bytecode` is `run` through an `os64_js_bytecode_t`, for a host that keeps
a classic script's compiled form (BYTECODE_CACHE.md § 4). Given a blob, it
reads and runs it; asked to keep, it hands back the blob of the compile it
made, `os64_malloc`'d and the caller's. A blob is libjs's framing round the
engine's bytes: a magic, the engine's build identity (`OS64_JS_ENGINE_ID`,
generated by `tools/js_prepare.py` from the prepared sources), the kind, and
the payload's length and FNV-1a digest. All are checked before the engine
reads a byte, because `JS_ReadObject` trusts its input; a blob that fails any
check, or that the engine refuses to read, is a miss, and the source, which
is always required, is compiled instead and its blob kept when asked. A read
that fails for the runtime's own reasons (its memory, its deadline) is the
runtime's failure, as the compile's would be. The blob is read under the
run's source name (patch 0015), so errors carry the name the host gives now.
Keeping never fails the script: a write the runtime's memory refuses keeps
nothing, and its refusal is taken back off the allocator's sticky flags (the
writer frees everything it took). The write is judged by the allocator
rather than by its answer: upstream's writer returns what it wrote whatever
a failed buffer growth or atom index left out of it, so a write during which
the allocator refused anything is no blob. The reader needs no such judge:
it answers every refused allocation with an exception (the runtime suite's
read sweep refuses each in turn). A compile
that throws keeps nothing; one whose run then fails keeps its blob, which
describes the source whatever became of the run. Reading and compiling are
both inside the deadline. A build with `OS64_JS_BYTECODE_ROUND_TRIP` (host
test builds only) runs every classic evaluation through a blob it writes and
reads back, so a suite proves the bytecode path with everything it runs.

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
a browser that fires an event per rejection sets a script host instead
(§ A script host), and then none of this paragraph's judging happens. A
browser's tasks and checkpoints use § Host tasks; the standalone profile does
not implement the HTML event loop. The tracker finds a rejection by its
promise in a table, so neither the thousandth rejection of a turn nor a
handler added to it costs more than the first.

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
- `os64_js_turn_open(runtime)` answers whether a job queued now would be
  drained by the turn under way: inside a call, an eval or a run, or while
  a task is open. A binding a host tells of a change between tasks (custom
  element reactions, told by the parser) asks it before queueing a job:
  a job queued outside every turn is drained by nobody, and makes the next
  `task_begin` BUSY.

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
  the bookkeeping once. With a script host, the host takes them instead
  (§ A script host).
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

## A script host

A browser reports a script's failures to the page itself: an uncaught
exception is an `error` event at the window, and a rejection nothing handled
by the end of a microtask checkpoint is an `unhandledrejection` event, a
handler added to it later a `rejectionhandled` (HTML § Runtime script errors,
§ Unhandled promise rejections). The outcome's one diagnostic cannot carry
that: the event needs the thrown value and the promise, and needs one per
failure. `os64_js_set_script_host(runtime, abi, host, outcome)`
(`js_engine.h`) gives them to the host:

- **`exception(opaque, context, error)`** is called with every exception
  eval, run, run_file, a call, a job drain or a checkpoint catches, before
  the outcome's diagnostic is written, every job exception of a checkpoint
  and not only the first. One a sticky failure threw (the interrupt that
  ends an overrun) is not handed over: the runtime is finished with.
- **Rejections are the host's to take.** With a host set, no checkpoint and
  no turn's end judges a rejection or releases one: each waits, oldest
  first, until `os64_js_take_rejection(runtime, abi, &promise, &reason,
  &tag)` hands its promise and reason over as new references, with the
  host's `rejected` answer for it. A rejection a later
  handler reaches before it is taken leaves as it always did. Taking is
  refused (false, both values undefined, the tag 0) inside a call, on a failed
  runtime, without a host, and while jobs are runnable, since a rejection
  is unhandled only once the checkpoint that could have handled it has run.
- **`rejected(opaque, context, promise)`** is called when a promise is
  rejected with no handler and the runtime starts holding it, at once,
  while the code that rejected it may still be on the stack
  (`JS_GetScriptOrModuleName`); its answer is kept with the rejection and
  handed back by `os64_js_take_rejection`'s `tag`. A browser decides there
  which script's rejection it is, which it cannot once the stack is gone.
- **`handled(opaque, context, promise)`** is called when a handler is added
  to a rejected promise the runtime no longer holds, which is one the host
  took: what `rejectionhandled` is fired from. The engine asks once per
  promise. A rejection the runtime could not hold (its table's or its
  record's memory refused) is no exception to "one the host took": every
  refused allocation fails the runtime, and a failed runtime's tracker
  calls no hook again.
- The hooks run inside the wrapper's guard, so they must run no script and
  make no wrapper call; they keep what they need (`JS_DupValue`) and act on
  it after the call returns. The outcome is unchanged by a host: a call that
  threw is still EXCEPTION with its diagnostic.
- `NULL` clears the host, and is accepted from a failed runtime too, so a
  host can clear itself before its opaque goes away; setting or clearing is
  BUSY during a call. A cleared host leaves the rejections it did not take
  to the next checkpoint's judging. Destruction releases whatever is left.

`tools/test_js_runtime_host.c`'s script host cases hold each rule:
exceptions from eval, a call and a checkpoint's job reaching the host beside
their outcome; rejections taken oldest first, never inside a call or with
jobs runnable; a rejection's tag, `rejected`'s answer asked while the
script that rejected it was on the stack; one handled before it was taken
gone, and five thousand
handled newest first leaving nothing; a handler added to a taken one
reaching `handled`; a cleared host handing rejections back to the
checkpoint; an overrun's exception kept from the host; and a host cleared
from a failed runtime.

## Modules

An ES module cannot be run the way a classic script is, in one call from
its source: its imports are other files, which a browser fetches, and the
module cannot be linked until every one of them is compiled. The pinned
engine resolves a module's imports as it compiles it, through a loader that
must answer at once, so an entry compiled before its imports are fetched
fails. Patch 0012 (`JS_EVAL_FLAG_NO_RESOLVE`) lets it wait, and the
wrapper's module entry (`js_engine.h`) is built on that:

- **A module is compiled under its name**, which is its URL: what its
  imports are resolved against and, unless the host gives another,
  `import.meta.url`. `os64_js_module_compile(runtime, abi, source, length,
  name, url, &module, outcome)` compiles it UNRESOLVED and hands back the
  host's handle; it runs no script, so it is accepted outside a call
  whether a task is open or not. A syntax error is EXCEPTION, its
  diagnostic in the outcome and nowhere else: no task is running to report
  it to, so a script host's `exception` does not hear it (a browser reports
  a module's syntax error when the script that imports it runs).
  `url` is for an inline module script, which HTML gives the document's
  URL while each still needs a name of its own: the engine finds modules
  by name, and a second module of one name is never found. Compile each
  name once.
- **Its requests are read before it is resolved**:
  `os64_js_module_requests(runtime, abi, module, each, opaque, outcome)`
  hands each static import and export-from specifier, as written, in
  source order. A host fetches what they name and compiles each as it
  arrives, and asks each of those for its own.
- **The loader** (`os64_js_set_module_loader`) is the host's two answers:
  `resolve(base, specifier)`, the name a specifier means in the module
  named `base` (os64_malloc'd; NULL names nothing and the import throws),
  and `source(name)`, a module's source (borrowed for the call; false for
  none). Resolving always asks `resolve`; `source` is asked only for a name
  no compiled module has: a dynamic `import()`, a static import nobody
  compiled, and a module the engine let go of. That last is the engine's
  rule, not the host's: a failed resolve unlists every module no run has
  resolved, other graphs' included (the handles keep theirs alive, but
  they are no longer found by name), so a host answers `source` for any
  name of its map, not only for names it has not fetched. Both run inside
  a call and must run no script; `source` may block (a browser fetches a
  dynamic import there).
- **A module runs once, inside an open task**: `os64_js_module_run(runtime,
  abi, module, outcome)` resolves its imports by name among the modules
  compiled so far (asking `source` for any it cannot find), links and
  evaluates, under the task's deadline and job cap. OK means the
  evaluation began; EXCEPTION means resolving or linking threw (an import
  nobody could load, an export that is not there) or the evaluation failed
  at once.
- **Its evaluation settles when it settles.** A module with a top-level
  `await` can leave the task with nothing runnable and its evaluation still
  pending, because what it awaits (a timer, a fetch) has not come:
  "drained" is not "finished". `os64_js_module_state(runtime, module,
  &failure)` answers UNRUN, PENDING, FULFILLED or REJECTED (with the
  failure's outcome), and a host asks it after every turn's drain, not only
  the first.
- **A module's failure is an exception, never an unhandled rejection.**
  HTML reports a module script's error as it reports a classic script's
  (an `error` event). The engine rejects the evaluation's promise instead,
  so the wrapper's tracker knows each handle's promise and takes its
  rejection as the module's failure: kept for `module_state`, and handed
  to a script host's `exception`, not its `rejected`. Patch 0012 also makes
  handled the inner promise of a module body the engine takes as the
  module's error, which the tracker otherwise heard as a second, unhandled
  rejection.
- **A handle is the host's until it lets go**: `os64_js_module_release`, or
  destroy for the ones still held (TEARDOWN.md). The module stays the
  engine's while anything imports it.
- **A module's bytecode is kept and read as a classic script's is**
  (`os64_js_module_compile_bytecode`, BYTECODE_CACHE.md § 9): the same
  framing, kind `m`, with the module's NAME in the head. Reading a module
  enters it among the context's modules under the name it was written
  with, and no read can give it another, so a blob is read only by a
  compile of that same name; any other is a miss and the source compiled.
  `url` is the compile's, as it is uncached: `import.meta.url` is set
  after the read and is not in the blob. A module is written unresolved,
  as it was compiled, and read unresolved. A read keeps nothing, and a
  NULL bytecode is BAD_ARGUMENT. Patch 0016 makes a read that fails part
  way leave nothing listed.

`tools/test_js_runtime_host.c`'s module cases hold each rule: a graph
compiled before it is resolved and run once found by name, with no load
asked; its request named first; an inline module's `import.meta.url` and
its import not evaluated again; a cycle; a module nobody compiled loaded,
and a dynamic `import()` answered inside the task; requests in source
order; a syntax error (the compile's alone, the host hearing nothing), an
import nobody can load, a missing export and an evaluation that throws,
each the module's own and the last heard once as an exception and never as
a rejection; a top-level await pending after its
task and fulfilled, or rejected, in a later one; and a failed resolve
reached again, by a graph sharing a module whose import failed and, for a
cycle whose entry failed, by a later graph and by `import()`. The module
sweep fails
every allocation of a graph's run in turn, on the target core and the
instrumented engine, under RECLAIM; it compiles uncut, because a compile
under memory pressure is DEBTS.md's 0011b row. The module bytecode cases
run a graph compiled keeping its blobs, then read from them in another
runtime (requests named, imports found by name, nothing loaded); an inline
module's blob read under another URL; blobs refused for another module's
name, a classic script's kind, and a module's offered to a classic run;
and a throwing compile keeping nothing. Their sweeps fail each allocation
of a read, and, for the same 0011b reason, each from a keeping compile's
last back through its write's first.

## Reserved for later work

Nothing. The three extensions DOM.md reserved are built: the host tasks
above (D7a), the callback budget check (D10, § Native callback budget
checking) and the creation-selected reporting destruction (D8, § Opt-in
reporting destruction). Each was reviewed as its own slice. The script host
(§ A script host) came later, with the Modern Web arc's P2a.

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

### A compile that ran out of memory

Patch 0010 adds no export. What it changes: a parse whose bytecode emit
ran out of memory is an exception (`InternalError: out of memory`), never
a function. The parser writes bytecode through a buffer that, on a failed
allocation, stops growing and marks itself failed, a mark the parser never
reads; `js_create_function` checks it on entry, before anything is
transformed, and that is the one place every function definition becomes
a function (eval, `new Function`, modules, class bodies, and each child
definition through the same call). `tools/test_js_runtime_host.sh
--compile-failure` fails each allocation of such a compile in turn, on the
target core and on the instrumented engine.

### Allocations the parser ignored

Patch 0011a adds no export. What it changes: an out-of-memory during a
parse is an exception (`InternalError: out of memory`) through more paths
where the parser ignored a failed allocation and went on:

- a child definition whose constant-pool slot could not be added
  (`js_create_function` asserted on it, which ended the program);
- a closure variable `resolve_scope_var` could not add (its failure was
  used as an index, emitted as nothing, or taken by `resolve_variables`
  as the next position to read);
- an `arguments` or function-name binding `resolve_scope_var` could not
  add (an enclosing function's failure was captured as a wild argument
  slot, which crashed; the function's own was looked up as a global,
  which answered wrongly);
- a bytecode buffer that failed to grow, which `get_lvalue` reported as a
  SyntaxError about the script's assignment.

This is not yet every path: a parse that reaches `resolve_labels` with a
failed output buffer can still crash, a failed atom in the tokenizer's
lookahead still reads as a SyntaxError, a failed pseudo-variable add can
still compile to a wrong value, and `add_eval_variables` still ignores a
failed closure-variable add. DEBTS.md books them.

### A module compiled unresolved

Patch 0012 adds `JS_EVAL_FLAG_NO_RESOLVE` and two exports,
`JS_GetModuleRequestCount` and `JS_GetModuleRequest` (a new atom, or
`JS_ATOM_NULL` past the end): a compiled-only module is kept unresolved
and names its imports, where upstream resolved them through the loader as
it compiled (its own comment marked the spot, "Could add a flag to avoid
resolution if necessary"). The flag is refused (a TypeError) with any
other eval type and without `JS_EVAL_FLAG_COMPILE_ONLY`, since an
unresolved module is never run. `js_execute_sync_module`, which takes a
module body's rejected promise as the module's error, now makes that
promise handled and tells the tracker, as `perform_promise_then` does.
And a failed resolve leaves no module it marked resolved: every module
the attempt marked, including those a loader's nested compile resolved
inside it, is unresolved again. A later run that reaches one resolves it
afresh instead of linking through the NULL of an import never found or,
in a cycle (whose members point back at the module that failed), through
a module the failure unlisted or freed. § Modules is the wrapper built
on it.

### A module read that fails part way

Patch 0016 adds no export. A module read from bytecode is born holding two
references, the context's list's and the value the read returns, and
upstream's failure path dropped one: a read that failed part way (an
allocation refused) left a half-read module listed under its name, found
by the next import of it, and walked by the GC through entry counts read
before their arrays were allocated. The read now unlists and frees it,
zeroing first any count whose array it never got.
