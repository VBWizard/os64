# To Opus: Yonder's two kernel-time states on the P5

2026-10-17, from Quinn. This is the handoff from the per-thread kernel-time work. The measurements and implementation contract live in [THREAD_KERNEL_TIME.md](THREAD_KERNEL_TIME.md); the GUI lock-site design is in [GUI_LOCK.md](../pending/GUI_LOCK.md). The cause-accounting implementation is `81eaea04`, and the final P5 interval is recorded in `d9141fbe` on `codex/thread-kernel-time`.

## How the investigation went

Chris's first `top` reading at danlegt.com's top put Yonder's main thread near 30% system and its ticker near 35% system. A later reading of the same visible page was much lower. We repaired the dead page-fault counters, measured fault time, then added per-syscall timing. Fault handling was real but small: about 0.3–0.4 seconds during a minute of Yonder, despite roughly 150,000 anonymous pages zeroed. The ticker had no page faults.

The per-syscall kernel caught a sustained high state: over 64.6 seconds, Yonder used 65.2% of one core in system time, split roughly 31.4% main and 34.1% ticker. Its main thread spent 15.44 seconds in `gui_window_publish`; the ticker spent 21.67 seconds in `gui_event_ring`. `/sys/gui` measured 21.84 seconds waiting at the ring lock site and 39.14 seconds with the compositor holding `kGuiLock`. This tied the ticker's system time to the global GUI lock, and much of the main thread's time to publish and other GUI syscalls.

We suspected the new syscall stamps might have caused the high state. Chris restored the saved fault-time kernel, which had GUI lock counters but no per-syscall timing, and restarted Yonder at the same page position. That kernel also gave 64.9% task system, 33.6% ring waiting, and 59.7% compositor lock holding over 62.8 seconds. A second 110.9-second interval stayed high. Earlier, that same fault-time kernel had shown a low state: main system about 12.5%, ticker about 5.2%, and ring waiting about 5% of a core. The high/low change occurred without changing that kernel binary. The syscall accounting change did not create the high state.

The final kernel added exclusive interrupt and self-IPI halt buckets and booted the P5 after one-core and two-core QEMU tests passed. Chris restarted Yonder at the same top-of-page view and saw about 54% user / 14% system in `top`. Our 70-second interval was low again:

| Measure, percent of one core unless noted | High, 64.6 s | Low, 70 s |
| --- | ---: | ---: |
| Yonder task system | 65.2% | 19.1% |
| Main thread system | 31.4% | 13.4% |
| Ticker thread system | 34.1% | 5.7% |
| All callers waiting at ring site | 33.8% | 5.3% |
| Compositor holding `kGuiLock` | 60.6% | 22.9% |
| Yonder publishes per second | 37.3 | 59.2 |
| Yonder ticker rings per second | 92.0 | 130.6 |

In the low interval, the task's 13.389 seconds of system time partitioned into 12.937 seconds of syscalls, 0.365 seconds of faults, 0.026 seconds of interrupts, 0.061 seconds of self-IPI halt, and 0.001 seconds of remainder. The ticker's 3.748 seconds in `gui_event_ring` closely matched the 3.725 seconds of ring-site waiting across all callers. Interrupts and self-IPI waits are tiny beside the GUI syscall time in this state. The high interval predates exclusive IRQ stamps, so it cannot supply a comparable high-state IRQ total; its GUI lock counters and syscall rows still locate its large measured cost.

## What the lock numbers suggest

The work done *while holding* the publish lock was similar in both states: about 0.96 ms per publish-site acquisition in the high run and 0.87 ms in the low run. The average *wait to acquire* that lock rose from about 0.38 ms to 5.12 ms. Ring-site waiting rose from about 0.41 ms to 3.64 ms per acquisition. The high state had fewer Yonder publishes and rings per second, despite much more system time. This looks like lock delay throttling the client, rather than more calls creating the delay.

The compositor takes `kGuiLock` across input routing, window-state work, and `composite_locked()` blits into the RAM backbuffer; framebuffer flush happens after release (`kernel/src/gui/compositor.c`, `guicomp_thread`). The total compositor hold time differs sharply, but the high-state record lacks a compositor acquisition count. We therefore cannot tell from these intervals whether it made more passes, held the lock longer per pass, or both. Damage shape, window coverage, core placement, and memory behavior are candidate influences, not established causes. A longer hold could also cause other threads to spin and reduce their progress, reinforcing the slow state; the current counters show the contention but do not prove that feedback path or its first trigger.

The concurrent Yonder [perf-4 investigation](../pending/YONDER_PERF.md) found a concrete source of excess work at this page position: an offscreen `text-shadow` animation can trigger layouts and whole-view paint, publish, and composite passes. Its perf-4a fix makes the animation in-place and skips it while offscreen. This is relevant to the lock workload, but it does not explain our high-to-low transition: the P5's `libflow.so` retrieved after the low interval has the same `motion_due` machine code as the local pre-fix build (455 instructions, matching bytes), while the perf-4a build differs (478 instructions). The whole P5 library is not byte-identical to that local build, and we did not archive the exact binaries from each earlier interval. Treat the perf-4 source diagnosis as a lead for the page's recurring work, not proof of what switched these two measured states.

## Most useful next observation

If the high state returns, take task/main/ticker status, their syscall rows, and `/sys/gui` together at both ends of an interval, as in the final measurement. Record the compositor site's **acquisition delta** as well as its hold delta. That first separates pass frequency from average hold time without changing code. If average hold is the moving part, the smallest new diagnostic would count damage rectangles and pixels processed inside each compositor pass alongside hold time, with enough distribution to notice a few long passes. Window geometry/coverage and the compositor's core would help interpret a difference. The aim is to explain why the same visible page can put the compositor in two lock states before choosing a GUI concurrency change.

The instrument is complete; the trigger for the two GUI lock states is not identified. I would treat the lock-site measurements as the starting point for that separate investigation, rather than attribute the improvement to the accounting kernel or to chance.
