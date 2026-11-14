# F.16d — native DOM recovery policy

Implementation PR: **#593**, source `0d23ad19`.

This chapter adds an opt-in libdom policy over the libjs protocol from #582.
It contains an attributed child EXECUTION episode after complete engine unwind,
settles native activity behind the execution fence, and permits independent
healthy work after commit. Yonder retains terminal handling. F.16 remains in
progress; browser routing, guest/WPT acceptance and Streams unwind are separate
obligations. Failed slots and backing remain charged until root destruction.

## Construction and supported boundary

`os64_dom_cluster_create_with_recovery` returns a paired runtime and cluster.
Native policy storage exists before engine exposure and outlives the runtime.
The caller admits documents through the existing cluster interfaces and owns
the runtime. Failure publishes neither handle. Teardown remains: stop delivery,
release native borrowers, drain cluster, destroy runtime, free cluster.
Default and timer-settings constructors do not select recovery.
The new constructor does not enable checked host settings.

Libjs still requires a completely attributed child-only EXECUTION episode,
closed failed task, idle engine, no queued engine jobs, rejection bookkeeping,
wrapper module handles or pending exception, and no global failure. Primary
author frames, unknown/unbound code, native callback/settings/setup/GC/job/
module/resume scopes, JOBS/MEMORY exhaustion, cancellation and allocator/clock
failure retain global fallback. A primary incoming context alone is neutral.
Every affected realm must have a live installed member in this cluster.

Streams are excluded by native evidence, without executing a JS probe during
cleanup. Private Readable/Writable/Transform record constructors call a captured
native hook in recovery-enabled clusters. Private native-to-Streams bridge use
also sets the sticky cluster flag. Any such use in any member requires root
cleanup, even when no engine jobs are pending. Installing modern interfaces
alone does not set the flag. Legacy clusters receive no callable hook and
retain their ordinary Streams behavior. This chapter certifies modern DOM
with unused Streams, not the consistency of interrupted Streams algorithms.

## Preparation and publication

`os64_dom_cluster_recover` calls libjs prepare before touching its outcome.
The wrapper protects an enclosing borrowed outcome and rejects active work.
Libjs fences raw executable entry and marks affected engine contexts and
wrapper realms failed/retired before calling the DOM policy. The policy records
the exact generation and pauses delivery across the cluster.

Before cleanup it checks cluster provider loans, dispatch, member walks,
reaction frames, pointer/animation/observer activity, and borrowed animation
layout generations. BUSY retains the fence, providers and backing; it grants
no opaque release permission. Pure native release followed by retry uses the
same generation. Every affected member is marked finishing before invoking
providers, so recursive finish/recovery cannot acknowledge work beneath them.
Callbacks return normally in C and cannot run healthy or failed author code.

The policy checks native loans again after provider callbacks. A layout loan
acquired during cancellation leaves preparation BUSY without publishing a
finished member. Repeated cleanup does not repeat final storage settlement or
request cancellation. Only completed cleanup detaches the remaining providers.
The coordinator checks loans before committing a separately prepared token.
Raw preparation alone does not resume DOM delivery; the coordinator publishes
healthy activity only for the matching completed and committed generation.
Repeated acknowledgment preserves final abandoned-task counts.

Late cancellation, clock/allocator/memory failure, new jobs or exceptions
observed by libjs prevent commit and retain root fallback. A failure return
never grants permission to free provider opaque storage, including after
partial native cleanup. No old task resumes, deadline/count renews, engine job
drains, or allocator/global signal clears. A later independent task uses normal
admission. A second child failure receives a fresh generation and affected set.

## Native state audit

