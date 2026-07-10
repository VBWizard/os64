# os64 Graphics & Windowing Subsystem

*The design record for the GUI foundation. Written so a future contributor (human
or model) can extend this without re-deriving any decision.*

## What this is

An optional, kernel-resident windowing system — the DOS↔Windows relationship:
**the OS runs fully without it.** Enabled by the `GUI` kernel cmdline flag
(there's a `/QEMU GUI Boot` Limine entry); without the flag, boot behavior is
byte-for-byte the pre-GUI kernel: text console, tests, `shutdown()` idle loop.

With `GUI`: after post-boot tests, `kernel_init()` calls `gui_start()` and parks.
The compositor daemon owns the screen: desktop, overlapping draggable windows,
mouse cursor, and a console window that receives everything `printf`/`print_n`
produce. Two demo apps (`/gbounce`, `/gkeys`) run as kernel threads against the
client API.

## Layer map (bottom-up)

| Layer | Files | Job |
|---|---|---|
| 0. Surfaces | `gui/gui_types.h`, `gui/surface.{h,c}` | `rect_t`/`surface_t`, clipping blits/fills/text, `surface_flush_rect` (the ONLY code that touches the hardware framebuffer) |
| 1. Input | `driver/system/mouse.c`, `keyboard.c` (`ps2_handle_irq`), `gui/input.{h,c}` | PS/2 drivers feed one unified event ring (`input_event_t`): keys + mouse in arrival order |
| 2. Window system | `gui/window.{h,c}` | `window_t`, z-order list, chrome (titlebar/border), hit-testing, per-window event queues |
| 3. Compositor | `gui/compositor.c` | The `/guicomp` daemon: frame loop = drain input → route → recomposite damage → flush; cursor; drag state machine |
| 4. Client API | `gui/gui_client.{h,c}` | Handle-based, **syscall-shaped** functions apps call; the future userland boundary |
| — | `gui/console_window.{h,c}` | print_n's sink → text grid → rendered like any window |

## The core invariants (break these and you get flicker/corruption)

1. **The backbuffer is the canonical screen image.** All rendering goes into
   cacheable RAM surfaces; the uncached (PCD-mapped) hardware framebuffer only
   ever receives finished pixels via `surface_flush_rect`, and is **never
   read**. Scrolling/clears never do read-modify-write on UC memory.
2. **Damage drives everything.** `gui_damage_add[_locked]()` accumulates one
   union rect (v1); each frame the compositor recomposites only that region:
   desktop → windows bottom-up (`wm_composite`) → cursor last → flush. Cursor
   motion damages old∪new positions; the recomposite restores what was under
   it — the backbuffer IS the save-under.
3. **kGuiLock serializes all window-system state** (z-list, event queues,
   damage, handle table; see `gui/gui_internal.h`). `wm_*` functions assume it
   held; `gui_*` client calls and the compositor acquire it. Compositing (RAM,
   sub-ms) happens under the lock; flushing (UC, slow) strictly after release.
   `gui_damage_add_locked` vs `gui_damage_add`: pick by whether you hold the
   lock — it is NOT recursive.
4. **IRQ handlers only enqueue.** Never call `scheduler_wake_isleep_task` from
   an ISR (`scheduler_trigger` does `sti/hlt`). The compositor polls.
5. **The console sink never draws and never takes kGuiLock.** `print_n` may be
   called from any context including exception handlers; the sink only appends
   to a character grid under its own tiny lock. The compositor turns the grid
   into pixels each frame (`gui_console_render_if_dirty`).
6. **Panic wins.** `panic()`/`panic_no_shutdown()` first call
   `gui_emergency_disable()` — one lock-free store (`kConsoleSink = NULL`) —
   then print via the legacy direct-to-framebuffer path over the desktop.

## Input path

```
IRQ1/IRQ12 → ps2_handle_irq (keyboard.c): drain 8042, dispatch on status bit 5
   ├─ keyboard_handle_scancode → emits on MAKE (press) + input_inject_key(down/up)
   └─ mouse_handle_byte (mouse.c): 3-byte packets, sync bit + 30ms timeout resync
        → input_inject_mouse(dx, dy, buttons)
input.c: one ring (irqsave spinlock), tracks cursor position (clamped) and
         diffs button state into discrete DOWN/UP events
compositor: drains per frame; routes under kGuiLock:
   clicks → hit-test top-down → raise+focus; titlebar+left = drag grab
   moves  → cursor damage; drag → wm_move; else deliver content-local to window
   keys   → focused window
```

### IRQ routing — the IMCR story (important history)

`remap_irq0_to_apic()` switches the platform IMCR to APIC mode, which cuts the
legacy PIC's INTR wire. Nothing programs LINT0 as ExtINT, so PIC-delivered
interrupts only kept working where firmware left virtual-wire mode on (QEMU
does; VBox/real hardware not guaranteed) — the keyboard was likely already
dead on real hardware post-boot and nobody noticed because nothing consumed
its buffer. Therefore **any IRQ we depend on rides the IOAPIC**:
`ioapic_adopt_isa_irq(irq, vector, &kIRQnUsesLapic)` (apic.c) programs the
redirection entry (honoring MADT ISO overrides — VBox burned us here before),
masks the PIC line, and flips the handler's EOI flag (LAPIC at +0xB0 vs PIC
`out 0x20`, dual EOI for slave-PIC IRQ12 in the fallback path). IRQ1 is
adopted unconditionally at boot; IRQ12 only when `GUI` is set.

