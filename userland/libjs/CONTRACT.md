# JavaScript runtime API contract

Status: embedding ABI 5, including the owner-thread quiescence query, 2026-11-12.
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
compare. Concurrent calls on one runtime must not overlap. Wrapper evaluation,
drain and setup entry points reject active-call re-entry with BUSY without
replacing the outer call's result or budget, except initial realm setup
(§ Realms). Native bindings must obey this rule when using the raw engine API. Independent runtimes may execute on different
threads; class IDs use `os64_js_class_id(&slot)`. The library serializes the
slot check and engine allocation together process-wide: a zero-initialized,
process-lifetime slot receives an ID once; later calls reuse it. Bindings access
shared slots through this function rather than reading/writing them separately.
NULL returns the invalid ID zero without entering the engine. The caller
registers the returned ID in each runtime with QuickJS's class API; allocation
and per-runtime registration are different operations.

`os64_js_context(runtime, OS64_JS_ABI_ID, outcome)` borrows the primary context.
It is available for idle setup and inside native callbacks executing in an
evaluation/job call; it is unavailable during destruction. Its caller-owned
outcome is required and must be separate from an active outer call's outcome.
It returns NULL with ABI_MISMATCH on header disagreement, BAD_ARGUMENT for a
null runtime/ABI argument, or FAILED_RUNTIME for a failed runtime, without
changing the runtime or active budgets. On success it initializes the outcome
with OK. Each translation unit using engine inline helpers checks through this
accessor before using the context. Consumers must
not free it, create contexts through the raw engine API, migrate its runtime, or replace the
library's allocator, interrupt hook, Promise tracker, or loader configuration.
The runtime and context opaque slots both hold wrapper state. Custom bindings
must leave them unchanged and keep native state in their own objects and
function data. A raw engine runtime constructed outside this wrapper has its
ordinary engine configuration, including shared-buffer exposure; this contract
governs the wrapper-owned contexts.

### Embedding global this

`JS_SetGlobalThis(context, value)` retains an object as the engine context's
default script/sloppy-call receiver. A trusted binding calls it on the owner
thread during setup before author execution, using a value from that runtime.
Like other raw JSValue arguments, cross-runtime values are outside the API
contract; the engine does not discover their runtime from an arbitrary object.
Non-objects return false without changing the receiver or pending exception.
Success duplicates the reference before releasing the old hold, which may
invoke native finalizers. The context marks the hold during GC and releases
it on destruction. No extra reference is held for the unconfigured default.

Global name/lexical storage and `JS_GetGlobalObject` keep their meaning.
The setter does not rewrite `globalThis` or DOM aliases. Script evaluation,
compiled script evaluation, indirect eval and sloppy null/undefined this
selection use the receiver; explicit `JS_EvalThis` and strict/module semantics
remain separate. The prepared-engine identity changes with this hook; public
wrapper layouts and ABI 5 are unchanged.

This primitive does not install a WindowProxy, protect a global object or
complete browser receiver semantics. Global-property accessor/reference paths
and the engine's `groupBy` callback receiver also need the F.4 security audit
before a proxy can be exposed. Existing consumers do not opt in. See
[the F.4 brief](../../docs/design/pending/IFRAME_F4_SECURITY.md).

## Dormant engine execution observations

The raw engine exposes explicitly enabled execution-scope observations for
trusted diagnostics. `JS_EnableExecutionScopeTracking` must run on the owner
thread outside engine entry, before code to be observed executes. Default wrapper
setup leaves tracking disabled. `JS_VisitExecutionScopes` is allocation-free and
visits borrowed incoming-context/code-environment facts, innermost first,
including pending calls before their interrupt poll. Its visitor must not
allocate, reenter the engine, release values or mutate runtime state.

Unknown proxy/custom dispatch, eval preparation and native job provenance are
explicit; observations do not certify complete interrupted-environment
attribution, browser caller authority or native consistency. Scope tracking
does not change eligibility, failure or budget behavior and cannot recover a
runtime. See [the F16 scope checkpoint](../../docs/design/pending/IFRAME_F16_SCOPES_VALIDATION.md).

## Dormant executable containment

`JS_MarkContextExecutableFailed` is an irreversible raw-engine mark, separate
from document retirement and wrapper failure. A trusted owner calls it after
engine/native unwind, with scope tracking enabled; tracked active entry is
refused without changing the mark. It allocates/releases nothing and does not
change queues, pending exceptions, cancellation or task budgets. Marking alone
grants no permission to resume a failed wrapper; reuse follows its results
and task contract.

