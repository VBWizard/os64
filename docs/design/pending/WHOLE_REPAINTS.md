# Whole repaints on the playing video page (design, before code)

Opus, 2026-11-22, after V.7.1 (#698, VIDEO.md § 5, The paint beneath the
picture) merged. Fable's pass of the same day is folded in (W1's shape,
the order, the primitives' two booked items).

## 1. What the P5 says

The P5 record of the playing minute after #698 (`--layout-profile`, 71 s,
maximized, a 30 fps stream):

| | count | cost |
|---|---|---|
| paints | 1,480 | render 12.6 s in all |
| covered (the video skipped beneath, V.7.1) | 1,452 | 8.5 ms a paint |
| whole-view repaints | 26, one every 2.7 s | up to 406 ms each, about 10 s in all |
| of which a script's change was the cause | 25 | |
| painted the ordinary way by #698's guards (budget or a sheet's size) | 27 | |
| frames shown / dropped | 22.8 a second / 6 a second | |

So 80% of the minute's paint time is the 26 whole repaints, and they are
the stutter Chris sees: each holds the main loop for about 400 ms, and
three times in the run several landed close together with forced layouts
(the 5-second rate lines arrived 6.4-6.8 s apart, with about 80 frames
dropped instead of 25). The minute's longest single script task was
2.5 s; nothing yet says what it was.

**Before Codex's guards (records 0113 and 0114) the same repaints were
covered too**, split about the picture, and the slowest paint of the
playing minutes was 100-160 ms. The guards made them correct in every case
Codex could build, and on this page they refuse every one.

## 2. What a whole repaint draws

The paint effects of the same minute, per whole repaint (27 of them):

| effect | size | each |
|---|---|---|
| a canvas drawn into its box, twice (two canvases, cross-faded) | 1780x1002 | 70 ms |
| `filter: blur(15px)` | 1780x1002 | 67 ms |
| `filter: blur(40px)` | 1735x871 | 66 ms |
| a warp (a group drawn through its matrix) | 1615x631 | 58 ms |

That is the page's ambient glow: two canvases holding a small copy of
the picture, in a container scaled up past the video's edges and blurred.
About 330 ms of the 400.

**The 40 px blur is there because yonder's canvas has no `filter`.** The
page's own script asks whether `CanvasRenderingContext2D.prototype` has
`filter`. Where it does, it blurs inside the small canvas as it draws
(`context.filter = "blur(5px)"`) and puts no CSS filter on the container.
Where it does not, it puts `filter: blur(40px)` on the scaled container,
which is a full-size layer at every paint that draws the glow. Other
browsers have the canvas filter, so the page takes the cheap path there.

## 3. Why the guards refuse (to be measured, not guessed)

The playing minute's real layers are about 3.3 million pixels against a
budget of four views (12.7 million at 2522x1257), and its largest laid
sheet is 1615x631 against 2,097,152 pixels. Neither should refuse. The
bound is cautious in one place by design: **inside a group drawn through
a matrix it counts each group's bounds whole**, because a kept drawing
(paint.h, `keep`) is made at the group's whole bounds and the walk cannot
know whether the store will answer MAKE. The glow is a scaled container,
and the page has other transformed groups; one of them, or the glow's
own layers counted whole, is the likely cause. The record does not say
which, and a replay of the kept page never finishes parsing (it waits on
what was not kept), so the first slice makes the record say.

## 4. The levers, in the order I propose

**W0. The record says why a paint was not covered.** For each paint the
guards send the ordinary way, which guard (the budget or a sheet), the
bound against its limit, and the group that put it over (the first that
does: its element and kind, the rect counted, inside a matrix or not).
At most the first few a reading, as the other lists are kept. Also: name
the reading's longest script task (what it was running, and the layouts
it forced). No change to what is painted.

W0 also says, for each whole repaint, what the script changed (the
element and the properties), so the P5 record confirms it is the glow
and not something else.

**W1. Decide once and carry it** (Fable's shape, 2026-11-22). Inside a
group drawn through a matrix the painter's view depends on the keep
store's answer (NO: the walk's view; MAKE or HAVE: the group's whole
bounds), and the paint itself moves the store. So the store is asked
ONCE: the bound's walk asks it for each group inside a matrix, through a
function the caller hands libflow, and RECORDS the answer for this paint
where the painter finds it; the painter obeys the record and does not
ask again. The one departure allowed is downward: a MAKE the painter
cannot carry out (no memory, the store changed its mind) becomes NO,
which takes the walk's view, never more than the whole bounds the bound
counted. That monotonicity is the whole argument, and the comment says
so. NULL (no store function) keeps today's whole-bounds rule. Asking the
store twice was rejected (two readers of a moving state disagree in the
middle of a paint); never making a kept drawing in a covered paint was
rejected (it starves them on a page where 98% of paints are covered).
Codex's cases stay as fixtures, refused where they were; mutants for "the
painter asks again" and "a recorded NO is painted as MAKE";
`--paint-check` runs over these paints too. Expected: whole repaints
covered again, about 100-160 ms where they are 400. **W1 is built only
if W0 says this caution is the cause**; if the budget or a sheet is
really past its limit, the record names what put it over, and that goes
back to Fable first.

