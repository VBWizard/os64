# Why this worktree is still here

`opus/backdrop-layer` (2026-10-20/21, Opus) is a **shelved** slice: built,
tested, measured slower on the P5, and deliberately **not merged**. Do not
delete it without asking Chris; do not merge it without the work below.

## What it is

A moving background picture (danlegt's `stars1.gif` behind `body`) drawn
by its own pixels: the view painted three times with that layer black,
white and left out, and each GIF frame composed from those where it
changed, instead of repainting everything in front of the stars.
`docs/design/pending/BACKDROP_LAYER.md` is the design, with a **Measured**
section at the end that says why it stopped.

## Why it stopped

On the P5 (2560x1440), all six of Chris's readings got WORSE — top, middle
and bottom of danlegt at 100% and 150% (his numbers: `/home/yonder-timings`
on the P5, 2026-10-20). yonder's own counters put the bottom at 150% at
about 24 ms of user time a starfield frame. The design priced a paint per
PIXEL; a paint also costs a walk of the whole box tree, and the layer
traded one whole paint for several small ones a frame (the holes under
form widgets, the rebuilds of what changed in front), and re-scaled the
150% tile every frame. QEMU (TCG) counted pixels and could not see it.

And the before-numbers were already good: 150% bottom was 8-14%, near the
5% target, and animated backgrounds are an old-web thing — the census
found them on danlegt, warpbox, theoldnet and fauux, not on the pages the
Modern Web arc is for. Chris and Opus agreed the redesign is not worth it
now.

## What reviving it would take

In order, each measured on the P5 before the next:
1. Decide compose-or-paint by measured WHOLE-FRAME time (holes, rebuilds,
   the libui paint of the rest), retrying the other way now and then —
   not by a per-pixel model.
2. Repaint a hole only where the background shows under it (the three
   paintings differ there); skip pixels the background cannot reach.
3. Keep each scaled frame of the tile instead of re-scaling every frame.
4. Rebuild what is stale as one rectangle: one tree walk a painting.

## What was carried forward

The bugs this slice found in code that ships today were fixed on their own
branches (see the Measured section). Everything here is still green on its
own: `tools/test_yonder_backdrop_host.sh` (419,842 checks),
`tools/test_yonder_scripts_host.sh` (5,878), and
`tools/test_yonder_backdrop_mutants.py` (68 of 68 caught).