Marked function/code contexts cannot be reentered through raw calls,
constructors, native/data/neutral functions, eval, module handles or suspended
resumptions. Marked execution/callback/activity contexts also block native job
bodies. Unknown custom call classes and over-deep wrapper chains refuse
conservatively once executable failure has been seen. Healthy callers receive
a catchable TypeError unless an uncatchable exception remains pending; that
original exception is preserved. A consumed exception marker does not suppress
a fresh refusal. Allocation/host failure retains the existing sticky contract.

Identity, ordinary data and initialized namespace exports remain retained.
Property access that invokes code still crosses the guard. Module creation
contexts are explicitly retained and GC-marked, including for native/unlinked
modules. These are ownership facts; they grant no origin authority and prove
no interrupted native algorithm consistent. Ordinary departed functions retain
their existing behavior. See [the engine checkpoint](../../docs/design/pending/IFRAME_F16_ENGINE_VALIDATION.md).

## Realms

One runtime owns one engine and up to `OS64_JS_MAX_REALMS` (65) contexts:
the primary plus 64 additional realms. `os64_js_primary_realm` returns the
primary handle; `os64_js_realm_create` creates an independent global and
intrinsics with the same capability profile. Both require the binding ABI.
`os64_js_realm_context` borrows a realm's context and
`os64_js_realm_from_context` checks membership without trusting a foreign
context's opaque slot. A context or realm handle remains borrowed until
runtime destruction, including after retirement.

The wrapper reserves a fixed descriptor table in its enclosing allocation,
charged before the engine memory ceiling is set. Contexts, retained values,
modules and rejection records use the shared engine allocator. A full table
returns non-sticky `OS64_JS_REALM_LIMIT` before allocation; failed construction
publishes no handle. Memory, cancellation, clock, execution and job-limit
failures remain sticky for the entire runtime. Ordinary engine stack overflow
remains EXCEPTION unless another sticky condition is observed. A failing child
does not isolate its parent from shared-runtime failures.

`os64_js_heap_headroom` reports the remaining engine allowance after the
runtime wrapper's storage and current allocator charge. It is an owner-thread
snapshot, usable inside callbacks and after failure, with no allocation, GC
or failure observation. A positive number does not certify runtime health.
`os64_js_realm_preflight` checks the ABI, runtime health, available slot and
a positive caller-supplied headroom requirement. An insufficient allowance
returns non-sticky `OS64_JS_ADMISSION_REFUSED`; a full table returns REALM_LIMIT.
Neither allocates, consumes a slot, touches the parent's outcome, advances its
deadline/job count, or drains queued work. A pending allocator failure cannot
be reported as local refusal. Invalid input and ABI mismatch remain refusals.

