# F.16e — host routing and acceptance

Acceptance base: merged userland `48d5ccb2`, including native recovery #593.
Prepared engine identity:
`3be09416c3df09ee3c4d85b8fe54af3d8f90d24ed32678042f8a7ca67ac4aa39`.
Embedding ABI 5 and existing config/outcome layouts are unchanged.

This chapter completes acceptance of the initial opt-in F.16 subset: a
completely attributed child EXECUTION episode, with the failed task closed,
engine/native work unwound and empty engine bookkeeping. It adds an explicit
host decision over the reviewed libjs/native protocol; it does not enable
visible frames, reclaim realm slots or switch Yonder's primary-only policy.

## Host contract

`os64_dom_cluster_resolve_failure(cluster, abi, generation, &failed_task)`
returns a compact result by value. The host obtains the episode generation
from `os64_js_recovery_inspect` when recording the failed task and retains
both the generation and original outcome through cleanup/retry. The outcome's
status, diagnostics and accounting remain unchanged, including after healthy
work starts. Admission status/limit/host-error fields are separate from it.

| Decision | Host action |
|---|---|
| ROOT_REQUIRED | Stop cluster dispatch and perform root cleanup after borrowers unwind. No affected provider release permission is granted. This is also the zero-initialized/default result and the refusal for invalid/nonterminal input. |
| WAIT | Park cluster dispatch, complete native unwind/task closure or release outstanding native loans, then retry the same generation. Do not run jobs or replace the failed task's budget to obtain admission. |
| READY | Native preparation and the matching commit succeeded. Affected DOM providers may release their opaque storage; failed activity stays sealed. Later independent events still use normal task admission and its health check. |

The host adapter validates the caller ABI, terminal verdict, observed original
cause and expected generation before preparation. A stale request cannot
cancel providers, clear a latch or authorize dispatch. A newer observed global
cause takes precedence over an EXECUTION candidate, including cancellation
after a successful local episode. The adapter polls health after commit too;
normal task admission handles cancellation arriving after its return.
Provider reentry returns WAIT/BUSY without borrowing an enclosing outcome as
output. Repeated acknowledgment is idempotent for the completed generation;
another interrupted child requires a new generation and decision.

The API routes runtime-terminal outcomes. It is not the ordinary exception,
rejection or retired-alias handler, and it supplies no general failure reset.
It is an owner-thread host protocol, not a capability boundary against C code.
Both DOM recovery entries require the paired DOM recovery constructor. An
ordinary DOM cluster wrapping a custom libjs recovery policy refuses before
calling that policy: a libjs commit alone does not certify DOM provider cleanup.

## Supported scope and integration gates

The protocol/native audits in [F16 child recovery](IFRAME_F16_CHILD_RECOVERY.md)
and [F16 DOM recovery](IFRAME_F16_DOM_RECOVERY.md) remain authoritative.
Plain child script/callable/native-event tasks can recover when their captured
scope set and native state meet those contracts. Retained data and pre-abort
mutations survive; abandoned author catch/finally and failed aliases do not run.

Primary author code, incomplete attribution, raw/checked settings, native
callback algorithms, setup, GC, jobs, modules and suspended resumptions retain
global fallback. JOBS, MEMORY, allocator failure, cancellation and clock/host
failure remain global. Pending engine jobs/rejections/module handles/exceptions
prevent recovery; native owed sibling queues are distinct. Any cluster Streams
record/private bridge use requires root cleanup. Failed slots/backing remain
charged until root teardown; F.15 owns reclamation.

In particular, this proof does not certify recovery through F.4's checked
callback settings. F.11 must evaluate the actual integrated host's supported
paths and explicitly accept/measure fallback where that host uses unsupported
settings or algorithms; an F.16 completion row does not extend this subset.
Broader recovery requires its own attribution/native consistency proof.

Yonder still constructs a primary-only runtime and retires it on terminal
outcomes. Its `scripts.c` scheduling/diagnostic path is unchanged. The new
browser policy regression checks recovery inspection is disabled, forces the
existing layout/click overrun, verifies script retirement and preserved
ordinary link activation, then checks the heap is empty. No P5 campaign is
required for this additive host API/test consumer: default browser execution
and the named web results below are unchanged. The accepted P5 bundle is not
replaced; visible-frame enablement retains its separate hardware gates.

## Shared host/guest oracle

`userland/libdom/tests/recovery_cases.h` supplies the same owner-thread host
loop to `--recovery-host` and the target `recovery-test` consumer. It creates A
and staged B/C bindings through public APIs, grants native transports, exports
trusted test globals, schedules independent events and exhausts an actual B
timer callback. The host saves the original outcome and acts on the explicit
decision instead of inferring permission from LIMIT alone.

