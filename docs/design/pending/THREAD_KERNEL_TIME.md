# Per-thread kernel time by cause

*Design and measurements, 2026-10-17. The implementation now records disjoint fault, syscall, returning interrupt, self-IPI halt, and remainder buckets; validation is recorded below.*

## Question and current evidence

The user/system split identifies a thread doing kernel work but cannot say why. A one-minute P5 interval with Yonder visible gave the main thread 8.20 seconds of system time (about 14% of one core), the ticker 2.72 seconds (about 4.5%), and the task 10.92 seconds (about 18%). These are interval figures. Chris reports that Yonder showed the same top of the page in this and the later high-system-time run. A rollback check below rules out the per-syscall accounting change as the cause of the difference; the variation between runs remains unexplained.

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

The next slice adds `fault_us` to task and thread status for the unified exception path. It closes a fault span on both user and kernel returns, so a fault during a syscall is included. A sleeping resolver accrues no cycles while switched out. Until IRQ boundaries are stamped, interrupt work that preempts a fault handler can be included in `fault_us`; this field is an inclusive fault span, not yet the final exclusive cause bucket. `EXCOLD` does not expose the field because its separate recoverable #PF path lacks the stamps. The slice booted in two-core QEMU with 34 preboot and 34 postboot tests passing; a `cat /proc/self/status` smoke read showed `fault_us` beneath `system_us`.

This boot was built from `userland` before the sibling GUI lock-count commit was merged. It therefore lacked the prior `/sys/gui` lock-site table; that commit has since been merged into this branch for the next kernel build. The thread counters above are unaffected.

### P5 reading with fault time and GUI lock sites

After the merged lock counters and inclusive `fault_us` were booted, a 67.3-second interval with Yonder visible at 1920×1080 gave:

| scope | user time | system time | inclusive fault time | anonymous pages zeroed |
| --- | ---: | ---: | ---: | ---: |
| main thread | 37.99 s | 8.45 s (12.5% core) | 0.334 s (0.5% core) | 146,925 |
| ticker thread | 0.02 s | 3.47 s (5.2% core) | 0 | 0 |
| whole task | 38.01 s | 11.92 s (17.7% core) | 0.334 s | 146,925 |

The 146,925 resolved faults were minor, about 2,183 per second. Fault handling accounts for about 4% of the main thread's system time; the large-buffer churn is real but not the main CPU cost at this workload. The ticker's kernel time is unrelated to paging.

Across all GUI callers in the same interval, the ring site made 8,811 acquisitions and waited 3.17 s for `kGuiLock`, holding it for 0.034 s. The publish site made 4,344 acquisitions, waited 1.73 s, and held the lock for 3.96 s. These site totals have no thread IDs, but their scale suggests lock acquisition as the next timing target. A per-syscall thread table can identify each thread's caller time.

On the next P5 boot, a 68.6-second desktop-only interval with Yonder absent made **zero** ring-site acquisitions and 139 publish-site acquisitions; those publishes held the lock for 0.002 s and waited less than 0.001 s. In the Yonder source, the ticker is the caller of `os64_gui_event_ring`. Together with the earlier Yonder interval, this makes the 3.17 s ring wait a strong explanation for most of the ticker's 3.47 s of system time, though the intervals are from different boots. Nearly all of the Yonder interval's publish lock time is additional to the desktop baseline. The per-thread syscall rows will confirm the ownership directly.

### Per-syscall timing slice

The next build keeps an active syscall row on `thread_t` across blocking and migration. `/proc/<pid>/thread/<tid>/syscalls` and `/proc/<pid>/syscalls` show nonzero `number name calls us` rows, with row 128 gathering numbers 128 and above. Status adds `syscall_us` and, under the unified exception path, `other_system_us` derived from raw system, fault, and syscall cycles. The per-thread row copy uses the accounting sequence; task rows sum those coherent thread copies.

Fault spans are excluded from syscall rows. Interrupts that preempt a syscall remain inside that syscall row until IRQ entry and return are stamped. The displayed `syscall_us` therefore measures an inclusive syscall span, and `other_system_us` is the unattributed residue at this stage; neither is the final exclusive partition. The QEMU build passed 34 preboot and 34 postboot tests, and both task and thread syscall files rendered expected rows. Before deploying this slice, the P5's fault-time kernel ran seven `syscallbench` rounds of 20,000 `taskid` calls; the best adjusted round was 21,924,111 cycles, or about 1,096 cycles per call. Use the same fixture after the new boot to measure the row-update overhead.

