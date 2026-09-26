# libdraw / libui — the os64 userland graphics library (design)

*The design record for the userland graphics stack: how a ring-3 program
draws into its window without touching a pixel — and eventually without
touching a primitive. Companion to GRAPHICS.md (which designs the KERNEL
side: compositor, windows, the GUI syscall boundary) and LIBOS64.md (the C
library this links beside). Design settled 2026-07-10. Naming — `libdraw`
for the drawing core, `libui` for the toolkit — deliberately echoes Plan 9's
`libdraw`: minimalist lineage, zero Win32 baggage.*

## Start here: the three exemplars (2026-08-19)

Writing a new GUI app? Copy a TESTED one — exemplars over templates, because
a template nobody runs rots while an exemplar is verified by every build.
One of each mode, all in `userland/apps/`:

| App | Mode | Copy it when... |
|---|---|---|
| **glogo** | immediate (libdraw) | you repaint most of the window anyway: animation, custom drawing. YOU are the painter — clear, draw everything, publish, every frame. Its numbered steps [1]-[9] are the whole liturgy, including the frame clock done right (dt-scaled state, boundary-anchored cadence). xlogo's heir, and exactly as ambitious. |
| **gclock** | retained (libui) | furniture that mostly sits still: labels, buttons, panels. Describe widgets once; then change state → mark dirty → `os64_ui_paint()`. Also the model for TIME-driven UIs (`os64_ui_run` blocks on events; a ticking app runs the paint loop itself). Chris's — the first user-authored g-app, and it found a window-manager bug before it could tell time. |
| **gterm** | hybrid (libdraw, one full-bleed surface) | the window IS one custom-drawn thing (a grid, a canvas, a plot). No widgets to retain, no frame-clear ceremony beyond your own — and a documented example of choosing libdraw over libui on purpose. |

(uiprobe exercises libui's whole contract — dispatch, grab, theme — and is
worth reading, but it is a FIXTURE: it proves the toolkit, where gclock
teaches the app author.)

