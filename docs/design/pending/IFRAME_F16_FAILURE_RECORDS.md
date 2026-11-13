# F.16c1 — terminal failure records

Source baseline: merged engine foundation #573, `f73ddd56403ee0fa708e55111f25e364e7f6e552`.
This splits the [F16c protocol cut](IFRAME_F16_RECOVERY.md) at its diagnostic
boundary. It supplies first-failure facts and later global-cause observation;
recovery admission, generation tokens and cleanup/commit remain subsequent
work. Runtime-wide terminal failure remains the contract for these APIs too.

## Interface and lifetime

`os64/js_failure.h` adds:

- `os64_js_create_with_failure_records`: the existing configuration, teardown
  policy, ABI and publication rules, with scope tracking enabled before exposure.
- `os64_js_observe_failure`: owner-thread observation inside native callbacks or
  after failure/task closure. OK means a record was copied, not reusable health.
  It validates runtime/ABI/output before polling; refused output is untouched.

A fixed record is reserved in the wrapper's charged allocation. The engine,
public existing layouts and ABI 5 are unchanged. Default constructors leave
tracking disabled. No production DOM/Yonder consumer uses the new constructor.
No allocation or GC root is added by capture/observation. Handles borrow stable
wrapper slots until destruction; the observer refuses destruction before roots
are released. Record storage must be separate from active outcomes.

## Facts captured before unwind

The first wrapper latch freezes `original`, the armed deadline, task owner,
explicit/implicit task flags and executed/discarded counts. No subsequent latch
replaces them. A failure outside a turn has zero owner/deadline/counts, even
when an earlier completed task ran jobs. Final task-close accounting remains in
the existing outcome. The failed-task close path also reports the actual pending
queue; it previously omitted `jobs_pending` despite retaining that queue.
The task owner can name a private setup slot that was never published. That
diagnostic pointer grants no membership, settings or execution authority.

Capture visits the engine's stack-local scope records into native scratch and
publishes after the visit returns. Membership scans the wrapper table without
trusting context opaque storage. It deduplicates up to 65 code-candidate realm
handles, innermost first. The 65th slot does not require a single-word bitmap.

A raw call's incoming context and the outer task owner do not create an author
frame. Native function definition realms are checked for membership but are not
added as abandoned author code. Registered UNKNOWN code candidates can appear
in the set, with their uncertainty retained. Primary author frames are recorded
when present; a classified set containing primary code is not recoverable proof.

Wrapper job execution, callback and activity provenance are captured separately
while its body and engine value cleanup run. Job callback ownership does not
become author-code identity. Raw-only job dispatch carries the engine's job/unknown
flags without wrapper provenance. A nested raw job retains the enclosing
wrapper dispatch's facts; those do not identify the nested raw job. Cap exhaustion between jobs records
consumption without assigning the failure to the next unexecuted queue head.

`scope_issues` exposes missing/unknown scopes, foreign contexts, native/job
scopes, candidate/setup transactions, checked native settings bodies and capacity
failure. `scopes_classified` means those observed scope facts were classifiable;
it certifies neither complete affected-environment coverage nor native safety.
Eval/module preparation and job scopes remain UNKNOWN. Hidden/native algorithm
state still needs its own audit. No context is automatically marked failed by
these diagnostics.

## Later global causes and unchanged admission

`global` preserves the first observed non-EXECUTION cause: JOBS, SOURCE, memory
refusal, OOM, cancellation or host failure. EXECUTION does not suppress later
cancellation/allocator observations or a negative clock read, including after
turn closure. Clock polling continues after a timeout while no global cause is
recorded; healthy idle observation does not sample a new deadline. Health is
polled on the owner's existing observation boundaries and explicit observer,
not by a background monitor. A cancellation arriving after a poll remains an
atomic signal for the next observation.

The original first-failure status still controls wrapper outcomes and admission.
Closing a timed-out task returns its original LIMIT/EXECUTION even when later
health is CANCELLED or HOST_FAILURE. First global cause, original timeout and
first-observation counters are independent. JOBS remains global; queues are
neither drained nor reassigned to a new budget by observation. Ordinary engine
exceptions and stack overflow do not acquire a structured sticky cause by text
matching. Legacy constructors preserve their failure precedence and clock
behavior, with the corrected pending-queue report on failed task close.

An empty global cause does not authorize execution. These APIs supply no reset,
containment acknowledgement, generation token or recovery commit. Existing
`os64_js_quiescent` still refuses a failed runtime. Starting a sibling task or
changing its budget still returns FAILED_RUNTIME.

## Next protocol boundary

Before recovery can be enabled, introduce the distinct generation-bound native
barrier, reconcile original/final task accounting, and prove the affected set's
completeness. Include setup, checked settings and captured callback lifetimes
in native exclusion. Prevent raw healthy-code reentry while containment is
pending, recheck global health before commit, and retain global fallback for
root/unknown/native/Streams cases or surviving unproved engine jobs. DOM/provider
consistency, host routing and guest/WPT/hardware acceptance remain separate.