On the P5 with per-syscall timing enabled, two 20,000-call `syscallbench` runs had best adjusted rounds of 19,438,353 and 24,592,722 cycles, or about 972 and 1,230 cycles per call. This run-to-run spread is larger than a plausible small row-update cost, so these samples do not establish an overhead percentage. Both runs completed correctly.

### P5 reading with per-thread syscall rows

With Yonder visible at 1920×1080, a 64.6-second interval on the syscall-timing kernel reproduced the high-system-time state from the original brief. Reads of task status, thread status, syscall rows, and `/sys/gui` were made by separate SSH commands, so the figures are approximate interval rates rather than one atomic snapshot.

| scope | user time | system time | inclusive fault time | inclusive syscall time |
| --- | ---: | ---: | ---: | ---: |
| main thread (135) | 27.93 s | 20.32 s (31.4% core) | 0.218 s | 20.10 s |
| ticker thread (148) | 0.015 s | 22.06 s (34.1% core) | 0 | 22.06 s |
| whole task | 28.09 s | 42.10 s (65.2% core) | 0.219 s | 41.87 s |

The main thread's largest syscall rows were `gui_window_publish`: 2,412 calls and 15.44 s; `gui_event_poll`: 5,034 calls and 2.71 s; `gui_event_wait`: 2,684 calls and 1.37 s; and `unmap`: 2,164 calls and 0.455 s. It zeroed 97,280 anonymous pages during the interval, yet fault handling used only 0.218 s. The ticker made 5,941 `gui_event_ring` calls taking 21.67 s; its 8,920 `read` calls took 0.070 s of *running* kernel time, excluding time asleep. It is not burning CPU in the pipe read.

Across all GUI callers in the same interval, the ring lock site made 5,994 acquisitions, waited 21.84 s, and held the lock for 0.005 s. Yonder's ticker and main thread together made 5,978 ring calls, so nearly every acquisition was theirs. The ring wait closely matches the ticker's 21.67 s in `gui_event_ring`. The publish lock site made 2,573 acquisitions, waited 13.17 s, and held the lock for 2.47 s; Yonder's main thread made 2,412 publish calls taking 15.44 s. The site counters include other applications and the asynchronous reads can differ by a fraction of a second, but the correspondence is strong. The composite site held `kGuiLock` for 39.14 s during this 64.6-second interval. It is the likely source of much of the contention seen by both Yonder threads; the lock-site table does not identify each holder or prove that every wait was against a composite pass.

This run reproduces the original 30%/35% reading and locates its immediate cost in GUI syscalls waiting for `kGuiLock`, particularly ring and publish. Chris reports the same top-of-page view as the earlier low-system-time runs. The inclusive syscall rows still contain any IRQ time that preempted them, so this measurement does not establish an exclusive IRQ total or an exact final partition. IRQ stamps remain necessary if the project needs an exact general-purpose cause partition.

### Controlled rollback check

Chris observed the same top-of-page view in the low-system-time and high-system-time runs, so the newly added syscall stamps were a possible regression. The saved fault-time kernel, which has GUI lock counters but no per-syscall accounting, was restored on the P5. The syscall-timing image was preserved as `os64_kernel.syscall-diagnostic-20261017`. With Yonder restarted in the same view, a single SSH command read task status, main and ticker status, and `/sys/gui` at each end of a 62.8-second interval.

| rate, percent of one core | syscall-timing kernel | restored fault-time kernel |
| --- | ---: | ---: |
| Yonder task system | 65.2% | 64.9% |
| main system | 31.4% | 31.0% |
| ticker system | 34.1% | 33.9% |
| all callers waiting at ring lock site | 33.8% | 33.6% |
| compositor holding `kGuiLock` | 60.6% | 59.7% |

On the restored kernel, the main thread used 19.47 s of system time, the ticker 21.27 s, and the task 40.75 s during 62.8 s. Ring acquisitions waited 21.12 s and held the lock for 0.005 s; publish acquisitions waited 13.07 s and held it for 2.41 s. The compositor held `kGuiLock` for 37.52 s. These rates match the diagnostic-kernel interval closely. The per-syscall accounting change did not cause the high-system-time run. The reason this same visible page sometimes has much less lock contention remains unresolved; the lock measurements identify where the current CPU time goes, not what changed between runs.

A further 110.9-second interval on the restored kernel stayed in the high-contention state: task system time was 65.1% of one core, main 32.2%, ticker 32.9%, ring lock waiting 32.7%, and composite lock holding 53.9%. The difference is sustained across minutes, rather than a brief launch spike.

After the comparison, Chris authorized restoring the per-syscall image. The P5 boot image was copied back from `os64_kernel.syscall-diagnostic-20261017`, verified byte for byte, and rebooted. The post-boot `/proc/75/status` and `/proc/75/syscalls` smoke reads showed `syscall_us`, `other_system_us`, and populated syscall rows.