**What W0 found (P5, 2026-11-22), and W1 as built.** The playing
minute's 23 refused paints were all `a sheet through a matrix` at the
glow's scaled container: 1916x1190 against 2,097,152 pixels, while the
budget's bound was 4.7 million of 12.7 and nothing was counted inside a
matrix. The keep store's caution was not the cause, so the shape above
is not built. The cause was the sheet's own bound: it met the group's
bounds with everything the walk takes in, which joins the view taken
back through the matrix with the upright place (the view moved by the
matrix's translation, for a face that refuses the sheet). With a large
scale those two lie far apart and their join spans the group, while the
painter's sheet (paint.c's `own`: the bounds met with the view taken
back, a pixel wider) is only the first, 1615x631 here. So the sheet's
bound is now the painter's, through the one function both ask
(`flow_through_sheet`: its bounds met with the view taken back through
the matrix, grown a pixel), so the two cannot drift apart again. That is still an upper bound of what
the painter asks: it takes the view back through its clip as well,
which only shrinks it, and its view is the walk's or less. The view
inside the group keeps the upright place, since the walk culls with it.
Codex's cases stay refused where they were; a fixture with a scale of 4
whose upright place lies far from its sheet is covered, and the mutant
that puts the upright place back in the sheet is caught.

The same record says what the playing minute's script rebuilds changed:
the progress bar's inline `transform` (`scalex`, 73 writes across its
three bars), a canvas's state (15, the glow), the chapter bar's width,
an animation's timing. Every one of them is a PAINT change by N2's
table, which is N3's case exactly.

**W2. The canvas's `filter`.** `CanvasRenderingContext2D.filter`, the
web's feature (a string, `none` by default, saved and restored with the
state; each draw filtered as by the CSS filter of the same text). Blur
first, through libcanvas's shadow blur, which is already a Gaussian of
the canvas's pixels; the other functions of the CSS filter list as the
same parse allows, each one a function libflow's filter code already
draws. A value the parse refuses leaves the filter as it was, as other
browsers do. On this page it moves the blur into the small canvas and
removes the 40 px layer from every paint that draws the glow: one
full-size blur fewer (66 ms) per whole repaint, and one layer fewer
against the budget. It needs the page's in-canvas path measured on the
P5 once it runs (a canvas filter costs at each canvas draw, which here is
the glow's own rate, not the view's). The filter is honoured by every
draw alike (`drawImage`, fills, strokes, text), or a page that tests for
it gets less than it was promised. New web surface, ruled in by
Chris on 2026-11-22 as its own small slice, in the place the table
gives; DEBTS books it meanwhile.

**N3, from Fable's brief: the paint follows the classes.** The structural
fix: a script's change damages what it changed (old and new painted
bounds), not the view. 25 of the 26 whole repaints are scripts' changes,
and `script_rebuild` marks the whole view after any of them without
asking what changed (STATES_AND_WHOLE_PAINTS.md in the kit has the
table). N3 stands on N2, which is parked on the old N1 and needs a
rebase and Fable's answers to its design. It is bigger than W0 to W2
together, and it is where most of the stutter goes away: with it, a
script changing the controls repaints the controls, and the cover keeps
the picture's part cheap.

**After those: the primitives' own speed.** A canvas drawn into its box
at 39 ns a pixel, a blur at 37, a warp at 57 are slow for what they are.
Booked now, with these numbers (Fable, 2026-11-22):
- **First item: every layer is drawn twice**, on black and on white, to
  recover its alpha, which doubles every effect in § 2. A layer drawn
  once into a surface that carries alpha is very likely the largest
  single primitive win; the slice starts by listing which draws cannot
  yet write alpha.
- **A filter computes only the output it will show.** The whole
  repaints are the glow's own ticks (a script changing the two
  cross-faded canvases every 2.7 s). After N3 that change damages the
  glow's painted bounds, nearly all of them under the picture, so the
  cover and the split leave only the ring outside it to draw; but a blur
  over a 1735x871 layer is still computed whole to draw a ring. The
  filter should compute the shown part of the damage plus its kernel's
  margin of input. General: any filtered layer mostly hidden or mostly
  off screen. The evidence, from the P5 after W1: splitting a whole
  repaint about the picture saves little for the glow, because each
  piece's blur margin nearly covers the whole layer (record 0121: the
  40 px blur drawn as pieces of 1735x337, 1735x277 and 253x743).

## 5. Slices

In this order (Fable's, 2026-11-22):

| slice | what | review | proof |
|---|---|---|---|
| 1. W0 + W1 | the record's reasons; the sheet's bound made the painter's (W0's finding) | Codex (it moves #698's guards) | #698's fixtures and mutants unchanged; the far-upright fixture covered; its mutant caught; the P5 run reads W0's lines |
| 2. N2 rebased onto today's N1, then N3 | Fable's brief | Fable's design pass, then Codex | the brief's oracle and its fixture page (`layout-ticks.html`) |
| 3. W2 (ruled in, 2026-11-22) | `context.filter` | Codex | canvas host tests to the pixel against the CSS filter on the same drawing, for every kind of draw; the P5 page takes the in-canvas path (no 40 px layer in the effects) and a line says what it costs per glow update |
| 4. the primitives | § 4's two booked items first | by risk | pixels unchanged; the effects' times |

If W0 shows W1 is not the cause, W2 moves ahead of N2, subject to Chris.
Each slice stops for a P5 run.

## 6. Open

- What the 2.5 s task was, and what each whole repaint's script changed
  (W0 names both).
- The `blur(15px)` group's owner: on the glow or above it (W0's lines
  name the groups).
- Whether W2 changes what the page draws per canvas frame enough to
  matter: measured, not assumed.
