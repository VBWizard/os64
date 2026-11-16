# F.1 realm validation

2026-11-10. Scope: [F.1](IFRAME_F1_REALMS.md), embedding ABI 3.
Baseline: `483d0734`, whose runtime/DOM/Yonder sources are also the brief's
`b81fe744` sources. Implementation: the code accompanying this record.
No engine source or manifest patch changed. No WPT expectation changed.

## Native and guest evidence

| Check | Baseline | Implementation |
|---|---|---|
| Contract headers and source manifest | Pass | Pass; all 54 declared embedding operations exported |
| Full runtime, target engine | 9,934 checks, 0 failures | 11,083 checks, 0 failures |
| Full runtime, instrumented engine | 27,601 checks, 0 failures | 56,396 checks, 0 failures |
| Final focused realm matrix | Not present | 1,152 target / 28,807 instrumented checks, 0 failures |
| Runtime constructor/compile sweeps and deliberate fatal modes | Pass | Pass, including full JSFA badge |
| CLI fixture / real library | 199 / 40 checks, 0 failures | 199 / 40 checks, 0 failures |
| Full DOM, target engine | 43,439 checks, 0 failures | 43,439 checks, 0 failures |
| Full DOM, instrumented engine | 307,535 checks, 0 failures | 307,535 checks, 0 failures |
| Yonder scripted-page host | 24,899 checks, 0 failures | 24,903 checks, 0 failures |
| Guest runtime consumer | Existing shared cases | 701 checks, 0 failures; exit `4a535254` (JSRT) |
| Shared tracker tests / iframe and HTTP2 CSV checks | Pass | Pass; 6 tracker tests |

The full runtime totals precede the final chronological-failure-order fixture;
the final focused matrix and guest include it. The sweeps enumerate measured
allocation inventories; their expectations were not relaxed. Runtime probes
reported zero live fixture allocations. ASan/UBSan and normal-exit LeakSanitizer
were enabled; engine instrumentation uses the port's existing ASan profile.
The sandbox's LSan attachment denial was diagnosed with `TracerPid: 0`, then
the preserved binaries ran outside the sandbox with `detect_leaks=1`.

The guest used QEMU q35, two CPUs, 2 GiB RAM, BIOS boot and no network.
An existing kernel image was reused with a disposable FAT root and a small
ext2 home filesystem. The tested cohort comprised `runtime-core-test`,
`libjs.so`, `libmath.so`, `libos64.so` and a small native parent runner,
recorded in a SHA-256 manifest. The consumer was installed at
`/tests/jsembedtest`; the parent used the kernel's single `/tests/testrun`
launch entry, avoiding two interactive shells racing over file fixtures.
The parent spawned the consumer and observed its actual `os64_wait` exit
code `4a535254`; both its receipt and the consumer summary were captured
in the serial log. No P5 acceptance is claimed or required
for this runtime-only cut.

The tests live in `userland/libjs/tests/realms.h` (shared host/guest) and
`tools/test_js_realms_host.inc` (allocation, clock, syscall and callback
observations), included by the established runtime runners. They cover:

- Independent globals/intrinsics and bytecode instantiation, shared engine,
  absent SharedArrayBuffer, ABI refusal, foreign-context rejection and module
  handle ownership.
- Separate module namespaces/providers and metadata, borrowed-function
  imports, clearing a child loader, captured watcher `then`, child job and
  late-module reporting, and chronological outcome delivery across realms.
- Promise-only identity lookup with cross-realm `then` and `await` handling;
  interleaved per-realm taking, after-take policy, mixed hosted/unhosted
  operation, suppressed reports and safe foreign-router refusal.
- Distinct output handles, console group/counter/timer state, borrowed print
  and console methods, and child Intl setup after parent execution.
- Native-callback creation/setup, recursive installer refusal, nested outcome
  alias protection, setup jobs retained by the enclosing task, aggregate job
  caps, inherited deadlines, runtime-wide sticky failure and task-end cleanup.
- All 65 slots, non-allocating/non-sticky overflow refusal, allocation-failure
  sweeps for child construction/output/Intl, and repeated cross-realm teardown
  with pending modules, jobs, functions and rejections under both policies.