| State | Preparation and retained ownership |
|---|---|
| Storage | One native final settlement while producer opaque is valid. First negative provider status is reported; serialization OOM is host error -3. Failure refuses certification. Changes/provider routes detach after the attempt; retained maps release at root. Ordinary retirement/root behavior still ignores final-settlement status. |
| Fetch/XHR | Unlink tokens before cancel through their original producer. No author abort/error delivery, Promise rejection or Streams settlement. Healthy sibling requests stay pending. |
| Timers, idle, rAF, messages, reports | Release obsolete affected callbacks/arguments/payloads. Healthy native task order survives. A failed cross-realm interval is removed from a healthy activity queue. |
| Events | Retain listener identity and registration data. Skip sealed callback contexts before event activation, then continue healthy listeners on adopted nodes. Draining a failed allocation owner would incorrectly erase healthy listeners and is not used. |
| Custom elements | Keep definitions and native records; discard payloads for inactive current activity documents. Temporary cluster pause is distinct from permanent activity eligibility. A retained failed registry can carry a healthy callback; use the healthy incoming context when the definition context is sealed, then check activity/callable eligibility before delivery. Live definition contexts retain their existing routing. |
| Mutation observers/slotchange | Clear affected payloads/transient registrations, retain registrations and shared registry order. Skip inactive registrations/slot owners before context borrow; a failed first member cannot stop later healthy deliveries. Native owed between-task queues are distinct from pending engine jobs. |
| Intersection/resize/performance | Clear affected delivery payloads; release resize observer holds. Registry backing remains through root. Observer finalizers mark dead records rather than unlinking the native walk. |
| Canvas/animation | Retain pixels, animation/effect objects and layout generations. Stop affected timeline activity without author settlement. Cluster-wide borrowed layout floors exclude preparation and commit. |
| Subtle crypto | Discard affected native queued payloads without executing continuations. Pending engine jobs already prevent admission. |
| Provider routes | NULL-only failed-realm module/script-host detach during guarded PREPARING. Clear the global-miss native callback before requesting a failed context; a retained engine trampoline sees no host callback. Detach option, geometry, image/text, history, cookie and observer providers after native exclusion. |
| Runtime output | Separately granted console/output descriptors remain engine state with their existing host lifetime; DOM provider completion does not transfer or release them. Failed executable aliases remain sealed. |
| Backing and budgets | No child document/state/anchor/context slot is freed or reused. Existing aggregate/local charges and root teardown order remain authoritative. Partial shared data and native mutations survive. |

The script-host/module-loader setter exception is restricted to NULL services,
recovery enabled, PREPARING, and an already failed realm. Healthy siblings and
other active phases remain BUSY; replacement services still fail retirement
checks. The native implementation requires no engine patch, settings identity
change, public config/outcome layout change or libjs ABI increment. Its original
engine identity was
`eb3773e6dac0a1b7f7d107384db954271905aa696bbf22def1a393b23866cce9`.
The #591 composition below records the integrated engine identity.

## Regression and validation receipt

`tools/test_dom_recovery.inc`, selected with `--recovery` and included in the
normal DOM matrix, uses real three-member DOM clusters under both teardown
policies. It exhausts child bytecode, preserves a mutation before interruption,
and asserts that abandoned finally/callbacks never run. It checks provider
reentry/fences/borrowed outcomes, storage and original-producer cancellation,
freeing successful failed-provider opaque storage, retained global-miss routes,
adopted listeners, retained Canvas pixels, observer/animation holds, shared owed
custom/observer order, healthy callbacks in failed registries, native queue
order, pre/post-provider loans, layout/reaction frames, separately prepared
tokens, multi-child and repeated generations, primary/unbound fallback,
Streams use, storage errors, new jobs/exceptions and late global health.
Constructor failure sweeps cover every allocation on each engine profile;
root reports require zero leaked engine/native allocations under both policies.

Two real regressions were reproduced before fixes: failed EventTarget callback
context lookup stopped a healthy adopted listener, and the shared observer walk
stopped at an inactive failed registration. The latter regression also led to
the sibling custom-definition/context and slotchange audit above.

Reproduce with leak detection enabled (outside an attachment-restricted sandbox
when required):

