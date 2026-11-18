# F15 scoped settings-owner observations

Implementation PR **#665**, source `eef717bf`.
Private C1/C2/C4 milestone after checked module providers (#662), on merged
userland `c9aa57c3` (#661/#662 plus debt documentation). C3, public activation,
reclamation, provider release and slot reuse remain gated. Engine identity is
`bcc933c20d304c1d5be5b1cb0e703b9d0f198bc7dc59fdf374050f48f190ca5f`,
ABI 5 / bytecode 6; separate WindowProxy/readonly-global work #664 is not part
of this pinned cohort.

## Contract and coherent PR

`js_owner_scope_settings` copies a by-value observation from a live invocation
loan and an active wrapper settings frame. Its result separates the admitting
owner ID from the selected entry/current/incumbent/native-caller original realm.
A selected engine context is ABSENT, MAPPED (immutable owner ID and current
primary/staged/retired/failed flags), or UNMAPPED. Non-mapped realm fields are
zero; absence does not guess the primary or admitting owner.

The operation validates its live loan and role, allocates nothing, issues no
holder, polls no clock/cancellation/allocator health, consumes no exception,
and changes no settings, task budget or retention flag. Refusal preserves
caller output. Outputs are distinct caller storage outside scope/runtime and
active outcomes. A copied observation survives scope/holder/runtime teardown
but grants no execution, origin, active-document, target or release authority.
Arbitrary stale scope pointers remain outside the invocation-only contract.

Trusted setup and synchronous cleanup may observe facts while a valid checked
settings frame exists. A scope's admitting realm may differ from the selected
role, including a staged installer borrowing a parent's task. A retired/failed
role may still be observed; that does not make it executable. With no admitted
wrapper settings frame, the operation refuses even if the loan remains alive.

Share only role validation/engine getter selection with the public query.
Public raw settings eligibility, failure/setup refusal and compatibility veto
remain unchanged, as do Promise incumbent capture and DOM security consumers.
This is a private scalar observation facility, not a C3 bridge migration or a
public checked settings profile. No engine patch or runtime fields are added.

## Proof plan

One implementation PR includes actual settings/native-call frames, admitting
vs selected ownership, nested staged setup, unmapped temporary engine contexts,
close/promotion, lifecycle snapshots, missing-frame/invalid-role refusal,
non-mutating output and no-allocation/no-health proof. Preserve legacy raw
getter retention and old settings semantics. Run both teardown/engine profiles
with ASan/UBSan/LSan, named behavioral controls, actual guest child/parent/heap
checks, affected DOM settings integration, public export/layout/header audits
and frozen qualified receipts. Only F15's tracker row changes; request Codex
review when ready and leave merging to Chris.

## Implementation and qualified validation

The implementation shares role validation and getter selection with the public
query, preserving its admission order, selected-realm eligibility and sticky
compatibility retention. The new operation separately validates the live loan
and wrapper frame, maps the selected context and publishes one scalar result.
It adds no runtime fields: target DWARF remains **13,080 bytes**. Shared exports
match #662 (**235 engine, 89 runtime**); the JS CLI/shared library cross-builds.

- `ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--owner-settings bash tools/test_js_runtime_host.sh`:
  **144 target / 144 instrumented checks**, zero failures, both teardown
  policies. The final runtime/driver were compiled against matching engine
  inputs; unchanged instrumented engine objects were reused from frozen #662
  after verifying prepared source/header identity and the digest above.
- The passive probe forces the next host allocation to fail, supplies a negative
  clock and makes additional loan admission unavailable while reading facts.
  Allocation attempts, holder/loan counts and retention remain unchanged; the
  original task stays healthy. Pending exception 77 remains intact. These are
  zero-allocation observation proofs, not new context-construction OOM claims.
- Complete runtime, raw construction, compiler allocation sweeps and all five
  intentional fatal modes: **41,432 / 141,804 checks**, zero failures and zero
  live allocations; compile sweeps and full fatal badges pass. Positive
  ASan/UBSan/LSan runs stay outside the sandbox with `detect_leaks=1`.
- `python3 tools/test_js_owner_settings_mutations.py --artifacts ... --output ...`:
  **28 behavioral counterexamples rejected**. Admission/selected-owner
  conflation, primary fallback, absence guessing, unmapped-as-absent, lost
  primary/staged/retired facts, missing frame guard, refusal clobber, extra or
  cleared raw veto, health polling, exception consumption and caller/current
  conflation fail named assertions on both engines. The unmodified controls
  selector passes; compile errors, sanitizer reports and fatal exits do not
  count as detections or positive leak evidence.
- `python3 tools/test_js_owner_runtime_guest.py --base-image ... --boot-iso ... --output ...`:
  **1,931 shared checks**, exact parent-observed **0x4f520b00**, target heap
  baseline/integrity/accounting pass. The five-file executable/library closure
  is frozen from copied source disk/boot inputs. All **30 source inputs** and
  four runtime/core/ticket/issuer object hashes match the tested source.
- Fresh DOM objects/fixture for merged #661's private layout: browser jobs
  **801 / 801**, synchronous settings **33,662 / 66,254**, import settings
  **1,378 / 1,378**, Promise settings **246 / 246**, Location exotics
  **1,932 / 3,052**, Location context **424 / 424**, Window authority
  **304 / 304**, binding authority **526 / 526**, navigation authority
  **1,796 / 1,796**, browsing contexts **1,078 / 1,078**, zero failures with
  leak detection enabled. No objects from the unmerged #664 layout/engine are
  used in this cohort.
- Public headers, six engine-preparation tests, teardown audit, CLI build,
  export/layout comparison, tracker validation, diff and stale-reference
  checks pass.

The native-frame fixture deliberately separates a staged B admission from
parent A's settings. A trusted nested engine frame then declares B as entry
and current while A remains incumbent/native caller. A temporary engine
context with no wrapper owner stays UNMAPPED, is freed before callback return,
and produces no fabricated primary ID. Promotion still grants B after observing
A. Holder close, retirement and native finalizers do not turn observations into
raw views; cleanup without a wrapper settings frame refuses unchanged output.
Copied staged/retired facts remain immutable after activation and root teardown.

No Promise incumbent bridge or DOM security consumer is migrated. Selected
roles describe original retained realms, not F4's active document, relevant
target, origin or publication state. Lifecycle observations do not authorize
entry and are not recovery/reclamation certificates. The public getter keeps
its private-setup/retired/failure refusal and raw-view veto, including within a
private observation scope. No public checked profile or P5/visible-frame
acceptance is claimed.
