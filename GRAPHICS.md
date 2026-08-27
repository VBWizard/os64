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
| — | VT8 seat | `gui_owns_glass()` (compositor.c) — the GUI is VT8's seated shell, and that predicate gates every flush. See the VT8 chapter below |
| — | ~~`gui/console_window.{h,c}`~~ | RETIRED 2026-08-19 with `kConsoleSink`; boot output lives in VT1's grid, and the shell-in-a-window is ring-3 `gterm` |

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
4. **IRQ handlers only enqueue.** Never call `scheduler_wake_task_waiter` from
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
   MOUSE, and the GUI does NOT own the glass → vt_select.c (the text VT)
   clicks → hit-test top-down → raise+focus; titlebar+left = drag grab;
            a click landing in a client's CONTENT starts the pointer grab
   moves  → cursor damage; WM drag → wm_move; pointer grab → its owner;
            else hit-test and deliver content-local to the window under it
   keys   → focused window
```

### The input fork (2026-08-21)

Mouse events belong to whoever holds the glass. `input_inject_mouse` used to
DROP them whenever a text VT was focused ("there is no consumer" — true until
this date); it now enqueues unconditionally, and the fork happens at the far
end of the ring, in `route_event_locked`, which is the only place that knows
who holds the glass at drain time. With VT8 up they route to windows; with a
text terminal up they route to **vt_select.c**, where they become gpm's old
gesture — select-to-copy, right-click-to-paste, and a pointer that is one
inverted character cell.

Keys are NOT forked here and never were: the keyboard driver routes those at
its own end, tty by tty. Only the pointer had nowhere to go.

Two consequences worth knowing:
- **No new thread.** The compositor already drains the ring every frame
  whether or not it owns the glass (it composites into the backbuffer always,
  and flushes only when the glass is its). The console overlay is painted in
  the same frame pass, in the branch where the flush would have gone.
- **Painting happens with kGuiLock RELEASED.** `vtsel_paint()` takes the tty
  lock and then the renderer lock; reaching those while holding the
  compositor's would invent a lock order nothing else in the system has. So
  events move state under the lock and the world is touched outside it.

### The implicit pointer grab (2026-08-21)

**While a button is down on a client's content area, that client owns the
pointer**: every move and the release go to it, wherever the cursor has got
to — including coordinates outside its own window, which is exactly what an
app tracking a drag past its edge needs to see. `s_pointer_window` in
compositor.c; released when the last button comes up, and by
`gui_grab_release()` if the window dies mid-gesture.

The WM's own gestures always had this (`s_drag_window`, `s_band_window`, and
the comment above them says why). Clients did not, and hit-testing every
event is a different promise: a drag-select that left the window lost its
tail, and the BUTTON_UP that ends a gesture went to whatever happened to be
underneath — a phantom release for a stranger, none at all for the app
mid-drag. scribe wore that as a quirk for a day (Chris hit it and filed it
under "quirk"); gterm's select-is-copy would have worn it as "the copy
sometimes doesn't happen", which is worse for being silent.

X11 named this in 1987 — the **implicit passive grab**. Every window system
has one, because every window system without one grows this same bug.

Note the two delivery rules that fall out of it: a hit-tested event must land
inside the content area (chrome belongs to the window system), while a
GRABBED event is delivered unconditionally, in the window's coordinates,
negative values and all. The grab starts ONLY on a press that reached the
client — a press on chrome would otherwise hand it a release it never asked
for.

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
  ~one tick. (Second healing, 2026-07-11: removing the SMP_MAGIC_NUMBER
  arming multiplier — SCHEDULER.md's autopsy — took the effective pass rate
  from ~33 to a true ~100/sec, and the ball from ~33fps to ~100fps.) The
  lesson stands: SIGSLEEP-paced clients are capped by signal cadence, not
  by the compositor.
- **Scar #2 (tick spin):** the interim "spin to the next tick while input is
  hot" workaround ate one full core — a full HOST core under VBox/QEMU —
  whenever the mouse moved. And its first version spun on `rdtsc()`: per-core
  TSCs under QEMU/WSL2 are desynchronized enough that a cycle target computed
  before a preemption could resume near-eternal. Never compare TSC values
  across a possible preemption.
- **Scar #3 — the acute half HEALED (2026-08-13, the backstop arc):**
  tickless mode (the DEFAULT since 2026-08-05; formerly the `BSPSCHED` flag)
  masks AP scheduler timers, which for a while made a pinned thread
  unpreemptable and nudge-only — the GUI boot entries carried an explicit
  `SCHED=periodic` while that was true. The preemption LEASE ended it: every
  non-idle AP dispatch arms a one-shot deadline, and the GUI entries dropped
  the workaround and run the default scheduler with `BACKSTOP=10` (verified
  2026-08-13 — GUI on tickless, no workaround, butter). STILL OPEN, the
  chronic half: `gui_compositor_affinity()` still declines the core-1 pin
  under `kTicklessScheduler`, and damage publication still has no wake IPI —
  the compositor's hlt-wait wakes on input IRQs and the backstop cadence, not
  on a publish. Both live in the DEBTS tickless-GUI row.

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
  64MB of canvas + 64MB of content — the `w/h ≤ wm_dim_max()` clamp (4096,
  or the screen if it is larger) in
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

**THE RULE FOR DECIDING WHERE A NEW FACT GOES (ruled 2026-08-25, on the
SIGWINCH question — Chris: "there will be many, so we should probably settle
on a pattern"): A SIGNAL TELLS A PROCESS SOMETHING; AN EVENT TELLS A WINDOW
SOMETHING.**

Both end with the app's own function running, so the pattern is not obvious
from the outside. The difference is WHEN, and on top of WHAT:

- An **event handler runs synchronously** — the app drains its queue at a
  moment of its own choosing, in its normal context. No reentrancy rules, no
  signal-safety question, and the fact can carry a PAYLOAD (which key, which
  button, the new rect) in order.
- A **signal handler runs asynchronously** — the kernel interrupts the thread
  wherever it is, pushes a frame, and the handler runs on top of that moment.
  That is why "which libos64 functions may be called from a handler" is a real
  open question (SIGNALS.md books it).

So: keys, mouse, resize, close, and every future window fact — focus, theme
change, publish-ack, a polite close-REQUEST — are EVENTS. Every windowing
system converged here independently (X11's queue, the Mac's `GetNextEvent`,
Win32's message pump, all mid-80s); nobody has ever delivered mouse-moves by
signal. **os64 could not even if it wanted to**: the pending set is
deliberately a bitmask, so two resizes coalesce into one with no dimensions
attached — a payload-free coalescing channel is disqualified as a GUI
transport by its own design (SIGNALS.md § "Explicitly not in scope").

### Covered windows (2026-08-27)

The first window fact after resize and close: **can anyone see this window?**
`GUI_WINDOW_COVERED` is set when a window is minimized, or its whole frame is
behind one window above it (`wm_rect_is_occluded`, the same single-window
containment publish uses), or a text terminal holds the glass.
`wm_cover_sweep_locked` recomputes it every compositor frame, after the
window manager has moved whatever it moved, and a flip queues one
`WINDOW_COVERED` / `WINDOW_UNCOVERED` event.

**The flag is the truth; the event is the nudge.** A queue is 64 deep and
drops the newest when full, so a client that believed the last event it saw
could sleep forever on a lost UNCOVERED. Every consumer re-reads the flag
through `gui_window_get_state` before acting — the event only says "look".

Who acts on it:
- **The frame clock** (`os64_frame_clock_bind`): an app that paces with
  `os64_frame_wait` naps while its window is covered and resumes when it is
  not, returning a dt of one budget rather than the minutes it was hidden.
  Five animating apps got quiet with one line each. Events are not consumed
  while napping; they wait for the app's next poll.
- **gterm** handles the events itself, because its loop must keep running
  whether or not anyone is looking (a hung-up session still closes the
  window; a paste still feeds the shell): the header poll continues, the
  cell snapshot and the paint stop, and the first uncovered frame repaints.
- **libui apps** block in `gui_event_wait` already and were quiet before.

What this is not: the publish-acknowledgment (Wayland's frame callback)
the occlusion row in DEBTS sketched, which makes idle the default for apps
with their own loops instead of asking them to look at a flag. The flag
covers every consumer that exists; the callback stays the more general
design if one outgrows it. Also still single-window containment — two
windows that jointly cover a third do not count.

Signals get process-lifecycle and stream-world facts: INT, TERM, HUP, PIPE,
SEGV — and WINCH, for the program whose whole world is a byte stream.
**SIGWINCH is not an exception to this rule, it is the rule applied to a
program that cannot see the GUI**: the process inside a pty has no window and
no queue, which is exactly why Sun invented the signal for 4.3BSD. Hence the
terminal-resize slice is TWO hops with two mechanisms — WM → gterm is an
event (already delivered today), gterm → the program inside is SIGWINCH — and
the DEBTS row that once called the second hop a client-notification-seam
customer was corrected the day this rule was written.

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

## VT8 — the GUI gets a terminal of its own (Phase B design — RATIFIED)

Status: design chapter written 2026-08-19; the four open questions RULED by
Chris the same night (see RULED below) — implementation authorized in the
migration order at the bottom. Chris's founding ruling (2026-08-17): the GUI
stays a cmdline option, and the compositor binds **VT8** — the X-on-tty7
lineage, done one better. When X11 wanted the glass it climbed onto a virtual
terminal like everybody else: `startx` seated the server on tty7, the text
consoles kept their lives on tty1–6, and Ctrl+Alt+F1 was always there when
the shiny thing wedged. os64 gives the GUI a VT of its OWN instead of a
diversion bolted under someone else's — which is an honest description of
what we have today.

### Today's arrangement is bigamy (the autopsy)

Two subsystems both believe they own the glass, and three pieces of tape
keep them from noticing each other:

1. **The tty layer thinks it owns the glass.** Grids are truth, the glass is
   a projection of `kTTYFocused` (tty.h's founding doctrine), `tty_focus`
   repaints-from-state. All still true in GUI mode — nobody told it.
2. **The compositor also owns the glass** — it paints the desktop over
   whatever was there and flushes damage forever after.
3. **`kConsoleSink` is the tape**: a diversion at the BOTTOM of the renderer
   (print_n's last stop) that reroutes the tty layer's glass paints into the
   GUI console window. The tty layer keeps projecting; the projection lands
   in a 96×30 window instead of on the iron. Nobody designed the consequence:
   the console window is a VT VIEWER by accident — switch "focused" VTs while
   in GUI mode and the window obediently shows the new grid, because the
   whole projection pipeline is being caught in its net.
4. **Input is delivered TWICE.** `keyboard_deliver_event` pushes key-downs
   into the tty ring (husk reads them) AND both edges into the GUI input
   queue (windows read them). Typing at a focused GUI window also types into
   the shell: "wake upkill -9 57" (2026-08-17, live, during Step 5 testing —
   the kill command half-eaten by gkeys). The comedy is the design flaw
   wearing a funny hat.

The panic paths, to their credit, already do the right thing and must keep
doing it: `gui_emergency_disable` (one store, `kConsoleSink = NULL`) and
`tty_emergency_direct` (`kTTYDirect = true` + lock-busting) both mean "all
diversions off, print straight at the iron, no locks between a panic and its
reader."

### The design: glass ownership IS VT focus

One owner variable, and it already exists: `kTTYFocused`.

- **VT1–7 stay text** — grids, scrollback, knock-summon, per-tty fgTask,
  everything tty.h built. Untouched.
- **VT8 is the GUI's seat.** `kTTY[7]` goes `TTY_LIVE` when `gui_start()`
  runs (the compositor is its "shell", seated exactly like husk is seated on
  a text VT). Its grid stays allocated but unused — dark glass behind the
  desktop. A boot with the `GUI` flag focuses VT8 after `gui_start`; without
  the flag VT8 is dormant like any unseated VT.
- **The compositor flushes ONLY while VT8 is focused AND seated.** One
  predicate — `gui_owns_glass()` ≡ (`kTTYFocused == &kTTY[7]` && the
  compositor is VT8's seated shell) — gates the flush loop. The second
  clause is ruling 2's: without the GUI flag, VT8 is an ordinary text
  terminal and the predicate is simply false forever.
  Compositing into the RAM backbuffer continues regardless (it is cheap, it
  is RAM, and it keeps window state current); only the UC flush is fenced.
  Switching back is therefore one full-screen damage add: instant, correct,
  repaint-from-state — the same move `tty_focus` makes for a text grid,
  because the backbuffer IS the GUI's grid.
- **Switching**: `tty_focus(7)` hands the glass to the compositor (publish
  owner, add full-screen damage, wake the compositor); `tty_focus(0..6)`
  from VT8 repaints the target grid over the desktop. Policy stays in tty.c
  where the chords already land; the compositor exposes two calls
  (`gui_glass_gained()` / nothing on loss — the flush gate handles it).
- **`kConsoleSink` RETIRES.** Boot spew and printf already land in VT1's
  grid; with ownership honest, the renderer's glass paints simply don't
  happen while VT8 is focused (the tty layer's own focused-check does this —
  it never paints an unfocused grid). The three sink call sites in
  BasicRenderer.c and the hook in console_window.c are deleted. Kernel log
  lives on VT1, the GUI lives on VT8, Alt+Left away when you want dmesg —
  precisely the Linux console / X division of labor.
- **The console window RETIRES with the sink** (ruled 2026-08-19 — no
  ring-0-rendered windows, period). The original draft proposed keeping it
  as an honest VT1 viewer; the ruling chose the cleaner cut: the GUI screen
  hosts no shell until Phase E's terminal, and husk is one Alt+F1 away —
  the startx-era workflow, accepted by name. Phase E's terminal keeps the
  viewer's intended shape anyway (grid in, keys out, source swapped to a
  pty slave), just in ring 3 where it always belonged.
- **Input routing follows the glass — dual delivery dies.**
  `keyboard_deliver_event` forks on ownership: VT8 focused → GUI input queue
  only; text VT focused → tty path only. The chords are consumed BEFORE the
  fork (Alt+arrows must work from either world; they are commands to the
  terminal stack, same doctrine as Ctrl+Alt+Del). The mouse already goes
  only to the GUI queue; on text VTs its events drop (gpm is a non-goal).
- **Emergency paths unchanged in spirit**: both hatches stay one-store.
  `gui_emergency_disable`'s store becomes the ownership override
  (`kTTYDirect = true` already bypasses every projection, GUI included, once
  the sink is gone — the two hatches may collapse into one; audit during
  migration step 4).

### Migration order — COLLAPSED IN THE BUILDING (2026-08-19, same night)

Steps 1–3 were planned as separate landings and turned out to be ONE atom:
the sink diverts at the BOTTOM of the renderer, so the moment focus honestly
moves to VT8 the console window's feed dies (it was VT1-focused paints), and
a switch to VT1 with the sink alive would paint the text VT into a window on
the desktop being left. Discovered before the first line was written; built
and tested as one coherent change. A fourth sink site turned up in tty_write
itself — one that caught bytes BEFORE the grid, meaning GUI-mode output never
reached VT1's history at all; retiring it is what makes Alt+F1 show the whole
story instead of a grid with a hole where the GUI session was.

**Acceptance, all run headless in QEMU the same night (screendumps in the
session record):**
- GUI boot: desktop up, console window gone, demos live.
- Alt+F1: full-screen VT1 with the COMPLETE boot story (both test suites
  24/24 on this kernel) and a live husk prompt.
- Alt+F8: desktop restored in one repaint — with the ball moved, proving the
  backbuffer kept compositing while the text VT held the iron.
- The comedy inverted: "ok" typed on VT1 is on husk's line; "stray" typed on
  the desktop is NOT.
- Alt+F2 on dormant VT2: knock-summons a fresh husk.
- Non-GUI boot: 24/24, and Alt+F8 lands on "os64 virtual terminal 8 — press
  any key to summon a shell" → husk. Ruling 2, verified.

**Still owed (step 4's audit):** TESTPANIC from GUI mode (the emergency path
changed shape: seated-flag store instead of sink-NULL — same one-store
doctrine, not yet exercised); mouse click-to-focus re-check on real input
(the gate passes events while VT8 owns the glass — structural, not yet
clicked); P5 and VBox passes; GRAPHICS.md layer map gains VT8.

### RULED — Chris, 2026-08-19

- **Chord set: what exists stands.** Alt+Left/Right cycles, Alt+F1..F8
  direct-selects (both already in the tree — the F-row chord landed after
  this worktree's fork point). The tests fire on ALT regardless of Ctrl, so
  the Ctrl+Alt muscle-memory spellings work as a superset. Under VT8 both
  chords are consumed BEFORE the input-routing fork, so they work
  identically from the GUI and from any text VT — which means GUI apps can
  never claim Alt+arrows or Alt+F1..F8 for themselves. If that ever pinches,
  the GUI-side fork can demand Ctrl+Alt explicitly (X11's convention, and a
  natural theme-table knob); not built until someone pinches.
- **VT8 without the GUI flag is just a terminal.** No reservation, no
  special case: dormant like any unseated VT, first keystroke summons a
  husk, eight text terminals total. The consequence for the design: the
  glass-owner predicate is NOT merely "focus == VT8" — it is "focus == VT8
  AND the compositor is seated there." `gui_start()` seats it (state
  `TTY_LIVE`, the compositor as its shell), which is also, for free, what
  stops the knock-summon from hanging a husk on the GUI's terminal.
- **The console window RETIRES — no VT1 viewer.** Ruled with the general
  principle: no ring-0-rendered windows at all (the demo windows already
  died in Phase A; the console window is the last one). Boot messages stay
  on VT1 — automatic, the grid is truth — and husk is one Alt+F1 away,
  which is exactly the startx-era workflow. NAMED CONSEQUENCE, accepted:
  between migration step 3 and Phase E's terminal, the GUI screen itself
  hosts no shell. (Related future feature, Chris's, parked for its own
  conversation: boot-time `printf` output is GLASS-ONLY — the tick-0 log
  catches every `printd` but not printf's lines — and he wants some of that
  moved or echoed into the log so it can't scroll out of existence.)
- **Mouse on text VTs: dropped, booked.** Events go nowhere for now.
  Wishlist, its own feature later: mouse selection copy/paste on text VTs —
  gpm's lineage (1994, Alessandro Rubini) — which will want those events
  routed after all. The fork's text-VT arm is where they'd land.

## Window resize — the capacity reservation (built 2026-08-19)

Chris picked the shape from a board of five: **Ctrl+Alt+right-drag resizes,
Ctrl+Alt+left-drag moves, anywhere in the window, with a rubber band.** The
alternatives (a Mac-1984 corner grip, a Windows-3.x eight-zone border, a
NeXT-style bottom resize bar) all wanted chrome; this one wants none, which is
why the entire slice touches `composite_one` not at all.

### Why the pointer never moves — and why that is the whole design

The dangerous version of resize is the obvious one: reallocate the canvas,
remap it, tell the app to re-fetch. That means the compositor thread mutating
ANOTHER TASK's page tables and unmapping a live user VA, which needs a
cross-core TLB shootdown — and until every core's TLB drops the entry, the
owner can write through a stale one into pages the allocator has already given
away. Silent corruption, not a fault; the lazy-HHDM tripwire would never see
it, because the pages are legitimately mapped to somebody else.

So the canvas is **reserved at capacity and never re-pointed**:

- Capacity is the screen (`wm_canvas_capacity_for` — or the window, if it was
  created larger; the client API allows up to 4096 a side, or the screen if
  that is larger). ONE function, used
  by both allocators, because a canvas smaller than the content snapshotted
  into it is a buffer overrun with a view of the desktop.
- `pitch_px` is the capacity's width, **for the window's whole life**. A pixel
  lives at `y * pitch_px + x` forever, so a resize relocates nothing: the image
  the app already drew is still exactly where it left it.
- Both stores get it — the task-backed canvas AND the kernel-side content — so
  `wm_resize` is two size fields, a fill of whatever growing exposed, a damage
  union, and an event. It **allocates nothing and cannot fail.**
- The price is standing memory, **measured** at 1024x768: a windowed task
  costs 7,616,144 bytes against a windowless one's 1,306,808, so a window is
  6,309,336 — two capacity surfaces (2 × 3,145,728) plus ~17KB of page tables
  and bookkeeping. The same window under content-sized allocation carried
  ~474KB of surfaces. Note that only HALF is visible from `/proc`: the
  task-side canvas lands in the heap range (it is what bumps `heapEnd` first,
  so a fresh GUI app's heap starts at exactly `0x70000000 + capacity + one
  guard page`), while the kernel-side `content` surface is `kmalloc`'d and
  appears nowhere a process can see. Lazy commit is possible and still
  shootdown-free, since the mapped region is always a prefix of the
  reservation — booked in DEBTS with the reason it wasn't taken.

**The ABI consequence, stated once:** `pitch_px != width` on a canvas. Every
libdraw primitive already addressed rows through pitch (`surface_row`), so
nothing in userland changed; the stale claim was a comment in `os64/gui.h`
promising they were equal, and that comment is now the explanation of why they
are not.

### The gesture

- The chord is BOTH modifiers, so a plain Ctrl-click or Alt-click still reaches
  the app. Ctrl+Alt rather than plain Alt because Alt belongs to the terminal
  stack (Alt+arrows, Alt+F1..F8 switch VTs and are consumed before any of
  this) — the VT8 chapter's ruling named Ctrl+Alt as the escape hatch, and
  this is the day it was needed.
- The quadrant the press lands in picks the edges that follow the pointer
  (mwm's rule): grab the left half and the left edge moves, so the opposite
  corner stays pinned.
- **The band is twm's 1987 wireframe, and not for nostalgia:** the app receives
  exactly ONE resize event per gesture instead of one per mouse packet, so a
  program that repaints slowly cannot be dragged into a repaint storm. Opaque
  (live) resize is the natural upgrade — same commit, called from the MOVE arm
  instead of the UP arm. The band damages four thin strips per frame rather
  than the rectangle it encloses, which is what took `DAMAGE_MAX_RECTS` from 8
  to 16 (eight band edges plus a cursor overflowed the list into the
  full-screen union fallback the damage-list work exists to avoid).
- `wm_clamp_frame` is shared by the preview and the commit, so the outline can
  never promise a size the window then refuses.

### What the slice found on the way

- **A use-after-free older than resize.** The drag state held a raw `window_t*`
  and nothing cleared it when a window died, so a task exiting while the user
  held its titlebar left the next MOUSE_MOVE calling `wm_move` on freed memory.
  Never hit by accident because nothing had ever died mid-gesture. `wm_destroy`
  now calls `gui_grab_release` first.
- **Modifier state was published in the wrong place.** The first version
  sampled the modifier bitmask in `keyboard_deliver_event`, which misses every
  modifier change that produces no delivered event — the extended path updates
  the state and returns without delivering for any key that is not an arrow or
  a named editing key, so holding Right Alt would change the driver's mind and
  tell nobody. State is published where it CHANGES now
  (`keyboard_publish_modifiers`), with the choke still covering the xHCI path.
- Mouse events carry `modifiers` (input.h, ABI-compatible — the union had 20
  bytes and was using 14), because a compositor keeping its own shadow copy of
  modifier state would drift out of sync across a VT switch.

## The window-management chords (built 2026-08-23 — Chris's QoL list)

Six things a desktop is expected to do, all built the same afternoon, all
as CHORDS on the focused window and none as chrome — the resize chapter's
ruling (Ctrl+Alt is the window system's escape hatch; plain Ctrl+letter
still reaches the app) extended from the mouse to the keyboard:

| Chord | Does | Notes |
|---|---|---|
| **Alt+Tab** / Shift+Alt+Tab (or Up/Down) | cycle focus | MOST-RECENTLY-USED order, not z-order; quick press = toggle between the two most recent; hold Alt and keep tabbing to walk the whole ring; **passing through a window is not using it** — only the one you release Alt over becomes recent (Chris's nit, first hour on the P5). A vertical strip of titles shows the walk, and **nothing in the scene moves until you let go**. A minimized window is in the ring, drawn dim, and comes back only if you release ON it — see the switcher chapter for the rule that had to die first |
| **Ctrl+Alt+P** | pin on top (toggle) | two bands in the z-list, pinned above all; white square at the bar's right end; a client cannot ask for it at create (`gui_client.c` masks the flag — a self-pinning app is a pop-up ad) |
| **Ctrl+Alt+T** | hide/show titlebar | content stays put, the frame's top edge moves; 1px border stays; `OS64_GUI_WINDOW_NO_DECORATIONS` honored at create too |
| **Ctrl+Alt+M** / titlebar double-click | maximize (toggle) | restore frame remembered; a manual move or resize clears it; raises, so a focused-but-buried window still visibly answers |
| **Ctrl+Alt+N** | minimize | off the glass, not hit-tested, occludes nothing, alive; focus goes to the most recent visible window; Alt+Tab brings it back — release the hold ON its dim row |
| `/home/desktop.conf` → `/etc/desktop.conf` | background | `color = 0xRRGGBB`, optional `image = /path` — **PPM (P6) or BMP, by magic bytes**, centered, never scaled (a `screendump` can be the wallpaper). Read by **`/bin/desktop`** since 2026-08-25, which paints it into a bottom-band WINDOW; `gui/desktop.c` and its in-kernel PPM decoder are deleted. With no shell running, the compositor's test pattern is what shows — it is the floor, and it stayed in the kernel for exactly that reason |
| `/home/gclock.conf` → `/etc/gclock.conf` | clock window state | `Position = x,y`, `Titlebar = on|off`, `Pinned = true|false`; no file = `(280,10)`, titlebar on, unpinned |
| `gui.conf` (via the config search path) | what starts with the desktop | `start = /bin/gterm` (repeatable, in order). **If the file exists its `start` lines are the whole list, including none** — that is how you say "start nothing"; the built-in demo pair applies only when no `gui.conf` is found. Read once by **`/bin/desktop`** as it starts (2026-08-25) — a shell reads its own rc. The `hello = yes|no` key and the window it switched are retired, and with them `gui/startup.c`: the kernel reads no config file for the GUI at all now |

Three things learned building them, for the next chord:

- **Match keys by ASCII, never by scancode.** The scancode field is PS/2
  set-1 on one keyboard path and a HID usage on the other (Tab is 0x0F
  there, 0x2B here). Alt+Tab worked in QEMU's PS/2 and did nothing on the
  P5's USB keyboard until the test became `ascii == '\t'`. Same for the
  Ctrl+Alt+letter verbs: Ctrl strips the letter to its control code on both
  paths, so `0x10` is Ctrl+P everywhere. (The 2026-08-21 chord-publish
  lesson, re-learned in one day — it is now the rule.)
- **The HID path emits releases now.** Until today xHCI delivered key-DOWN
  only: no key-up, no event at all for a modifier-only report. The Alt+Tab
  hold therefore could not end on USB. `hid_process_keyboard_report`
  emits a release edge per usage that left the report and a press/release
  per modifier bit that flipped (scancode `0xE0 + bit`, ASCII 0, so the
  text path ignores them exactly as it ignores PS/2 modifier keys).
  And the hold's end is not "the Alt key's release event" at all — PS/2
  never delivers one for Right Alt — but the first key event of any kind
  whose modifiers lack Alt.
- **Recency is its own fact** (`window_t.focusSerial`, stamped by
  `focus_window()` at every focus assignment). The first Alt+Tab walked
  the z-list; pin-on-top broke it within the hour, because a pinned window
  holds the top of the stack while focus goes elsewhere. Z-order is where
  things are drawn; recency is what the user did last; they stopped being
  the same number the moment one window could refuse to be buried.
- **THREE BANDS SINCE 2026-08-25**, when the desktop became a ring-3
  client: pinned (2) above ordinary (1) above **desktop (0)**, which nothing
  can get beneath. `band_of()` in window.c returns the rank and both
  `link_on_top` and `at_band_top` derive from it, so a fourth band costs one
  line — the two-band code's own warning was that "three copies of find the
  band boundary is three chances to disagree". The desktop band buys the
  z-position, no chrome, an exemption from Alt+Tab and a refusal of the WM
  verbs that would make the window vanish or float (the whole contract is
  listed on the flag in os64/gui.h) — and keeps what matters most: the window
  is still focusable and still gets keys and clicks, because being the
  target of a click that lands on no application is the whole point (that
  click is where a root menu and the
  launcher come from). It IS skipped by the Alt+Tab walk — a desktop is not
  something you tab to — and pin/maximize/minimize/decorate/close decline it,
  guarded at the `wm_` setters so every caller is covered rather than just the
  chords. The two verbs with no setter of their own — chord move and chord
  resize — are declined at the gesture in the compositor, like Alt+F4 (Fable's
  review, 2026-08-25: without that, Ctrl+Alt+drag on the wallpaper moved the
  desktop, and the title toggle painted a titlebar across it).

The harness grew to test these: `utility/gui_run.sh` drives a headless
GUI boot from a key script, and speaks **QMP** (`-qmp unix:`) for raw
key-down/key-up and button events — HMP `sendkey` cannot hold Alt across
two Tabs or click twice inside 500ms. Launch with `-device qemu-xhci
-device usb-kbd` to test the HID dialect; QEMU routes input to USB when it
is present.

## The Alt+Tab switcher (built 2026-08-23)

*Chris, the first afternoon with minimize: "I minimized everything, and the
only way back to the gterm I was on was to Alt+Tab through a bunch of
things, which brought them all back up."* That is the one rule in the chords
chapter that was wrong, and it was Fable's call: "stepping onto a minimized
window restores it, and it stays restored if the walk moves on." With raise-
as-feedback there was no other way to show the user where they were. A
visible list is the other way, and it is what every desktop grew for the
same reason.

**THE LIST IS VERTICAL — one window per row.** Fable's design said a
horizontal strip of cells, on the Windows 3.1 (1992) precedent. Chris asked
the question that undid it — *"what's in each box? The name of the app?"* —
and the answer settles the layout: the box holds a TITLE, and every
horizontal switcher you have ever seen holds an ICON. Icons are square,
small, and sixteen fit across a screen; titles are wide, variable, and
getting wider (Chris, same conversation, taking the job: *"I agree with you
that each window should have a descriptive title"*). Sixteen windows
sharing 1024 pixels leaves 61 per cell = **five characters** — `gterm`,
`scrib`, `bounc` — and it degrades further the better the titles get. The
same sixteen stacked cost 384 pixels of height, fit inside 640x480, and
show all 32 characters.

The lineage backs the arithmetic. The vertical list of window titles is the
older and specifically Unix answer — twm's window menu (1987), then fvwm's
`WindowList` (1993) — reached by people who also had no icons to show; even
Microsoft's own pre-icon Task List (Alt+Esc, Windows 3.0) was a vertical
list box, and went horizontal only once there were icons to put in it.

### What changed

- **While Alt is held, NOTHING in the scene moves.** No raise, no restore,
  no focus change, no recency stamp. The z-order the user had is the z-order
  they keep until they let go. The old raise-per-step is gone, and with it
  the `focusSerial` save/restore that used to undo the stamp `wm_raise`
  takes — nothing needs undoing when nothing happens.
- **A strip appears** on the first Tab: centered, one ROW per window in the
  recency ring (`wm_recency_ids`, most recent first), the title in each row,
  the current step highlighted. Minimized windows are in the ring and drawn
  dimmed (gray text) so "bring back the one I hid" is a visible choice, not
  a guess. 16 rows max (`ALTTAB_RING_MAX`); the strip is sized to the count.
- **The snapshot now carries titles and dim flags**, not just ids. The draw
  path then dereferences no window at all, and the picture cannot reflow
  under the user's hand mid-walk. (Nothing in the scene is allowed to change
  during a hold anyway, so a live re-read could only differ by being wrong.)
- **Tab / Shift+Tab move the highlight, and so do Up/Down.** Nothing else.
  The vertical arrows are free precisely because the keyboard driver spends
  Alt+Left/Right on the virtual-terminal cycle: horizontal arrows walk
  terminals, vertical ones walk windows. Arrows do not START a hold — an
  Alt+Up that reached no switcher would be silently stolen from the app.
- **Releasing Alt commits**: the highlighted window is restored if
  minimized, otherwise raised — both paths focus and recency-stamp on their
  own. ONE window changes, the rest stay exactly where they were. The strip
  disappears. Escape during the hold cancels (strip goes, nothing changes);
  its release edge is swallowed too, so no app is handed half a gesture.
- **A VT switch away abandons a live hold.** It is the keyboard's version of
  the stale-pointer-grab bug the resize slice found: a hold ends on the first
  key event arriving without Alt, and while a text VT holds the iron every
  one of those goes to the console. Cleared where `s_glass_regained` is
  consumed, beside `s_pointer_window = NULL`, for exactly the same reason.
- **`wm_touch_focus` is deleted.** Its only purpose was putting back a
  recency stamp that raise-as-feedback kept taking; nothing raises during a
  hold now, so it had no caller and its header comment had become a lie.

### Where it lives

A scene layer in `composite_locked`: desktop → windows → band → **switcher**
→ cursor. It is NOT a window (no `window_t`, no owner, no event queue, not
in the z-list, not hit-testable) — the rubber band (`band_composite`) is the
precedent: compositor-owned pixels drawn straight into the backbuffer for a
rect the compositor damages itself. `switcher_composite(backbuffer, damage)`
draws it, `switcher_layout_locked()` sizes it once per hold, and
`switcher_damage_locked()` republishes the whole strip rect on every
highlight move — the band damages four thin edges because it is
window-sized and moves per mouse packet; the strip is small, moves once per
keystroke, and repaints its highlight in the middle of itself, so the union
is both simpler and cheaper than the bookkeeping that avoids it.
(It sits above the band rather than below it, being the more modal of the
two; in practice they cannot coexist, one needing a Ctrl+Alt mouse drag and
the other Alt held.)

Row geometry: 8x16 glyphs (`surface_draw_text`), rows at `SWITCHER_ROW_H`
(24) pitch, all rows one width — the longest title plus padding, clamped to
the screen — and titles clipped to the character count that width admits.
The clip is structural, not remembered: `surface_draw_text` paints an
OPAQUE background per glyph, so a title allowed to run past its row would
repaint the frame beside it. Strip = rows + border, centered.

The palette is the chrome's, and that is why the four `WINDOW_TITLEBAR_*` /
`WINDOW_BORDER_*` values moved from `window.c` to `window.h`: the
highlighted row must be the same blue a focused titlebar is, or the strip is
describing a different desktop than the one behind it. One copy, two
consumers.

**No separator lines.** Each row is a tile laid on the dark slab and one
pixel shorter than its pitch, so the frame shows through between them: the
gap IS the separator. It costs no colour, no extra draw and no decision
about how dark a divider should be, and it makes the highlighted row read as
a raised tile rather than a painted stripe — which still works when two
adjacent rows are the same grey. No translucency; there is none yet (known
limitation 7).

### What does not change

- The toggle: a quick Alt+Tab (one Tab, release) still lands on the second
  entry — the strip flashes for a frame and that is fine.
- Recency semantics: passing through is still not using (the strip makes
  that literal — nothing is touched until commit).
- The USB/PS/2 rules: Tab by ASCII, hold ends on the first key event
  without Alt.
- Ctrl+Alt+N to minimize; Alt+F4 to close.

### The trap this slice found: ESCAPE IS NOT `ascii == 0x1B`

The design above originally said "Escape by ASCII `0x1B`", which is the
natural thing to write and is **wrong on both keyboard paths**. Arrow keys —
and the Home/End/PgUp/Del family — never reach the GUI as keys at all: both
drivers translate them into VT100 escape sequences at the source, delivered
as THREE separate key events (`ESC`, `[`, a final byte), the 1979 vocabulary
kept so a future vim-over-serial reads them unchanged. The first of those
three carries `ascii == 0x1B`.

So "Escape cancels the hold" would have fired on the first third of every
arrow press — and since Up/Down had just been given to the switcher, Up
would have cancelled the hold it was meant to walk. Caught before it
shipped, by checking rather than trusting the design note.

What separates them is the SCANCODE: a burst carries the scancode of the key
that produced it, so only a real Escape carries Escape's own (PS/2 `0x01`,
HID usage `0x29`). Both facts now live in one dialect-aware helper each —
`keyboard_is_escape_key` and `keyboard_arrow_updown` in `keyboard.h`,
alongside `keyboard_fkey_number`, which exists for the same reason. The
arrow helper is where the dialects genuinely COLLIDE: `0x50` is Down in PS/2
set-1 and Left in HID, and nothing but `KEYBOARD_MOD_HID` can tell them
apart. **This is the 2026-08-21 chord-publish lesson in its third costume:
any key without a unique ASCII needs a helper that knows both dialects, and
any key WITH one still needs a scancode check if some other key's escape
sequence can forge it.**

### Verification (run 2026-08-23, `utility/gui_run.sh` + QMP)

QMP is the rig — HMP `sendkey` cannot hold Alt across two Tabs. Boot the
GUI entry, then:

*(As run in 2026-08-23 the entry gave three windows — `hello os64`, `keys`,
`bounce`. Since 2026-08-25 the desktop is `/bin/desktop` and what starts is
whatever `gui.conf` lists, so put the windows you want in that file first —
and note the DESKTOP window is skipped by the Alt+Tab walk, so it never
appears in the strip.)*

| Script | Confirmed by screendump |
|---|---|
| `down:alt down:tab up:tab`, shot BEFORE `up:alt` | strip up, highlight on entry 2, **z-order and focus unchanged from boot** |
| a second `down:tab up:tab` | highlight moved; the window walked PAST was not raised |
| `up:alt` | that one window raised + focused, strip erased cleanly, others untouched |
| `ctrl-alt-n`, then Alt+Tab onto the dim row | row drawn gray; **the window stays off the glass while highlighted** — the old rule, dead |
| release there | it comes back: restored, raised, focused |
| `down:esc up:esc` mid-hold | strip gone, focus and z-order exactly as before |
| `down:down` / `down:up` mid-hold | highlight steps once per press (the log shows one `alt-tab step` per arrow, not three) |

Zero panics across the runs; `grep alt-tab` on the serial log reads
step/step/ended and step/cancelled as designed.

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
5. ~~Window resize~~ **BUILT 2026-08-19 (see the resize chapter above)**;
   ~~minimize, `GUI_WINDOW_NO_DECORATIONS` honor~~ **BUILT 2026-08-23 (the
   chords chapter above)**. Still open: close buttons (and any titlebar
   button at all — the chords were chosen so none is needed yet), a
   launcher/taskbar (Alt+Tab is the only way back from minimize), and
   re-reading `desktop.conf` without a reboot — **which is a much smaller job
   now that the reader is `/bin/desktop`**: re-reading a config and
   repainting is an ordinary thing for a program to do, where it used to mean
   the compositor re-entering the VFS. Same for the launcher, which now has a
   window to live on.
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
  AP under tickless, (c) qISleep wake delivery.
- **Cursor frozen but ball animating:** input events not reaching the queue —
  check IRQ12 routing (`IOAPIC: IRQ12 mapped...` in serial) and 8042 packet
  sync (`mouse: resync` spam = desync storm).
- **(Predicted, userland era) HHDM "use-after-free or wild pointer?" panic
  with the GUI publish or composite path in the stack:** a window outlived
  its canvas pages — teardown-order invariant violated, or a task exit path
  is missing the `gui_task_destroy_windows` hook. Windows die before pages,
  always.