Both teardown policies exercise successful containment, cancellation, Streams
use, pending independent engine work, a borrowed layout generation and an
unpaired custom libjs policy. The
native producer checks recursive decisions cannot acknowledge cleanup.
Success preserves adopted data and C1/C2/A timer order while sealing B;
fallback runs no queued engine work and refuses new sibling execution.
Host-only edge cases cover invalid arguments/ABI, nonterminal and memory
verdicts, successive child episodes, stale acknowledgments, native settlement
errors and cancellation after READY. Native admission codes never replace
the original failed outcome.

## Validation receipt

- Final host acceptance: **674 checks per engine profile**, with both teardown
  policies and zero engine/native allocations left. Native recovery, F4
  scheduled/listener/timer settings and affected lifecycle selectors pass on
  both profiles with ASan/UBSan and LeakSanitizer enabled outside the sandbox.
- Final shipping DOM: **333,305 checks, zero failures, 12,949 native allocation
  sweeps**, including the final paired-policy guard and shared consumer.
- The unchanged runtime retains its composed full-runtime receipt of
  **22,014 / 69,404 checks**, zero live allocations, parser-failure and invariant
  checks. This chapter changes no engine/runtime source or layout.
- Yonder's explicit opt-out/overrun/teardown regression: **18 checks, zero
  failures** with leak detection enabled.
- Four isolated defects are caught on both engine profiles by named host
  assertions: publishing WAIT as READY, ignoring a generation mismatch and
  losing observed global-cause precedence, plus certifying DOM cleanup through
  an unpaired custom policy. Compilation failures are not
  counted as detection.

### Target guest

`make -C userland dom-recovery-test` builds a three-realm child and its parent.
The public QEMU driver freezes the child, parent and recursively matching
shared libraries with a SHA-256 manifest. It copies supplied boot/disk images
into a new output directory, preserving the inputs and existing cohorts.

The guest uses the real monotonic **100 ms** allowance, exercises six cases
under both teardown policies, checks zero leaked engine roots and verifies
the target heap after each teardown. Live block/byte counts return to the
process baseline and the allocator accounting identity holds. The parent
observes `os64_wait` and exact child exit **0x446f0200** before reporting PASS.
The frozen cohort has **19 files**, approximately **22 MB**. This uses the
accepted NONET validation kernel; its unrelated `net_arp_pending_order` failure
is outside the userland consumer verdict, not a passing kernel-suite claim.

### Named web results

WPT pin: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`. Yonder's real headless
runner self-tests pass. The merged baseline `48d5ccb2` and candidate are
compared across 17 families: DOM nodes/ranges/events; custom elements;
intersection/resize observers; performance timeline; web/CSS animations;
animation frames; pointer events; and six Streams families (readable streams,
readable byte streams, writable streams, transform streams, piping, transferable).

All **61,023 named entries** are identical, including duplicate-name verdict
sequences. Harness/error, exclusion and summary sequences are identical too.
Expected failures and worker/shadow-realm exclusions remain explicit; no
expectation file is weakened or rewritten. Paired runners share the unchanged
objects, with baseline core/recovery/public/internal headers compiled from the
merged base and candidate core compiled from the final implementation.
These are shipping-engine web regressions with ASan/UBSan on native/wrapper
code and leak checks enabled; they are not browser iframe recovery tests.

## Reproduction

```sh
DOM_HOST_KEEP=1 bash tools/test_dom_host.sh --recovery-host
YONDER_RECOVERY_KEEP=1 bash tools/test_yonder_recovery_policy_host.sh
make -C userland -j8 dom-recovery-test
python3 tools/test_dom_recovery_guest.py --boot-iso BOOT.iso --base-image BASE.img --output NEW-DIRECTORY
bash tools/test_js_contract_headers.sh
python3 tools/test_js_prepare.py
python3 tools/test_js_teardown_audit.py
python3 tools/test_js_port_target.py
python3 tools/host_suites.py --list
```

Keep `ASAN_OPTIONS=detect_leaks=1`. Retain the canonical DOM binaries and run
the default shipping matrix plus the native recovery/callback/lifecycle
selectors from the preceding native audit. Use the canonical Yonder WPT
runner with the pinned manifest and unchanged expectations for each family;
compare `(family, page, subtest)` verdict sequences rather than pass totals.
No guest, sanitizer or web evidence substitutes for F.11's integrated browser
acceptance or F.15's reclamation proof.
