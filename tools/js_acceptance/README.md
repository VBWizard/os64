# JavaScript consumer acceptance (V1)

This suite exercises the published embedding contract without including R2's
fixtures or private runtime state. Quinn authored the suite and the runtime;
separate consumer tests and upstream expected values do not replace review by
another person or agent. V1's independent review remains a handoff requirement.

Run `tools/test_js_acceptance_host.sh` from the checkout. It builds the strict
target consumer, verifies the upstream fixture hashes, and runs two engines:
the cross-built target core with M1's actual target maths objects, and a second
ASan-instrumented engine with those maths objects. The wrapper, host services,
and support code use ASan/UBSan. The cross-built engine/maths objects are not
sanitizer-instrumented. Neither executable links host libm. Host heap, handles
and clocks stand in for OS services; those runs do not establish guest behavior.
LeakSanitizer remains enabled for normal exits and caller options are preserved.

The suite reports each selected upstream function as PASS, FAIL or SKIP. The
unit is a test function, not an individual assertion or test262 case. The
manifest inventories the original driver calls in language, built-in, closure
and BigInt tests from QuickJS 2026-06-04: 59 functions, 55 supported and four
explicit skips. Those four require upstream `std.gc` or `os.setTimeout` host
capabilities, which the standalone profile does not grant. This does not
classify WeakMap or WeakRef themselves as unsupported. The files' optional
`__loadScript` attempt falls back to their retained assertion implementations.
Other upstream test files and test262 are outside this selection.

`prepare.py` verifies file/licence hashes and driver inventory, then generates
definitions and separate calls. Each supported function gets a fresh runtime.
Its original assertions and expected values are preserved. A deliberately
wrong expected value verifies that an assertion failure reaches the host.

| Observable contract | Consumer case |
| --- | --- |
| ABI compatibility | Creation refuses mismatched headers without publishing a runtime; a binding's context refusal preserves the existing runtime. |
| Capability grants | Registered native arithmetic works; output/arguments/file/network/worker/shared-buffer globals are absent before setup. |
| Setup ownership | Arguments survive caller buffer edits; console-only setup preserves a separately registered print function; destruction leaves borrowed output open. |
| Exceptions and reuse | A native callback throws; the diagnostic reaches the host; the context remains reusable. A script exception retains queued work until the host drains it. |
| Re-entry | Evaluation, job draining and setup return BUSY inside callbacks from source execution and Promise jobs, without replacing the active outcome. |
| Rejection checkpoints | Unhandled rejection is reported at queue exhaustion and does not recur in the next turn. |
| Limits and host failures | A Promise chain exhausts its total job cap; a real invalid output handle fails; both retire the runtime. |
| Error-reporting exhaustion | A thrown object's conversion exceeds the engine memory ceiling. Host-only injection also refuses OS allocations during conversion and checks complete reclamation. |
| Native value ownership | A marked child closes a native/JavaScript cycle; collection preserves the rooted resource, then finalizes the unreachable cycle once. |
| Cancellation and teardown | Cancellation interrupts script with queued work; reuse fails; destruction discards jobs and finalizes the native resource with context access closed. |
| Fatal invariant policy | A leaked engine value terminates a separate process; host hooks verify the complete JSFA badge. The guest observes the real process status. |
| Language/numeric semantics | Original upstream assertions exercise operators, coercion, signed zero/NaN, number formatting, Date, regex, JSON, typed arrays, closures, generators and BigInt, including a fixed 2,000-digit pi expectation. |

The host-only `--diagnostic-oom` mode isolates the persistent allocation-refusal
regression. Before patch 0005, error backtrace assembly uses an error freed by
replacement of `current_exception` and crashes in `find_own_property`. Retaining
the borrowed error and cleaning early buffer exits fixes that lifetime defect.
The test requires structured HOST_FAILURE, refused reuse, and zero live host
allocations; changing the fixture to avoid the conversion would remove coverage.

For guest validation, `make -C userland -j8 js-acceptance-test` creates
`userland/obj/js/acceptance/consumer`. This is an alternative consumer in the
reserved `jsembedtest` slot; do not load it together with that fixture in one
process. Install it as `/tests/jsaccepttest`, with the checkout's matching
`libjs.so`, `libmath.so` and `libos64.so`, into a disposable validation image.
It remains outside the normal root-image inventory; normal installation is I1.
After the fresh guest's boot tests settle, run:

```text
/tests/jsaccepttest > /home/jsv1.txt
echo $? > /home/jsv1-status.txt
/tests/jsaccepttest --fatal-leak
echo $? > /home/jsv1-fatal-status.txt
sync
```

Wait for each command to finish before capturing its status. Expect ordinary
acceptance status 0, and fatal status `0x4A534641` (decimal `1246971457`). Copy
the consumer and three libraries through `/home` for host byte comparisons,
and inspect the explicit upstream results in `jsv1.txt`. Evidence and limits of
the recorded run are in `userland/libjs/VALIDATION.md`.