## Cause timing contract

Each non-idle thread's committed `systemCycles` is partitioned into syscall work by number, resolved page-fault work, interrupt work, self-IPI halt time, and a remainder. Every cycle belongs to one bucket at a charge; a blocked thread accumulates no cycles. A task view sums its threads' raw counters before conversion to microseconds. The remainder is derived from the same coherent thread snapshot as `systemCycles`, never by subtracting separately rounded text values. Independently rounded `/proc` microsecond fields can differ by a few microseconds when added as text.

The active cause belongs to `thread_t`, because a blocked syscall may resume on another core. A syscall entry closes the user span, records its number and call count, then opens the syscall cause under one local interrupt mask. Its return closes the syscall span and clears the cause under the same mask. The scheduler continues to close an outgoing thread's open span and charge the pass to its separate core bucket. TSC deltas remain same-core.

A page fault inside a syscall closes the syscall span and opens the fault cause. The exception frame saves the prior cause. On return, including a recoverable ring-0 return, the fault span closes before the prior cause resumes. The unified exception path supplies the entry and return boundaries. The old exception path remains bootable, so any shipped timing must either instrument its recoverable fault return too or mark that configuration unsupported for cause timing.

Timer, keyboard, mouse, NIC, and returning IPI stubs close the interrupted span and push an interrupt cause, then close it and pop on return. A committed scheduler interrupt does the same before changing address spaces and closes the interrupt span before the scheduler's core-only span. The fatal freeze IPI has no return boundary. A nested fault or interrupt takes the top cause, with bounded stack overflow charged to the remainder. `scheduler_trigger` bookends its own `sti;hlt` wait so those cycles do not enter a syscall row. Entry prologues and return tails outside the stamps remain in the adjacent cause; the partition is exact for committed cycles at each accounting charge, not an instruction-level profile.

Use `/proc/<pid>/thread/<tid>/syscalls` for nonzero rows (`number name calls us`) and `/proc/<pid>/syscalls` for the sum across the task's thread list. Keep thread `status` short with fault, interrupt, halt, and remainder totals beside `system_us`. The table uses 128 syscall rows plus an overflow row for numbers 128–255; each row has 64-bit call and cycle counters, about 2 KiB per thread. An overflow row makes a later syscall-number addition visible rather than silently dropping it. The reader copies the table under the thread's accounting sequence. Reuse the existing TSC stamp at syscall boundaries; `syscallbench` measures the cost of the extra index and counter updates.

## Verification

Exercise a short syscall, a blocking syscall, an anonymous fault, a fault while copying a user buffer in a syscall, and an interrupt nested in each mode. For each coherent thread read, the raw cause buckets plus remainder must equal `systemCycles` and must remain monotonic across sleep and migration. Boot one-core and SMP QEMU, then repeat the P5 interval with Yonder in the same visible state. Compare `syscallbench` against the user/system split baseline before accepting hot-path stamps.

### Final accounting slice

The kernel built with the new nested cause stack and returning interrupt stamps. In both one-core and two-core QEMU, 34 preboot, 34 postboot, and 4 late built-in tests passed with no failure. The P5 booted the same 3,498,560-byte image (CRC32 `eb366b39`), and the desktop task's live status showed nonzero fault, syscall, interrupt, and self-IPI halt buckets. For example, one `/proc/66/status` read reported `system_us=56680`, `fault_us=4661`, `syscall_us=51206`, `interrupt_us=136`, `self_ipi_halt_us=669`, and `other_system_us=7`. The displayed components sum to 56,679 µs because each field is rounded separately; the kernel checks the raw subset relationship before publishing a snapshot.

On that P5 boot, the existing seven-round `syscallbench` fixture measured a best adjusted 24,481,281 cycles for 20,000 `taskid` calls, about 1,224 cycles per call. This falls inside the previous syscall-timing boot's observed 972–1,230 cycle range; these noisy samples cannot isolate the additional IRQ stamp cost, which short syscalls do not normally exercise.

Two reads of the desktop task's threads about 20 seconds apart also checked live deltas. Thread 66 gained 1,678 µs of system time, split into 516 µs syscall, 209 µs interrupt, and 953 µs self-IPI halt; the fault and remainder fields held steady. Thread 70 gained 447 µs of system time, while its displayed components gained 146, 38, and 262 µs respectively (446 µs after separate rounding). Both threads' counters were monotonic. Yonder was not running after the reboot, so the same-view Yonder interval still needs a visible session.
