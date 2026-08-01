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
| **No task teardown — EVERY SPAWN LEAKS A TASK.** Nothing in the tree ever frees a dead task's memory: `task_reap_eligible_zombies` → `scheduler_reap_zombie_thread` only DEQUEUES the zombie thread (sets state NONE); the `task_t`, its PML4 + page tables, its kernel/user stacks, and its argv/env pages are never `kfree`d. Every `/bin/hello` from husk leaks the lot. Harmless at hobby scale today, fatal for a shell you leave running. Fix = a real `task_destroy()` (free stacks → page tables → argv/env → task_t), called from the reaper. It ALSO unblocks failing cleanly *after* allocation: `task_create` currently must validate the path BEFORE it builds anything (`elf_can_load`) precisely because there is nothing to unwind with | Hole (slow leak) | M | before anything runs for a long time spawning children | `task.c` reap path / `scheduler.c` |
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

## GUI (all GRAPHICS.md "future work")

| Debt | Sev | Cost | Gate | Source |
|---|---|---|---|---|
| BSPSCHED + GUI coexistence — add the damage-wake nudge IPI in `gui_damage_add`, lift `gui_start()`'s pin refusal | Feature-gate | S | if BSPSCHED+GUI is wanted | GRAPHICS #1 / SCHEDULER #6 |
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
| **virtio-net polls** (processSignals rider, the xhci_poll precedent) — NETWORK.md Phase 1 planned MSI-X from the start; polling shipped first because it made packets move the same day. Interrupts before throughput ever matters; mind the house rule (AP-routed vectors ≥ 0x40) | Feature-gate | M | before any throughput-sensitive consumer (TCP at real speeds) | virtio_net.c / NETWORK.md Phase 1 |

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
