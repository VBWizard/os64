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
| Allocator silent-OOM → panic | Robustness | S | none | MEMORY #1 |
| `#DF` IST emergency stack | Robustness | S–M | none | ABI (deferred) |
| MAX_CPUS=24 vs. the 3900X's 24 threads (index 24 = one past every per-CPU array) — raise the constant or guard with MAXCORES | Robustness | S | before booting the 3900X | SCHEDULER #5 |
| `free_memory` return value must never be used as an index post-merge/compaction (documented at both ends; latent footgun) | Cleanup | XS | none | MEMORY #4 |
| `mp_timesEnteringScheduler` stride mismatch: `uint32_t[]` in C, but scheduler.S increments `qword ptr [.. + rax*8]` — core N's asm bumps elements 2N/2N+1 while C reads element N (first-pass check reads a value asm never touched; masked by `mp_CoreHasRunScheduledThread`). Make it `uint64_t[]` in C | Hole (latent) | XS | none — found 2026-07-10 during tick diagnostics | SCHEDULER (scheduler.S / scheduler.c) |

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
| Kernel makefile `-MMD` dependency tracking is broken (no `.d` files generated) — forces a manual `make -C kernel clean` after every header change | Robustness | S–M | none — real daily footgun | VERIFICATION § build |
| os64 FAT driver can't read a sub-FAT32-minimum volume: `mformat -F` forces FAT32, but <65525 clusters (~34MB) yields a malformed hybrid the driver misclassifies as FAT16 → every open fails. Currently worked around by the 64MB DISK_SIZE_MB floor. Fix = teach the driver FAT16 (or drop `-F` and let mformat pick), so the ramdisk can shrink below 64MB | Robustness | M | if the ramdisk ever needs to be <64MB | GNUmakefile disk target / fat driver |

## Explicitly NOT debts (recorded so they aren't re-litigated)

- **Shared kernel upper half** (every task PML4 → kernel's own tables,
  U/S-protected): a deliberate, sound decision for os64's threat model
  (we run our own binaries). Meltdown/KPTI unshared it for a speculative
  side-channel that is out of scope. See ABI § Memory-protection.
- **The "Time to make the donuts" scheduler message**: load-bearing
  tradition, not debug noise. Never remove/reword. See SCHEDULER § donuts.