```sh
DOM_HOST_KEEP=1 bash tools/test_dom_host.sh --recovery
JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
bash tools/test_js_contract_headers.sh
python3 tools/test_js_prepare.py
python3 tools/test_js_teardown_audit.py
CCACHE_DISABLE=1 make -C userland -j8 js-runtime-test dom-library
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
bash tools/stale_refs.sh
```

Retain the canonical DOM artifacts, run `target-core-probe` without a selector
for broad shipping-engine coverage, then run affected selectors on both
`target-core-probe` and `sanitized-core-probe`: `--recovery`, `--timer-settings`,
`--callback-reports`, `--realms`, `--realm-retirement`, `--departure`,
`--stream-realms`, `--child-install`, `--transport-retirement`,
`--visitor-retirement`, `--custom`, `--observer`, `--events`, `--resize`,
`--intersection`, and `--animation-retirement`. Set
`ASAN_OPTIONS=detect_leaks=1`; retain ASan/UBSan on native/wrapper code and ASan
on the fully instrumented engine. No guest, WPT or P5 recovery acceptance is
claimed by this native chapter.

Validation on the #582 integration base `59545734`, with the unchanged engine
identity above:

- Final focused DOM recovery: **2,516 shipping / 6,660 instrumented checks**,
  both teardown policies; **59 / 1,095 constructor allocation cuts per policy**.
- Full runtime: **22,056 / 69,404 checks**, zero live allocations; parser failure
  checks and fatal invariant/exit-badge checks pass on both profiles.
- Broad shipping DOM: **325,068 checks / 12,938 allocation sweeps** before the
  final retained-definition context-routing refinement and timer/private-bridge
  fixture additions. All affected shared dispatch/lifecycle selectors are
  rerun on both profiles for the final source; the broader selector list above
  also passes on both profiles before that refinement.
- Manifest/preparer tests, host/cross headers, target libdom/runtime builds,
  target teardown audit, exported paired constructor/coordinator symbols and
  tracker/stale-reference checks pass. Leak detection remains enabled.
- Ten deliberate defects are rejected by named assertions on both profiles:
  ignored provider/layout/post-provider loans, certified storage/Streams failure,
  wrong native generation, dropped healthy reactions, and failed definition,
  observer or listener context lookup stopping healthy siblings. Mutants compile
  in isolated source directories against the coherent engine/native objects;
  compilation failure is not counted as detection.

## Composition with merged F4 listener settings

The merge forward incorporates #591 through `d014f77c`. Conflict resolution
retains both the recovery and listener-settings test selectors/default matrix
calls, and both clusters' policy flags. The paired recovery constructor still
does not enable checked listener settings. Event dispatch keeps the early
failed-context skip that preserves later healthy adopted listeners, together
with F4's captured entry/incumbent scope, post-getter eligibility checks and
uncatchable terminal-exception handling.

Patch 0031 is imported unchanged, SHA-256
`8926970d5204a3ceca655888b82b25ccbbcbc2dc358e55670a163f5f3e591b47`.
Patches 0029/0030 retain their original hashes. The composed engine identity is
`3be09416c3df09ee3c4d85b8fe54af3d8f90d24ed32678042f8a7ca67ac4aa39`;
ABI 5 is unchanged. Engine object creation identity and native listener layout
require rebuilding engine, wrapper and DOM objects together. The composition
does not broaden native recovery admission or claim browser/guest acceptance.

Fresh engine, wrapper and DOM cohorts pass with ASan/UBSan and LeakSanitizer
enabled under both teardown policies:

| Integrated check | Shipping / instrumented |
|---|---:|
| Native DOM recovery | 2,520 / 6,660 |
| Paired-constructor allocation cuts per policy | 60 / 1,095 |
| Complete runtime, zero live allocations | 22,014 / 69,404 |
| Libjs child recovery / raw recovery barrier | 4,637 / 4,637; 505 / 505 |
| F4 listener settings / timer settings | 1,736 / 1,736; 718 / 718 |
| Events / callback reporting | 1,020 / 1,020; 1,118 / 1,118 |
| Shared custom elements / mutation observers | 298 / 298; 32 / 32 |
| Realm routing / retirement queues | 678 / 678; 9,770 / 9,770 |
| Departure / Streams / child installation | 353 / 353; 104 / 104; 58 / 58 |
| Transport / visitor retirement | 1,084 / 1,084; 972 / 972 |

