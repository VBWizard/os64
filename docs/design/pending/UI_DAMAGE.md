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

**What the built list showed (QEMU, 2026-10-19), and it corrects the
above.** The whole views on danlegt's middle were NOT parts far apart
uniting. They are the page's starfield: `body` carries `stars1.gif` (6
frames, 90 ms each) and `html` has a background colour of its own, so
`html` owns the canvas and `body` is an ordinary box over the whole page.
Every frame marks body's part of the view, which IS the view, so every
frame is a whole view, correctly: the starfield changes behind
everything. The old label lumped that in with parts far apart because it
asked only whether the parts' box spanned the view. With the list in
place the record counts the dirty list (rectangles, the most in a paint,
the full lists made one: 7 at most, one collapse in three minutes) and
says a part as big as the view by name, so it now reads "1294 a moving
part as big as the view" where it read "parts that reached the whole
view".

So the list does not remove danlegt's whole views; nothing that keeps
the starfield moving can. What it removes is the cost of everything else:
on the same page, scroll position and three minutes (TCG, so counts and
pixels, not times):

| | paints | whole | Mpx of the view | Mpx a paint |
|---|---|---|---|---|
| userland (one rectangle) | 2080 | 1129 | 757.8 | 0.36 |
| the list | 2362 | 1274 | 571.0 | 0.24 |

The paints between starfield frames dropped from about 0.30 Mpx to about
0.03 (the view is 0.42 Mpx), a quarter fewer pixels in all while painting
13% more often. The starfield's own cost is the next question, and a
different item: a moving background behind the whole view repaints
everything in front of it at the background's frame rate.

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

`os64_ui_render` walks the tree ONCE, parent before children, and paints
each widget that meets any rectangle of the list. How depends on a new
class flag, `paints_part`:

- **A `paints_part` class** paints only what `ui->dirty` covers of it, so
  it is painted once for each rectangle it meets, with `ui->dirty` set to
  that rectangle. The panel is one (it fills `bounds ∩ dirty` and draws
  only the border edges that meet it), and so is yonder's view, which
  already cut its painting to `ui->dirty`.
- **Any other class** paints its whole bounds, as it always did, so it is
  painted once, with `ui->dirty` the box around the rectangles it meets.

Why the flag, and not every widget painted once per rectangle (this note's
first plan): a widget that paints its whole bounds would then be painted
whole once for every rectangle it meets, which is SLOWER than today for any
big widget that does not cut, and every class would have to be surveyed
and taught to cut before the slice could ship. With the flag nothing is
slower than today: a class that cuts gets the list, and one that does not
is painted exactly as before. Teaching another big class to cut (the
textview, the dataview) is then a one-class change, measured on its own.

The one walk keeps the toolkit's paint order: a widget is painted for all
of its rectangles before any of its children is, so a parent painted for
the second rectangle never overpaints a child painted for the first.

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
appearance) and hands back the bounding box. A new `os64_ui_render_list`
hands back the rectangles; `os64_ui_paint` publishes each of them, and
yonder's paint does the same.

### 4. What yonder changes

- `glass_paint` publishes the list and counts it: the view's pixels are the
  sum of what each rectangle covers of it, and a paint is a whole view when
  one rectangle covers the view. Parts far apart are then no longer a whole
  view, so the `whole views:` line's "parts that reached the whole view"
  shrinks to what it says.
- `ticks_before_paint` ticks every ticking animation when the whole view is
  being painted anyway. Its test becomes "one rectangle covers the view"
  (`os64_ui_dirty_covers`), which is what it meant: a list whose bounding
  box spans the view does not repaint the view.
- The view's class says `paints_part`.
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
| any | `any_dirty` cleared by hand (tests do it, an app may) | empty: `any_dirty` is the authority, and the next mark starts a new list |

Growing an entry is not followed by a pass that merges it with the others
(the compositor's choice too): a mark that bridges two entries can leave
them overlapping, which paints their overlap twice. Waste, not wrongness;
if the counts ever show it, the fix is a sweep after an entry grows.

## Proof

- **Host, pure:** the list's rules against a brute-force oracle: random
  marks, then for every pixel, whether the list covers it (it must cover
  every pixel marked), how many entries cover it (the waste), and that the
  list never exceeds MAX. Mutants for each rule.
- **Host, libui** (`tools/test_ui_damage_host.sh`, 13 mutants in
  `tools/test_ui_damage_mutants.py`): a window with marks far apart paints
  only what they mark (a canary on every pixel outside them survives); a
  root mark leaves one rectangle; a full list collapses, counted; a
  `paints_part` class is painted once a rectangle and any other once with
  the box, parents before children; the panel fills and outlines only the
  rectangle it is given; a hand clear of `any_dirty` clears the list.
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
