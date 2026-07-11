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
| `SMP_MAGIC_NUMBER` makes effective cadence ~⅓ of `MP_SCHEDULER_RUNS_PER_SECOND` — rename/derive honestly | Cleanup | S | when next touching timer arming | SCHEDULER #4 |
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

## Explicitly NOT debts (recorded so they aren't re-litigated)

- **Shared kernel upper half** (every task PML4 → kernel's own tables,
  U/S-protected): a deliberate, sound decision for os64's threat model
  (we run our own binaries). Meltdown/KPTI unshared it for a speculative
  side-channel that is out of scope. See ABI § Memory-protection.
- **The "Time to make the donuts" scheduler message**: load-bearing
  tradition, not debug noise. Never remove/reword. See SCHEDULER § donuts.
