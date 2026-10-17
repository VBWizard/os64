# Per-thread kernel time by cause

*Design, 2026-10-17. Fault counters are implemented in `d40ae452`; cause timing remains to be implemented.*

## Question and current evidence

The user/system split identifies a thread doing kernel work but cannot say why. A one-minute P5 interval with Yonder visible gave the main thread 8.20 seconds of system time (about 14% of one core), the ticker 2.72 seconds (about 4.5%), and the task 10.92 seconds (about 18%). These are interval figures; the earlier 35% ticker reading and a later 4% reading came from different display states.

In that same interval `/sys/gui` added 2.38 seconds of ring-lock waiting across all callers. Publish held the lock for 3.50 seconds and waited for 1.75 seconds across all callers. Those site totals cannot be assigned to Yonder threads from `/sys/gui` alone. The ticker's ring wait and its system time are similar in size, while the main thread still needs a page-fault reading.

## Faults first

`/proc/<pid>/status` previously printed task fault fields that no code incremented. A resolved page fault now increments a counter on the faulting thread. Task status sums the current thread counters, and thread status reports its own. The existing `faults` line keeps its `minor` and `major` numbers: `major` means the resolver entered a file-reading path, including a read served from filesystem cache; `minor` means it did not. These numbers do not claim to distinguish physical device I/O. `anon_zero_pages` counts anonymous pages allocated and zeroed during fault resolution, including a discarded twin resolution. Fatal faults do not enter the resolved counters.

Read task and thread status before and after a minute of the same Yonder display on the P5. Thousands of `anon_zero_pages` per second on the main thread would support the large-buffer churn hypothesis. A small count would rule out that specific mechanism as a major part of its system time. A fault count by itself is not a timing measurement.

### P5 reading after the fault-counter boot

With Yonder visible at 1920×1080, a 67.6-second interval on 2026-10-17 gave:

| scope | user time | system time | resolved minor faults | anonymous pages zeroed |
| --- | ---: | ---: | ---: | ---: |
| main thread | 37.71 s (55.8% core) | 8.38 s (12.4% core) | 152,278 | 152,278 |
| ticker thread | 0.02 s | 2.88 s (4.3% core) | 0 | 0 |
| whole task | 37.73 s (55.8% core) | 11.26 s (16.7% core) | 152,278 | 152,278 |

The main thread zeroed about 2,252 pages, or 8.8 MiB, per second. This confirms recurring anonymous page allocation on the main thread, though it is far below the 68,000 faults per second estimated for an 8 MiB buffer replaced on every 33 Hz paint. The count cannot say what fraction of its 8.38 seconds of system time was spent resolving those faults. The ticker's kernel time has a separate cause; it resolved no faults in the interval. No major fault was resolved during the sample.

This boot was built from `userland` before the sibling GUI lock-count commit was merged. It therefore lacked the prior `/sys/gui` lock-site table; that commit has since been merged into this branch for the next kernel build. The thread counters above are unaffected.

## Cause timing contract

The next measurement should partition each non-idle thread's committed `systemCycles` into syscall work by number, resolved page-fault work, interrupt work, self-IPI halt time, and a remainder. Every cycle belongs to one bucket at a charge; a blocked thread accumulates no cycles. A task view sums its threads' raw counters before conversion to microseconds. The remainder is derived from the same coherent thread snapshot as `systemCycles`, never by subtracting separately rounded text values.

The active cause belongs to `thread_t`, because a blocked syscall may resume on another core. A syscall entry closes the user span, records its number and call count, then opens the syscall cause under one local interrupt mask. Its return closes the syscall span and clears the cause under the same mask. The scheduler continues to close an outgoing thread's open span and charge the pass to its separate core bucket. TSC deltas remain same-core.

A page fault inside a syscall closes the syscall span and opens the fault cause. The exception frame saves the prior cause. On return, including a recoverable ring-0 return, the fault span closes before the prior cause resumes. The unified exception path supplies the entry and return boundaries. The old exception path remains bootable, so any shipped timing must either instrument its recoverable fault return too or mark that configuration unsupported for cause timing.

Interrupt time cannot honestly be called remainder while an IRQ entered in the middle of a syscall inherits the syscall's active cause. Timer, keyboard, mouse, NIC, and IPI stubs therefore need entry and exit stamps before an exact cause partition is exposed. Their saved-register and stack contracts differ; each stub must be checked separately. A self-IPI wait in `scheduler_trigger` needs its own bookends so time halted there does not masquerade as syscall work.

Use `/proc/<pid>/thread/<tid>/syscalls` for nonzero rows (`number name calls us`) and `/proc/<pid>/syscalls` for the sum across the task's thread list. Keep thread `status` short with fault, interrupt, halt, and remainder totals beside `system_us`. The table uses 128 syscall rows plus an overflow row for numbers 128–255; each row has 64-bit call and cycle counters, about 2 KiB per thread. An overflow row makes a later syscall-number addition visible rather than silently dropping it. The reader copies the table under the thread's accounting sequence. Reuse the existing TSC stamp at syscall boundaries; `syscallbench` measures the cost of the extra index and counter updates.

## Verification

Exercise a short syscall, a blocking syscall, an anonymous fault, a fault while copying a user buffer in a syscall, and an interrupt nested in each mode. For each coherent thread read, the raw cause buckets plus remainder must equal `systemCycles` and must remain monotonic across sleep and migration. Boot one-core and SMP QEMU, then repeat the P5 interval with Yonder in the same visible state. Compare `syscallbench` against the user/system split baseline before accepting hot-path stamps.
