# Popuptest unrelated-readiness regression

2026-10-11. Chris reported five QEMU runs: the first failed the line-55
`event_wait_any(&ready)==1 && ready==b` assertion; four subsequent runs passed.
This report is QEMU evidence, not P5 acceptance. The original failure output
contains neither the return code nor the ready handle/event, so its particular
wakeup cannot be identified from that report.

## Diagnosis and controlled reproduction

The assertion assumes the next readiness notification belongs to B's delayed
worker. The syscall promises an owned window with pending events; asynchronous
pointer, focus, appearance or visibility events can instead make A ready.
Draining once before the worker starts does not prevent later arrivals. The
old test also never consumed and verified the expected doorbell after that wait.

On the same E1K kernel, injecting `os64_gui_event_ring(a, 0x40)` immediately
before the old assertion reproduced [one failure](before.txt), at line 56 after
the inserted line. [The reproducer patch](reproduce-before.patch) reconstructs
that tested pre-fix fixture from the current source; apply only in a separate
checkout, build popuptest and run it in the scratch guest.

## Fix

- Match the target handle and doorbell mask, consuming unrelated records through
  the existing wait/poll APIs. Require receipt of both deliberately injected A
  noise and B's real worker notification, without assuming their ordering.
- Apply the same event filtering to notification waits in the competing-waiter
  tests and to the initial nonconsuming-readiness proof.
- Bound notification waits with a five-second watchdog that destroys the test
  windows and reports failure if the notifications never complete. Join it before
  the separate signal test so it cannot claim that task-wide handler.
- For the any-waiter/legacy-waiter conflict case, a thread's entered flag does
  not prove it owns the waiter slot. A delayed notification releases whichever
  thread wins a scheduling race; allow bounded retries of that race, but require
  an actual BUSY result. Preserve unexpected-error failures.
- Signal/last-window teardown checks drain intervening normal readiness before
  checking their terminal return. Add a startup explanation of the window churn.

Only the diagnostic and its evidence/handoff records changed for this fix.
The production E1K kernel and libraries were reused.

## Validation

The strict cross build passed. On four-core QEMU with the original E1K kernel,
**ten consecutive full runs each passed 153 checks with zero failures and exit
code 0**. Every run includes the deterministic unrelated-notification regression.
[Runner output](repeat.log), [guest serial](serial.log), `after-1.txt` through
`after-10.txt`, [build output](build.log) and [source/binary hashes](hashes.sha256)
are retained here. [repeat.py](repeat.py) drives the existing scratch QMP helper,
waits for each process's exit and flushes the reports before extraction.

The earlier 146-check result and hashes in the parent directory remain historical
evidence for the original diagnostic. This packet supersedes its readiness-test
acceptance. Chris subsequently confirmed validation of the corrected diagnostic
in both QEMU and on the P5. This is user-reported acceptance; no additional run
counts, logs or installed-binary hashes were supplied. Independent code review
remains outstanding.
