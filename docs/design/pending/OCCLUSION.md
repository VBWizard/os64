# OCCLUSION.md — each screen pixel painted once (design)

*2026-10-18. Performance item 2 after the paint scratch (YONDER_PERF.md
§ perf-5). Opus writes, Fable reviews (kernel GUI); Fable's review
(OCCLUSION_REVIEW.md, untracked) is folded in below. Chris's question that
started it, asked of Fable the same day: "If yonder is fullscreen but mostly
covered, should the compositor still require as much CPU as when yonder is
topmost? The only time compositor usage goes down is when the window is
completely covered."*

## What a frame cost before

The compositor rebuilt each damaged rectangle bottom-up: the kernel's
background (skipped only when ONE window covered the whole rectangle,
`wm_rect_is_covered`), then every window touching it in z-order, each in
full (`wm_composite` → `composite_one`), then the band, the switcher and the
cursor. A window's publish damaged its whole changed rectangle unless one
window above covered ALL of it (`wm_rect_is_occluded` in `gui_window_publish`).

`/sys/gui` counts what the layers write (`compositor painted:`, the
pixels the background and windows draw, over the damaged pixels). QEMU,
yonder animating the top of danlegt, 20 s per scene:

| Scene | Damaged | Painted | Times each pixel |
|---|---|---|---|
| A: yonder on top | 90.2 M | 261 M | **2.89** |
| B: a gterm covering most of yonder | 89.3 M | 259 M | **2.89** |

Two wastes, and they need different cures:

1. **Overdraw.** The desktop shell is a fullscreen WINDOW at the bottom, so
   every damaged pixel was painted by the shell, then by any window between,
   then by the window you see. A: shell, the gterm behind yonder, yonder.
2. **Publish ignored partial cover.** B damaged exactly what A did: yonder's
   whole changed rectangle was recomposited, the gterm drawn over it again,
   and all of it flushed to the framebuffer, though almost none of it could
   change on the glass.

## The rule

**A screen pixel is written by the one layer that is visible there, and a
publish damages only what can reach the glass.**

### 1. Composite top-down, the visible parts only

For each damaged rectangle D:

- `pieces` starts as `{D}`: what is still unclaimed.
- Walk the windows from the top (skipping hidden ones). For each piece,
  `inter = piece ∩ w->frame`; if not empty, `composite_one` paints `w` with
  `inter` as its view (it already clips everything to the view it is given),
  and the piece is replaced by `piece − w->frame`. Rectangle minus rectangle
  is at most four rectangles (above, below, left, right of the hole).
- Whatever pieces are left are the background's: `surface_blit` from
  `kDesktop`, piece by piece.
- The band, the switcher and the cursor are drawn over D afterwards, as now.
  They are not windows and claim nothing.

`wm_rect_is_covered` and its special case go: the background is just the
last layer, and gets only what no window claimed.

