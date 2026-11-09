# E5B — dialog close latency investigation

Status: diagnosed; production fix not implemented. Quinn investigated after
Chris approved the E5A resolution-restoration fix. This is separate from PR
#322's layout/persistence implementation and from parent-owned stacking.

## Report and regression check

On P5, Chris reported Open/Save sometimes closing immediately and sometimes
pausing about one second. He initially noticed the titlebar X, then tested
Cancel three times on each dialog and Escape, observing the same pattern.

The E5A changes, including R1's pre-creation size clamp, do not modify
`work.c`, `browser.c` or `ui_dispatcher.c`. All three dismissal routes converge
on `os64_ui_file_dialog_close()`, which destroys the browser and joins its
workers before destroying the native window. Browser creation starts two
workers. The worker pool's shared jobs pipe has multiple readers, while the
kernel pipe has one `readWaiter` slot. A later waiter replaces the earlier
registration. Closing the write end wakes the registered reader; an
unregistered reader can remain asleep until `PIPE_BACKSTOP_TICKS`, one second.

## Native reproduction

A temporary diagnostic used the existing production libos64 and kernel in a
disposable Q35 VM (4 CPUs, 4 GiB, 1024×768). No production code changed. It
created an idle pool, slept for a controlled interval, then measured pool
destruction. It compares one worker with the browser's two-worker arrangement.

| Idle before close | One worker | Two workers |
| --- | ---: | ---: |
| 30 ms | 4.474 ms | 980.069 ms |
| 200 ms | 1.222 ms | 803.078 ms |
| 500 ms | 11.042 ms | 509.302 ms |
| 800 ms | 10.748 ms | 206.254 ms |

[Diagnostic source](evidence/e05b/workclose.c) and
[native log](evidence/e05b/native-timing.txt). The time remaining until the
one-second wake explains the varying delay without involving a titlebar action
or an active directory scan. These are QEMU timings, not P5 measurements.
Chris's matching X/Cancel/Escape observations support the common-cleanup path.

The diagnostic was linked with the normal launch stub and libos64 at the
filedialogtest application base (`0x3b000000`) and installed as
`/tests/filedialogtest` only in the scratch root. Its scratch `gui.conf`
autostarted that path. The original library was unchanged; no P5 or repository
disk images were modified.

## Follow-up scope

Fix the shared worker/pipe wake-up behavior so idle workers can all promptly
observe shutdown. Review waiter registration, lifetime, interruption, EOF and
ordinary data readiness together; the same shared-reader pattern can delay
job delivery as well as shutdown. Keep worker completion before freeing pool
or browser storage. Kernel synchronization changes need focused design/review
and native multi-worker regression evidence. Parent-owned stacking remains a
separate window-manager task.
