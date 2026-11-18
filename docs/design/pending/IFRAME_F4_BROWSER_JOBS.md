# F.4b — browser job selection and reporting

This milestone connects the deferred cleanup queue to Yonder's task selection
and defines the embedding policy for abrupt jobs with no reporting realm.
It supplies explicit native opt-ins; ordinary Yonder configuration leaves them
off. F.4b's remaining binding/security acceptance and F.4c/d still precede
publication of frames.

## Reporting without invented authority

`os64_js_enable_browser_jobs` composes neutral Promise settings, constructor
capture and deferred FinalizationRegistry tasks before execution. For a prepared
neutral job with neither entry nor reporting realm, the wrapper retains the
exception diagnostic in its outcome but calls no realm's exception hook.
The context used to inspect/free the thrown value is not a reporting global.
The checkpoint preserves its first exception outcome and continues runnable
microtasks; sticky failures still take precedence. Manual drains return after
an exception and leave subsequent work for the next drain.

This is an explicit Os64 policy for an unresolved interoperability case, not a
claim that HTML mandates this answer. [HostEnqueuePromiseJob](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob)
and [WHATWG issue 10526](https://github.com/whatwg/html/issues/10526), checked
2026-11-17, leave the reporting global unclear when an empty-handler reaction's
custom capability throws. No queue/settlement/capability realm is promoted into
security authority. Prepared neutral host continuations use the same policy.
Other runtime opt-ins retain their reporting behavior. Generic unprepared
native jobs retain their existing destination; deferred registry tasks report
to their registry, including bootstrap creation without an incumbent.

## Binding composition and installation

`os64_dom_cluster_create_with_browser_jobs` combines this runtime policy with
the established callback settings. `os64_dom_create_with_browser_jobs` supplies
the same composition to a standalone binding without adopting its document or
page state. Its caller keeps the existing drain/engine-destroy/binding-free and
layout-borrower obligations. A private binding flag and `d_settings` select the
existing conversion/capture implementations; cluster family opt-ins keep their
independent selections.

Standalone installation opens a bounded task and closes it before returning.
A failed installation publishes no binding, closes its task and releases its
native records; the caller must retire the runtime as with ordinary failed DOM
construction. Install unused-realm facilities such as console and Intl before
this constructor. Yonder's opted-in construction does so, then installs the
binding and providers. The default construction order is preserved.

## Browser scheduling

The native `yonder_scripts_options_t.browser_jobs` option selects the new
constructor. It has no preferences or command-line switch. Both the shown-page
selector and loading/coming-page selector poll deferred cleanup as runnable
work. Their shared host adapter alternates one cleanup visit with an opportunity
for ordinary ready work. If no ordinary work is ready, cleanup can run again;
it does not wait for an idle callback grant. History events retain their order
within the ordinary sources. Existing rendering and idle scheduling remain
separate from engine cleanup tasks.

`os64_js_finalization_task_begin` admits one registry-owned task and leaves it
open. A true `begun` obliges the host to close it even after failure; an empty
queue opens none. `os64_dom_finalization_task_fire` uses that split to settle
queued DOM errors, rejections and their microtasks through `d_task_finish` before
closing the original task. The existing complete runtime dispatch API retains
its own checkpoint/close behavior. This avoids deferring error events to an
unrelated future task or granting their handlers a fresh budget.

A visit consumes one queued cleanup entry and its microtask checkpoint. The
runtime's task deadline, job cap, cancellation and lifecycle admission apply.
Reentrant BUSY is not counted as a task. Cleanup outcomes use the browser's
normal diagnostics, storage settlement and sticky-failure retirement path.
The wake predicate includes deferred work, while a retired host exposes none.
There is no new thread, timer, queue allocation or reclamation permission.
This policy offers progress between cleanup and ordinary sources, not the
future F.8 aggregate fairness proof across a frame tree.

## Compatibility and validation

Wrapper ABI 5, bytecode format 6 and the prepared engine are unchanged.
The runtime wrapper remains 13,080 bytes; its policy bit fits existing storage.
Rebuild native header consumers with the additive API. Engine/global failure
classification and F16's recovery subset are unchanged.

Shared host/guest runtime tests cover manual drains, explicit checkpoints and
task-end checkpoints, independent realms and both teardown policies. They
preserve the old opt-in's queue-context reports and generic native-job reports,
and verify missing entry is not fabricated. DOM tests exercise standalone
callback families, sampled early/late binding allocation refusals, and the
cluster constructor with foreign registry/callback/incumbent roles. Yonder's
actual host loop tests cleanup/ordinary-source progress, consecutive cleanup
without ordinary work, exactly-once callback/microtask/error delivery and neutral
job diagnostics. The DOM budget counterexample uses a two-job cap: cleanup and its first
microtask exhaust the cap, its error handler runs, and that handler's microtask
refuses without a new budget. The host runner also includes the shipping media-session
translation unit, which its source list had omitted.

Focused runtime passes **4,459 checks in each allocator profile**; DOM passes
**801 in each profile**, including eleven sampled binding allocation failures
from 1,263 construction points. These samples include an optional allocation
whose refusal still permits successful construction. Full Yonder host passes
**26,035 checks**; the final DOM pending-query adjustment is covered by the
**92-check** focused Yonder rerun. No failures or sanitizer/leak reports occur.
Shipping Yonder, CLI and runtime consumer builds pass, as do source preparation,
cross-header checks and **235 engine / 89 runtime export** audits. ASan, UBSan
and LeakSanitizer remain enabled. No guest execution or P5 acceptance is claimed.
