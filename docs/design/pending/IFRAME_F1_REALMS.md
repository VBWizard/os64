# F.1 — realms and runtime service routing

**Proposed implementation brief, 2026-11-10; ready for design review.**
This is tracker slice `F.1`, the first cut of IFRAME.md's F1 foundation
stage. Source inspection used `userland` `fb68bdcd` (the merged design
refresh, PR #528) plus tracker commit `483d0734`. It does not report an
implementation, runtime test pass, or Fable review.

The result is a libjs runtime that can own several explicitly addressed
realms, with independent modules, output and reporting services. Existing
single-document consumers continue to address its primary realm through
their existing entry points. A two-realm native harness proves the new
boundary; the existing DOM and Yonder hosts prove compatibility.

## Scope and dependencies

F.1 provides engine realms and service routing. It does not enable iframe
elements, create two DOM bindings in one runtime, relax `d_context`, or
introduce WindowProxy. F.2 owns the DOM cluster, arenas and adoption;
F.3 owns independent retirement, pre-execution job suppression and the
failure-policy split; F.4 owns the browser settings/security bridge and
WindowProxy/Location. F.5 uses those contracts for cross-realm messaging.

This distinction refines the tracker seed's phrase "two-document host":
F.1 tests two **JavaScript realms**, with separate native fixture services.
The two-document DOM host arrives with F.2. No frame WPT pass can be claimed
from this harness.

Changes between the design's `91ad9413` source baseline and this brief's
base in libjs, libdom and the inspected Yonder script/module/window files
are Web Animations B1 and its review fixes (`15e232dd`, `472c074d`). They
do not remove the single-context assumptions below. F.1 does not change
animation ownership. Workers retain their own runtime and thread, as
WORKERS.md specifies; this slice adds neither a thread nor a wake primitive.

## What the code establishes

Paths in this table are relative to `userland/`. Names are search anchors.

| Site | Existing contract | Consequence for this cut |
|---|---|---|
| `libjs/runtime/runtime.c`, `struct os64_js_runtime` | One `context`, script host, loader, module list, captured `promise_then`, output handle/depth and installer flags | Separate realm services from engine/turn state |
| `os64_js_create_with_teardown`, `os64_js_context` | Construction removes SharedArrayBuffer and owns both opaque slots; context access checks the binding ABI | Apply the capability profile to each realm; keep legacy access on the primary |
| `enter`, `leave`, `evaluate`, `checkpoint` | One entry guard, deadline, job count and sticky failure; setup also has a budget | Realm selection cannot reset the enclosing task or release its guard |
| `module_normalize`, `module_load`, `module_watch`, `module_owed_fold` | Runtime-global loader and watcher state; engine callbacks supply a context | Resolve the provider from the realm and retain a module's owner through late failure |
| `rejection`, `os64_js_take_rejection` | A runtime table keyed by promise, one host, one take queue | Reporting ownership must survive until taking; another realm must not consume the record |
| `console_call`, `output_call`, `setup`, `os64_js_install_intl` | Native callbacks read context opaque as a runtime; installer readiness is runtime-wide | Audit the opaque-slot change and make installation/output state per realm |
| `libdom/core.c`, `os64_dom_create`, `d_context`, `d_callback` | A second binding is rejected by runtime class registration; validation compares against the primary context | Preserve those guards in F.1; do not replace equality with a same-runtime test |
| `libdom/report.c`, `d_report_host`, `d_report_drain` | Installing/clearing a host affects the runtime; script-name records belong to one binding | Legacy setters must affect the primary realm only |
| `apps/yonder/modules.c`, `yonder_modules_new`, `yonder_modules_drop` | Installing/clearing the module provider affects the runtime | Compatibility calls select the primary; F.2 adapts the provider to an explicit realm |
| `apps/yonder/scripts.c`, `ensure_runtime`, `retire` | A script host creates and destroys its runtime | Keep the single-document lifecycle; it is not a child-retirement operation |
| `libjs/upstream/quickjs.c`, `js_new_module_def`, `JS_ExecutePendingJob`, `js_call_c_function` | Module lists belong to contexts; jobs retain a context; native functions use their creation realm | Reuse engine module separation, preserve queue ordering, and distinguish native context from browser caller/settings policy |

## Ownership decisions

Introduce an opaque `os64_js_realm_t`. A runtime owns its primary realm
and a list of additional realms. Realm handles and borrowed contexts stay
valid until runtime destruction. F.1 exposes no realm destroy/retire call.
Keeping contexts this long is an explicit, charged ownership boundary,
not a claim of reclamation when a document departs.

| Owner | State | Release boundary |
|---|---|---|
| Runtime | Engine, allocator/ledger, limits, cancellation, sticky failure, entry guard and active outcome | Runtime destruction |
| Runtime turn/task | Deadline, total executed jobs, task owner, source diagnostic state, checkpoint state | End of the enclosing turn/task; no reset on a realm crossing |
| Runtime | Shared job queue, rejection lookup and ordering, late-module-failure order | Records released at their consumption/cleanup boundary; residual records at destruction |
| Realm | Context, runtime back-pointer, module handles/provider, captured Promise intrinsic, script host, evaluated/installer flags, output handle and console depth | Runtime destruction, after service detachment and value cleanup |
| Module handle | Its owning realm and engine values, evaluation/failure state | Explicit release or runtime destruction |
| Rejection record | Its reporting realm, promise/reason references, sequence and host tag | Handled-before-take, take, unhosted checkpoint cleanup, or runtime destruction |
| Host | Provider opaque objects and installed output handles | Host clears its provider before releasing its opaque; output handle remains valid until runtime destruction |

Embed the primary descriptor in the wrapper allocation, which construction
already reserves against the configured memory limit. Additional descriptors
are wrapper-owned `os64_malloc` allocations so they can outlive engine
destruction without a `js_free_rt` call on a freed engine. Charge their
actual `os64_malloc_size`, checking the combined ceiling before admitting
the descriptor. Track the total as `realm_storage_bytes` and lower
`allocator.allocation_limit` by that total; the allocator already takes
the smaller of its owner ceiling and the engine's ceiling. Restore this
reservation on construction rollback. Refusal latches the same memory or
host-allocation failure as the corresponding engine allocation.

Contexts, captured values and service records use the engine allocator.
There is no new allowance per realm. Keep existing heap counters' engine-
charge meaning and add the owner-thread, script-free query
`os64_js_realm_storage_bytes(runtime)` for the additional wrapper storage
(zero for NULL). Tests reconcile both against the configured ceiling and
the primary wrapper reservation. Heap census remains runtime-wide: shared
objects cannot honestly be assigned to one realm by summing counters.
Aggregate native/document budgets are F.8.

Destruction disables host dispatch, clears rejection/module references
and realm-owned captured values, then drops the owned context references
and destroys the engine. Keep realm descriptors and host backing valid
through engine finalizers and the reporting-teardown boundary. Free the
additional descriptors with `os64_free` after engine teardown and any
RECLAIM ledger cleanup, then release the primary's enclosing wrapper.
No engine callback may reach a descriptor after that point. Finalizers run no script.
Retained cross-realm functions are part of the destruction tests.

## Public API and compatibility

Proposed names below are the implementation contract for review, not
symbols already exported. Keep public config/outcome structures unchanged.
Advance the wrapper revision in `OS64_JS_ABI_ID` from `/2` to `/3` when
implementing: the context opaque contract and available embedding surface
change. Rebuild matching libjs/binding/consumer artifacts; preserving old
entry-point signatures does not promise mixed-header binary compatibility.

```c
typedef struct os64_js_realm os64_js_realm_t;

os64_js_realm_t *os64_js_primary_realm(
    os64_js_runtime_t *runtime, const char *caller_abi,
    os64_js_outcome_t *outcome);
os64_js_status_t os64_js_realm_create(
    os64_js_runtime_t *runtime, const char *caller_abi,
    os64_js_realm_t **out, os64_js_outcome_t *outcome);
JSContext *os64_js_realm_context(
    os64_js_realm_t *realm, const char *caller_abi,
    os64_js_outcome_t *outcome);
os64_js_realm_t *os64_js_realm_from_context(
    os64_js_runtime_t *runtime, JSContext *context, const char *caller_abi,
    os64_js_outcome_t *outcome);
```

The accessors borrow. Context-to-realm lookup checks membership in the
specified runtime, without treating a foreign context's opaque pointer
as our structure. An ABI mismatch or nonmember context exposes no handle.
As with existing native APIs, callers must supply live handles; this is
not a validator for arbitrary addresses. Context access during a callback
uses a separate outcome and does not enter JavaScript.

The runtime opaque slot stays the runtime; a context opaque slot becomes
its realm. Change the three direct wrapper readers (`module_rejected`,
`output_call`, `console_call`) together, and search the tree again before landing.
No native binding may replace either slot. Do not implement realm selection
by temporarily swapping `runtime->context`: borrowed callbacks, queued jobs
and nested calls make that an unreliable owner.

Add realm-addressed counterparts to the following families. Except for
the checked release described below, replace the first runtime argument
with a realm and retain the existing remaining arguments and outcomes.
Factor shared internals; legacy functions select the primary and retain
their existing entry/setup restrictions. The new realm creation/installers
have the explicit nested-setup exception described below; legacy installers
do not gain that permission by delegating.

| Family | New entry points |
|---|---|
| Source execution | `os64_js_realm_eval`, `os64_js_realm_run`, `os64_js_realm_run_bytecode`, `os64_js_realm_run_file` |
| Task entry/call | `os64_js_realm_task_begin`, `os64_js_realm_call` |
| Installers | `os64_js_realm_install_output`, `os64_js_realm_install_args`, `os64_js_realm_install_intl` |
| Script reporting | `os64_js_realm_set_script_host`, `os64_js_realm_take_rejection` |
| Module provider/handles | `os64_js_realm_set_module_loader`, `os64_js_realm_module_compile`, `os64_js_realm_module_compile_bytecode`, `os64_js_realm_module_requests`, `os64_js_realm_module_run`, `os64_js_realm_module_state`, `os64_js_realm_module_release` |

Checkpoint, task-end and sliced job-drain remain runtime operations:
there is one queue to drain and one open task to close. Cancellation,
budget queries, execution-limit settings, heap counters and destruction
also remain runtime operations. There is no realm-local job drain, fresh
budget for a nested call, or realm-local cancellation in F.1.

A module handle records its realm. Status-returning module operations
refuse a live handle belonging to another realm before using it.
`os64_js_realm_module_release(realm, abi, module, outcome)` returns a status;
release remains usable for cleanup after runtime failure, with null module
a successful no-op. The legacy void release delegates on the primary and
treats an owner mismatch as native API misuse, like release during an
active call. State inspection of a mismatched handle returns UNRUN and,
when supplied, a BAD_ARGUMENT failure outcome. No mismatch removes a
handle from another realm's list.

## Creation, setup and shared turns

Create a realm with language intrinsics and the same capability profile
as the primary: remove SharedArrayBuffer; expose no host output, files,
network or workers. Capture that realm's pristine Promise `then` before
publishing it. A partial construction publishes no handle, frees its own
partial context/references, and leaves no callback pointing at a freed
descriptor. Allocation refusal keeps the runtime's sticky failure policy.
Do not destroy the existing runtime inside a failed additional creation.

Support synchronous creation and initial setup under an existing task:
F.6's initial blank document can be requested while a parent callback is
running. Deferring realm creation until after that callback would not
serve synchronous `contentDocument` access. The entry protocol has three
cases, shared by realm creation and the realm installers:

1. An idle runtime with no pending turn uses a guarded setup turn and the
   configured execution limit, as setup does today.
2. Between calls of an open host task, acquire the entry guard but keep
   that task's deadline, owner and executed-job count.
3. Inside a guarded call, borrow the outer guard/budget using a separate
   outcome. Do not replace the outer active-outcome pointer, update the
   stack limit origin, clear the guard, or finish its turn on return.

An idle continuation with pending jobs outside a host task must drain
before a new realm/setup operation starts. During destruction, refuse
access. A realm may install each facility once, before that realm's first
evaluation, even if another realm has evaluated. Use an installation-in-
progress guard to refuse recursive setup of the same facility while its
property traps run. Preserve output's staged-property rollback and Intl's
failure rules; a setup failure that becomes sticky affects the runtime.

Ordinary eval/run/call entry points retain the existing re-entry rules.
A native callback making a synchronous nested script call uses `JS_Call`
on the appropriate context under the enclosing budget. A host call can
target B during a task opened for A; the task owner remains A. Tasks do
not overlap. Promise jobs from both realms drain in queue order with one
total cap. An exception may leave the runtime reusable; an interrupt,
memory refusal, cancellation or host failure remains runtime-wide and
sticky in this cut. Child-only survival is an F.3 proof, not an F.1 result.

## Service routing and reporting policy

| Operation | Service owner |
|---|---|
| Module resolve/load | Realm mapped from the engine callback's context |
| Compiled module, cached-bytecode read, import metadata, evaluation watcher | Realm recorded on the handle; same URL in A and B can denote separate evaluated modules |
| Console/print | Realm of the installed native function, including when borrowed by another realm |
| Source exception | Realm selected for that source entry |
| Host call exception | Realm selected for that host invocation; the thrown object's provenance is not the reporting destination |
| Host job exception | Realm mapped from the context returned by the job execution |
| Late module failure | Module handle's realm, even if another realm opened the task whose checkpoint surfaced it |
| Promise reject/handle notification | Reporting realm selected by the host routing hook below; native operation context remains separately available |

Pass the actual context through diagnostic, bytecode and module helpers.
An outcome is a receipt for the enclosing operation, not a substitute for
the per-realm script-host notifications. Preserve chronological selection
of late module failures for the single aggregate outcome. Store the
failure's own source information; do not overwrite it with the task's
name or deliver its error hook to the task owner merely because it drained
the queue. The primary-only case must preserve existing outcome behavior.

Each realm owns its loader, script host and output state. Setting or
clearing B's provider leaves A's provider intact. Detach hooks outside an
active call before freeing their opaque, including cleanup after failure.
For synchronous child initialization, the new realm setters also permit
the first installation of a host/loader into an unused, unconfigured realm
under the enclosing guard. They copy the provider without executing script
or altering that guard. Replacement/clearing during an active call remains
BUSY, and the legacy primary setters retain their existing restrictions.
This exception is needed alongside nested creation: a freshly created
context without installable providers cannot become a usable child binding.
An output function borrowed from B keeps B's handle and console nesting;
it shares the runtime deadline while converting arguments.

**Engine context is not an HTML settings implementation.** QuickJS's
native-call path switches to the function's creation realm. HTML's promise
reporting selection considers the running script, falling back to current
settings; job callbacks also carry host settings state. Neither the
promise's constructor/prototype nor the task owner supplies that policy.
See [HTML's rejection hook](https://html.spec.whatwg.org/multipage/webappapis.html#hostpromiserejectiontracker)
and [job callbacks](https://html.spec.whatwg.org/multipage/webappapis.html#hostcalljobcallback).

Provide a runtime-owned, script-free routing callback through
`os64_js_set_rejection_router(runtime, abi, router, outcome)`. Its proposed
record contains `opaque` and a selector receiving the native `JSContext`,
promise and handled/rejected flag, returning a live realm of this runtime
or NULL to suppress reporting. The selector runs while the triggering
stack is available, under the existing guard; it must not execute script,
alter providers, or replace the outer outcome. A returned foreign realm
is a HOST_FAILURE, not a fallback to the primary. Validate membership by
pointer comparison before reading a returned realm. Setting/clearing the
router follows the script-host setter's idle/failed-runtime cleanup rules.

Without a router, select the engine callback's realm. That is libjs's
native embedding default and preserves the current primary-only host;
it is not advertised as cross-realm HTML attribution. F.1's harness proves
both the default and explicit routing, including a native callback whose
context differs from the selected reporting realm. F.4 must implement and
prove the real browser settings selection before exposing cross-realm
browser access. Its proof must include borrowed built-ins, promise jobs,
direct/indirect eval, muted errors and absent running script. This cut
does not infer that bridge from `JS_GetScriptOrModuleName` strings.

Store pending rejection records with their selected realm and host tag;
lookup for reject/handle processing is scoped to that reporting realm and
promise identity. Taking A's oldest record cannot consume B's. Preserve
per-realm order and a runtime sequence for choosing the standalone
diagnostic at a checkpoint. For a handled notification after a record was
taken, dispatch to the policy-selected realm's host; its outstanding
promise bookkeeping determines whether there is an event to emit. Do not
add an unbounded strong-reference registry for already-taken promises.
The selector runs for both reject and handle operations; a native callback
context is not silently replaced by a saved promise-construction realm.

Hosted rejections wait for that realm's take operation; unhosted realms'
records are judged/released at checkpoint exhaustion. Clearing B's host
returns B's untaken records to standalone judging without changing A.
Taking is refused during a call, on failure, or while **any** runtime job
is runnable. Keep existing module-evaluation rejection handling separate
from ordinary unhandled rejections.

## Implementation surface

- `libjs/include/os64/js.h`, `js_engine.h`, `CONTRACT.md`: opaque realm API,
  compatibility wrappers, shared budgets, cleanup and reporting semantics;
  ABI revision and matching examples/contract fixtures.
- `libjs/runtime/runtime.c`: explicit realm parameters in service helpers,
  counted descriptor ownership, construction/setup guard modes, module and
  rejection ownership, and multi-context teardown. No mutable current-realm
  alias used as a provider selector.
- `libjs/intl/intl.c`: audit per-context installation and shared compiled
  bytecode. Reuse the existing process cache; it must not retain a value
  or context from a particular realm. Change it only if that audit finds
  a required correction.
- Runtime host fixtures and reusable guest cases: add the cases below to
  the existing runners rather than inventing another build pipeline.
- DOM/Yonder remain primary-only consumers for this cut. Change call sites
  only where the revised ABI/contract requires it; preserve their existing
  native callback guards and runtime teardown order.

No engine patch is assumed for F.1's routing mechanism. If implementation
finds a required engine change, name its exact behavior and test before
expanding the cut; use the port's patch/manifest mechanism, never edit the
prepared or vendored engine silently. Pre-execution job control and full
browser settings tracking remain explicit later-slice work.

## Required proof

These are acceptance cases for the implementation PR, not tracker rows.
Use two realms A/B and, where stated, an independent runtime C. A harness
may share values between A and B through its native setup; it must not
pass C's JSValues into their engine.

| Case | Required observation |
|---|---|
| Identity and capability profile | Distinct globals/intrinsics; shared runtime; both lack SharedArrayBuffer and uninstalled host capabilities; legacy context remains A |
| ABI and ownership refusal | Wrong ABI, foreign context and module-owner mismatch expose no foreign handle and leave the original owner intact |
| Modules | Same URL compiled in A/B has independent state; different loaders resolve the same specifier differently; dynamic import and import.meta route correctly; clearing B's loader leaves A working |
| Bytecode | One compatible cached source blob can be read into each realm with distinct instantiated values; kind/name checks remain; no compiled module handle crosses realms |
| Reporting | A/B exceptions reach their selected hosts; a B job and B top-level-await failure drained by A retain B's reporting/source identity; first-outcome selection is stable |
| Rejection routing | Interleaved A/B records are taken only by their selected realm; handled-before/after-take and a router selecting A from a B native context follow the defined policy; foreign router result fails safely |
| Mixed host modes | Hosted A and unhosted B do not suppress each other's checkpoint handling; clearing one host leaves the other installed; no report is duplicated |
| Output/setup separation | Distinct handles, group depth, label counters and timers; borrowed console method uses its installing realm; args/Intl installation in B still works after A has evaluated |
| Nested creation/setup | A callback creates and initializes B under A's budget; setup traps cannot recursively reinstall the same facility or overwrite A's active outcome; pending jobs and task-end ownership remain correct |
| Shared turns | A-to-B raw calls, then jobs in both realms, share the original deadline/job cap; wrapper re-entry is BUSY; no checkpoint or realm crossing grants time/jobs |
| Failure policy | Allocation/timeout/cancel/host failure stays sticky for the runtime; later A/B entries refuse; task-end still closes its task; no child-survival claim |
| Allocation and teardown | Sweep additional-context, captured-intrinsic and installer allocations; partial construction publishes nothing; repeat clean create/destroy with cross-realm functions, pending jobs, rejections and module handles; both teardown policies retain their promised behavior |

Run the existing tools on the implementation and its base, recording exact
revisions and outcomes. The runtime host script exercises the target engine
and its separately instrumented build. Keep ASan/UBSan and leak detection
enabled. If sandbox attachment denies LSan, diagnose with
`LSAN_OPTIONS=verbosity=1:log_threads=1`, preserve the built binaries, and
use the normal outside-sandbox approval path with
`ASAN_OPTIONS=detect_leaks=1`. The runtime runner currently removes its
temporary directory unconditionally; give it a failure/keep-artifacts path
when needed to reuse those binaries. Do not turn a leak report into a skip.

```sh
tools/test_js_contract_headers.sh
tools/test_js_runtime_host.sh
tools/test_js_cli_host.sh
tools/test_dom_host.sh
tools/test_yonder_scripts_host.sh
```

The runtime/DOM/Yonder scripts above are the direct regression evidence.
Guest validation must run the reusable realm cases through
`js-runtime-test` in QEMU; host execution alone does not establish the
guest ABI/stack behavior. F.1 has no new visible frame behavior requiring
a P5 gallery campaign. Subsequent host-enablement slices retain their P5
acceptance.

For WPT, retain the existing required/pass baseline in
`tools/wpt/expected/html/webappapis/scripting.txt` and
`tools/wpt/expected/html/semantics/scripting-1/the-script-element/module.txt`
at the runner pin `a699fb16767be39ba3e61a8e8b6bef60383f08ee`. These are
regression sets, not newly enabled cross-frame coverage. Run both sets
before and after implementation; record attempted/pass/fail/excluded counts
and explain changes. Do not refresh expectations to conceal regressions.
Fable reviews this selection alongside the native matrix. A missing pinned
checkout is missing evidence, not zero failures. The brief inspection did
not fetch or execute WPT and supplies no new counts.

```sh
tools/test_yonder_wpt_host.sh html/webappapis/scripting
tools/test_yonder_wpt_host.sh html/semantics/scripting-1/the-script-element/module
```

## Review and landing

Fable's design read should settle the explicit service/turn ownership,
nested setup protocol, routing-hook boundary with F.4, rejection lifetime,
and both teardown paths. Confirm this is still one reviewable implementation
cut; if its proof requires a split, revise the slice map before coding.
The libjs owner reviews the API/ABI and failure contract. This brief does
not claim their approval. The child-only interrupt-soundness approval is
still an F.3 gate.

Keep `F.1` at `design` while this proposed brief awaits its read; move to
`design-review` when that review starts, then follow the tracker contract.
The documentation PR, if separate, does not mark the implementation done.
Its final implementation PR carries the applicable tests, as-built notes,
tracker completion and PR number. Single-document behavior remains the
shipping browser path throughout F.1.

Useful references: [iframe design](IFRAME.md),
[tracker contract](IFRAME_TRACKER.md),
[libjs contract](../../../userland/libjs/CONTRACT.md),
[QuickJS runtime/context model](https://bellard.org/quickjs/quickjs.html#Runtime-and-contexts).
The port's pinned source is authoritative for its actual callback behavior;
the external specifications define the browser behavior the later host
integration must prove.