## Validation

Prepared engine identity remains:
`da43badc52c71c121b1be56f8f08077531c31fcacdb9a82b6751f6baad739b94`.
No engine patch or manifest hash changes. Wrapper/runtime fixture sources are
rebuilt; matching shipping and ASan engine objects are reused. DOM/native
source and public existing layouts are unchanged, so their matching objects
are reused with the rebuilt identity-bearing wrapper.

```sh
ASAN_OPTIONS=detect_leaks=1 CCACHE_DISABLE=1 \
JS_RUNTIME_FOCUSED_SELECTOR=--failure-records \
bash tools/test_js_runtime_host.sh
```

The focused matrix passes **819 checks per engine profile**, covering both
FATAL and RECLAIM teardown. It checks mixed B/C and primary author stacks,
idle incoming contexts, spoofed foreign opaque data, native budget observation
before return, eval uncertainty, frame-free checked settings and staged setup,
raw/wrapper job provenance, neutral queue heads, slot 65, pending exception
identity, allocation-free capture/observation and terminal admission. Later
cancellation, allocator limit, OOM and negative clock reads preserve the
original verdict while latching the first independent global cause. A failure
after a completed task cannot inherit its job count.

| Check | Shipping | Fully instrumented |
|---|---:|---:|
| Complete runtime | 16,340 | 63,688 |
| Existing containment | 244 | 244 |
| Execution scopes | 119 | 119 |
| Checked settings bridge | 769 | 769 |
| Settings scopes | 724 | 724 |
| Native-call facts | 324 | 324 |
| Global-this | 67 | 67 |
| Compiler refusal | 40 | 40 |

Zero fixture failures and zero live allocations. Shipping raw construction and
both fatal/reclaim invariant matrices pass. Full shipping DOM passes **322,022
checks** and **12,941 native allocation cuts**. Instrumented routing **678**,
retirement **9,770**, departure **353**, Streams **104** and child installation/
engine budgets **1,306** pass. The measured LP64 wrapper reservation grows by
**640 bytes**, charged against the configured ceiling; existing measured child
admission counts remain unchanged. Hardware browsing acceptance is not claimed.

Thirteen source mutations are caught by named assertions on **both** profiles:
caller/code confusion, ignored foreign membership, job owner treated as author,
suppressed late clock reads, lost global cause, rewritten original cause,
invisible settings/setup bodies, lost wrapper-job provenance, dropped slot 65,
omitted failure-close queue state, disabled scope tracking and inherited counts
from a closed turn. Each exits with fixture failure; sanitizer crashes or leaks
are not counted as a successful mutant oracle.

ASan/UBSan and LeakSanitizer remain enabled. Leak-enabled execution uses the
supported environment outside the sandbox attachment restriction. Target
library/consumer links and both new exports pass; standalone host/cross-header
syntax, six preparation tests, teardown audit, tracker, shell syntax, diff and
stale-reference checks pass. The earlier scope prose that said libjs never
enables tracking is corrected for the opt-in constructor. These tests establish
terminal diagnostic behavior, not a recovered browser or guest/WPT acceptance.

## Merge-forward qualification: captured callbacks

PR #576 advanced `userland` to `60357e71`. The resolution retains both
`js_failure.h` and `js_callbacks.h`, and accepts both `--failure-records` and
`--callback-settings` in the focused shell allowlist. Both selectors and both
lanes in the complete realm matrix remain present. Callback capture/invocation
still use checked settings admission; clear/mark preserve their failure,
retirement and finalizer ownership rules. No F16 failure policy changes in
this merge.

The prepared engine, patch hashes and private wrapper layout are unchanged.
The wrapper and runtime fixture sources are rebuilt against both APIs;
matching engine objects are reused. Unchanged DOM/native objects are relinked
with the rebuilt identity-bearing wrapper. The full shipping DOM allocation
campaign and 13-mutant totals above qualify the preceding F16 record cohort;
those campaigns are not repeated for this include/selector resolution.

Both profiles pass failure records **819**, captured callbacks **197**, checked
settings bridge **769**, containment **244**, execution scopes **119**, settings
scopes **724**, native-call facts **324** and global-this **67** checks. Complete
runtime passes **16,536 shipping checks** and **63,884 instrumented checks**,
zero failures and zero live allocations. Compiler refusal (40 each), shipping
raw construction and both fatal/reclaim invariant matrices pass with leak
detection enabled outside the sandbox.

Both DOM profiles pass routing **678**, retirement **9,770**, departure **353**
and Streams **104**; child installation/engine budgets pass **1,402 shipping**
and **1,306 instrumented** checks. Target library/consumer links, header/manifest,
tracker, shell syntax, diff and stale-reference checks pass. The prior preparation
and teardown audits continue to qualify the unchanged prepared engine.
