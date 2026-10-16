# yonder's paint performance (the perf slice)

Chris, 2026-10-16: "animated GIFs cause the entire page to be redrawn" —
the first fix — and "look and see what others you can identify". This note
is the slice's design: what was found, how it is measured, what is fixed
in what order, and the state each fix adds. It is written before the code,
as every part of it that adds state is (PILE3.md is the pile this closes;
YONDER_DIAGNOSTICS.md is where the numbers land).

## What a frame costs, and why

**The build.** Until `opus/yonder-o2` (e4603c0b) yonder and the libraries
its paint runs through — libos64's draw, text and font code, libimage,
libpng, libgzip, libfetch — built `-O0`; the rest of its libraries already
asked for `-O2`. That branch makes them `-O2`. Chris on the P5 (2560x1440
screen, yonder's window about half of it), danlegt.com: `-O0` "slow &
choppy" throughout; at `-O2` the main thread runs 45-55% with only the
starfield in view, 70-75% at the QR panel, 100% at "Scratch 2000.exe"
(several animations), 80-85% at the signature wall and its GIFs, 75-85% at
the top (the roaches, the penguin, the fire, the counter). `-O2` ships
with this slice: it is not merged to userland before the fixes are.

**Three ways a picture's frame becomes a large repaint.**

1. **Every widget the dirty rectangle touches paints its whole bounds**
   (`paint_recursive`, libos64/ui.c — "whole widget even when only partly
   dirty"). yonder's view clips itself to the part; the root panel, which
   is the whole window, does not, so any mark fills the whole window, and
   the toolbar, status line or a scroll bar the mark reaches repaints
   whole.
2. **One rectangle holds every mark** (`os64_ui_mark_dirty` joins them).
   Two pictures far apart repaint everything between them: danlegt's
   roaches hang off opposite sides of `#main`, so each frame repaints a
   band the width of the page (PILE3.md § The one dirty rectangle).
3. **A page whose background is a moving picture repaints the whole view
   at every frame** (the canvas's owner; `picture_on_screen` marks the
   view). danlegt's body background is `stars1.gif`: a 304x234 tile, six
   frames of 90 ms, five of which replace nearly the whole tile — so the
   whole view, and every box over it, is painted eleven times a second.
   That is the 45-55% floor under every section of the page: about 40-50
   ms of main thread per frame for a view of about 1.8 million pixels,
   some 25 ns a pixel for a frame that is mostly a tile — a copy would be
   nearer 1. On danlegt, (3) also hides (2): the view is dirty every 90 ms
   whatever else moves.

**Other costs on the paint path, found by reading it.**

- A picture shown at any size but its own is smooth-scaled again at every
  paint (`yonder_draw_picture`), a GIF's every frame and every still
  picture in the part.
- Which pictures are on screen is asked per picture, at every tick and
  every reschedule (`picture_on_screen`), each asking walking every image,
  every background and the view's boxes: N moving pictures cost N walks a
  frame before anything is painted.
- Pictures' deadlines are not shared: two GIFs of one delay a few
  milliseconds apart paint twice.
- Nothing on the paint path is kept between paints: a shadow is blurred,
  a gradient computed pixel by pixel, a turned box warped through its
  matrix in doubles, every time.

## Measure first

Each number above is either Chris's or read off the code; none says where
a frame's 40-50 ms goes. The first change is the instrument, and every fix
after it is judged by it, on the P5 and in QEMU.

**What is counted, per page, since the record was last written:** paints;
the area they painted; and for each paint the time spent in its stages —
the whole render (libui's walk, the root panel and the chrome included),
the view, and within the view the canvas (the page's background), the
pictures, the text, opacity groups, shadows' masks, gradients and turned
boxes; and publishing the damage to the compositor. Per stage: the total,
the most one paint took, and how many paints were whole views. The ticks
too: how many, and the time spent finding what they mark.

**How it is timed.** A paint draws thousands of runs, so a stage is timed
with the processor's time-stamp counter (`rdtsc`, readable in ring 3),
not a system call; the counter's rate is taken from `os64_micros` across
the page's life when the record is written, so the record says
milliseconds.

**Where it is read.** A `paint:` section on the page's record
(YONDER_DIAGNOSTICS.md) at each write. To measure one part of a page —
the starfield alone, the roaches, Scratch 2000 — the record can be written
on demand: the `d` key in the view writes it ("written: when asked"),
keeps the counts since the last `d` as a numbered reading with the scroll
position, and starts them again — so readings of several parts of a page
sit side by side in one file: scroll there, `d`, wait ten seconds, `d`.

| State | Begun | Changed | Written | Let go |
|---|---|---|---|---|
| Paint counts (per stage: total, most, paints) | when a page is shown | at every paint and tick | at every write of the page's record | after an asked-for write (`d`), and with the page |
| The readings (`paint reading N`) | when a page is shown, numbered from 1 | one more at each `d` | kept in the record from then on | with the page's record |
| The counter's rate | at the first paint of the run | — | used at each write | never: the processor's |

**The first reading (QEMU, emulated, so its milliseconds are not the
P5's — their proportions are the point).** danlegt's top, from a local
mirror, the view 828 px wide, eleven seconds: 26 paints, every one a whole
view (the starfield), 121 ms each — gradients 63, warps 31, backgrounds 11,
publish 11, pictures 3, text 1, the window 2. And 354 ticks of 22 ms each:
**the ticks took about two-thirds of the main thread, the paints about a
quarter.** Two things the reading of the code did not rank: the gradients
(the page's buttons and panels, computed pixel by pixel at every whole-view
paint), and the ticks — 38 GIFs, each on its own deadline, each tick
walking the page once per moving picture to find what to mark.

**The P5's readings (Chris, 2026-10-16, danlegt, the view 826 px wide,
a whole view about 0.94 Mpx).** At the top: 40 paints a second, 6.4 ms
each, and 306 ticks a second at 0.4 ms. In the middle, Scratch 2000.exe
beside the GIF strip: 14 paints a second at 53.5 ms each — gradients 30.7,
shadows 11.0 — the thread full. At the bottom, the starfield alone: 14 a
second at 12.3 ms, every one a whole view, and 251 ticks a second at 0.8
ms. The canvas's tile is 1.1-1.7 ms of any paint, the window 0.4-0.8: the
starfield costs what the content over it costs to repaint, eleven times a
second, and the ticks cost 12-20% of the thread on their own.

## The fixes, in the order the readings set

**perf-1 (this branch, under `-O2`):**

1. **The instrument**, above, with the work beside the paints counted too:
   handing the ticker its next deadline (`schedule`, a call), and the
   main loop's time asleep, so a reading says how busy the thread was.
2. **Shared deadlines.** A moving picture's next frame is due on a 10 ms
   grid of the clock (`frame_due`, FRAME_GRID_MS): pictures of one delay
   share their ticks and their paints, and there are at most a hundred
   ticks a second. A GIF's delays are hundredths of a second, so its
   cadence is kept.
3. **One pass over the page a tick.** `pictures_seen` answers which
   pictures meet the view, and marks the ones a tick moved, in one pass —
   what `picture_on_screen` answers for one picture, which walked every
   image, every background and the view's boxes (resolving each layer's
   address) once a picture, at every tick and every schedule. Ticks and
   schedules use it, with the flags on the stack for up to 64 pictures and
   asking picture by picture when a page has more and no memory for them.
   No state outlives the call.

4. **A due deadline the loop meets itself is not rung for.** The P5's
   readings after (2) and (3): ticks cheap, but ~350 a second against ~38
   paints, and a second yonder thread (the ticker) at 10-25%. The counts
   named it: ~240 deadlines a second already past when handed to the
   ticker, all the page's script timers, while ~21 timers a second ran. A
   timer runs at the loop's turn after the events (script_turn); a
   schedule made among the events — a bell's own — saw it still due,
   handed the ticker the past, the ticker rang at once, and the bell,
   dispatched in the same batch, could not run it: the batch rang itself
   some thirty times a turn. Now a deadline the loop meets after the
   events (a script's timer, the settle, the stream's wait for its sheets)
   is handed to the ticker as the clock's next tick when it is already
   past; the bell still meets its own at once (pictures, ticks, layouts,
   an arriving page's wait). QEMU, danlegt with scripts on, 13.9 s: 1982
   bells to 169, the ticker's wakes 4098 to 447, paints 67 to 90.

**perf-2:** the gradient and shadow caches — each box's gradient pixels
and blurred shadow mask kept between paints, keyed by what draws them —
with their lifecycle table here before their code.

**Then measure again**, and decide whether these are still worth their
complexity:

- **A background frame that repaints only the background.** The boxes
  over a moving canvas do not change when it does; for ordinary
  compositing a pixel is `background x T + C`, where T (how much of the
  background shows through) and C (what the boxes put on it) do not
  depend on the background — so the view's content can be painted once
  over black and once over white, which gives both, and each background
  frame is then one pass over the view. Its own design, with its own
  lifecycle table (what invalidates the layer: a scroll, a layout, any
  other animation's mark, a picture arriving), before any code.
- **libui paints the part** — the root panel's fill, and every widget's,
  cut to the dirty rectangle — and **the dirty area becomes a short list
  of rectangles**, so two marks far apart paint two parts. Worth more on a
  page without a moving background than on danlegt.
- What the instrument says next: scaled frames kept, warps.

Also owed to this slice (PILE3.md): the paused-animation test, "throttle,
not freeze", and the frame-timing switch.