## Frame pacing (and its scars)

- **The design: hlt-wait, always.** The compositor never sleeps through the
  scheduler and never busy-waits. Each frame pass it checks for work (input
  queued? damage pending?) and, finding none, executes `sti; hlt` — halting
  its core until the next interrupt. kernel_init routes the input IRQs
  (1, 12) at the compositor's core precisely so a mouse packet or keystroke
  ends the halt directly: input-to-screen latency is one interrupt, and an
  idle (or even actively-mousing) desktop costs ~zero CPU, guest and host.
  The `cli` → check → `sti; hlt` sequence is the standard lost-wakeup-free
  idle idiom (sti's one-instruction interrupt shadow covers the hlt).
- **Scar #1 (SIGSLEEP pacing) — HEALED:** wakes ride the scheduler's
  `processSignals` pass, which ran only ~5-10/sec during bring-up — worst-case
  a few hundred ms of input latency, and why the SIGSLEEP-paced bounce demo
  crawled at ~5-10fps. The cadence bump (`MP_SCHEDULER_RUNS_PER_SECOND`
  10→100 = one pass per 10ms tick) fixed both: nominal wake latency is now
  ~one tick and the ball runs ~33fps. The lesson stands: SIGSLEEP-paced
  clients are capped by signal cadence, not by the compositor.
- **Scar #2 (tick spin):** the interim "spin to the next tick while input is
  hot" workaround ate one full core — a full HOST core under VBox/QEMU —
  whenever the mouse moved. And its first version spun on `rdtsc()`: per-core
  TSCs under QEMU/WSL2 are desynchronized enough that a cycle target computed
  before a preemption could resume near-eternal. Never compare TSC values
  across a possible preemption.
- **Scar #3:** `BSPSCHED` mode masks AP scheduler timers
  (`enableAPScheduling_ISR`) — a thread pinned to an AP runs unpreemptable and
  nudge-only. The compositor pins to core 1 for smoothness **except** under
  `kBspSchedulerMode` (`gui_compositor_affinity()`), where it stays unpinned
  on the BSP. The GUI boot entry deliberately omits `BSPSCHED`. (Fixable:
  the hlt-wait compositor plus a damage-wake nudge IPI in `gui_damage_add`
  would let the two coexist — see SCHEDULER.md's BSPSCHED section.)

## The userland boundary — full design (unbuilt; implement from this section)

*This chapter is the design record for moving GUI clients to ring 3. It is
written to be sufficient on its own: an implementer who has read the rest of
this file should need no other source of decisions. Design decided 2026-07-07.*

### The cost model (why the design is shaped this way)

A `SYSCALL`/`SYSRET` round trip is ~100-300 cycles — under 100ns. One
1024×768×32bpp frame is 3MB; copying it once at ~10GB/s costs ~300µs, three
to four **orders of magnitude** more than the syscall requesting it. So ring
crossings are not the enemy; **pixel copies and per-operation chatter are.**
Every surviving windowing system converged on the same split (X11 painfully
via XShm/DRI after its serialize-drawing-over-a-socket protocol proved slow;
Wayland was founded on "pixels never travel over the protocol"; NT4 moved GDI
into the kernel for the same reason). The os64 statement of it:

> **Syscalls carry handles, rects, and signals — never pixels.**

(Same principle behind Linux's vDSO `gettimeofday`: when a call is hot
enough, even 100ns matters, so the hot path moves to shared memory and the
syscall remains only as the slow/setup path.)

### The three planes

| Plane | Mechanism | Frequency | Cost |
|---|---|---|---|
| Data (pixels) | shared canvas mapped in the task's PML4; publish snapshots the damage rect into the compositor's copy (see "Atomic frames") | every frame | zero syscalls, one damage-bounded RAM copy |
| Control | 7 tiny syscalls (numbers 16-22, below) | window lifetime + 1 publish/frame | ~100ns each; ~6µs/sec at 60fps |
| Events | per-window queue filled by the compositor; app polls (later: blocks) | input rate | one syscall per poll/wait |

### Syscall surface (numbers frozen in syscall_numbers.h)

Existing calls keep their reserved numbers and the `user_ptr_mask` annotated
at each definition in `gui_client.c`. os64 convention throughout (RAX/RDI/
RSI/RDX/R10/R8/R9, negative errors) — ours, not Linux's.

| # | Name | Notes |
|---|---|---|
| 16 | `gui_window_create(title, x, y, w, h, flags)` | title copied in via `copy_user_string` |
| 17 | `gui_window_destroy(handle)` | owner-checked (below) |
| 18 | `gui_window_get_surface(handle, surface_t *out)` | **the pivot** — returns a TASK va in `out->pixels` (below) |
| 19 | `gui_window_publish(handle, const rect_t *damage)` | damage in content coords, NULL = whole content. Renamed from `gui_window_present` at design review — "present" doubles as an adjective ("is it present?") and is swapchain jargon besides; the code rename lands with the dispatch rows |
| 20 | `gui_event_poll(handle, input_event_t *out)` | non-blocking; 1/0/negative |
| 21 | `gui_screen_info(uint32_t *w, uint32_t *h)` | |
| 22 | `gui_event_wait(handle, input_event_t *out)` | NEW, ships LAST — blocking poll; see "Event delivery" |

Add one error to gui_client.h: `GUI_ERR_NOT_OWNER (-5)` — handle exists but
belongs to another task.

The dispatch rows are mechanical: `SYSCALL_DEFINE(...)` in `syscall_table[]`
(syscall.c), `needs_cr3_switch=false` (handlers run under the user CR3; GUI
state is upper-half and visible from any CR3), masks as annotated, struct
copies via `copy_user_buffer` exactly like `syscall_write` does.

### The surface pivot (the one real implementation change)

Today `wm_create` → `surface_create` backs window content with
`kmalloc_aligned` and `gui_window_get_surface` hands that kernel pointer to
kernel-thread clients. Under userland each window gets TWO pixel stores, and
— important — **every step below runs inside the kernel's syscall handlers.
The app never allocates surface memory; its own malloc/heap is uninvolved.**
The app's whole world is: get a pointer once, draw into it, publish.

1. **The content surface stays exactly as it is** (kernel-side
   `kmalloc_aligned`): it is the compositor's stable copy, and the
   compositing path does not change at all.
2. `gui_window_create`'s handler additionally allocates the window's
   **canvas** with `allocate_memory_aligned(w*h*4)` (task-owned memory, NOT
   kmalloc — see CLAUDE.md "When to use each") and maps it into the task's
   PML4 at a task VA with `PAGE_PRESENT | PAGE_WRITE | PAGE_USER` —
   **eagerly, at create time**. The kernel records `phys | kHHDMOffset` in
   `window_t` as its own view of the same pages.
3. `gui_window_get_surface` returns the canvas task VA in `out->pixels`
   (`pitch_px == width`; format stays XRGB8888).
4. `gui_window_publish`'s handler copies the damage rect canvas → content
   (reading through the HHDM alias) under kGuiLock, then queues screen
   damage. See "Atomic frames" below for why this copy exists and what it
   costs.

Pixels cross the ring boundary zero times and are copied once,
damage-bounded — that copy is the price of tear-free frames, and it is the
same kind of blit the compositor already does content → backbuffer on every
damaged frame.

**Invariants introduced here:**

- **Canvases are eagerly backed, never demand-paged.** The publish handler
  reads the HHDM alias, which only exists for *allocated* pages; a
  demand-paged canvas would make the kernel fault on pages the task hasn't
  touched yet. Allocate and map everything up front (a 4K×4K max window is
  64MB of canvas + 64MB of content — the `w/h ≤ 4096` clamp in
  `gui_window_create` is also the memory bound).
- **VA placement:** carve a dedicated per-task region for canvas mappings
  (a VMA, like heap/ELF segments use) so canvases can never collide with the
  heap at 0x70000000 or loaded segments. One region per window; unmapped at
  window destroy.
- **Visibility needs no fences:** the app's pixel stores precede its publish
  syscall in program order on the same thread, so the kernel-side copy sees
  them all (x86-TSO); cross-core consumers (the compositor) only ever read
  the content copy, which is written under kGuiLock.

### Ownership and lifetime (the rules that keep the tripwire silent)

The handle table today is global and unchecked — fine for kernel threads,
unacceptable for tasks (any task could present/destroy any window).

- `window_t` gains an owning task id. Every handle lookup in a syscall path
  validates owner == current task (else `GUI_ERR_NOT_OWNER`). The console
  window and kernel-thread demos are kernel-owned.
- **Teardown ORDER is an invariant:** unlink from z-order + handle table and
  damage the vacated area (all under kGuiLock) **before** freeing canvas pages
  or unmapping the task VA. Freeing first leaves a live window whose canvas
  HHDM alias is gone — the next publish (or composite, in the bring-up
  no-copy variant) hits the lazy-HHDM use-after-free panic. (That panic
  firing with the GUI in the stack IS this bug: a window outlived its pixels.
  The tripwire is doing its job; fix the order.)
- **Task death:** `task_exit_finish()` (task.c — already kernel-context, safe
  to take kGuiLock) must call a new `gui_task_destroy_windows(taskid)` BEFORE
  the task's address space and pages are torn down. Same hook on any kill/
  reap path that frees task memory. Rule of thumb: **windows die before
  pages, always, on every exit path.**
- `wm_destroy` already damages the vacated region, so the compositor
  repaints from surviving windows — nothing else to clean visually.

### Event delivery

- Queues stay per-window (`GUI_WINDOW_EVENTS_MAX 64`, drop-newest on full —
  apps must drain; mouse coords stay CONTENT-local, translated by the
  compositor at routing time).
- `gui_event_poll` (20) is the v1 interface: non-blocking, pairs with the
  app's own frame pacing.
- `gui_event_wait` (22) blocks the calling thread (ISLEEP) until an event
  arrives. The waker is **the compositor** at `wm_deliver_event` time — it
  runs in thread context, so waking from there does NOT violate the "IRQ
  handlers only enqueue" rule (invariant 4). Wake latency stopped being a
  blocker when the scheduler went to 100 passes/sec (scar #1, healed): a
  blocked app now wakes within ~one 10ms tick, plenty for input. It still
  ships LAST — polling works meanwhile, and the block/wake plumbing is the
  only genuinely new machinery in the migration — but that's ordering, not
  viability.

### Atomic frames (snapshot-on-publish) — a design entry, not a maybe

Smooth balls bounce off walls tear-free; that is a requirement. The
mechanism is the canvas/content split above — double buffering with the
kernel holding the front buffer:

- The app draws into its canvas at leisure; nothing on screen changes.
- `gui_window_publish(damage)` snapshots the damage rect canvas → content
  under kGuiLock. The compositor only ever composites content, so a frame on
  screen is always a frame the app finished.
- Cost: one damage-bounded RAM copy per publish (~µs for typical damage; the
  full-screen worst case ~300µs is the same order as the composite blit that
  follows it anyway).

Why copy instead of swapping buffer pointers (true page-flip): the app keeps
ONE stable canvas pointer forever (no "which buffer am I on?" protocol in
the ABI), the compositor needs zero changes, and after a flip the app's new
back buffer would hold stale pixels it must repaint in full anyway — the
copy does that reconciliation for free. If profiling ever shows publish
copies dominating (at these sizes they won't), page-flip is the
optimization, hidden behind the same syscall.

During bring-up it is acceptable to land migration step 3 with publish as a
pure damage-forward (no copy; the compositor composites the canvas directly
via its HHDM alias) — that gives transient shear inside the damage rect,
never corruption — but the committed design, and where step 3 must end, is
snapshot-on-publish.

### libos64gfx — the userland library

The kernel keeps ONLY compositing, input routing, and window management.
Everything else is a ring-3 library over the raw surface + the 7 syscalls
(lives in `userland/` per the userland roadmap; no kernel changes needed to
iterate on it): glyph/text rendering (kernel fonts stay kernel-side; ship a
font with the library), rect/line/fill helpers (surface.c's clipped
primitives are the reference implementations — port, don't reinvent),
event-loop sugar, and eventually widgets. The `gui/demo/` apps use only the
client API by design: **rebuilding them as ring-3 ELFs, pixel-identical, is
the migration acceptance test.**

### Migration order (each step lands green before the next)

1. Ownership first: owner field + owner checks + `gui_task_destroy_windows`
   hook in every exit path. (Do this BEFORE any task can hold a surface.)
2. The six dispatch rows (16-21) + `GUI_ERR_NOT_OWNER`; rename
   present → publish in code while touching it.
3. The surface pivot (canvas allocate/map + snapshot-on-publish above).
4. Port `/gbounce` + `/gkeys` to ring-3 ELFs on libos64gfx; acceptance =
   identical on-screen behavior + the non-GUI regression greps still pass.
5. `gui_event_wait` (22) — last only because everything else works without
   it, not because anything still blocks it.

## Testing / debugging

- `DEBUG_GUI` (CONFIG.h bit 21; also a cmdline token) gates all GUI printd's:
  compositor heartbeats (1/s: frames + flushes), input events, wm operations.
- QEMU monitor (telnet :55555) drives everything headless: `sendkey`,
  `mouse_move dx dy`, `mouse_button 1/0`, `screendump file.ppm`. Keep single
  moves under ±255 (PS/2 packet range); giant deltas overflow the 8042 during
  bursts (the 30ms resync recovers, but why make it).
- Regression rule: a default (non-GUI) boot must run tests and idle exactly as
  before — `grep -c guicomp qemu_com1.log` must be 0.

## Known limitations / future work (in rough priority order)

1. Event-driven compositor wake (see pacing scars above).
2. Userland migration — fully designed; see "The userland boundary" chapter
   above and implement in its migration order.
3. PAT write-combining for the framebuffer flush (per-core IA32_PAT MSR setup
   in `ap_initialization_handler` + PTE PAT-bit plumbing in paging.c; replaces
   the PCD/UC mapping at its paging.c call site). Delete this item's need by
   measuring first — dirty-rect flushes are already small.
4. Damage rect LIST instead of single union (confined to `gui_damage_add_locked`
   + the frame loop).
5. Window resize, close buttons, minimize; `GUI_WINDOW_NO_DECORATIONS` honor.
6. Mouse wheel + 5-button (IntelliMouse magic sample-rate handshake).
7. Alpha translucency (X byte in XRGB is reserved for it; `surface_blit_masked`
   already does shaped blits).
8. Runtime resolution switching (only `framebuffers[0]` used; fixed by
   limine.conf `resolution:`).
9. The legacy text console (`BasicRenderer.c`) still has no SMP locking
   (pre-existing; the GUI path doesn't use it while active).
10. Scheduler follow-ups observed during bring-up: SIGSLEEP wake latency
    (was 100-500ms) is fixed to ~one 10ms tick by the 100-passes/sec cadence
    bump (scar #1) — remaining: verify it holds under load. A CPU-bound
    thread on an AP still starves siblings between scheduler passes
    (pre-existing, now user-visible via the GUI).

## Failure modes seen during bring-up (for the next debugger)

- **Recursive #PF → triple fault at early boot:** a page fault before CLS/GS
  setup made the fault handler chase a junk `cls->task`. Now guarded in
  `handle_page_fault` (panics with context instead). If you see
  "Page fault with no task context", something faulted before `kernel_init`
  line ~149 — find THAT fault.
- **Compositor heartbeats stop, system alive:** thread wedged. Check (a) it
  isn't spinning on a stale TSC target (use ticks!), (b) it isn't pinned to an
  AP under BSPSCHED, (c) qISleep wake delivery.
- **Cursor frozen but ball animating:** input events not reaching the queue —
  check IRQ12 routing (`IOAPIC: IRQ12 mapped...` in serial) and 8042 packet
  sync (`mouse: resync` spam = desync storm).
- **(Predicted, userland era) HHDM "use-after-free or wild pointer?" panic
  with the GUI publish or composite path in the stack:** a window outlived
  its canvas pages — teardown-order invariant violated, or a task exit path
  is missing the `gui_task_destroy_windows` hook. Windows die before pages,
  always.
