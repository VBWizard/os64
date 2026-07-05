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
- **Scar #1 (SIGSLEEP pacing):** wakes ride the BSP's `processSignals` pass —
  only ~5-10/sec, worst-case a few hundred ms of input latency. This is also
  why SIGSLEEP-paced *client* animation (the bounce demo) runs ~5-10fps: the
  cap is signal-processing cadence, not the compositor.
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
  on the BSP. The GUI boot entry deliberately omits `BSPSCHED`.

## Client API = the userland boundary

Every app-facing call in `gui/gui_client.h` is already syscall-shaped: handle
based, ≤6 register args, `int64_t` result, negative errors. Reserved numbers
16-21 live in `syscall_numbers.h`; each function's future `user_ptr_mask` is
commented at its definition in `gui_client.c`. Demo apps (`gui/demo/`) use
ONLY this API — they are the migration acceptance tests.

### Userland migration recipe

1. Add six `SYSCALL_DEFINE(...)` rows to `syscall_table[]` (syscall.c),
   `needs_cr3_switch=false`, masks as annotated; copy string/struct args with
   `copy_user_buffer`/`copy_user_string` like `syscall_write` does.
2. **`gui_window_get_surface` is the one real change:** instead of returning
   the kernel pixel pointer, allocate the window's pixel pages via
   `allocate_memory_aligned`, map them into the task's PML4 at a task VA
   (`paging_map_pages(task->pml4v, ..., PAGE_USER)`), and return that VA.
   The compositor keeps using the HHDM alias of the same physical pages —
   genuine zero-copy shared memory.
3. `gui_event_poll` stays a poll; add a blocking `gui_event_wait` only after
   an IRQ-safe wake primitive exists.
4. Ship glyph/text drawing as a userland library over the pixel surface —
   the kernel keeps only compositing, input, and window management.

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
2. Userland migration (recipe above).
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
10. Scheduler follow-ups observed during bring-up: SIGSLEEP wake latency is
    100-500ms under load (caps idle GUI responsiveness), and a CPU-bound
    thread on an AP starves siblings between scheduler passes. Both
    pre-existing, now user-visible via the GUI.

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
