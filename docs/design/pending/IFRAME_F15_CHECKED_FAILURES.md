# F15 — pointer-free checked failure observations

Proof baseline: merged `userland` `b1ff4125` (#645/#646), embedding ABI 5,
bytecode ABI 6, engine digest
`3e9a81ec0428aaf6456af21c9d8cf5b152793b747ebb503bd9cc86ff5a91afd4`.
This follows [scoped access](IFRAME_F15_SCOPED_ACCESS.md) and the failure-observer
migration in the [export-lifetime proposal](IFRAME_F15_EXPORT_LIFETIMES.md).

## Implemented private boundary

`js_owner_runtime_create_with_failure_records` explicitly composes charged owner
admission with existing terminal failure recording and execution-scope tracking
before returning the runtime. It enables no recovery/reset or public checked
profile. Existing constructors keep their selections and raw-export contracts;
`js_owner_runtime_create` keeps failure recording disabled. There is no extra
capture registry, arena reservation or wrapper field. The wrapper remains
**13,080 bytes**, and the old and diagnostic selected paths have matching
requested/charged metadata, wrapper charge and engine allowance in the fixtures.

`js_owner_runtime_observe_failure` copies original/global causes, deadline/counts,
original engine scope facts and task/code/job provenance into bounded scalar
storage. Each carried native reference becomes an immutable owner ID plus:

| Reference state | Meaning |
|---|---|
| `NONE` | No reference was carried; the ID is zero. |
| `ASSIGNED` | The original retained native slot was admitted and mapped. This is not current lifecycle/execution admission. |
| `UNPUBLISHED` | A consumed construction reservation has an ID but no assigned usable realm. It grants no ticket. |
| `UNMAPPED` | A non-null reference has no unambiguous admitted/reserved identity. The ID is zero. |

`owners_mapped` covers references actually carried by the record. It is separate
from `scopes_classified`, foreign/unknown-code issues and native safety. A mapped
record can still have incomplete engine attribution. Neither bit authorizes
execution, containment, recovery or release. Code candidates retain their original
deduplicated set/order; job execution, callback and activity remain separate roles,
including neutral absence. No native defining context is substituted for author
code identity.

## Lifetime and observation rules

Conversion reuses the existing cumulative ID-to-slot map. The selected diagnostic
runtime has global terminal failure semantics and no recovery. Once a failure
contains references, later admission cannot assign or reuse their slots. Assigned
and consumed unpublished rows remain retained. Before failure, a healthy record
has no references. Missing or ambiguous mappings stay explicit unknowns; no
address-derived or replacement ID is invented. The observer refuses recovery-enabled
runtimes rather than extending this invariant to resetting failure episodes.

The converter compares retained native addresses without dereferencing a record's
borrowed realm pointer. It publishes no pointers, contexts, values, tickets,
loans or recovery tokens. It uses bounded native stack storage, with no allocation,
engine entry, author execution, exception consumption, queue draining or deadline
renewal. Health polling follows existing precedence: a late global cause can be
observed after an EXECUTION timeout while original causes/identity/counts stay
frozen. Invalid, disabled and destroying calls preserve output and poll no health.
Caller-owned output must be distinct from runtime, metadata and active outcomes.

Copied diagnostic data needs no holder and remains readable after ticket close
and runtime destruction. IDs retain the issuer's
[process-local loaded-library namespace](IFRAME_F15_INCARNATION.md); copied data
is not a lookup or a new lifetime promise across unloading/process boundaries.
No owner resources are released by a snapshot or by dropping one.

The legacy observer remains unchanged: pointer-bearing records retain their ABI 5
borrowed lifetimes and record sticky compatibility retention. The checked observer
converts the internal record directly; it does not call the legacy export path or
silently shorten its promises. Zero holders or an unset veto is not a release
certificate. No DOM migration, per-owner graph/native release, slot reuse or C3
security/target/publication contract is introduced. F15 remains in progress.

Quinn agreed the boundary before implementation. Runtime edits are confined to
private construction and scalar conversion adjacent to existing failure APIs.
Capture (`failure.inc`), job dispatch/reporting, recovery transitions and DOM/browser
integration are unchanged. Her F4b browser-job milestone remains independent.

## Validation

- Focused host: **401 shipping / 4,745 instrumented checks**, zero failures.
  Actual selected-diagnostic child-admission sweeps cover **27 / 570 allocation
  cuts per root policy**. Refusal preserves output tickets and the consumed
  unpublished identity; root cleanup balances native metadata and engine roots.
  First wrapper/metadata/engine constructor cuts publish no runtime.
- Real three-realm jobs prove task A, execution C, callback B and activity A
  remain distinct; neutral callback/activity stay absent. Author-code B is not
  replaced by native definition C. Native observation attempts no allocation.
  Late clock failure advances the global cause without replacing frozen facts.
- Shared host/guest cases cover healthy/disabled/invalid observations, in-callback
  holder close and cancellation, independent loan preservation without legacy
  escape, root-finalizer refusal, legacy raw-pointer retention, retired identity,
  copied data after destruction and rejection by a new runtime incarnation.
  An owner-capacity refusal with setup timeout proves an unreserved slot stays
  `UNMAPPED`; failed engine construction stays `UNPUBLISHED`.
- **18 named behavioral controls** reject substituted primary IDs, published
  failed rows, false mapping completeness, conflated activity/callback roles,
  omitted code candidates, accidental raw export, overwritten original causes,
  invented absent references and omitted execution tracking. Compilation failures
  and invariant aborts do not count as sensitivity evidence.
- Actual cross-built Os64 consumer: **722 checks**, target heap integrity/balance,
  exact parent-observed exit **`0x4f520600`**, complete PASS. Matching production
  objects and five-file library closure, source/build-input hashes, command and
  serial output are frozen. Source disk/boot images and accepted P5 cohorts are
  preserved. This is no new browser/P5 acceptance campaign.
- Default CLI **199 + 40**, shared test262 builder **4/4**, strict target build,
  public header/symbol/production-link (**235 engine / 87 runtime exports**),
  source-manifest, teardown inventory and wrapper-layout checks pass. This PR
  adds no public exports or engine patch. Sanitized runs keep ASan/UBSan and
  LeakSanitizer enabled outside the sandbox.

Full runtime: **38,723 shipping / 132,373 instrumented checks**, zero failures
and live allocations, including existing failure/recovery regressions, compile-
failure cuts and fatal-invariant paths. Tracker status remains `code`.

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 \
    JS_RUNTIME_FOCUSED_SELECTOR=--owner-failures bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_failures_mutations.py \
    --artifacts <retained-runtime-artifacts> --output <fresh-output-directory>
```

Negative controls demonstrate oracle sensitivity, not positive sanitizer/leak
evidence. Scope-classification and mapping results describe diagnostics, not a
native safety or reclamation proof.
