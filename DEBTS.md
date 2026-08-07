# os64 Technical Debt Ledger

*The single sortable index of every known debt, harvested from the design
docs. Each subsystem doc remains the SOURCE OF TRUTH for its own debts (full
rationale, code sites, gotchas live there); this file is the cross-cutting
worklist you scan on a fix day to answer "what's the highest-value cheap fix
right now?" — a question no single doc can answer.*

**Maintenance rule:** when a debt is paid, delete its row here AND update the
owning section in its source doc (the two must not drift). When a review adds
a debt to a subsystem doc, add its row here too. Severity/cost are
hobby-scale judgment calls — re-rank freely.

- Severity: **Hole** (real defect exploitable/crashing) · **Robustness**
  (turns silent failures into loud ones, or prevents a known future crash) ·
  **Feature-gate** (blocks a planned feature) · **Cleanup** (honesty,
  cruft, perf-at-scale).
- Cost: **XS** (minutes) · **S** (an hour) · **M** (a session) · **L**
  (multi-session / discipline-wide).

## Quick wins (high value ÷ low cost — start here on a fix day)

| Debt | Sev | Cost | Source |
|---|---|---|---|
| Zombie tasks hold their ELF's FAT lock slot until phase-2 burial (window = spawn rate × kworker cadence; FF_FS_LOCK=64 is headroom, not a cure) — close `image->file` at ZOMBIE transition instead, no fault can need it after the last thread dies | Robustness | S | ffconf.h FF_FS_LOCK comment, 2026-08-07 |
| `mp_schedStack` mapped USER (`0x7`→`0x3`) — ring 3 can scribble kernel scheduler stacks | Hole | XS | ABI § Memory-protection #1 |
| Enable **SMEP** (CR4, CPUID-gated) — blocks ring-0 exec of user pages | Robustness | S | ABI § Memory-protection #2 |
| Allocator OOM: silent `cli; hlt` → panic naming size + largest free extent | Robustness | S | MEMORY #1 |
| Delete debug residue: `>= 200000000` stub in allocator + `kmalloc_dma` twin | Cleanup | XS | MEMORY #3 |
| `#DF` IST emergency stack — turn stack-death triple faults into panics | Robustness | S–M | ABI (deferred) |

## Security / correctness

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| `mp_schedStack` USER bit (`0x7`→`0x3`) | Hole | XS | none | ABI § Mem-prot #1 |
| SMEP enable (CPUID.(07H,0):EBX bit 7) | Robustness | S | none | ABI § Mem-prot #2 |
| SMAP enable + `stac`/`clac` bracketing (CPUID bit 20) | Robustness | L | when copy helpers are the ONLY user-access path (argv setup in task.c is the other) | ABI § Mem-prot #3 |
| Page-0 / VA-0 NULL guard — unmap the low identity window so kernel NULL derefs trap (mind the AP trampoline) | Robustness | M | none | MEMORY #5 / ABI |
| Drop IOPL=3 for ring 3 (currently ring 3 can execute `out`) | Hole | S | once fixtures stop using `out` | ABI (deferred) |

