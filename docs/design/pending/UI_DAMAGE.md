# UI_DAMAGE.md — libui repaints a short list of rectangles (design)

*2026-10-19. Opus writes. YONDER_PERF.md's "Then measure again" list named
this item twice; perf-6's reading made it the next one. Chris's two rulings
are at the end. Step 1, the compositor's rule moved to `shared/damage.c`
unchanged, ships first; the libui half (§1-§4) is built on it.*

## Why

libui (`libos64/ui.c`) keeps ONE dirty rectangle: `os64_ui_mark_dirty`
unions every mark into `ui->dirty`, `os64_ui_render` paints every widget
that meets it, and the window publishes it. Two marks far apart repaint
everything between them.

perf-6 (YONDER_PERF.md) took away the other cause of whole views. On the
P5 at 2560x1440 in the middle of danlegt, every layout for the animations
now marks only what it changed (597 of 597), and what is left is this one:

- 28% of paints are whole views, and 656 of the 658 were "parts that
  reached the whole view": the GIF strip, the Scratch 2000 panel and the QR
  code's text are far apart, and their marks unite into the whole view.
- The paints that are not whole average 1.1 Mpx, a third of the 3.3 Mpx
  view, for the same reason.
- The whole views are about 54% of the pixels painted, at 18.7 ms a paint
  (the view's painting is most of it), with the main loop 85% busy.

The compositor solved the same problem on its side in 2026-08
(`kernel/src/gui/compositor.c`, the damage list above `DAMAGE_MAX_RECTS`),
and its comment carries the lessons this design takes.

## What changes

### 1. The list

`os64_ui_t` keeps up to `OS64_UI_DAMAGE_MAX` rectangles instead of one,
with the compositor's two rules:

1. **Merge when merging is cheap.** A mark unions into the first entry
   whose union wastes at most `OS64_UI_DAMAGE_SLACK` pixels beyond the two
   areas: a mark inside another, or sharing a whole edge with it, merges;
   marks far apart stay apart. A partial (diagonal) overlap can cost more
   than the slack and stay apart, and its overlap is painted twice: waste,
   never a wrong pixel. (The compositor's comment claimed overlapping marks
   always merge until the shared rule's host test, 2026-10-19, showed they
   do not.)
2. **Overflow falls back to today.** A full list that nothing merges into
   becomes one rectangle, the union of all of them: never a wrong window,
   only a slower one. Counted, so a page that lives there is visible.

One addition the compositor lacks, because libui's marks include the
window itself: **a mark that contains an entry swallows it.** A root mark
(a resize, a theme change) then leaves one rectangle, not the window plus
every part inside it painted again.

`ui->dirty` stays, as the rectangle being painted (§2), and the list's
bounding box when no paint is running, so a reader that asks "is anything
dirty, and roughly where" is unchanged.

### 2. Painting

`os64_ui_render` paints the list one rectangle at a time: `ui->dirty` is set
to the rectangle, and `paint_recursive` runs as it does today, visiting the
widgets that meet it. A widget that cuts its painting to `ui->dirty` (yonder's
view does) needs no change.

**A widget that paints its whole bounds** is painted once for each
rectangle it meets. For a button that costs nothing; for the root panel it
is the whole window filled again for every rectangle. So `panel_paint`
fills `bounds ∩ ui->dirty` and draws only the border edges that meet it.
That is the "libui paints the part" half of the item, limited to what is
big: the panel, and any widget class the survey finds that fills a large
area. The survey (every `cls->paint` in libos64 and the apps) is part of
the slice, written down here before the code.

Overlapping rectangles that the slack let through paint the same bytes
twice, which is waste, never a wrong pixel: every widget repaints from
state, so painting a pixel again writes what it already holds.

### 3. Publishing

The window publishes each rectangle: one `os64_gui_window_publish` per
rectangle, the call every app already makes. The kernel cuts each to what
can be seen (`wm_visible_damage`) and merges them into its own list, so the
compositor's side is unchanged.

The cost of that choice: the compositor may run between two publishes of
one frame, and show the first rectangle's new pixels a frame before the
second's. Each rectangle is whole, so nothing tears inside one; two regions
far apart are a frame apart at most. If a page shows it, the answer is one
publish of the list (a new call, and kernel work with its own review), not
a different libui.

`os64_ui_render(ui, damage)` keeps its signature for its callers (gterm,
appearance, yonder) and hands back the bounding box. A new
`os64_ui_render_list` hands back the rectangles.

### 4. What yonder changes

- `glass_paint` publishes the list and counts it: the view's pixels are the
  sum of what each rectangle covers of it, and a paint is a whole view when
  one rectangle covers the view. Parts far apart are then no longer a whole
  view, so the `whole views:` line's "parts that reached the whole view"
  shrinks to what it says.
- `ticks_before_paint` ticks every ticking animation when the whole view is
  being painted anyway. Its test becomes "one rectangle covers the view",
  which is what it meant: a list whose bounding box spans the view does not
  repaint the view.
- `mark_part` and `mark_whole` are unchanged: they mark, and the list sorts
  out where.

### The list's lifecycle

| The list is | When | It becomes |
|---|---|---|
| empty | a mark | one rectangle |
| n rectangles | a mark that merges cheaply into entry i | entry i, grown |
| n rectangles | a mark that contains entries | those entries gone, the mark added (then rule 1) |
| n < MAX | a mark that merges into none | n + 1 |
| MAX | a mark that merges into none | one rectangle, their union; counted |
| any | a mark of nothing (empty, or no widget) | unchanged |
| any | render, painted | empty, and `ui->dirty` empty |
| any | render with no root | unchanged (today's behaviour: nothing is painted) |

Growing an entry is not followed by a pass that merges it with the others
(the compositor's choice too): a mark that bridges two entries can leave
them overlapping, which paints their overlap twice. Waste, not wrongness;
if the counts ever show it, the fix is a sweep after an entry grows.

## Proof

- **Host, pure:** the list's rules against a brute-force oracle: random
  marks, then for every pixel, whether the list covers it (it must cover
  every pixel marked), how many entries cover it (the waste), and that the
  list never exceeds MAX. Mutants for each rule.
- **Host, libui:** a window with marks far apart paints only what they
  mark (a canary pixel between them is not repainted); a root mark leaves
  one rectangle; the panel fills only the rectangle being painted.
- **yonder:** the scripts suite's counts through a real frame (two parts
  far apart are two rectangles and no whole view); the `whole views:` line.
- **QEMU and the P5:** the middle of danlegt at 2560x1440 again, against
  perf-6's reading: whole views, Mpx of the view a second, the paint's ms,
  the main loop's busy share.

## Rulings (Chris, 2026-10-19)

1. **One list rule, shared.** The compositor's merge rule moved to
   `shared/damage.c` (`os64_damage_add`, `os64/damage.h`), built into the
   kernel and libos64 alike, as copy_fill.c is. The kernel calls it from
   `gui_damage_add_locked`; libui will call the same copy. The move is its
   own first commit, with a host test that holds the shared rule to the
   kernel's old one as an oracle, and the slice earns a kernel review.
   ("Just asking the question means you know the answer.")
2. **N publishes, or one?** Deferred to me: N publishes now, and a list
   publish only if a page shows the frame between them.
