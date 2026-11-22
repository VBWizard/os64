# N3: the paint follows the change

Opus, 2026-11-22. BRIEF_LAYOUT_NEEDS.md § N3: "A PAINT change damages the
element's painted bounds (old and new, with its transform and its ink:
shadows, outlines, filters), not the view." Approved by Fable the same day
with seven changes, folded in below (his numbering in brackets).

## 1. What the P5 says

After N2 (#700), the playing minute of the video watch page:

| | count | each |
|---|---|---|
| whole-view repaints, all from a script's change | 29 a minute, one every 2.5 s | 100-300 ms (after W1's cover) |
| frames lost per pause (the per-second rate line) | 4-7 typical, 15 worst | |

The pauses come at the whole repaints' rate, and they last as long as one
of them takes. The load has 73 more, about 200 ms each, 14.6 s of the 75
(Opus #2's timeline).

Every one has the same cause: `script_rebuild` ends with
`mark_whole(WHOLE_SCRIPT)`, whatever changed. A new tree replaces the old,
and nothing compares the two for the paint.

What the minute's scripts changed:
- the progress bars' inline `transform`;
- a canvas;
- the chapter bar's width;
- an animation's timing;
- `aria-valuenow`;
- the player's classes;
- the clock's text, once a second.

## 2. The rule

After a script's changes are made current, the view is marked with what
changed paints, before and after, and nothing else, when that can be
bounded. When it cannot, the whole view is marked and the record says why.
N3 changes WHAT is repainted, never WHEN the rebuild happens. The when is
BRIEF_RENDER_STEP.md, the slice after this one. It composes with N3: one
rebuild covering several tasks' changes still compares one tree with the
next.

**The direction of every rule below is the same.** Anything N3 does not
fully account for marks more, never less. Over-marking costs a little
paint; under-marking is a picture left stale on the screen.

## 3. The cases, by how the tree was made current

**A. N1 kept the tree** (`flow_restyle_in_place`, FLOW_RESTYLE_PAINT): the
same boxes, styles rewritten in place. `flow_restyle_in_place` gains a
`mark` callback in `flow_motion_tick`'s shape, sharing the tick's marking.
Each element whose style it writes is marked, before the write and after,
with what it paints:
- **Its ink:** shadows, outlines, the filter's reach, its transform.
- **Its subtree's painted reach, when the changed property applies to
  everything inside it** [4]: opacity, transform, filter, clip and
  clip-path, a mask, an overflow clip. Not only its own box.
- **The boxes outside it that take from it:** the enclosing layered group,
  whose sheet is redrawn whole, and a backdrop filter that reads where it
  painted.
- **The canvas:** where it may take the element's background, refused as
  CANVAS, the whole view.

**B. A layout made a new tree** (the graft or a fresh build): the old tree
is compared with the new one before `page_show` frees it, as the animation
path already does, by a function of its own, `flow_change_parts`, beside
`flow_layout_parts`. It shares `boxes_differ`'s walk and the marking; the
animation path's contract stays as it is.

1. **Boxes, with contained moves** [6]. The two trees are walked together.
   - Where a box is alike in its own rect, its layer and its scroll, but
     what is inside it differs (another list of children, or a child laid
     out otherwise), that box's painted reach is marked, old joined with
     new, and the walk descends no further there. The damage is the
     smallest unchanged box around what moved: the clock's digits changing
     width mark the time display, not the view.
   - Refused, as MOVED, when that reach is not bounded by the box
     (something positioned escapes it, a layer differs) or passes half the
     view.
   - LAYERS and SCROLLED refuse as today.
   - This is paint only. Not laying a moved box out at all stays N4.
2. **Styles: every field, effective values** [2]. Every element both trees
   styled is compared by its whole computed style, not a list of the
   fields the paint reads. With boxes alike, a field a layout reads can
   still change pixels (`text-transform`, a face swapped for one with the
   same metrics). The old tree's values are its ticked ones and the new
   tree's are sampled now, so an animated element that moved comes out as
   differing. One that differs is marked as in case A.
3. **What is styled but is not an element** [3]:
   - `::before`, `::after` and their generated text are compared as their
     element is, text included;
   - `::marker` is compared with its text, so a counter renumbered by an
     inserted sibling marks the marker;
   - `::placeholder` is compared with its control;
   - anonymous boxes' inherited copies come from a parent compared itself;
   - the decoration copies a fragment holds come from the element
     compared, and a fragment that differs is a contained move (1).
4. **Attributes: marked unless known to reach the paint only through
   style** [1]. An attribute change marks its element's box (inside an
   inline SVG, the svg root's) unless the attribute is one of: `class`,
   `id`, `style`, `aria-*`, `data-*`, `role`, `title`, `tabindex`. Those
   reach the paint only through the style, which item 2 compares. The
   exemption is void while any sheet uses `attr()` (garb says so), except
   where item 3's compare already covers the generated text.
5. **Text** (a TEXT entry in the change log since the old tree): marks its
   text's boxes, before and after; a text whose boxes moved is a contained
   move (1).
6. **What paints from neither style nor the log** [5]. The model's drawn
   state is compared, old against new, for each control:
   - a `value` or `checked` set as a property;
   - a `<select>`'s choice;
   - focus and the focus ring;
   - a selection and the caret;
   - the hovered and pressed link `script_rebuild` resets to none (a lit
     one's box is marked).

   What cannot be compared refuses, by name. None of these is silent.
7. **Gaps:** an element no box stands for marks nothing, as today. A
   change the log cannot name (the log lost) refuses.

**C. Neither** (the old tree gone, another width or zoom, a refusal): the
whole view, by name.

The marked parts go to the dirty list like any other, so the cover (V.7.1)
still splits a part that meets the playing picture.

## 4. The proof

**`--damage-check[=N]`** [7]: every Nth rebuild N3 bounded is checked, and
that includes a rebuild whose marks came out EMPTY, which is where a wrong
exemption would hide. After it, the whole view is painted offscreen the
ordinary way and compared with the window, pixel for pixel. A difference is
a mark N3 missed, recorded with its rectangle and the element that
changed.
- **What moves by itself is held still for the compare:** both paints
  read one timeline sample, and a playing video's picture is the same
  frame in both (its pane is excluded where it cannot be), as
  `--paint-check` already does.
- **Host proof:**
  - fixtures for each case and each kind of change, the refusals included;
  - a random walk of scripts' changes (styles, classes, attributes, inline
    SVG, text, generated content, counters, controls' properties,
    canvases, children), with the check at every rebuild;
  - mutants for each rule above, among them "an unlisted attribute marks
    nothing" and "an opacity change marks only the element's own box".

## 5. The record

- `whole views` already names each whole repaint's cause.
- New, per reading: `script changes painted in parts`, with how many
  paints, their area, how many came out empty, and the refusals by reason
  (`kRefused`'s names, plus the log lost and the controls' state). A
  refused contained move counts as MOVED, so the P5 shows what N4 would be
  worth.

## 6. Not in N3

- When the rebuild runs: BRIEF_RENDER_STEP.md, the next slice.
- What a layout costs: Opus #2's L1 and L2, and N4 (a box that only moves).
- Whole repaints that are not a script's change (a picture arriving, the
  window).

## 7. What it should take off the playing minute

Of the 29, most should become parts:
- the record's `layout needed` line says 18 of that minute's unforced
  layouts moved nothing, which is case B's ground;
- the clock's once-a-second text is a contained move;
- the paint-only changes are case A's.

The rest stay whole until N4. So most of the regular pauses should drop
from 100-300 ms to the size of what changed. That is an estimate until the
P5 runs it with `--damage-check=1` for a playing minute.

## 8. Settled at the design pass

1. Case B compares every styled element, whole styles, rather than
   trusting the cascade's dirty set.
2. Case B is its own function, `flow_change_parts`, sharing
   `boxes_differ`'s walk and the marking with `flow_layout_parts`.
3. The record room: N2's summary lines went into a room of their own as
   this branch's first commit (`ROOM_SUMMARY`).
