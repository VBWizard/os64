# F15 checked module-provider bootstrap

Private C1/C2/C4 milestone after checked native tasks (#660), on merged
userland `c9fe984b` (#659/#660). Engine remains
`bcc933c20d304c1d5be5b1cb0e703b9d0f198bc7dc59fdf374050f48f190ca5f`,
ABI 5 / bytecode 6. C3, public activation, reclamation and slot reuse stay gated.

## Coherent implementation

Add ticket-based registration/detachment of existing module-loader and
import-base providers. Resolve the original retained realm through a shared
status-only helper, also used by checked script-host registration. Delegate
existing setter rules: no execution/health admission or raw context getter,
no new allocation, no native pointer/value export, and cleanup after
failure/retirement remains possible with a valid retained holder. Closing a
ticket does not detach a provider. Native opaque/source storage stays alive
until successful detach or root shutdown, independently of holder counts.

Module resolve/source callbacks retain their existing string/byte contracts:
resolve transfers an os64 allocation, source lends bytes until the enclosing
call returns, and callbacks run no author code. Actual imports inside a checked
task use its original owner loan, guard and budget. A callback may close its
admitting holder; that does not permit provider teardown during the active
call. The wrapper copies source bytes before invoking compilation. Retirement
or cancellation retains the existing conservative post-callback checks.

Import-base registration lends no raw view. Its existing callback does receive
a context and return an owned value: delivery keeps the sticky compatibility
veto before the native call. An incidental raw getter still has no exemption.
Initial private setup registration, active replacement/detachment refusal,
ABI/outcome alias handling and failed-host cleanup follow the old setters.

No module compile/run/handle/watch behavior changes. In particular, a one-shot
native compile/run/release wrapper must not silently lose pending top-level
await's late-failure tracking. These status-only bootstrap operations create no
persistent checked module/value tickets or provider release certificate. Native
loader and import-base contracts remain distinct; C3 and graph migration are
still required for reclamation.

## Proof plan

One PR combines implementation, real module graphs and host/guest controls.
Cover independent child providers, dynamic import and dependency/TLA execution
under checked tasks, close/promotion inside loader callbacks, active detach and
outcome protection, copied source surviving provider teardown, later TLA work,
import-base raw-veto delivery, retirement/cancellation, staged bootstrap,
failed/retired detach, stale handles and no-allocation status-only operations.
Use both teardown policies, target/instrumented engines, real allocator cuts,
ASan/UBSan/LSan, named behavioral controls, actual QEMU child/parent/heap checks,
affected DOM integration, export/layout/header/preparation audits and frozen
qualified receipts. Only F15's row changes. Request Codex review when ready;
Chris controls merging.

## Qualified validation

The implementation changes only private registration and the checked host
setter's shared status resolver. Target DWARF keeps the wrapper at **13,080
bytes**; the public shared export set matches #660 (**235 engine, 89 runtime**).
Default constructors, public/engine ABI identity and module-handle/watch behavior
stay unchanged. The shipping JS CLI/shared library builds against these inputs.

- `ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--owner-providers bash tools/test_js_runtime_host.sh`:
  **354 target / 1,656 instrumented checks**, zero failures. The final driver
  and runtime were recompiled against matching engine objects; unchanged
  instrumented engine objects were reused from the frozen #660 cohort after
  verifying prepared source/header identity and the engine digest above.
- Both teardown policies exercise **3 / 46 cached-call allocation cuts** each,
  plus deterministic engine-quota refusals before native source copying and
  raw import-base string creation. Native resolver strings, engine graphs,
  metadata and failed-host detachment balance without reclaiming leaked roots.
  Status-only registration allocates nothing, acquires no loan and polls no
  clock, including a forced negative clock and unavailable invocation pool.
- Complete runtime, raw construction, compiler sweeps and five intentional
  invariant modes: **41,289 / 141,661 checks**, zero failures/live allocations;
  compile sweeps and full fatal badges pass. Positive ASan/UBSan/LSan probes
  run outside the sandbox with `detect_leaks=1`.
- `python3 tools/test_js_owner_provider_mutations.py --artifacts ... --output ...`:
  **20 behavioral controls rejected**, both engines. Wrong loader/base realm,
  dropped registration, implicit raw export, failed-runtime cleanup refusal,
  alias clobber, stale-holder acceptance and missing base/getter veto trigger
  named assertions. The unmodified fail-fast selector passes both profiles;
  compiler errors, sanitizer reports and invariant exits do not count.
- `python3 tools/test_js_owner_runtime_guest.py --base-image ... --boot-iso ... --output ...`:
  **1,798 shared checks**, target heap baseline/integrity/accounting and exact
  parent-observed **0x4f520a00** pass. Preserved source disk/boot inputs are
  copied; the five-file executable/library closure is frozen. All **29 source
  inputs** and four runtime/core/ticket/issuer object hashes are verified.
- Fresh merged-#659 DOM objects and fixture, with both engine profiles:
  browser jobs **801 / 801**, import settings **1,378 / 1,378**, callback
  reports **1,118 / 1,118**, recovery host **3,198 / 3,198**, Promise settings
  **246 / 246**, child installation **58 / 58**, and child installation engine
  **1,330 / 1,258**, zero failures, with leak checks enabled.
- Public contract headers, six preparation tests, teardown audit, export/layout
  comparison, CLI cross-build, tracker validation, diff and stale-reference
  checks pass.

## Preserved proof boundaries

The existing Promise incumbent-capture bridge calls the public raw settings
realm query and records compatibility retention when Promise/import settings
are selected. The initial fixture assumed that bridge created no retention;
source-site tracing and a bounded backtrace identified the actual query in
`job_incumbent_capture`. Its conservative behavior is preserved. Pure loader
no-export proofs use default or host settings without legacy Promise capture;
import-base fallback explicitly selects import settings and retains its raw
veto. This milestone supplies no ambient exemption or fully checked browser
settings profile.

Parser allocation-pressure defects remain separately tracked as DEBTS 0011b.
The cached-call sweep compiles its real graph before injection, then fails
runtime/provider work; source-copy and base-return quota refusals precede
parser work. This does not claim general module compilation under allocator
failure. Leak detection stays enabled for the supported proof runs.

No native module handle is returned or released by these APIs. A pending module
in the shared oracle is engine-owned: it resumes in a later checked task after
providers detach and their source storage is poisoned. Existing late-failure
module watchers, provider opaque lifetime obligations, foreign/native graph
classification and the C3/reachability/teardown/reuse exit are unchanged.
No P5, visible-frame or public checked-profile acceptance is claimed.
