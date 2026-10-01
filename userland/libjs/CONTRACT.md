# JavaScript runtime API contract

Status: R0 proposal for Fable's interface review, 2026-10-01. The declarations
in `include/os64/js.h` and `include/os64/js_engine.h` are a concrete interface
proposal, not implemented library symbols. The source pin and host smoke test
are available; R1's target adaptation and R2's runtime remain implementation
work. No libjs shared object is registered or shipped by this slice.

## Creation and ownership

`os64_js_create` takes a configuration, `OS64_JS_ABI_ID`, an output runtime
pointer, and a caller-owned outcome. These pointers must be valid. It clears
`*out` before attempting creation and returns an independent runtime on success.
A null configuration, zero limits, or unrepresentable deadline is a bad
argument. There is no unlimited sentinel; zero is rejected. Explicit development budgets appear
in the examples; production defaults wait for J2 guest measurements and can be
introduced through a configuration helper without changing this structure.

The ABI identifier covers the engine release, os64 wrapper revision, LP64
layout, 16-byte JSValue, 64-bit limbs, and Atomics configuration. The runtime
compares it before exposing a context. Relevant public configuration changes
require a new identifier. This detects accidental header/library disagreement;
it does not validate or isolate native code.

Creation installs language built-ins, including Promise and Date, but no host
output, arguments, file, network, process, worker, Atomics, or shared-buffer
capabilities. R2 must remove/omit shared-buffer exposure explicitly; disabling
Atomics alone does not do that. No filesystem module loader is installed.

Creation binds the runtime to the creating thread. The same thread owns setup,
evaluation, jobs, diagnostic extraction, and destruction. This is a caller
obligation: os64 does not expose a current-thread identifier for a runtime to
compare. Calls on one runtime must not overlap. Wrapper evaluation/drain/setup
entry points reject active-call re-entry with BUSY without replacing the outer
call's result or budget. Native bindings are responsible for obeying the rule
when using the raw engine API. Independent runtimes may execute on different
threads; class ID allocation uses `os64_js_new_class_id`, serialized process-wide.
The caller registers that ID in each runtime with QuickJS's class API.

`os64_js_context` borrows the owned context. It is available for idle setup and
inside native callbacks, and returns NULL for a failed runtime. Consumers must
not free it, create additional contexts, migrate its runtime, or replace the
library's allocator, interrupt hook, Promise tracker, or loader configuration.
Library ownership of QuickJS's opaque slots must be specified in R1; custom
bindings should keep native state in their own objects and function data.

## Capabilities and native bindings

`os64_js_install_output` borrows an already-open output handle and installs
`console.log` and `print`. The host keeps that handle valid until destruction;
the library does not close it. Values convert with QuickJS string conversion,
spaces separate arguments, and one newline terminates a call. Conversion may
execute script and remains subject to the active execution budget. A conversion
exception is a script exception. A write failure is HOST_FAILURE with its os64
error code. A blocking handle operation has the handle's ordinary blocking
semantics; the engine interrupt does not cancel a blocked native write. Hosts
needing bounded output must choose an appropriate handle or custom binding.

`os64_js_install_args` copies UTF-8 argument strings into `scriptArgs`; count
zero permits a null argument array. Other null entries are invalid. The caller
may release its array/strings when installation returns. The runner supplies
the filename as argument zero; the library does not invent it. Installers run
while idle, before the first evaluation, and are each installed once. Calling
one again, or attempting setup after evaluation starts, returns BAD_ARGUMENT.

Custom bindings use QuickJS values and lifetime rules directly. `examples/binding.c`
shows argument conversion, a thrown callback exception, and ownership on
property-install failure. Setup failure can leave partially installed objects;
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
errors are HOST_FAILURE. Non-file handles are outside this helper's contract.
The execution deadline starts immediately before compilation/evaluation, after
file loading. Input size and memory cap bound loading; the initial helper does
not promise an I/O deadline. Native bindings likewise supply their own bounds.

One turn starts at evaluation and ends when its runnable job queue is empty.
It has one monotonic execution deadline and one total executed-job cap. Time
spent between manual job slices counts against that deadline; slicing does not
refresh either budget. `eval` can return OK with `jobs_pending` true.
`drain_jobs` executes up to positive `slice_jobs`, returning MORE_JOBS if the
queue remains nonempty. `run` drains until the queue empties or a failure occurs.
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
values and bookkeeping count against the memory budget. Browser checkpoint
policy will require a reviewed extension; this contract is for the standalone
runner, not the HTML event loop.

## Results, limits, and reuse

Outcomes use caller-owned fixed-capacity diagnostic arrays. Each operation
initializes its outcome, with NUL-terminated fields, even when formatting fails.
Truncation sets `diagnostic_truncated`; unavailable locations are zero. The
structured status and limit identify failure without parsing text.
`jobs_executed` is cumulative for the current turn, not just the last slice. Diagnostics
may use engine conversions, which can fail or invoke script; R2 must bound
that work and retain a non-allocating fallback. Returned status equals the
outcome's status. There is no release operation for an outcome.

OK and MORE_JOBS keep the runtime usable. BAD_ARGUMENT and BUSY reject the
operation without changing its runtime. ABI_MISMATCH applies to creation and
publishes no runtime. Execution failures (EXCEPTION, UNHANDLED_REJECTION, LIMIT,
CANCELLED, HOST_FAILURE) make the runtime failed: no further script or jobs run,
and pending work is discarded at destruction. An already-failed runtime returns
FAILED_RUNTIME. This conservative first policy avoids resuming a partly executed
script or inventing a public job-purge API. A browser can disable that document's
script environment; reuse after execution failure needs a separate contract.

Limit classification must be reliable when the wrapper detects it: SOURCE,
MEMORY, EXECUTION, and JOBS are wrapper-owned counters/flags. STACK is reported
only when the engine exposes a reliable signal; do not recognize it by matching
exception text. Otherwise stack overflow remains EXCEPTION. Memory accounting
uses `os64_malloc_size` plus documented metadata charges; realloc failures keep
old allocation/accounting intact. DOM allocations have their separate budget.

`os64_js_cancel` atomically latches cancellation and is the cross-thread
operation. The owner observes it before execution/job entry and through the
interrupt hook. Cancellation is sticky, with no reset/reuse operation. The
caller keeps the runtime alive until cancellation callers have finished. It is
not a signal-handler API and does not interrupt arbitrary blocking native code.

Destroy runs on the owner outside active callbacks, frees queued values and
contexts, and runs native finalizers before releasing the engine. NULL is a
no-op. The host must end access to its native state after finalization, not
before. A leaked value or engine invariant violation diagnoses and exits the
host with `OS64_JS_FATAL_EXIT`; it is not an outcome. Teardown-leak containment
for Yonder remains D0 work under the conditions in JAVASCRIPT.md.

## Review and implementation gates

Fable reviews this proposal, including the conservative reuse rule and the
continuous budget across manual slices, before C1 depends on it. R0's header
and examples have syntax checks; they do not prove any runtime behaviour.
R1 supplies target C adaptation and compatibility-header publication without
bringing host libc into the exported binding header. R2 implements and tests
the lifecycle, limits, installers, error fallback, job tracking, cancellation,
header mismatch, custom callbacks, and fatal teardown contracts. J2 selects
production defaults using guest evidence. M1 supplies the maths library.