**The geometry lives in its own file.** `kernel/src/gui/visible.c` has no
kernel headers (frames.c's pattern): given D and frames in top-down order,
it hands back pieces, each owned by the index of the frame that claims it
or unclaimed, and how many frames it consumed. The compositor paints each
claimed piece with its owner and the unclaimed ones with the background;
publish (below) calls the same function with the frames above a window and
keeps the unclaimed pieces, which are what is visible. One helper, two
callers.

**Bound.** Pieces are kept in fixed arrays (`VISIBLE_PIECES_MAX`, 64),
built ping-pong: a window's subtraction goes into a second array, and if
that would overflow, it is discarded and the walk stops before that window.
Every window above the stop has been subtracted from every surviving piece,
so none of them touches a survivor. The claimed pieces are final; the
survivors go to today's painter, background then `wm_composite` bottom-up,
which skips the windows above by its own intersect test. Nothing is painted
twice and nothing is lost: the fallback is "stop claiming, hand what is
left to the old painter". The same holds when there are more windows than
the frame array takes. A window is painted once per piece it owns, through
`composite_one` with the piece as its view; that runs the decoration's
layout per piece, which is cheap, and it must not be "optimised" into one
call with a union view, which would paint over pieces claimed above.

**Opacity is an invariant, and tested.** A window claims its whole frame
because `composite_one` writes every pixel of frame ∩ view. Today that holds
because three things agree: the decoration insets (borders on all four
sides, the title bar from the border to `insets.top`), content sized to the
frame minus those insets (`wm_create`, `wm_resize`), and `composite_one`
painting border, bar and content, on both the decorated and the legacy
path. Bottom-up, a gap showed the layer below, painted that same pass, and
nobody saw it; top-down it shows last frame's backbuffer, a wrong pixel
after a reveal. And the decoration view comes from ring 3 at runtime
(`wm_set_decoration`), so the invariant must hold for every view the kernel
accepts. So `composite_one`'s comment states it in the present tense, and
two tests paint over a sentinel colour and require that none survives
inside frame ∩ view. The kernel LATE test `test_window_opacity` (beside
`test_window_minimum.c`) paints every chrome configuration (titlebar,
settings, pinned, maximized, UTF-8 title, `NO_DECORATIONS`, desktop), whole
and in pieces, inside the screen and hanging off its edges, with whatever
decoration view is installed. A default boot installs none (the desktop
keeps the built-in chrome until a choice is saved), so the decorated paint
has its own case in `tools/test_decoration_host.sh`, against views prepared
with varied borders, padding, buttons and the settings filter: the
decoration's paint plus the content where the kernel places it must cover
the frame. Translucency, the day it exists, makes a window claim nothing;
the code says where (`claim_stack`).

### 2. Publish damages the visible part

`gui_window_publish` keeps its snapshot exactly as now (canvas → content,
before anything else), because a covered region revealed later is damaged
by whatever revealed it and must composite from an up-to-date copy. Then,
instead of asking whether ONE window covers all of the damage, it asks
`visible.c` for the unclaimed pieces of the damage under the frames above
it. Nothing visible: no damage, as today's fully-covered case already does.

**The damage list sets publish's limit, not the piece array.** The list
holds `DAMAGE_MAX_RECTS` (16), and a full list collapses into the union of
everything pending, far worse than any one window's box. So publish adds
at most `PUBLISH_PIECES_MAX` (4) pieces, one subtraction's worth, which is
the cover-in-the-middle, ring-visible case; past that it adds the bounding
box of the VISIBLE pieces. For a window covered on one side that box is
the visible sliver; only the ring flushes covered interior, and the
top-down walk still paints only the ring. If the walk did not consume every
window above, the unconsumed survivors count as visible (more damage,
never less). `PUBLISH_PIECES_MAX` is a knob in gui_client.c, and
`/sys/gui`'s collapse count says whether it is right.

## Why nothing goes stale

No state is added. The visible pieces are worked out per frame from the
z-list and frames the compositor already walks, under `kGuiLock`, and are
thrown away with the frame. There is no maintained region to keep in step
with moves and raises, which is the bookkeeping X11 spent years getting
right.

What publish-side clipping DOES newly rely on: **every change to what is
visible damages the area it changes.** Bottom-up whole-frame repaints have
been hiding any operation that did not, because the next publish of the
window below repainted all of it anyway. Audited 2026-10-18:

| Operation | Damages | Covers what it reveals |
|---|---|---|
| `wm_move` | old frame ∪ new frame | yes |
| `wm_raise` | the raised frame (and the old focus's, for its titlebar) | yes: everything newly on top is inside it |
| `wm_set_minimized` | the frame, both directions | yes |
| `wm_set_maximized` | through `wm_raise` + `wm_resize` | yes |
| `wm_set_pinned` | the frame | yes |
| `wm_resize` | old ∪ new | yes |
| `wm_create` | the new frame | yes |
| `wm_destroy` | the frame | yes |
| `wm_set_decorated` | old frame ∪ new | yes |
| a decoration view change (`wm_set_decoration`) | each reshaped frame's old ∪ new | yes |
| glass regained from a VT | the whole screen | yes |

A new operation that changes visibility without damaging would now leave
stale pixels behind a covering window's departure. The proof below includes
the reveal cases so the suite says so if one is ever added.

## What it does not do

- **Tell the client its visible region.** yonder still PAINTS its hidden
  parts every frame, and still copies the whole changed rectangle at
  publish; the compositor just stops compositing and flushing them. A
  visible-region contract between a client and the window system is a
  change to the ABI (Wayland's frame callback is the precedent) and waits
  for a consumer. The publish buffer swap (perf item 3) attacks the copy
  from the other side.
- **Translucency**, above.
- **Change `wm_cover_sweep_locked`.** "Wholly covered" still tells a client
  to stop; partial cover is the compositor's business only.

## Proof

- **The geometry, on the host, against a pixel oracle** (frames.c's
  pattern; `tools/test_visible_host.sh`). For random D and random frame
  stacks (empty, negative and off-screen coordinates, nested, identical,
  one pixel wide, and stacks deep enough to trip the cap), owner indices
  are painted into a grid two ways: by brute force, the first frame
  top-down containing each pixel, and through the pieces. They must agree
  at every pixel; the pieces must be pairwise disjoint, inside D, and sum
  to its area; and when the cap trips, every pixel is either owned by a
  consumed frame or in a survivor. Plus the named scenes (A, B, the ring,
  identical, nested, the cap). A family of small holes and bands across them
  drives the piece arrays to their limits, which random stacks almost never
  reach. `tools/test_visible_mutants.py` breaks each rule of the cut, the
  intersection, the walk and the cap (23 mutants, all caught, 16 s); it
  builds each mutant in a temporary directory, frames.c's harness's shape,
  so it never edits the tree and needs no runner.
- **The counter is a partition check, so it is exact.** The pieces handed
  out for D partition it, so on every rectangle the walk finished, `painted`
  equals `pixels` exactly, not "falls toward 1.00". The kernel checks each
  rectangle: if the pieces' areas do not sum to area(D), it counts a
  partition fault, and a rectangle the old painter had to finish counts as
  a stop; `/sys/gui` shows both. Overlap reads above, a gap below; either is
  a geometry bug the glass might not show for weeks.
- **Opacity**, by the kernel LATE test above.
- **The readings.** QEMU's scenes A and B: A's ratio from 2.89 to exactly
  1.00, B's damaged pixels down to the visible sliver. Measured 2026-10-18,
  20 s per scene:

  | Scene | Damaged | Painted | Times each pixel |
  |---|---|---|---|
  | A: yonder on top | 129.9 M | 129.9 M | **1.0000** |
  | B: a gterm covering most of yonder | 13.3 M | 13.3 M | **1.0000** |
  | A again, after B | 132.8 M | 132.8 M | **1.0000** |

  No stops and no partition faults all boot (408 M pixels). A's damage is
  higher than the earlier run's because yonder's own frame rate moves
  between runs; A measured again after B is what says B's fall is the
  clipping and not yonder producing less. A gterm dragged off most of
  yonder revealed it whole and current. On the P5, Chris's
  mostly-covered fullscreen yonder, where guicomp's CPU should fall with the
  covered fraction. A before/after screendump of a static reveal scene goes
  in the PR as an exhibit; correctness is decided by the oracle, because
  two kernels cannot be held frame for frame (the cursor, the clock and
  animation phase all drift).

## Size and order

Kernel GUI, S–M. The counter first (written: `painted` in
`gui_composite_count_t`, `compositor painted:` in `/sys/gui`), then
`visible.c` and its oracle, then the top-down walk and publish's clipping
on it, the partition tripwire, and the opacity test.

**Prose this makes stale, fixed in the same commit:** compositor.c's file
comment and the comment above `composite_locked` ("scene layers
bottom-up"); `wm_composite`'s contract in window.h; `wm_rect_is_occluded`'s
block comment, which called this slice its own and whose one remaining job
is `wm_cover_sweep_locked`; GRAPHICS.md's layer map and § Covered windows'
last sentence (single-window containment stays true of the sweep, no
longer of publish); and DEBTS' "Occlusion — half paid" row, whose known
limit this pays. `tools/stale_refs.sh` before the commit that deletes
`wm_rect_is_covered`. The sweep's own single-window test stays, booked in
DEBTS: the helper makes an exact "nobody can see any of you" cheap the day
a consumer wants it.
