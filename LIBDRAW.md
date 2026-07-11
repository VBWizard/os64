# libdraw / libui — the os64 userland graphics library (design)

*The design record for the userland graphics stack: how a ring-3 program
draws into its window without touching a pixel — and eventually without
touching a primitive. Companion to GRAPHICS.md (which designs the KERNEL
side: compositor, windows, the 7-syscall boundary) and LIBOS64.md (the C
library this links beside). Design settled 2026-07-10. Naming — `libdraw`
for the drawing core, `libui` for the toolkit — deliberately echoes Plan 9's
`libdraw`: minimalist lineage, zero Win32 baggage.*

## What this is

The library that turns os64's raw window surface into something a person
wants to program against. GRAPHICS.md hands an app a shared pixel canvas,
seven syscalls, and `publish(damage)`; by itself that means hand-blitting
every line and glyph forever. libdraw/libui exists so **no app author ever
sets a pixel by hand, and eventually never draws a primitive by hand
either.**

The happy accident that makes this cheap: the kernel's `gui/surface.c` is
already a complete, clipping 2D software rasterizer, and `gui/gui_types.h`
was written dependency-free ON PURPOSE ("so any layer — and eventually
userland — can include this"). So libdraw's core is largely a **port of
tested kernel code** into ring 3, not a new rasterizer.

## Three layers (each one raises the floor)

| Layer | Name | Job | Kills |
|---|---|---|---|
| L0 | (libos64) | window lifecycle + shared canvas + events — thin wrappers over syscalls 16-22 | — |
| L1 | **libdraw** | immediate-mode drawing on the canvas: primitives, text, clipping, a draw context, a real-time frame loop | pixel-pushing |
| L2 | **libui** | retained widgets on top of L1: widget/container/dispatch model | primitive-pushing |

L1 is the immediate relief; L2 is the "I described a UI instead of drawing
one" endgame. **L1 is designed in full here; L2 is designed as a *model*,
not an enumerated toolkit** — widgets grow app-driven (see below).

## L0 — the boundary it stands on (recap, owned by GRAPHICS.md)

Syscalls 16-22: `window_create`, `window_destroy`, `window_get_surface`
(returns the task-mapped shared canvas), `window_publish(damage)`,
`event_poll`, `screen_info`, `event_wait`. Drawing happens in the app's
address space on that canvas — **zero syscalls per draw**; only `publish`
crosses to the kernel, and the compositor snapshots the damage rect for a
tear-free frame (GRAPHICS.md "Atomic frames"). libdraw never talks to the
compositor except through `publish`.

## L1 — libdraw (the drawing core)

**A near-direct port of `surface.c`.** Everything except
`surface_flush_rect` comes across (apps never flush hardware — they
publish). Shared types (`rect_t`, `surface_t`, XRGB8888, the color
constants, `rect_union`/`intersect`/`contains`) come from the dependency-
free types header, dual-homed into `abi/include/` so kernel and userland
share ONE definition.

The ported primitives, all clipping to the surface (a caller may pass rects
hanging off any edge):
`fill_rect`, `blit`, `blit_masked` (shaped art — the cursor uses it),
`draw_hline`/`draw_vline`, `draw_rect` (outline), `draw_text` (glyph run).

**The draw context (`draw_ctx`)** — decided per #4: a light handle holding
the target surface, a clip rect, current fg/bg, and a text pen position. It
buys ergonomics (`draw_text(ctx, "hi")` flows without repeating args and
advances the pen) WITHOUT hiding the explicit primitives — the raw
surface.c-style calls stay available underneath for when you want them. The
context is convenience, never a mandate.

**Text — one embedded font, by design (#3).** libdraw ships its OWN PSF1
glyph data (8×`charsize`, opaque fg-on-bg, ported from `surface_draw_text`)
and renders text entirely in userland — the kernel stays out of app text.
os64's aesthetic is "one good font, possibly forever" (an intentional lack
of flash) — so proportional fonts / multiple faces are a documented future
slot, present-tense scope is one bitmap font. `draw_text` does pen-advance
only; wrapping and flow live in helpers / L2.

**Publish helper.** `draw_publish(ctx, dirty_rect)` wraps `window_publish`,
translating the app's content-local damage the way GRAPHICS.md specifies.

### The frame loop — cadence-agnostic, in real time (the FPS lesson)

Animation timing is expressed in **milliseconds, never frame counts**:

```c
while (running) {
    advance_state(dt_ms);
    draw_scene(ctx);
    draw_publish(ctx, dirty);
    frame_wait(&clock, 16);   // sleep the remainder of a ~16ms budget
}
```

`frame_wait` sleeps the leftover of the target budget against
`kTicksSinceStart`-based real time, so an app takes **whatever cadence the
scheduler currently delivers and gets smoother for free when the scheduler
gets faster** — no recompile. Today the practical ceiling for a sleep-paced
app is ~33 fps: the system clock is 100 Hz, but SIGSLEEP wakes are checked
at scheduler-pass cadence, which `SMP_MAGIC_NUMBER=3` divides to ~33 Hz
(SCHEDULER.md limitation #4 / DEBTS). Fixing that one constant lifts the
ceiling to ~100 fps with zero app changes — which is exactly why the frame
loop must not bake in a rate. (~33 fps is already "smooth as can be" on
bare metal, so this is headroom, not a blocker.)

### Crisp frames come from double buffering — not from frame rate

Frame rate and crispness are separate problems, and libdraw depends on the
second being solved by the kernel. **Judder** (how far the ball jumps per
frame) is a frame-rate thing; more Hz helps it. **Tearing/flicker** (the
compositor catching the canvas mid-draw — half-erased, ball half-drawn) is a
per-frame-integrity thing, and it does NOT improve with frame rate: a torn
ball is torn at 33 fps and at 1000 fps.

The fix is **double buffering, which is already the committed design as
GRAPHICS.md's snapshot-on-publish**: the kernel's content surface is the
FRONT buffer, the app's task-mapped canvas is the BACK buffer, and
`publish()` copies the damage rect back→front under the compositor lock, so
the compositor only ever composites a *finished* frame. libdraw gets crisp,
tear-free animation for free — the app's job is simply to draw a COMPLETE
frame into its canvas before each `publish` (clear → draw everything →
publish), never a partial one. No app-side third buffer is needed; the
canvas/content pair IS the double buffer.

Cost is negligible: one damage-bounded RAM copy per publish (µs for a small
sprite; the full-screen worst case ~300µs is the same order as the composite
blit that follows). Memory is the 2× surface already budgeted in the
surface pivot. So it's a pure win — crisp frames at trivial cost.

**Note:** a `gbounce` that still tears is running the bring-up
damage-forward path (compositor composites the live canvas, GRAPHICS.md's
allowed interim), NOT a libdraw bug. Crispness arrives when
snapshot-on-publish is actually implemented — it's designed, not
necessarily coded yet.

## L2 — libui (the toolkit MODEL, grown app-driven)

The endgame: instantiate a button, don't draw one. Designed here as a
*contract*, not a catalog — because widgets should come into existence the
same way syscalls and libos64 calls do: **when a real app needs one.** The
first GUI app that wants a textbox is what brings the textbox into being.

The model:
- **A widget** = `{ bounds (rect), draw(ctx) callback, event(ev) handler,
  optional child list }`. It draws itself into a libdraw context clipped to
  its bounds; it consumes or passes events.
- **A container** = a widget whose `draw` composites its children and whose
  layout assigns their bounds (start with the trivial layouts: fixed,
  vertical/horizontal stack).
- **Dispatch** = one `ui_dispatch(root, event)` that hit-tests pointer
  events to the deepest child and routes key events to the focused widget —
  reusing the same hit-test logic pattern the compositor already uses for
  windows.
- **The app loop** at L2: `event_wait` → `ui_dispatch` → if anything marked
  itself dirty, `ui_draw(root, ctx)` the dirty region → `draw_publish`.

Starter widgets when the first apps demand them: `label`, `button`,
`panel`, `textbox`. Not built speculatively.

## The canonical app loop (L0+L1, what gbounce/gkeys use)

```c
win  = window_create("bounce", x, y, w, h, 0);   // L0
surf = window_get_surface(win);                  // L0 — shared canvas
ctx  = draw_ctx_init(surf);                       // L1
frame_clock_init(&clock);
while (event_drain(win, &ev) || animating) {      // L0 poll
    handle(ev);
    draw_fill(ctx, GUI_COLOR_DESKTOP);            // L1 — pure userland writes
    draw_ball(ctx);
    draw_publish(ctx, whole_or_dirty);            // L0 crossing
    frame_wait(&clock, 16);                        // L1 — cadence-agnostic
}
```

## First consumers = the migration acceptance test (#5)

Porting `/gbounce` and `/gkeys` from kernel-threads to **ring-3 ELFs on
libdraw** is GRAPHICS.md's stated acceptance test, and it's libdraw's first
two apps: `gbounce` pulls `fill`/`blit` + the frame loop; `gkeys` pulls
`draw_text` + events. Acceptance = pixel-identical on-screen behavior to the
kernel-thread originals, plus the non-GUI regression greps still clean.
App-driven from the very first line.

## Conventions

- Coordinates: content-local, origin top-left (matches the events the
  compositor delivers).
- Color: XRGB8888, the `GUI_COLOR_*` constants from the shared types header.
- Failure is never fatal: a window that can't be created returns an error
  to the app (surface.c's rule — "never panic over a window").

## Kept / new

- **Kept (ported from surface.c):** every clipping primitive + PSF1 text —
  tested kernel code, moved to ring 3.
- **New:** the `draw_ctx` ergonomics, the embedded font, the cadence-
  agnostic `frame_wait`, and the entire L2 widget model.

## Failure fingerprints (symptom → cause)

- **Nothing appears though drawing calls "ran":** no `publish` (or an empty
  damage rect) — drawing into the canvas is invisible until published.
- **Tearing / half-drawn frames:** NOT a libdraw bug — it means
  snapshot-on-publish isn't active (either still the bring-up damage-forward
  interim, or the kernel snapshot regressed). See "Crisp frames" above. The
  app-side check: confirm you draw a COMPLETE frame before each publish, not
  a partial one.
- **Animation janky at exactly ~33 fps:** not a libdraw bug — the
  `SMP_MAGIC_NUMBER` wake-cadence ceiling. Confirm `frame_wait` is
  real-time-based (not spinning) and move on; the fix is the scheduler
  constant.
- **Text draws garbage/blank:** the embedded font blob didn't link in, or a
  byte past the glyph table was indexed (non-ASCII with a 256-glyph font).
- **Draw corrupts neighbouring windows:** a primitive wrote outside the
  canvas bounds — a clip was bypassed. Every primitive must clip to the
  surface; that's the one invariant ports must preserve.

## Known gaps / future work

- L2 widget catalog (grows app-driven; only the model is fixed here).
- Proportional / multiple fonts (present scope is one embedded bitmap font).
- Alpha/translucency (GRAPHICS.md future item; `blit_masked` already does
  shaped, not blended).
- The `SMP_MAGIC_NUMBER` wake-cadence fix that lifts the frame ceiling
  (SCHEDULER.md #4 / DEBTS) — not a libdraw change, but what unlocks its
  headroom.
- Everything rides the userland roadmap: libos64 scaffolding → the GUI
  syscalls (16-22) land in the dispatch table → gbounce/gkeys port.