The headroom requirement is the host's measured peak installation cost plus
its sibling operating margin. Preflight does not reserve bytes; subsequent
allocations can invalidate it. `os64_js_realm_create_with_headroom` repeats
the check immediately before ordinary realm creation, including when nested
inside a parent callback. It publishes no handle on refusal. The legacy
`os64_js_realm_create` keeps its existing slot-only admission. Neither path
promises that arbitrary later allocations or system OOM are recoverable.
libdom's `os64_dom_create_child` supplies staged installation and native
rollback; its 8 MiB admission allowance includes the measured interface peak
and sibling margin. [F.3b's measurements](../../docs/design/pending/IFRAME_F3_DOM_VALIDATION.md)
cover both allocator profiles. A bare engine realm's size is not the cost of
an installed DOM binding.

`os64_js_realm_stage` performs guarded creation and reserves an Installing
slot. It exposes a native handle for private installation, but ordinary
source/module/task entry returns non-sticky REALM_INSTALLING. Keep its objects
unexposed until `os64_js_realm_activate` succeeds. Activation requires a
successful `os64_js_realm_setup` and no pending candidate work; it does not
open a turn or change the budget. A slot remains consumed if setup fails. Refusal before guard entry (such as
BUSY or invalid input) leaves the private candidate unchanged for retry or
explicit retirement. Once the guard starts, failure retires it.

`os64_js_realm_setup` calls a trusted native installer, permits its private
bootstrap, and drains its candidate-tagged jobs before returning success.
The callback returns OK, ADMISSION_REFUSED with no pending engine exception,
or EXCEPTION with a pending exception. An ordinary exception/native refusal
retires the candidate without poisoning healthy siblings; engine memory,
cancellation, clock and budget failures still poison the shared runtime.
An unhandled candidate rejection is a private setup failure, without calling
script-host reporting hooks. Native partial-state cleanup belongs to the host.

A standalone setup guard requires no earlier pending turn. Nested setup borrows
the enclosing task or evaluation's deadline and executed-job count, protects
its outcome storage, and neither runs nor reorders sibling jobs. Setup cannot
nest another candidate installation. Installers may use separate outcomes for
nested installation APIs, but may not call siblings/external providers or
publish candidate references. Output and module providers remain closed until
activation, including after their hooks have been installed privately.

Patch 0020 attaches a stable transaction group independently of execution
ownership. An intrusive second queue permits constant-time selection of the
next candidate job while preserving the global sibling FIFO. Neutral jobs
and descendants enqueued under the guard belong to the candidate. Abort
releases that group's entries without executing them or settling promises.
GC may enqueue an existing sibling registry's cleanup during a candidate
allocation; that cleanup stays outside the candidate group. Group storage
lives in the retained realm slot through engine teardown. The raw group APIs
are trusted embedding machinery, not a way to bypass wrapper job budgets.

`os64_js_realm_retire` irreversibly marks a realm ineligible. It is owner-thread,
idempotent and allocation-free, including inside a native callback with a
separate outcome. It also remains available after failure for native cleanup;
it does not change the failure latch. `os64_js_realm_is_retired` reads the mark.
New source/file/bytecode/module execution, installers and realm tasks return
non-sticky REALM_RETIRED. Host function calls also check the function's realm,
so selecting A's context cannot schedule a departed B callback. Existing
JavaScript stacks may finish/unwind, and live scripts may directly call
retained ordinary functions. Retirement neither frees context/global/native
storage nor removes queued entries under an active stack.

`os64_js_quiescent` is an owner-thread, allocation-free snapshot for native
cleanup. It returns BUSY while an engine call, task/turn, checkpoint, installer
or queued job remains. The queue check also rejects malformed raw work outside
a turn; it does not authorize executing such work. FAILED_RUNTIME covers latched failure, unobserved allocator
failure or cancellation, and destruction; that result requires the root cleanup
path after its normal unwind. An ordinary sibling exception does not prevent a
later OK result. Argument and ABI errors are ordinary refusals, and an aliased
active outcome is left untouched.

The query does not run jobs, sample the clock, change budgets or latch a failure.
OK is not a lock or permission to free arbitrary binding state: the host must
also exclude native dispatch, reactions and provider borrows, and perform no
intervening engine entry before the protected cleanup. ABI 5 publishes this
entry; the bytecode engine identity is unchanged because the engine sources
and compilation profile are unchanged.

Script reporting and new module-provider/output activity stop at the mark.
At the host's safe point after active entries and checkpoints, clear script
and module hooks before freeing their opaque storage. Context/member queries
and module release remain available while their ordinary runtime-health rules
allow them. The libjs mark is not DOM departure: a DOM host must separately
stop its timers, observers, network activity and binding providers.
libdom's `os64_dom_depart_finish` joins this engine barrier with native walk
and provider-loan checks, clears obsolete work and detaches providers while
retaining exported objects. Its successful return permits releasing provider
opaque storage; a sticky failure requires root teardown instead.

Queued work carries native execution context, callback/code owner and an
optional activity owner. Patch 0019 retains these independently. Promise
reaction ownership follows the handler, thenable work follows `then`, and
handlerless propagation is explicitly neutral. Custom capability resolve/reject
functions use ordinary Call inside an admitted job. Native data functions and
async continuations retain their creation/code realm; bound/proxy traversal is
iterative, with revoked proxies falling back to the enqueue context without
allocating or throwing at enqueue. FinalizationRegistry work additionally
checks the registry's activity realm. Dynamic imports retain importing context
ownership. Binding jobs default to the enqueuing context; native hosts can use
`JS_EnqueueJobOwned` for explicit or neutral ownership and must gate records
inside a neutral shared delivery queue themselves.

Patch 0021 adds `JS_NewCFunctionDataNeutral` for trusted native continuations
used by self-hosted bindings. It retains the ordinary function realm and data
holds, including `JS_GetFunctionRealm` results, but promise-job discovery treats
it as neutral through direct, bound and proxy references. It does not exempt a
job from execution budgets or installation transaction tagging. The binding
must check author callback eligibility inside the continuation. Libdom uses
this constructor only behind its private Streams factory; page JavaScript has
no API for assigning neutral ownership. The generated engine identity changes
with the patch; the wrapper's public configuration and value layouts do not.

Manual drains, task checkpoints and candidate drains inspect before execution. Dead entries release their holds without
running a job or settling its promises, preserving surviving FIFO order.
Foreign contexts are invariant failures, not neutral work. Discarded entries
count against slice visits and the original deadline, but not executed jobs.
Dead heads can be removed at the job cap; the next eligible entry still fails
with LIMIT/JOBS. Obsolete reporting records/owed module errors do not surface
in a live sibling's checkpoint.

Legacy verbs select the primary realm. Explicit realm verbs select the owner
for source/bytecode execution, task begin/call, module operations, script hosts,
rejection taking, output and Intl. They do not swap a runtime context pointer.
There is no realm-specific eval, file or argument installer: legacy eval/file/
arguments retain their primary-only behavior. A realm module handle is refused
by another realm's module operations, even within the same engine; release
returns BAD_ARGUMENT rather than releasing a foreign handle. Release remains
available after runtime failure. Module state without an ABI parameter assumes
the caller already obtained the realm through the checked binding interface.

Tasks, the engine job queue, deadline, job count, guard and active outcome are
shared. A task begun for A can call B with `os64_js_realm_call`; raw native
bindings can call shared-runtime values through `JS_Call`. Neither changes the
enclosing task's budget. Checkpoints drain the shared queue. Exceptions from a
job use the context returned by the engine; late module failures use their
module's owner and source name, with runtime-wide first-failure ordering.
Values from a different engine must not be passed to these calls.

The opt-in `os64_js_realm_with_settings` bridge supplies checked native entry
and callback scopes without changing these legacy execution verbs. It requires
an existing active turn, or an open task between calls. Entry and an optional
captured incumbent must be published, non-retired members of the same wrapper
runtime. Private candidate setup, failed/cancelled runtimes, foreign realms,
invalid ABI and missing arguments refuse before the native body runs. Scope
nesting is bounded by the engine's `JS_SETTINGS_SCOPE_LIMIT`; overflow is a
non-sticky ADMISSION_REFUSED. A null incumbent means ordinary script entry;
a non-null incumbent prepares callback suppression and backup selection. The
host supplies that captured realm explicitly; this API does not capture it.

An outer bridge call acquires the wrapper guard; nested native calls borrow it
without updating the stack origin. Separate outcome storage is required at
each level, including queries. The guard covers the native body's whole
lifetime, so task closure, top-level run/call, destruction and quiescent cleanup
cannot pass through the interval between its JS calls. The body must return
normally in C. Entry counts as evaluated when the admitted body starts.
No task is opened, deadline renewed, queue drained, rejection reported, or
engine exception consumed. Existing failure observation runs before/after the
body. Outcomes report queued/executed/discarded job counts without checkpointing.

Initialize the `JSValue` output to JS_UNDEFINED. Refusal leaves it untouched;
admission transfers the body's value after restoring engine scopes. Release
the value even when post-body failure observation returns LIMIT/CANCELLED/
HOST_FAILURE. A body returning JS_EXCEPTION produces EXCEPTION with the engine
exception still pending, unless a sticky failure takes precedence. The caller
must propagate or consume that exception; no diagnostic conversion or script
host exception callback runs in this bridge. These semantics differ from the
reporting/discarding `os64_js_realm_call` API.

`os64_js_settings_realm` maps ENTRY or INCUMBENT to a borrowed wrapper handle
inside an active checked scope. It checks membership and current wrapper
lifecycle on each query; missing settings, a raw-only scope, or an unregistered
engine context give ADMISSION_REFUSED. Retired/installing contexts give their
lifecycle refusal. Unknown roles are BAD_ARGUMENT. Queries do not allocate,
read the clock, latch a failure, consume an exception, or extend lifetime.
Recheck after reentry or retirement. Runtime health checks include pending
cancellation and allocator failure, without converting them into a new latch.

The opt-in `os64/js_callbacks.h` adds a native-owned inline callback record.
Initialize `os64_js_callback_t` with `OS64_JS_CALLBACK_INIT`; populated records
own one engine value reference and must not be shallow-copied. Their wrapper
runtime must outlive them. The binding accounts for the record storage as part
of its holder and supplies values from the same engine, as for raw engine APIs.
Output/result storage must not overlap the record or the invocation's inputs.

`os64_js_callback_capture` runs at the conversion point after binding-specific
type validation. It accepts an object and an empty record, retains the value,
and captures the checked INCUMBENT realm. It neither calls JavaScript nor
reads properties, allocates, infers the associated entry realm, or checks
callability. Missing checked settings, invalid input and health/lifecycle
refusals leave the record and pending exception untouched. The callback's
value, the native registration binding and the captured incumbent may all
belong to different realms within the runtime.

`os64_js_callback_with_settings` takes a separately established associated
entry realm and an invocation adapter. The bridge's guard, budget, eligibility,
outcome-alias and exception rules apply. The adapter runs property lookup,
argument/result conversion and invocation under the prepared settings; it
must apply the binding's callback type and activity/security checks. This
helper does not infer entry from `JS_GetFunctionRealm`, a bound/proxy target,
`handleEvent`, the registration binding or the task owner. The held value and
captured incumbent are fixed for that invocation. The adapter borrows the
value, returns an owned result as usual, and may clear/reuse the record or
release its enclosing holder. The temporary value hold is freed before the
checked scope exits, including normal native finalization. Refusal before
admission leaves the caller's value output untouched.

`os64_js_callback_clear` empties the record before releasing its value so
reentrant native cleanup sees an empty record. Clearing an empty initialized
record is idempotent. Clear remains available after cancellation, failure and
realm retirement, and during an engine holder's destruction. It validates
runtime/ABI and rejects active outcome aliases, without sampling time,
consuming exceptions or acquiring a guard. Normal value finalization can run.
C-root records must be cleared before destroying the runtime. An engine-owned
holder instead calls `os64_js_callback_mark` from its `gc_mark` and clears the
record in its finalizer; marking the engine edge permits cycle collection.
A C root must not be reported as an engine edge. Captured realm handles do not
extend realm eligibility: retirement prevents later delivery, while cleanup
can still release the retained value.

These interfaces have no production browser consumer. Current/relevant role
attribution, WebIDL conversion-site integration and associated-object realm
proofs, Promise reaction/thenable capture, DOM entry-site integration and origin
security checks remain separate work. A settings handle is not a permission.
F.16 recovery must preserve the bridge's native-body guard and synchronize
wrapper lifecycle with engine failed-context state before admitting recovered
work; raw engine marking alone is not a wrapper recovery protocol.

Creation and the explicit realm output/Intl installers support three setup
modes: idle setup owns a bounded turn; setup between calls of an open task
borrows its budget and acquires the guard; setup inside a native callback
borrows the existing guard and budget. The last mode preserves the outer
outcome and stack origin. Each nested operation needs separate outcome storage.
An installer requires an unevaluated realm and an uninstalled facility; a
recursive install of the same facility is refused. A binding can install host
objects or traps between creation and installation, so those traps remain
inside this guard. Setup that queues jobs leaves them with its enclosing task,
or with its own idle setup turn for draining. Legacy installers retain their
idle-only rule. Wrapper run/call re-entry remains BUSY.

A child's script host and module loader may be installed for the first time
inside a native callback while that realm is unused. Replacing or clearing
configured services during a call is BUSY. Outside a call, explicit realm
service setters can copy or clear their non-executing configuration after
a sticky failure; this does not restore execution. The legacy module-loader
setter retains its FAILED_RUNTIME entry refusal. Script hosts, loaders, module lists and captured
Promise `then` belong to each realm. A borrowed script function's dynamic
import uses its code realm's loader. Output handles, console group depth,
counts, timers and Intl belong to the installing realm. QuickJS data-bearing
native callbacks receive the caller's context, so console methods and module
watchers retain their owner's slot explicitly in their function data.

Rejection lookup is keyed by promise identity across the engine. Each record
also carries a reporting realm; taking A's oldest record leaves B's records
alone. Handling a promise before take removes its pending record regardless
of the attaching realm or routing policy. Handling after take uses the policy
at that later moment. Hosted realms retain records for their hosts; unhosted
realms are judged at checkpoints without consuming hosted siblings' records.

The optional runtime rejection router selects a reporting realm while the
native callback stack is present; absent a router, the engine callback context
selects it. The router must execute no script. NULL suppresses a new report;
a foreign handle causes sticky HOST_FAILURE. Host hooks receive the triggering
native context even when policy selects a different host. Pending-record removal
precedes router invocation. This is a native policy seam, not HTML settings-
object attribution; the browser bridge and any needed record changes belong
to F.4. F.1 does not expose arbitrary realm selection to page JavaScript.

Destroy disables service reporting, releases per-realm retained values and
module handles, drops all contexts, then destroys the engine before freeing
the fixed descriptor table. Both teardown policies apply to this whole set.

## Capabilities and native bindings

`os64_js_install_output` borrows an already-open output handle and installs
only the names selected by its bitmask: OS64_JS_OUTPUT_PRINT grants `print`,
OS64_JS_OUTPUT_CONSOLE grants the WHATWG Console Standard's `console`
namespace. Its levels (`log`, `info`, `debug`, `warn`, `error`, `trace`,
`dir`, `dirxml`, `table`) all print as `log` does, to the one handle.
`print` and every console method write each value as its string
conversion, but an Error as other browsers' consoles do, reading only
strings it (or its ordinary prototype) holds, so no page code runs: its
name and message, then its stack, the later lines indented with the line's
console group;
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

`os64_js_install_intl` installs ECMA-402's `Intl` (docs/design/pending/INTL.md)
with `locale` as the realm's default locale; NULL or empty is BAD_ARGUMENT, and
a tag the data does not hold (it holds `en` and `en-US`, as written) is a setup
exception: ECMA-402 requires the default to be an available locale. It follows the installers' rules above: idle, before the first
evaluation, once, under the setup budget and the runtime's memory. The object
is published as one global property (writable, configurable, not enumerable)
only once all of it is made, so a refused allocation or a failed definition
leaves no `Intl`. As the script's last step it redefines the built-ins
ECMA-402 redefines (`Date.prototype.toLocaleString` and its date and time
halves, `Number`'s and `BigInt`'s `toLocaleString`,
`Array.prototype.toLocaleString`, `String.prototype.localeCompare`,
`toLocaleUpperCase` and `toLocaleLowerCase`); every refusal that can come
before that step (a default locale the data does not hold among them)
leaves them as they were, and any after it (an allocation refused, the
setup deadline, a cancel) is the runtime's sticky failure. Its script captures the built-ins it calls
as it loads, which is why it must run before any page script. Its CLDR data is evaluated
by the realm's first construction (a constructor or `supportedLocalesOf`),
not by the install, the number tables within it by the realm's first
`NumberFormat`, `PluralRules` or `RelativeTimeFormat`, and the collation by
its first `Collator`, each inside whatever call made it and under
that call's budget: a refused allocation there is the runtime's sticky
failure, as in any script. The compiled script and data are kept for the process and read
by every later runtime; that bytecode never leaves the process. The
machine's zone is `TZ`'s, by the policy `Date`'s local time follows
(`os64/date.h`), resolved to a zone name as INTL.md § 5 says.

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
`drain_jobs` visits up to positive `slice_jobs` entries (executed or discarded),
returning MORE_JOBS if the queue remains nonempty. `run` drains until the queue empties or a non-success
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
(§ A script host), and then that realm's records wait for its host. A
browser's tasks and checkpoints use § Host tasks; the standalone profile does
not implement the HTML event loop. The tracker finds a rejection by its
promise in a table, so neither the thousandth rejection of a turn nor a
handler added to it costs more than the first.

## Results, limits, and reuse

Outcomes use caller-owned fixed-capacity diagnostic arrays. Each operation
initializes its outcome, with NUL-terminated fields, even when formatting fails.
Truncation sets `diagnostic_truncated`; unavailable locations are zero. The
structured status and limit identify failure without parsing text.
`jobs_executed` and `jobs_discarded` are cumulative for the current turn, not
just the last slice; only executed jobs consume the execution cap. Diagnostics
may use engine conversions, which can fail or invoke script; the original turn
budget and entry guard remain active during exception extraction, with a fixed
fallback when conversion fails. Rejection checkpoint diagnostics avoid executing
reason objects: primitive conversion or real Error own string data is used;
accessors, proxies and other objects retain the fallback. Locations remain zero
because the retained API does not expose structured line/column data; no error
message is parsed. Returned status equals the
outcome's status. There is no release operation for an outcome.

OK and MORE_JOBS keep the runtime usable. BAD_ARGUMENT and BUSY reject the
operation without changing its runtime. REALM_LIMIT and ADMISSION_REFUSED
refuse realm admission before allocation and leave the runtime usable.
ABI_MISMATCH publishes no runtime at
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
  sticky status. Script execution stays refused; non-executing access, service
  cleanup and module release retain their documented failure-state contracts.
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
  so the wrapper takes that rejection as the module's failure: kept for
  `module_state` under the module's name, and handed to a script host's
  `exception`, not its `rejected`. A failure at once is the run's own
  outcome. A module still evaluating when its run returns is **watched**:
  the wrapper attaches a rejection handler of its own through the engine's
  `then`, read at construction before any script ran (a page may replace
  the one it can reach), so its failure is heard even when an `import()`
  of it has handled the promise and the tracker stays silent. `then` asks
  the promise's `constructor` for its species, which a page can replace;
  the evaluation's promise is the engine's own and no script holds it, so
  the wrapper gives it an own `constructor` of undefined first, and
  watching asks the page nothing. A failure heard after the run returned
  has no call of its own, so it is **owed** to the outcome of the call
  whose jobs surfaced it, as an EXCEPTION naming the module, when that
  outcome is still OK; another waits for the next call that runs jobs or
  checkpoints (never a task's begin), in the order they failed, so each is
  in exactly one outcome. Any call that drains jobs can therefore answer
  EXCEPTION for a module that is not its own. A host that keeps one
  failure a task keeps the first: a second late failure in a task that
  already failed is heard by the host's `exception` and no further. Patch 0012 also makes
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
task and fulfilled, or rejected in a later one and that call's outcome,
reported once; a late failure an `import()` also waits on; two late
failures surfaced by one call, the first its outcome and the second the
next call's; a page's counting species getter, which watching a module
reads no more than running it does; and a failed resolve
reached again, by a graph sharing a module whose import failed and, for a
cycle whose entry failed, by a later graph and by `import()`. The module
sweep fails
every allocation of a graph's run in turn, on the target core and the
instrumented engine, under RECLAIM; it compiles uncut, because a compile
under memory pressure is DEBTS.md's 0011b row. The watch sweep does the
same to a watched module's life, its run and the later task that fails
it: each refusal either leaves the failure reported or ends the runtime,
and destroy reclaims all. The module bytecode cases
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

### Refused UTF-8 exports

Patch 0017 releases the reference `JS_ToCStringLen2` holds on its source
when allocation of the encoded copy fails. It still returns NULL and a
zero length with the engine exception pending. This includes a rope
linearized by `JS_ToString`; the temporary string belongs to the conversion
until it is returned or released. `tools/test_js_port_host.sh` refuses the
conversion of both a Latin-1 and a wide string and checks teardown ownership.

## Native call context facts

`JS_GetNativeCallContexts` is an additive raw-engine query for the innermost
active C-function/data invocation, including optimized native iterator dispatch.
It copies borrowed `incoming` and `defining` contexts without allocation,
reference changes or exception access.
No invocation (or a null runtime) returns false and clears a supplied output;
a null output returns false. Query on the owner thread and do not retain the
borrowed pair beyond the native invocation.

Generic C functions receive their defining context as their callback argument;
data functions receive the incoming context while retaining their defining
context. Optimized `iterator_next` callbacks also receive the incoming context;
their direct dispatcher checks executable containment and records an opt-in
native execution scope before invoking the callback, without boxing its result.
The query preserves those conventions. Nested calls restore the
enclosing pair on normal or abrupt return. During JS reentry, that enclosing
pair still describes the native invocation, not the current JS realm. Other
class/provider callbacks do not establish a new pair.

These are dispatcher facts, not browser security authority, entry/incumbent
settings, job ownership or an interrupted-environment set. Raw host callers can
choose the incoming context. Production DOM/Yonder do not use the query; the
remaining browser attribution obligations are in
[the F.4b interface note](../../docs/design/pending/IFRAME_F4_SETTINGS.md).
Wrapper ABI 5 is unchanged; use matching rebuilt engine objects and headers.