For background work that needs to wake a window, see
[Window doorbell](GUI_DOORBELL.md#using-the-callback): callback setup,
worker notification, result handoff, repainting and shutdown order.

## What this is

The library that turns os64's raw window surface into something a person
wants to program against. GRAPHICS.md hands an app a shared pixel canvas,
window syscalls, and `publish(damage)`; by itself that means hand-blitting
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
| L0 | (libos64) | window lifecycle + shared canvas + events — thin wrappers over GUI syscalls | — |
| L1 | **libdraw** | immediate-mode drawing on the canvas: primitives, text, clipping, a draw context, a real-time frame loop | pixel-pushing |
| L2 | **libui** | retained widgets on top of L1: widget/container/dispatch model | primitive-pushing |

L1 is the immediate relief; L2 is the "I described a UI instead of drawing
one" endgame. **L1 is designed in full here; L2 is designed as a *model*,
not an enumerated toolkit** — widgets grow through applications and the
Appearance Workshop gallery (see APPEARANCE.md).

## L0 — the boundary it stands on (recap, owned by GRAPHICS.md)

Syscalls 16-22: `window_create`, `window_destroy`, `window_get_surface`
(returns the task-mapped shared canvas), `window_publish(damage)`,
`event_poll`, `screen_info`, `event_wait`. Drawing happens in the app's
address space on that canvas — **zero syscalls per draw**; only `publish`
crosses to the kernel, and the compositor snapshots the damage rect for a
tear-free frame (GRAPHICS.md "Atomic frames"). Window operations use the
public GUI wrappers; drawing primitives stay in userland.

The boundary also provides `window_get_state` (48),
`window_set_min_size` (57), and `event_ring` (58).

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

**Text — the current renderer.** libdraw ships embedded PSF1 glyph data
(8x16, opaque foreground/background cells) and renders app text in userland.
The embedded face is an implementation constraint, not a design restriction
against other fonts. Font selection, proportional faces, and scaling need
rendering and layout support; changing a theme's cell metrics does not add
that support. `draw_text` advances the pen; wrapping and flow live in helpers
and L2. The later font/display investigation is tracked in APPEARANCE.md.

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
gets faster** — no recompile. This already paid out once: when the
SMP_MAGIC_NUMBER multiplier was removed (2026-07-11, see SCHEDULER.md's
autopsy), sleep-paced animation jumped from a ~33 fps ceiling to ~100 fps
overnight, with zero app changes — the bounce demo visibly tripled its
speed. That event is the standing proof of why the frame loop must never
bake in a rate.

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

## L2 — libui (the toolkit model)

The endgame: instantiate a button, don't draw one. Designed here as a
*contract*, with a catalog developed through both applications and the
Appearance Workshop gallery. Reusable controls may be built before their
first production consumer. Their input behavior, visual states, and theme
support are developed together.

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

The toolkit includes panels, labels, buttons, checkboxes, integer sliders,
text fields, text views, scrollbars, swatch lists, and an HSV color picker. APPEARANCE.md describes their gallery
and the appearance editor's remaining slices.

`os64_ui_render` draws and consumes a UI's pending damage without publishing.
This lets separate UI contexts share a canvas and publish one composed frame.
`os64_ui_paint` retains the usual render-and-publish behavior. The Workshop
uses two contexts so experimenting with a preview does not recolor its editor.

`os64_ui_theme_palette` selects a color building block while preserving
metrics, including button treatment. `button.bevel` values 0 and 1 are flat;
values 2 through 4 include the outer border and add inner relief using
`button.highlight` and `button.shadow`. The painter caps the bevel at four
pixels and half the control's dimensions; negative values draw no relief.
Stock defaults remain flat and square. `control.radius` (0..8) rounds button,
checkbox, slider/scrollbar thumb, and text-field painting without changing
widget bounds. The Workshop's gently rounded choice is six pixels. The
rounded primitives clip to the surface and clamp radius to the smaller half
extent; rounded button captions stay inside the straight center strip.

`ui_theme.c` owns the property schema, supported ranges, parsing, and session
serialization. Startup `theme.conf` uses the shared configuration search and
parser. Invalid configuration leaves the compiled defaults in place.
Apply payloads contain the full color set, button relief, and corner radius. Startup preservation
payloads begin with `inherit = startup` and carry the configured live keys,
leaving absent keys at each application's defaults. Both forms exclude layout metrics;
decoding is validated before replacing the caller's theme.

`ui_session.c` overlays `/sys/appearance` at initialization and handles
`OS64_GUI_EVENT_APPEARANCE` through libui dispatch. Reads and validation are
cached per process; each UI context installs its own generation and repaints.
Colors, button relief, and corner radius merge without copying geometry or resetting widget
state. Set `follow_session = false` for an independent draft. Custom consumers
initialize supplied defaults through `os64_ui_theme_current`, and use
`os64_ui_theme_session` for live updates. The initializer keeps disk geometry
but resolves colors against the pinned override when startup selection has
changed the file. Disk-only startup APIs read the next-boot choice. Grootmenu
repaints its open cascade levels after adoption. Invalid payloads retain the last usable theme. `os64_ui_theme_apply`
merges selected components against the current session and reports publication
conflicts; it does not save persistent files. These APIs are not signal-safe.

Buttons and checkboxes activate on left-button release inside the control,
or on release of Space/Enter while focused; key repeat does not repeat the
activation. Sliders support dragging, arrow steps, and Home/End. Programmatic
checkbox/slider setters repaint without firing interaction callbacks.
Tab/Shift+Tab traverse enabled, visible controls in tree order. Editable text
views accept literal Tab and use Ctrl+Tab/Ctrl+Shift+Tab for traversal.

`os64_ui_set_enabled` and `os64_ui_set_hidden` reconcile focus and grabs,
including descendants. Resize cancels held gestures and hover while retaining
valid keyboard focus; layout that hides/disables/removes the target clears it.
`os64_ui_cancel_gestures` retains focus, while `os64_ui_cancel_interaction` also
clears focus for tree replacement or modal transitions. Window focus loss
cancels gestures but retains the
logical focus target for return. A class's optional `cancel` hook resets its
private gesture state. Hover uses the compositor's coalesced pointer snapshot;
ungrabbed motion does not invoke a widget's drag handler.

Interaction colors are `button.face.hover`, `hover.border`, `focus.ring`,
`disabled.bg`, and `disabled.fg`. Disabled descendants receive a muted theme
for painting and do not accept input. `os64_draw_text_clipped` bounds captions
to a supplied rectangle; empty outline rectangles paint nothing.
`checkbox.size` sets the square indicator size; `slider.track.h` sets track
thickness. Both clip to the control's bounds. The slider thumb uses `scroll.w`,
bounded to 4–40 pixels and the available control width.
Disabled slider tracks retain an outline in `disabled.fg` so their shape
remains visible against the disabled background.

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
- **Animation capped well below the scheduler rate:** not a libdraw bug —
  SIGSLEEP wake cadence is the ceiling for sleep-paced apps. Confirm
  `frame_wait` is real-time-based (not spinning), then check the scheduler's
  actual pass rate (SCHEDULER.md; its autopsy section is the case study).
- **Text draws garbage/blank:** the embedded font blob didn't link in, or a
  byte past the glyph table was indexed (non-ASCII with a 256-glyph font).
- **Draw corrupts neighbouring windows:** a primitive wrote outside the
  canvas bounds — a clip was bypassed. Every primitive must clip to the
  surface; that's the one invariant ports must preserve.

## Known gaps / future work

- L2 widget catalog expansion (applications and the Workshop gallery).
- Proportional / multiple fonts (present scope is one embedded bitmap font).
- Alpha/translucency (GRAPHICS.md future item; `blit_masked` already does
  shaped, not blended).
- ~~The `SMP_MAGIC_NUMBER` wake-cadence fix~~ — DONE 2026-07-11; the frame
  ceiling is now ~100 fps (SCHEDULER.md autopsy).
- Everything rides the userland roadmap: libos64 scaffolding → the GUI
  syscalls (16-22) land in the dispatch table → gbounce/gkeys port.

### Named appearance compositions

Appearance Workshop lists compiled presets and personal saved themes, loads a
selection into its preview, saves full schema snapshots, and chooses an independent
startup snapshot. Save is separate from live Apply. The reusable `os64_ui_listbox`
uses application-owned labels, supports keyboard navigation and pointer
selection, and can be paired with `os64_ui_scrollbar`. Saved themes use
`ui_saved.c`; full encoding and startup validation share the theme schema in
`ui_theme.c`. Publication and failure contracts are recorded in APPEARANCE.md.

### Color editing controls

`os64_ui_colorpicker` provides a saturation/value square and hue strip using
integer HSV conversion. Pointer gestures clamp to their starting area, and
focus loss/hiding/disablement cancels the drag. Arrow keys change saturation
and value; Shift+Left/Right changes hue. Programmatic color setters preserve
the exact opaque RGB input and do not notify callbacks. User gestures notify
when the resulting RGB value changes. The caller owns hex entry and history.

A listbox may supply a `swatch` callback to draw color chips before its labels;
the chip and label share the row's existing mouse and keyboard behavior.
Palette-role and schema color accessors live in `ui_palette.c` / `ui_theme.c`.
APPEARANCE.md records role inference, persistence compatibility, and Undo scope.