Terminal records and the remaining settings/failed-code/scope selectors pass
on both profiles. Parser failure and fatal/reclaim invariant/exit-badge checks
also pass. Omitting the early failed-listener skip produces four named adopted
listener regression failures per profile, despite F4's later settings guards.
These isolated mutant runs validate the preserved gate without changing it.
Target libraries, headers, six source-preparer tests, teardown audit and
tracker/stale-reference checks pass; target link audits find 217 engine and
78 wrapper exports with the expected imports.

Reproduce the composition using the canonical build commands above. After
`--recovery`, run `--listener-settings` and the affected dispatch/lifecycle
selectors on both rebuilt DOM binaries. Run the full canonical runtime suite,
then both runtime binaries with `--child-recovery`, `--recovery-barrier`,
`--failure-records`, `--callback-settings`, `--settings-bridge`,
`--current-settings`, `--failed-code-only`, `--execution-scopes-only`,
`--settings-scopes`, `--native-call-contexts`, and `--global-this`.
`python3 tools/test_js_port_target.py` audits the target link/import surface.
The initial chapter's broad DOM and ten-defect evidence remains qualified by
its earlier engine identity; this receipt establishes the affected composition.

## Composition with merged F4 scheduled callbacks

The merge forward through `2415f60b` incorporates #594. Settings and recovery
policy flags and the recovery/scheduled/listener test selectors remain present.
The recovery constructor keeps scheduled settings disabled; checked settings
overruns and pending engine microtasks retain their existing global fallback.
Frame and idle cleanup now release the imported `DScheduledCallback` records
through `d_scheduled_clear`, preserving callback holds and report/activity
binding lifetimes until the existing safe boundary.

In modern bindings, the recovery regression queues failed B frame/idle callbacks in healthy C
before C's own callbacks, and queues affected B frame/timed-idle work. After
containment it checks that C skips sealed callables and continues its own work,
while B's queued native ownership is released during preparation. Retained data,
provider release and native timer order remain covered by the same fixture.
All DOM units are rebuilt for the new records and shared header layout.
Engine identity remains `3be09416c3df09ee3c4d85b8fe54af3d8f90d24ed32678042f8a7ca67ac4aa39`;
the engine/runtime sources and runtime test units are unchanged from the #591
composition, whose complete-runtime receipt above remains applicable.

With ASan/UBSan and LeakSanitizer enabled, both teardown policies pass:

| Integrated check | Shipping / instrumented |
|---|---:|
| Native DOM recovery, including frame/idle continuation | 2,532 / 6,672 |
| F4 scheduled settings | 5,570 / 6,066 |
| Listener settings / timer settings | 1,736 / 1,736; 718 / 718 |
| Frame callbacks / retirement / routing | 106 / 106; 1,196 / 1,196; 564 / 564 |

The earlier dispatch/reporting, shared custom/observer, realm routing,
retirement, departure, Streams, installation and provider lifecycle selectors
also pass on both freshly rebuilt DOM profiles. Removing failed-status
suppression from `d_scheduled_call` produces six named recovery failures per
profile: a sealed callback must not stop healthy frame/idle work. The isolated
mutant leaves the real source unchanged.

The unchanged runtime cohort is reused for the recovery/fence/terminal-record
and F4 settings selectors; each passes on both profiles. Target libraries,
header/preparer/teardown/link audits and tracker checks pass. Stale references
and whitespace are checked against the merged PR base, preserving unrelated
upstream imports. Reproduce with the canonical DOM `--recovery` build, then
the composition selectors above plus `--scheduled-settings`, `--animation`,
`--animation-retirement`, and `--animation-routing` on both binaries. This
receipt does not extend the original broad-DOM or browser acceptance claims.