## Kernel robustness

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **Task teardown v1 SHIPPED 2026-08-06 (the graveyard shift) — residuals only.** `task_destroy()` exists: kworker buries COLLECTED corpses (retValCollected via wait/reap, autoReap by decree, or orphans) in a two-phase sweep (unlink pass N, free pass N+1 — grace period for lockless /proc walkers), freeing per-thread stacks (the 1MB+64KB per command), thread_t + TID + syscall scratch, path/cwd, static elf_image_t + its open backing file (un-haunts ext2's rm-refusal), and the task_t. STILL LEAKING, deliberately deferred (docs/task_cleanup_notes.md in the net tree is the map): (1) VMA backing pages + vma structs — CoW/fork sharing needs page refcounts before freeing is safe; (2) argv/env blob pages — env is CoW-inherited by children; (3) page-table pages — the paging pool is a bump allocator, pool-free is its own slice; (4) mmaps/shared_objects dlist nodes. Also still true: `task_create` must validate before building (`elf_can_load`) — task_destroy only buries the fully-born; a mid-construction unwind path is separate work. And the /proc grace-period discipline deserves a grown-up successor (snapshot-under-lock or generation counts) | Hole (shrunk to slow drip) | S–M each | VMA/env pages: with page refcounts (fork/CoW arc); pool-free: its own slice | `task.c` undertaker / notes doc |
| `elf_resolve_dynamic_dependencies` still PANICS on a missing/malformed shared-object dependency (task.c). Ring-3-reachable if a dynamically-linked program with a bad `.so` is ever spawned — the static path no longer panics (`elf_can_load`), this one still does | Robustness | S | when userland gains dynamic binaries | `task.c` |
| Allocator silent-OOM → panic | Robustness | S | none | MEMORY #1 |
| `#DF` IST emergency stack | Robustness | S–M | none | ABI (deferred) |
| MAX_CPUS=24 vs. the 3900X's 24 threads (index 24 = one past every per-CPU array) — raise the constant or guard with MAXCORES | Robustness | S | before booting the 3900X | SCHEDULER #5 |
| `free_memory` return value must never be used as an index post-merge/compaction (documented at both ends; latent footgun) | Cleanup | XS | none | MEMORY #4 |
| `mp_timesEnteringScheduler` stride mismatch: `uint32_t[]` in C, but scheduler.S increments `qword ptr [.. + rax*8]` — core N's asm bumps elements 2N/2N+1 while C reads element N (first-pass check reads a value asm never touched; masked by `mp_CoreHasRunScheduledThread`). Make it `uint64_t[]` in C | Hole (latent) | XS | none — found 2026-07-10 during tick diagnostics | SCHEDULER (scheduler.S / scheduler.c) |
| **Panics print raw stack RIPs, no symbolization** — os32 had real traces (`stack_trace.c`: EBP frame-chain walk + in-kernel `.symtab` scan → `module:function`, cascading process→libraries→parent→kernel); os64 regressed to a decoder-ring workflow (addr2line on the host). Design sketch, ratified in spirit 2026-07-26: (1) get the kernel's own ELF via **Limine's kernel-file request** and parse symbols **once at boot into a sorted RAM table** — the panic path may touch NO filesystem and NO kmalloc (the `/partition_info` corpse: that panic fired *because* the fs was unreadable; a trace that reads the disk deadlocks on its own cause); (2) walk the **RBP chain** (honest at `-O0`; the day we build `-O2`, `-fno-omit-frame-pointer` goes in or the chain lies), hopping the 5-QWORD interrupt frames so the trace crosses from fault back into the syscall; (3) `.symtab` name+offset is v1 — Chris leans toward **full debug info (DWARF file:line) loaded at boot** as the destination; the DWARF line program is a real interpreter, so it's a follow-on slice, not a blocker | Robustness | M | none — every future panic pays interest until this lands | panic.c / os32 `stack_trace.c` precedent |

## Userland-gating (blocks the shell roadmap)

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **Interruptible syscall bodies** — `sti` after entry frame / `cli` before sysret; `g_saved_cr3` per-CPU→per-thread; audit handlers | Feature-gate | M | HARD GATE before `read` (userland step 2) | ABI § Interruptibility |
| RAX:RDX error convention (resolved in design) | Feature-gate | S–M | with libos64 scaffolding | ABI § register contract |
| Natural typed handler signatures via SYSCALL_DEFINE cast (resolved in design) | Cleanup | S | with the RAX:RDX refresh | ABI § add-a-syscall |
| Fork register-load path (`scheduler_load_thread` panics on `justForked`) | Feature-gate | M | when fork/exec is implemented (both patterns now first-class; fork is the author's favorite — treat it well) | SCHEDULER #1 |
| FPU/SIMD state in the context switch (lift `-mno-sse` from user builds after) | Feature-gate | M | before userland wants floating point (blocks ps/top-style cpu%) | ABI (deferred) / LIBOS64 |
| libos64 `LIBOS64_HIDDEN` macro + `-fvisibility=hidden` build wiring (PLT-free internal calls, the modern replacement for libChrisOS's `I` twins) | Feature-gate | S | first libos64 scaffolding task | LIBOS64 § shared object |
| libos64 buffered `<os64/stdio.h>` layer (FILE*/fread/fwrite over raw handles) | Feature-gate | M | second phase — raw `<os64/io.h>` is enough for the shell | LIBOS64 § handles |
| **`map`/`unmap` syscalls + a libos64 `malloc`.** Userland has NO dynamic memory yet. Design ratified 2026-07-12: **NO `brk`/`sbrk`** — a single "end of heap" pointer can only return memory from the END, so one long-lived allocation on top pins everything under it (musl abandoned `brk` entirely for exactly this). Instead the kernel exposes **regions** (`map(len) → VA`, `unmap(VA, len)`) over the VMA demand-paging layer that already exists — a mapped region costs nothing until touched — and libos64 carves them into a `malloc`/`free`. Kernel stays dumb (regions); the cleverness lives in the userland allocator, so it can be swapped without touching the kernel. `TASK_HEAP_START` stops meaning "bottom of one growing heap" and starts meaning "where anonymous mappings live" | Feature-gate | M | before any app needs dynamic memory (i.e. the first non-trivial utility) | MEMORY / `vma.c` |
| **Userland signal delivery.** `SIGPIPE`'s default action (terminate) is currently enforced *inside the kernel* (`raise_sigpipe_and_die`) because ring 3 cannot install a handler yet. Consequence: a program that wants to SURVIVE a vanishing reader can't — it always dies. Fix = deliver signals to ring 3 (handler + a way to return), after which `pipe_write` can hand `PIPE_ERR_CLOSED` back to a program that asked for it | Feature-gate | M | when a program needs to catch SIGPIPE (or any signal) | `signals.h` SIGPIPE / `syscall.c` |
| **Named pipes (FIFOs).** Same pipe object, same semantics — the ONLY difference is the rendezvous: a VFS name instead of inheritance across spawn. A FIFO holds no data (0 bytes on disk forever); it is just a name that resolves to a kernel pipe object, and `open()` blocks until both a reader and a writer arrive. `pipe.c` was written unaware of how it was created, precisely so this is additive. Also the first VFS node that isn't a file — the same door **procfs** walks through | Feature-gate | M | when two UNRELATED processes need to talk | `pipe.c` / VFS |
| **`backgroundJob` is not inherited across spawn.** A background job that spawns its own child mints a FOREGROUND grandchild, whose console reads rejoin the keyboard queue and compete with husk for keystrokes — exactly the hole `&` closed, reopened one generation down. Latent: nothing backgrounded can spawn today. When the ruling comes, inherit-if-parent-is-background is the likely shape (one line in `spawn_do_create`), but it interacts with `fg` — un-backgrounding a job should probably reach its live descendants too, which is where this stops being one line | Hole (latent) | S | when anything backgrounded can spawn (husk scripts, a backgrounded shell) | `syscall.c` spawn_do_create / `console.c` |

## Performance / fairness / cleanup

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **PAID 2026-08-06 — the buffer cache (block_cache.c).** The old row: one NVMe command per fs block, 30MB ≈ 10s. The cache's 64KB line fills ARE the coalescing (one command per 64KB cold, zero warm — 46MB wc: warm pass ≈ 2s, memcpy-bound), and both filesystems plus the partition scanner ride through the interposed bops with zero call-site changes. RESIDUE, kept honest: `ext2_bmap` still kmallocs scratch and re-asks for indirect blocks per data block — the cache absorbs the DISK cost of that (indirect lines stay hot) but the CPU churn remains; a per-handle indirect-block cache in ext2_handle_t retires it if it ever shows in a profile | Perf | S (residue) | if ext2 CPU time ever matters | the grep autopsy → the caching arc, 8/6 |
| **`kmalloc_dma` guarantees nothing about WHERE.** It identity-maps whatever phys `allocate_memory_aligned` serves — no address ceiling, no device-reachability contract. Today's devices (NVMe/xHCI) survive on their own 64-bit DMA capability, not on any guarantee of ours; an allocator that serves high addresses first (Chris's experiment did exactly this) would also expose the SECOND tooth: identity-mapping high phys means mapping at that same high VA — upper-half KERNEL territory, a collision waiting for a page. Fix: give kmalloc_dma an explicit reachability contract (bound the phys, or map at HHDM instead of identity and hand back the phys separately), per-device-class ceilings documented at the callers | Hole | S–M | before any 32-bit-DMA device (and e1000's rings are a fine excuse to look) | Chris's allocator experiment, 2026-08-05 |
| **FAT backward seek is O(position): `FF_USE_FASTSEEK=0`.** Every backward f_lseek rewinds to the file's FIRST cluster and walks the chain to the target — readline's seek-back gait went O(n²) on the 46MB log (4,320 lines/60s, decelerating; the grep autopsy 2026-08-06). os64_linereader cured the LINE-READING case userland-side (forward-only, zero seeks); this row is everyone else — any seek-heavy access to a big FAT file pays it still. Fix: FF_USE_FASTSEEK=1 + a per-FIL cluster link map (cltbl, allocated at open for files past a size threshold) | Perf | S–M | the next seek-heavy consumer of big FAT files (a streaming less would qualify) | ffconf.h line 37 / the grep autopsy |
| **NVMe PRP-LIST path: now LOAD-BEARING (2026-08-06)** — every block-cache line fill is a 128-sector (64KB = 16-page) command, so the once-never-run `prpCount > 2` branch executes hundreds of times per boot and the whole 24+34 suite rides on it (green first boot). Remaining diligence: a deliberate edge test (runs straddling the 2-page and MDTS boundaries, unaligned starts) has still never run ON PURPOSE — cheap insurance now that the branch carries the OS | Robustness | XS | next test-writing pass | the envelope autopsy 8/5; baptized by the cache 8/6 |
| **Caching arc phase 2** (each its own ruling, per the 8/6 phase split): memory-pressure eviction + the os64_memory_t `reclaimable` seat (drop-on-demand must be REAL before `available` counts cache bytes — the allocator discovers pressure holding its irqsave lock, so the eviction hook needs its own design conversation); write caching (deferred: no consumers; the 46MB-copy disk-full adventure = first wild consumer sighted); async readahead (wants completion infrastructure — the NVMe-interrupts era). Update-in-place on write was PROMOTED OUT of this row the night it was written: the first measured ext2 copy showed invalidate-and-refill reading 1.2GB to copy 1.5MB | Feature-gate | M–L | when a consumer demands it, ruling by ruling | block_cache.h phase-2 block |
| **NVMe queue depth: one command in flight, ever** (nvme_do_io submit→spin→reap under ioLock — the 64-deep submission queue stands empty). Evidence from the 8/6 iostat histogram: the ext2 copy's 29,760 writes were 2-block commands paying full serialized round trips each. Fix = pipelined multi-command submission WITHIN a request (submit K, reap all before returning — durability preserved, NOT write caching). Re-measure after cp (1MB buffer, Chris's) + update-in-place land: both may shrink the command count enough to demote this | Perf | M | re-measure first; then the next write-heavy sufferer | the iostat confession, 8/6 late |
| **`top` defeats parking: settle-on-read wakes EVERY core.** `mpAcctSettleAll` (the /proc CPU-time render) IPIs all cores so each settles its in-flight span on its own TSC — correct books, but a PARKED core gets woken just to report "nothing in flight," so `top` shows "0 parked" on an idle machine. Observer effect: the measurement destroys the state it reports (spotted by Chris on the 2-hour VBox e1000 soak, 2026-08-06, idle high-90s%). Fix sketch: settle the books AT PARK TIME — the pass that sends a core into `hlt` charges its in-flight span first (one TSC read, on the core itself, so the no-cross-core-TSC-math law holds); the reader then skips cores sitting in their idle thread and counts them parked. Bonus: parked count becomes an honest first-class stat instead of a self-defeating one | Perf | S | when top's parked count should read true (it just started to matter) | smp_core.c settle-on-read / procfs.c / the tickless default |
| AP sibling starvation — no intra-pass timeslicing under a CPU-bound thread | Robustness | L | when it bites | SCHEDULER #2 / GRAPHICS #10 |
| Move scheduler printds OUTSIDE `kSchedulerSwitchTasksLock` (the convoy: DETAILED must be slow-but-honest, never a lockup) | Robustness | M | none — Chris's "bend to my will" requirement | SCHEDULER #4 / autopsy |
| Wall-clock hardening: early EOI via prologue reorder (guard before `temp_rsp` store, then EOI after the 5 frame reads) and/or IRQ0 above scheduler priority | Robustness | M | Chris's #1 pick post-autopsy | SCHEDULER #7 / autopsy |
| `kSchedulerSwitchTasksLock`: irqsave + bounded-spin panic tripwire (send_ipi ICR pattern) — also the VBox-freeze hunt instrument | Robustness | S | none | SCHEDULER #8-9 |
| **logd file sink** — at root-mount, logd drains to a file (ramdisk = memcpy-speed, kills steady-state queue creep; NVMe boots get persistent logs). Serial stays for panics/BOOTMARK/early-boot + optional tee. PRECONDITION: FatFs write support is UNVERIFIED ("the FAT library has never written anything" — check FF_FS_READONLY, then a create/write/readback post-boot test BEFORE wiring the sink) | Robustness | M | after hello-userland (Chris's ordering) | log.c / never-drop rule / 2026-07-11 plan |
| ring-3 klogd (userland log daemon reading kernel log via syscall) | Feature-gate | M | post-shell — needs read-from-kernel-log plumbing | LIBOS64/ABI roadmap |
| Resync `kSystemCurrentTime` from the RTC at end of kernel_init — boot tick-starvation currently leaves the OS clock ~3-4s behind forever (Chris declined for now: "steady is fine"; one-liner when wanted) | Cleanup | XS | when the lag annoys | kernel.c / 2026-07-11 |
| `kCPUCyclesPerSecond` miscalibrated ~10× under QEMU/TCG — trust cycle counts, fix the calibration | Cleanup | S | before anything times itself with it | kernel.c tscGetCyclesPerSecond |
| Uncomment the divide-config write in `ap_configure_scheduler_timer` — arming currently depends on the divisor leftover from calibration | Cleanup | XS | none | smp_core.c |
| Allocator first-fit linear scans (merge scans whole ledger; compaction every 10th free) | Cleanup | L | measure before caring | MEMORY #2 |
| IRQ-safe sub-tick wake primitive | Feature-gate | M | only if a real need appears (100Hz covers the GUI) | SCHEDULER #3 |
| Pipe waiter slots are SINGLE (one parked reader + one parked writer per pipe). A second waiter on the same end is still correct — it falls back on its ~1s SIGSLEEP backstop and re-checks — but it is slow. Fix = a real wait list per end | Cleanup | S | when >1 reader or >1 writer on one pipe becomes normal | `pipe.c` |
| Console scroll runs with **interrupts off**: the renderer spinlock is held across a ~3MB full-screen `memmove`, so a scroll can delay that core's tick by a few hundred µs. Correctness beats the jitter (the console is not a hot path), but if it ever matters the fix is a scroll that does NOT hold the lock — not a lock that does not cover the scroll | Cleanup | S | if the console ever gets hot | `BasicRenderer.c` kRendererLock |
| `handle_alloc`/`handle_close` take no lock — safe today because a task's handle table is touched only by that task's own syscalls (and by `spawn` while the child is still being built, before it is schedulable). Grows a lock the day handles are shared between threads of one task | Cleanup | S | when a task has >1 thread using handles | `handle.c` |

## Threads (os64's first ring-3 threads, 2026-08-02)

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **No thread teardown.** An exited thread becomes a ZOMBIE and its user stack, kernel stack, and `thread_t` are never reclaimed — a program that creates threads in a LOOP leaks a megabyte-plus each time. Fine for the current shape (start N workers, join them, exit, and task teardown takes the address space); not fine the day something pools threads. Rides the same missing machinery as the standing task_destroy debt | Robustness | M | when any program creates threads repeatedly rather than once | `thread_join.c`, `syscall_thread_exit` |
| **No locks, no atomics** — deliberate v1 (Chris's ruling): threads share everything, so the moment two of them touch one variable a mutex is needed, and a mutex designed before its first consumer is the wrong mutex. `threadtest` and `hog` share nothing by construction | Feature-gate | M | the first program with genuinely shared mutable state | `os64/thread.h` |
| `thread_join_create` leaks the `thread_t` if the stack-mapping check fails (the only failure path after createThread). Cannot currently happen — the stack was just mapped — and fixing it properly needs the thread teardown above | Cleanup | XS | with thread teardown | `thread_join.c` |
| No thread affinity control from ring 3: a program cannot say "put this one on its own core." The scheduler's own delegation handles the useful case today | Feature-gate | S | when a program has a reason to care | `syscall_thread` |

## Kernel structure

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **`syscall.c` does too much work.** Chris's os32 principle (2026-08-02): the syscall file should DISPATCH — validate arguments, copy user memory, call the subsystem that owns the verb — with cases of five or ten lines, not implementations. os64 has drifted in layers: the newest syscalls already delegate (`net_dial`→`tcp_conn_dial`, `klog_read`→`klog_dequeue`, `thread`→`thread_join_create`), while the oldest carry their work inline (`read`/`write` own all the chunking and bounce-buffer discipline; `spawn` builds its own parameter block). The seam is obvious when it's worth cutting: boundary work stays at the door, verb work moves to the owning subsystem | Cleanup | L | explicitly LOW priority — "a working OS with more syscalls than its ancestor has earned a fat dispatcher" | `syscall.c` / os32 comparison |

## Userland utilities

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| `tail`: a SHORT READ inside the backward block scan silently loses lines. If the fill loop ends with `filled < blockSize`, only the bytes that arrived are scanned but `blockEnd` still moves a whole block, so newlines in the unread tail are never counted — fewer lines printed than asked, no error. Needs an I/O error or a mid-read shrink to trigger. Fix = treat a short fill as an error, or rescan from `blockStart + filled` | Cleanup | XS | when a real short read is ever observed | `tail.c` find_tail_start |
| `tail` reads no STDIN — `something \| tail` exits 2 instead of following the pipe. A pipe can't seek, so `-n` would need a keep-the-last-N ring instead of a backward scan; that is the actual work | Feature-gate | S | when a pipeline wants it | `tail.c` main |
| `tail` computes "the last N lines" from the `stat` size taken BEFORE the open, so a file that grows in between is measured slightly stale. Self-corrects instantly under `-f`; harmless otherwise | Cleanup | XS | never, probably | `tail.c` main |

## GUI (all GRAPHICS.md "future work")

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| Tickless + GUI coexistence — add the damage-wake nudge IPI in `gui_damage_add`, lift `gui_start()`'s pin refusal. Grew teeth 2026-08-05: tickless is the DEFAULT now, so every GUI entry rides `SCHED=periodic` until this lands | Feature-gate | S | when the GUI should boot on the default scheduler | GRAPHICS #1 / SCHEDULER #6 |
| Legacy text console (`BasicRenderer.c`) has no SMP locking | Robustness | S | if legacy console is used under SMP (GUI path doesn't) | GRAPHICS #9 |
| Damage rect LIST instead of single union | Cleanup | S | when multi-region damage matters | GRAPHICS #4 |
| PAT write-combining for the framebuffer flush (replaces PCD/UC mapping) | Cleanup | M | measure first — dirty-rect flushes are already small | GRAPHICS #3 |
| Window resize / close / minimize; honor `GUI_WINDOW_NO_DECORATIONS` | Feature-gate | M | UX | GRAPHICS #5 |
| Mouse wheel + 5-button (IntelliMouse sample-rate handshake) | Feature-gate | S | UX | GRAPHICS #6 |
| Alpha translucency (X byte in XRGB reserved; `surface_blit_masked` already shapes) | Feature-gate | M | UX | GRAPHICS #7 |
| Runtime resolution switching (only `framebuffers[0]` used) | Feature-gate | M | UX | GRAPHICS #8 |
| Verify SIGSLEEP wake latency holds under load (fixed to ~1 tick; unverified at load) | Cleanup | S | none | GRAPHICS #10 |
| **libdraw/libui** userland graphics library — port surface.c primitives to ring-3 (L1) + the widget model (L2, grown app-driven); embedded PSF1 font; cadence-agnostic frame loop | Feature-gate | M–L | after GUI syscalls 16-22 land in the dispatch table + libos64 scaffolding | LIBDRAW.md |

## Build system

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **PIE + loader-assigned per-TASK load bias.** Apps are static `ET_EXEC`. Each now links at a UNIQUE base (`userland/tools/app_bases.py`, hashed from the app name) so GDB — which has one global symbol table keyed by address — can hold every app's symbols at once; without it, two programs in a pipeline both sat at `0x400000` and address→symbol lookup was a coin flip. That is a **stopgap** with three limits: (a) two instances of the SAME program (`upper \| upper`) still share a base and are indistinguishable in GDB; (b) a 16MB image cap per app; (c) 104 app slots. Real fix: build apps `-fPIE -pie`, teach `elf_loader` `ET_DYN` (choose bias, map segments at bias+vaddr, apply `R_X86_64_RELATIVE`), assign a unique bias **per task** into `task->loadBias`. Most plumbing already exists — `loadBias`, `elf_relocate.c`, and the autoloader's `add-symbol-file -o <bias>`. | Feature-gate | M | when a pipeline runs the same program twice, or an app outgrows 16MB | `task.c` debug_task_loaded / `app_bases.py` / `elf_relocate.c` |
| `--defsym` **must precede `-T`** on the ld command line or it is silently ignored and every app links at the `0x400000` fallback — builds fine, runs fine, and fails only in the debugger. Guarded by a comment at the flag; a build-time assert (entry == assigned base) would make it loud instead of documented | Robustness | XS | none | `userland/GNUmakefile` APP_RULE |
| os64 FAT driver can't read a sub-FAT32-minimum volume: `mformat -F` forces FAT32, but <65525 clusters (~34MB) yields a malformed hybrid the driver misclassifies as FAT16 → every open fails. Currently worked around by the 64MB DISK_SIZE_MB floor. Fix = teach the driver FAT16 (or drop `-F` and let mformat pick), so the ramdisk can shrink below 64MB | Robustness | M | if the ramdisk ever needs to be <64MB | GNUmakefile disk target / fat driver |

## Networking (the arc's booked honesty — NETWORK.md carries the arguments)

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| **DHCP lease renewal** (T1/T2 timers) + RELEASE/DECLINE courtesies — v1 leases once and never renews; slirp/VBox leases are functionally eternal, a real LAN's are not | Feature-gate | M | before long uptimes on a real LAN (the P5) | dhcp.h policy note |
| ARP cache miss DROPS the packet (counted, -2 to caller) after firing the query — the honest 1982 behavior. Fix = park one packet per neighbor, send on reply | Cleanup | S | when first-packet loss annoys a caller that can't retry-loop | ipv4.c ipv4_send |
| IPv4 fragment REASSEMBLY — arrivals with MF/offset drop loudly (counted + logged). Modern DF-world paths rarely fragment; "rarely" is not "never" | Feature-gate | M | when something real fragments at us | ipv4.c fragment stance |
| ICMP port-unreachable when UDP has no binding (how `traceroute` ends and `nc -u` probes learn) — currently counted-and-silent | Feature-gate | S | when a consumer needs the courtesy | udp.c rx_no_binding |
| **virtio-net polls** (processSignals rider, the xhci_poll precedent) — NETWORK.md Phase 1 planned MSI-X from the start; polling shipped first because it made packets move the same day. The e1000 crossed over 2026-08-06 (INTx doorbell, probe-confirmed routing); virtio stays polled ON PURPOSE until its own conversion — virtio interrupts are per-queue MSI-X machinery, a different slice. Mind the house rule (AP-routed vectors ≥ 0x40) | Feature-gate | M | before any throughput-sensitive consumer (TCP at real speeds) | virtio_net.c / NETWORK.md Phase 1 |
| processSignals runs the device polls AND the whole inline protocol stack UNDER kSchedulerSwitchTasksLock — its hold time scales with traffic. Chris's ruling (2026-08-01): fine under BSPSCHED at current load (the beefy pass runs on the core that owns scheduling anyway); do NOT shrink the lock scope now — that cleanup lands WITH the net bottom-half thread (the MSI-X row above), when the polls move to a kworker and processSignals returns to sleepers + wake sweeps. Measure (DEBUG_NET cycle stamp) before any surgery | Cleanup | S | with the MSI-X / bottom-half work, pre-TCP-throughput | signals.c processSignals / 8/1 design conversation |
| **e1000 INTx Stage 2 — the bottom half**: today the ISR raises kE1000RxWork and processSignals drains on its next pass (doorbell ends the empty-mailbox reads; latency still ≤ one pass). The full prize is the ISR WAKING a drainer thread directly — which needs a wake-from-ISR primitive the scheduler doesn't have (wake sweeps live in processSignals precisely because of the 9badced lock saga). DESIGN CONVERSATION BEFORE CODE, per the known-debt-beforehand rule (2026-08-06) | Feature-gate | M | with the bottom-half/lock-scope work above — one design, three rows retire together | e1000.c e1000_isr / 8/6 briefing |
| **No shared-INTx framework**: e1000's ISR practices shared-line etiquette (ICR==0 → count a stranger, decline politely) but nothing CHAINS handlers on one GSI. Also note: e1000_enable_intx sets PCI Interrupt Disable (command bit 10) on every OTHER device — os64's drivers all poll, so this makes the de facto official; the day a second driver wants a doorbell it must clear its own bit AND this framework gets built. intx_strangers climbing = a neighbor moved onto GSI without telling us | Feature-gate | S | the day a second PCI device adopts INTx | e1000.c enable_intx probe-safety comment |

## Explicitly NOT debts (recorded so they aren't re-litigated)

- **Shared kernel upper half** (every task PML4 → kernel's own tables,
  U/S-protected): a deliberate, sound decision for os64's threat model
  (we run our own binaries). Meltdown/KPTI unshared it for a speculative
  side-channel that is out of scope. See ABI § Memory-protection.
- **The "Time to make the donuts" scheduler message**: load-bearing
  tradition, not debug noise. Never remove/reword. See SCHEDULER § donuts.
- **Pipes carry TWO copies (user→kernel ring→user), not shared memory.** The
  tempting "map one page into both tasks" optimization puts the ring's
  head/tail pointers in memory USERLAND CAN WRITE, so a buggy program could
  corrupt or hang the process on the other end. The kernel must be the arbiter
  of the buffer. (Same call GRAPHICS.md makes: syscalls carry handles, never
  pixels.) A future zero-copy `splice` can be added *behind the handle API*
  without changing one line of any program — that is what handles are for.
- **A bounded pipe is not a limitation — the bound IS the flow control.** A
  writer that fills the 64KB ring BLOCKS, which throttles the producer to
  exactly the consumer's speed and is the only reason a pipeline runs in
  constant memory. Do not "fix" this by growing the buffer on demand; an
  unbounded pipe is a memory leak with a friendly API. See `pipe.h`.
- **spawn does NOT blanket-inherit the parent's handle table** (Unix does). A
  child gets the console plus exactly the handles it was explicitly given.
  Deliberate: a child can never accidentally hold some unrelated pipe end open,
  which is a classic way pipelines hang. Do not "fix" this into Unix semantics.
- **Write atomicity is capacity-bounded, not `PIPE_BUF`-bounded.** os64's rule
  is one sentence — *a write of ≤ PIPE_CAPACITY lands whole, or it waits* —
  instead of Unix's magic 4096 constant that everyone has to memorize. Reads
  return SHORT and writes land WHOLE; the asymmetry is intentional (a reader
  that waits to fill its buffer deadlocks interactive pipelines; a writer that
  lands whole keeps records intact). See `pipe.h`.
