# F.16c — opt-in libjs child recovery

Implementation PR: **#582**, source `9d2b1265`.

This chapter composes the merged terminal records (#577) and F4 current-realm
engine (#579) with an executable recovery path for an audited native host.
It implements the libjs protocol and its engine fence together. F.16 remains
in progress: libdom/Yonder do not select this constructor; DOM/native/Streams
consistency and coherent host/guest acceptance remain subsequent chapters.

## Supported behavior

`os64_js_create_with_child_recovery` requires a native preparation policy before
engine exposure. Default and record-only constructors stay terminal. A complete
child-only EXECUTION episode may be prepared after its task closes and engine
work unwinds. A primary incoming context is neutral; primary author code on the
abandoned stack forces global fallback. Multi-child stacks contain the complete
set. Retained shared data and mutations survive; abandoned catch/finally do not
run. Failed executable aliases refuse while ordinary retired aliases retain
their previous behavior.

The first subset supports ordinary script eval and child callable tasks.
Unknown/foreign attribution, native callbacks, raw/checked settings and setup, GC,
module evaluation/resolution, suspended resumptions and jobs are unsupported.
JOBS, MEMORY, cancellation, allocator/clock/host failure remain global. Pending
engine jobs, rejection bookkeeping, wrapper module handles or exceptions
prevent preparation. Healthy engine jobs never acquire a fresh budget by
recovering a different child.

## Protocol and ownership

| State | Admission and meaning |
|---|---|
| NONE | No failure episode; no recovery admission |
| ABORTED | Candidate captured before unwind; original task still failed |
| PREPARING | Engine fenced, affected realms failed/retired, native callback guarded |
| WAITING | Native callback returned BUSY; same fence/generation, loans must finish |
| PREPARED | Native callback certified completion; exact runtime/generation token issued |
| COMMITTED | Approved local latch cleared; only a later independent task may enter |
| GLOBAL | Root cleanup required; no recovery acknowledgement can authorize entry |

Preparation first proves closed wrapper work and engine idle, acquires the raw
engine fence, and irreversibly marks every affected context and wrapper realm.
Only then can the host release affected callback/registry roots and activity.
It preserves healthy native queue order and shared consistency. BUSY grants no
provider-storage release permission: provider opaque pointers remain alive
until loans have returned through native release, then preparation retries the
same generation. The callback returns normally in C and performs no author
execution. Error, newly queued work, pending exception or late global health
keeps the fence closed for root teardown.

Commit requires an exact prepared runtime/generation, idle work, empty
bookkeeping and a final health poll. It releases the engine fence and clears
only the approved local execution latch and consumed uncatchable flag. It does
not renew a deadline, resume the abandoned task, drain jobs, reset counts,
clear cancellation/allocator flags or overwrite the first global cause.
Normal later task admission checks health again, including cancellation racing
commit. Generation advancement prevents stale acknowledgement of a later
episode; equal generations in distinct runtimes do not substitute.

First-failure facts remain immutable within an episode. Inspection also keeps
final abandoned-task job counts across later independent sibling tasks; a new
failure starts a new generation and record. Failed slots/backing stay charged
until root destruction. The protocol allocates no storage; its native callback
still bears its ordinary host/allocator obligations. Both teardown policies
require clean root release; recovery is separate from reclamation/slot reuse.

## Engine/source audit

Patch `0029-recovery-entry-barrier.patch` follows accepted patch 0028 without
changing F4 entry/incumbent/current/native-caller meanings. It adds scalar
owner-thread execution facts and a runtime/generation-bound raw fence. Tracking
is opt-in. The prepared engine identity is
`c0df922a1972f114bd80180bafc9f03249ad7b7ba6a15bff3663605fed70428c`;
patch SHA-256 is
`36b3f6e7877adc06c1007016277b95ecd6eca607d760e56268e698b35da39a5b`.
Rebuild dependent objects together. Embedding ABI 5 and existing public
config/outcome layouts remain unchanged; REALM_FAILED is an appended status.

- `JS_CallInternal`, constructor entry, ordinary/compiled eval and optimized
  iterator dispatch check the fence before executable work. Compiled eval
  consumes its owned input through normal release; finalizer effects remain
  subject to native proof. Refusal returns the existing exception sentinel
  without replacing a pending interruption.
- `JS_ExecuteJobGroup` refuses before queue unlink and clears its output
  context. `JS_ResolveModule` refuses before loader/normalizer work.
  `JS_RunWithSettings` refuses before its native body and preserves output.
- Idle requires empty execution/native-call/settings markers and no interpreter
  frame, instrumented host callout or GC operation. Whole-GC depth is tracked
  because `gc_phase` is NONE during portions of collection. Marking failed
  executable state requires the same idle condition.
- Instrumented native callouts cover exotic property/prototype methods, module
  hooks/initializers, native jobs, interrupt/rejection hooks, finalizer/mark and
  buffer/shared-buffer callbacks, debug output and global-miss hooks. Generic
  C/data/iterator dispatch and custom callable classes retain their existing
  native/scoped markers. Fixed internal engine helpers and allocator callbacks
  do not supply external-native attribution; allocator reentry is prohibited
  by the embedding contract. These facts do not certify outside native loans.
- Recovery classification subtracts only its own active budget interrupt hook
  from native callback depth. Any other callout, GC, native execution, raw settings depth or checked
  wrapper body fails closed. Incoming contexts are checked against the wrapper
  table; opaque slots never establish membership or author identity.
- Ordinary EVAL/EVAL_FUNCTION preparation has a known code/entry context but
  UNKNOWN code kind while parsing. The recovery policy accepts that preparation
  only alongside an actual bytecode scope and no other unknown dispatch. Source
  audit follows `__JS_EvalInternal` through compile/value ownership into
  `JS_EvalFunctionInternal` and `JS_CallInternal`: uncatchable unwind releases
  compiler/stack temporaries and var refs before returning. Compiler-only
  interruption stays unknown. MODULE_EVAL/MODULE_RESOLVE have distinct kinds;
  jobs, resumes, proxy/custom/over-deep unknown dispatch cannot borrow the
  ordinary-eval classification. Builtin native algorithms on the stack retain
  native classification and force global fallback.

The raw fence permits ordinary data/native cleanup, which may enqueue work or
throw. Libjs validates those effects before publishing preparation/commit. It
is a trusted-owner protocol, not a capability boundary against hostile C.

## Proofs and reproducible checks

`--child-recovery` uses four real wrapper realms, retained cross-realm data,
actual deadline interruption, a native callback registry/ordered event queue,
and independently allocated provider storage with a loan. It checks native
BUSY/retry, cleanup ordering, independent sibling admission, retained failed
aliases, primary/multi-child attribution, unsupported scopes, job-cap/global
fallback, late signals at each boundary, nonzero abandoned counts, repeated
failures, stale/foreign tokens and permanent realm admission accounting. Root
cleanup reports zero leaked roots under FATAL and RECLAIM.

`--recovery-barrier` uses the terminal diagnostic constructor to independently
check raw engine entry families, pending exception identity, consuming input,
queue-head retention, module hooks, native callout/GC exclusion, token replay
and suspended generator state. Ending this raw fence never clears the terminal
wrapper latch.

Build both engine profiles through the canonical runner:

```sh
CCACHE_DISABLE=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--child-recovery \
  bash tools/test_js_runtime_host.sh
```

If the sandbox denies LeakSanitizer thread attachment, rerun the retained
`probe` and `sanitized-probe` outside it with `ASAN_OPTIONS=detect_leaks=1`;
retain ASan/UBSan. Run both binaries with `--child-recovery` and
`--recovery-barrier`, then the complete runtime/compiler/invariant matrix from
`tools/test_js_runtime_host.sh`. Raw-engine no-recovery and F4 settings suites
are also rerun on that coherent engine.

Final-source focused checks pass on both the shipping target core and the
instrumented engine, with ASan/UBSan support fixtures and LeakSanitizer enabled:

| Proof | Shipping / instrumented checks |
|---|---:|
| Child recovery (both teardown policies) | 4,525 / 4,525 |
| Raw engine recovery fence | 505 / 505 |
| Terminal failure records | 819 / 819 |
| F4 current settings | 379 / 379 |
| Settings bridge / captured callback settings | 769 / 769; 197 / 197 |
| Failed code / execution scopes | 244 / 244; 119 / 119 |
| Settings scopes / native-call facts / global-this | 724 / 724; 324 / 324; 67 / 67 |

The complete runtime matrix passes **21,944 / 69,292** checks with zero live
allocations. Both parser allocation-failure profiles pass their 40 assertions;
construction sweeps and fatal/reclaim invariant cases pass, including exit 99
and the full `4a534641` badge.

Twenty-two deliberately faulty variants are rejected by named assertions:
17 wrapper variants on both profiles and five engine variants on the
instrumented engine (39 executions). No timeout, crash or sanitizer failure is
counted as a successful oracle.

| Fault deliberately introduced | Named proof that rejects it |
|---|---|
| Caller used as code; primary author admitted; foreign context ignored | Complete affected set and global fallback |
| Native depth, raw settings depth, module or resume exclusion removed | Exact unsupported-scope issue |
| Module or queued/rejection bookkeeping ignored | Refusal before native containment |
| Engine or wrapper child seal omitted | Failed state visible before native preparation |
| Native BUSY treated as completion | Provider storage retained; no token issued |
| Runtime identity or generation check removed | Foreign/stale token cannot commit |
| Commit health poll removed | Late global failure blocks recovery |
| Abandoned counts zeroed or replaced with sibling counts | Nonzero final charge and independent inspection |
| Call or eval fence omitted | Refusal before body effects/allocation |
| Job fence removed | Queue head/provenance preserved |
| Whole-GC depth omitted | Active collection reported during mark callback |
| Engine host callouts untracked | Native activity reported without interpreter frame |

The broad DOM shipping-engine regression passes **322,022 checks**, including
**12,941 native allocation cuts**. It precedes the final generation-exhaustion
diagnostic refinement; that branch is unavailable to these opt-out consumers.
DOM routing, retirement, departure, Streams, installation
and provider/transport/visitor suites pass on both coherent engine profiles
after that refinement. These DOM cohorts precede the final raw-settings
classification exclusion, also unavailable to their opt-out policy. The new
raw-settings regression fails before that exclusion and passes afterward on
both profiles. Unchanged DOM/native objects are reused; runtime and engine
objects are rebuilt together. These checks cover the opt-out lifecycle and
are not browser recovery acceptance.

Target libjs, runtime and language acceptance consumers link. Manifest/header,
six preparer tests, teardown allocation/global/visibility audit and target
import/export/ELF checks pass. The export audit's stale pre-realm inventory is
corrected to include the existing realm/settings/record APIs alongside the new
recovery APIs: 215 engine exports and 76 wrapper exports, with the existing
32 math and 26 os64 imports. Tracker and stale-reference checks pass. The old
checkpoint links reported as retired names still target retained evidence files.

Guest/WPT/P5 recovery evidence is not claimed by this chapter. DOM/native
certification and host adoption remain open; the F.16 tracker row stays in
progress.