## Routing mutation evidence

`tools/mutants_parallel.py test_js_realms_mutants.py:12` ran twelve isolated
workers. Each worker passed the unmodified focused suite before mutation.
All twelve mutations were caught by test assertions; no build failure counted
as a kill:

| Selection deliberately broken | Result |
|---|---|
| Module resolver → primary | Caught after distinguishing provider namespaces |
| Module watcher lookup list → primary | Caught |
| Watcher's captured `then` → primary | Caught |
| Output handle → primary | Caught |
| Console group depth → primary | Caught |
| Exception script host → primary | Caught |
| Late module report owner → primary | Caught |
| Rejection record owner → primary | Caught |
| Classic source execution → primary | Caught |
| Pending rejection removal restricted to attaching realm | Caught |
| Nested setup resets enclosing budget | Caught |
| Borrowed console uses caller context | Caught |

The first resolver mutant survived because both fixture resolvers returned
identical names. Distinct provider namespaces now make that mistake observable;
the corrected positive baseline and targeted mutant passed/caught respectively.
The two rejection-record mutants were rerun after compacting owner storage.
The watcher fixture observes the runtime's native call boundary and then calls
the real engine unchanged, checking the captured intrinsic's identity even
though the derived private promise is not exposed to script.

## WPT comparison

Both revisions used checkout pin `a699fb16767be39ba3e61a8e8b6bef60383f08ee`
and the existing expectation files. Counts and every named verdict match.

| Directory | Pass | Fail | Timeout | Not run | Total | Pages attempted | Pages excluded |
|---|---:|---:|---:|---:|---:|---:|---:|
| `html/webappapis/scripting` | 532 | 1,065 | 35 | 9 | 1,641 | 112 | 10 |
| `html/semantics/scripting-1/the-script-element/module` | 119 | 118 | 6 | 26 | 269 | 114 | 26 |

Excluded pages are worker/shadow-realm variants (9 / 25) and one XML document
per directory. The scripting run exits 1 on both revisions because the existing
required/pass baseline loses
`events/event-handler-handleEvent-ignored.html` →
`plain object "mouseenter" handler`. The module run exits 0 on both revisions.
This records an existing failure; it does not rebaseline it or claim new frame
coverage. Required/lost sets and pass/fail/timeout/not-run names were compared,
not just aggregate totals.

## Findings resolved during validation

QuickJS `CFunctionData` callbacks receive their caller's context. Inferring
ownership from that context misrouted a child's late module failure and borrowed
console methods. Those callbacks retain a stable owner slot in function data;
ordinary C functions such as `print` keep their engine-provided creation realm.
The brief's blanket native-function statement was corrected.

A pointer in each rejection record enlarged the instrumented DOM flood beyond
its existing 64 MiB ceiling: LIMIT/MEMORY, no host reports. Storing the fixed
realm-table index beside the existing tag preserves the 56-byte record layout.
The same 200,000-rejection case then returned the expected unhandled-rejection
outcome and 17 bounded host reports, with a 64,343,200-byte charged heap peak.
The memory limit and fixture expectation were preserved.

The CLI's exhaustive status-name switch now includes REALM_LIMIT. The runtime
runner retains failed/explicitly-kept artifacts for leak-enabled reuse. Comments,
ABI exports, tracker data and `git diff --check` were audited; the stale-reference
scan's relative `libjs/upstream/quickjs.c` path still names the existing file
under the brief's explicit `userland/` path base. The review's
`realm_storage_bytes` reference intentionally preserves the discarded proposal
that the fixed descriptor table replaced.

F.1 supplies engine realms, not multiple DOM bindings, iframe loading, independent
retirement, or HTML settings-object attribution. Those remain F.2 onward.

## Code review acceptance

Codex completed review of `fb70d6e5ace1fa108d18a679403ed35d68e023fe` with
no findings on 2026-11-10 ([review receipt](https://github.com/VBWizard/os64/pull/532#issuecomment-6774790328)).
The final tracker/acceptance commit changes documentation only. PR #532 carries
the completion flip for merge into `userland`; no P5 acceptance is required
for this runtime-only slice.
