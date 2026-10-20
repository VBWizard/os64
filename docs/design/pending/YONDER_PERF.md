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
boxes, and inside the shadows and gradients the part spent laying their
pixels on the glass; and publishing the damage to the compositor. Per stage: the total,
the most one paint took, and how many paints were whole views. The ticks
too: how many, and the time spent finding what they mark. And the rest of
what the main loop does between paints: the layouts and the script tasks
(counted and timed), the bells the loop took and the rings the ticker
made (rings made before the loop takes one arrive as one bell), the
deadlines already past when
handed to it and where each came from, how late moving pictures' frames
were shown, and the loop's longest single wait.

**How it is timed.** A paint draws thousands of runs, so a stage is timed
with the processor's time-stamp counter (`rdtsc`, readable in ring 3),
not a system call; the counter's rate is taken from `os64_micros` across
the page's life when the record is written, so the record says
milliseconds. Each read is fenced (`lfence` on each side, and a compiler
barrier), so neither the compiler nor the processor moves the measured
work across it. That trusts the cores' counters to agree: the main thread
may move between cores between two readings, ring 3 cannot tell, and a
system call at every stage would cost more than the stages it timed.
`os64_micros` makes the same assumption, and the boot-time check that
would make it a guarantee is booked (DEBTS § micros). So an interval whose
second reading is behind its first counts as nothing, and the reading
says how many there were ("the counter stepped back N times"): a machine
whose counters disagree, and that moves the thread, shows it there rather
than in numbers nobody can see are wrong.

A script task is timed from `task_begin` to `task_said`, the two every
task passes through — a script, a timer, or an event (`load`,
`DOMContentLoaded`, a click) — so the count is every task the loop runs,
by kind. The layouts it forces while it runs are in its time; the rebuild
after it is a layout's.

**Where it is read.** A `paint:` section on the page's record
(YONDER_DIAGNOSTICS.md) at each write. To measure one part of a page —
the starfield alone, the roaches, Scratch 2000 — the record can be written
on demand: the `d` key in the view writes it ("written: when asked"),
keeps the counts since the last `d` as a numbered reading with the scroll
position, and starts them again — so readings of several parts of a page
sit side by side in one file: scroll there, `d`, wait ten seconds, `d`. A
`d` whose file is not written keeps its reading in the record and lets
the counts run on, and says so; the next `d` writes both.

| State | Begun | Changed | Written | Let go |
|---|---|---|---|---|
| Paint counts (per stage: total, most, paints) | when a page is shown | at every paint and tick | at every write of the page's record | after an asked-for write (`d`) that reached its file, and with the page |
| The ticker's counts (its rings and looks, how late it rang, how far past its time it slept) | when the ticker starts | at each look, on the ticker's thread, under its lock | at every write of the page's record | restarted with the paint counts; with the ticker on the way out, after which the record says it is not running |
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
2. **Shared deadlines — withdrawn.** A frame's next deadline was put on a
   10 ms grid so that pictures of one delay would share their ticks and
   paints. It never did anything (Codex, #283 round 3): yonder's clock is
   the kernel's 100 Hz tick, a GIF's delays are whole hundredths, and the
   floor is 100 ms, so every deadline was already on the grid and none was
   moved; the host test had used times the clock cannot produce. The
   readings below that credit (2) are (3)'s and (4)'s. Sharing ticks for
   real needs another rule — each picture keeps its exact time and shows
   at a coarser step — which is a design of its own (below, Then measure
   again).
3. **One pass over the page a tick.** `pictures_seen` answers which
   pictures meet the view, and marks the ones a tick moved, in one pass —
   what `picture_on_screen` answers for one picture, which walked every
   image, every background and the view's boxes (resolving each layer's
   address) once a picture, at every tick and every schedule. Ticks and
   schedules use it, with the flags on the stack for up to 64 pictures and
   asking picture by picture when a page has more and no memory for them.
   No state outlives the call.

4. **A due deadline the loop meets itself is not rung for.** The P5's
   readings after (3) (and (2), which changed nothing): ticks cheap, but ~350 a second against ~38
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
   an arriving page's wait) — including at a moment one of its own shares
   with the loop's, which the clock's 10 ms step makes common
   (`deadline_offer`; Codex, #283 round 4). QEMU, danlegt with scripts on, 13.9 s: 1982
   bells taken to 169, the ticker's wakes 4098 to 447, paints 67 to 90.

5. **What the ring loop was hiding.** With (4) in, every GIF on danlegt's
   top paused together about once a second, with neither thread busy. The
   reading grew the numbers that named it: how late frames were shown
   against when they were due (the most, and how many more than 30 ms),
   the main loop's longest single wait, and the ticker's own counts (how
   late it rang against its deadline, or against when the deadline was
   handed over if that was later, and how far past its time a sleep
   lasted). The P5: frames up to 250 ms late, the loop's longest wait
   249-264 ms, the ticker on time to within 19 ms. The ring was reaching
   a loop on its way into the GUI event wait, and the kernel lost it to
   the wait's quarter-second backstop; the ring loop had covered it by
   ringing again within milliseconds. The cure is the kernel's
   (WAKE_OWED.md, #282): a wake that lands mid-park is owed and paid by
   the park. The P5 after it (record 0020, danlegt's top): the longest
   wait 25 ms, frames late at most 20 ms, none more than 30.

**Where perf-1 leaves danlegt on the P5** (Chris, a 2560x1440 screen):
at the top the main thread ~75% and the ticker ~25%, and at the bottom
~50-60% and ~10%, neither reaching 100% even in a very large window. The
middle, Scratch 2000.exe beside the GIF strip, still fills the main
thread: its paints are gradients and shadows (perf-2).

**perf-2: the paint cache.** What a gradient and a blurred shadow cost is
arithmetic on values that rarely change: a gradient works out each pixel
in doubles (a hypot, often a pow), and a shadow blurs its mask six times.
Both are worked out again for the dirty part at every paint, so a GIF
beside a panel of gradient buttons pays for the panel eleven times a
second. The cache keeps what that arithmetic makes, keyed by the values
that make it, and a paint copies its part out.

*What an entry is.* A gradient's entry is part of its TILE — the picture
one copy of the gradient is, `w` x `h` (the tile's size, which for a
gradient is its positioning area or what `background-size` makes of it),
each pixel straight-alpha ARGB already relit for a dark page. Where a box
draws the tile, how it repeats, and the box's rounded corners are not in
it: a paint takes each pixel it draws from the tile (wrapped, as `wrap`
does) and cuts it to the corners as it does now. A shadow's entry is part
of its whole MASK — the shape's coverage, blurred, times what the box
allows (outside its border box, or inside its padding box) — across the
shape and the blur's reach around it, before its colour: a paint lays the
view's part of it on in the colour, as `mask_emit` does now. An entry is
one BLOCK of its picture, 128 x 128 pixels (less at the picture's right
and bottom edges), and a paint makes only the blocks its part reads. So a
picture larger than the view — a page-long box, or a tile
`background-size` makes two and a half times its box, which an animated
`background-position` slides under the box — is kept as the view's
share of it, and a slide makes only the blocks a 128-pixel edge brings
into view.

*The key is what draws it, after it is worked out.* A gradient's: its
kind, whether it repeats, its line (the direction and length) or its ray
(centre and radii), its stops RESOLVED — positions, colours with
currentColor already the box's, hints — the tile's size, whether the page
is dark, and the block's place. Two gradients written differently that
resolve the same share entries; one gradient on two boxes of one size
shares them; a layout that makes the same box again finds them. A
shadow's: its shape and the shape it is cut to, both moved so the mask's
corner is (0, 0) — moved by whole pixels, which changes no pixel's
coverage — whether it is inset, the blur, the mask's size, and the
block's place. Its colour and offset are not in the key: the offset is in
where the shape is, and the colour is laid on after.

*The same pixels, by construction.* A tile pixel is the colour the
gradient gives at its middle, which is what the uncached path asks at
`x + 0.5 - tx` wrapped — the same doubles, since every operand is a whole
number plus a half. The mask blocks a paint is missing are made
together, as the uncached path makes the view's part: their pixels and
the blur's reach around them, blurred once and cut into blocks after —
a block blurring a halo of its own would cost, with a wide blur, many
times what the whole does (Codex, #284 round 2) — and every pixel at
least the reach inside a cut edge is the infinite plane's (the reasoning
at `shadow_cast`, which holds on each axis). What every missing block
needs is worked out before any entry is made, so an entry is published
only once nothing can fail before it is filled. So a page painted with the cache is the page painted without it,
pixel for pixel, and that is the test: the host suite paints each of its
pages both ways, again after moving and duplicating boxes, and again with
a budget too small to hold them, and the verbs each records must be the
same.

*What is not kept.* A gradient that is one colour (the flat ellipse, a
repeat shorter than a pixel) is a fill, already cheap; an outer square
shadow with no blur is rectangles, already cheap.

| State | Begun | Changed | Let go |
|---|---|---|---|
| An entry (one block of a tile or a mask, and its key) | made at the first paint that draws from a block not held, when every block that picture's part needs fits (below) | used at each paint that draws from it: it goes to the front of the list, stamped with the paint; RELEASED — stamped with no paint and put at the back — when its picture's part could not be had whole | evicted from the back of the list when making another would pass the budget, or when the heap refuses the memory for another below it (asked again after each), if no paint has used it in the last two; all of them when another page is shown, and at exit |
| The budget | eight screens of four-byte pixels across both kinds — pixels, keys and each entry's own bytes — never more than a sixteenth of the memory installed, never under 32 MiB nor over 1 GiB (`yonder_paint_cache_budget`), the screen and the memory asked once when the cache is made | — | — |
| The list (most recently used first) and the table (by key's hash) | with the first entry | at each entry made, used, released or evicted | with the last entry |
| The cache's counts (blocks found and made, evicted, pictures not kept and why, bytes and blocks held) | with the paint counts | at each draw that asks | restarted with the paint counts; written in the record's `paint:` section |

*A picture is kept whole or not at all, and a paint never churns.* For
each picture, a paint first finds the blocks its part needs that are
held, then asks whether the rest fit — in what is free and what may be
evicted — and only then makes them. When they do not fit, it makes none,
releases the ones it found, and works the picture out for the view's part
as the uncached path does; a block whose memory is refused releases the
picture's blocks the same way. Nothing a picture cannot use is left
holding the budget. And an entry used by this paint or the one before it
is never evicted: a paint that needs more than the budget keeps the same
pictures every paint and works out the same others, instead of evicting
each block just before the same paint wants it again (the P5's reading
below is what that looks like). A view whose pictures pass the budget
costs what it costs today, and never more.

*Who owns it.* The window does (`g`), and hands it to `yonder_paint`; NULL
paints as before, which is how the host suite paints its oracle. It is
emptied when another page is shown, not at a layout, a zoom, a resize or
a dark toggle: the keys already say all of those, and what they made
ages out. One window paints on one thread, so the cache has no lock.

*The first reading (QEMU, emulated; the proportions are the point; the
cache then kept bands, below).* danlegt's top from the local mirror,
scripts on, the view 828 px wide, the same build with and without the
cache (only `/bin/yonder` differs), 47 s each after the page settled:
without it, 290 paints, every one a whole view, 130.4 ms each, gradients
64.1 of them; with it, 608 paints (384 whole views), 59.5 ms each,
gradients 16.3 — taken then for laying the copied pixels on the glass,
which the cache does not change; perf-3's instrument found it was mostly
the copying (below). The cache held 6 bands, 315 KiB, and
found 1824 bands without making one. The main loop stayed busy (98-99%):
it spends what it saved on painting twice as often, so the next reading
to ask for is the P5's.

*The P5's first reading, and why entries are blocks* (Chris, record
0104, the screen 1920x1080, yonder full screen, the view about 1.75 Mpx;
the cache then kept full-width bands of 64K pixels in 16 MiB and evicted
least recently used first, whoever had used it). The top: 60 paints a
second at 5.5 ms, gradients 0.3; the bottom: 20 a second, every one a
whole view, at 10.9 ms (12.3 at half the view, before). The middle,
Scratch 2000.exe beside the GIF strip: gradients still 25.7 ms a paint,
the loop 93% busy — in 25 s the cache made 4237 bands, evicted 4130, and
worked 2726 pictures out for want of room. Scratch 2000's panel lays
three gradients at `background-size: 160% 160%, 170% 170%, 260% 260%`,
slid by an animation of `background-position`, under a scanline
gradient: a band the width of a tile 2.6 times the box's keeps 2.6 times
what the view shows, the panel's share passed the budget, and LRU evicted
each band just before the same paint wanted it. Blocks keep the view's
share; the last two paints' entries are not evicted; and a picture whose
blocks do not fit takes none of the budget (Codex, #284 round 1, found
that last from the code).

**perf-3: drawing out of the cache.** What the cache leaves is not
laying the pixels on the glass, as the first reading above supposed. The
instrument now times that part on its own (`laid`: the mask and pixels
verbs, inside `shadows` and `gradients`), and at Scratch 2000 in QEMU,
every block found, a whole-view paint's gradients took 460.7 ms, of which
27.0 were laid. The rest is working the picture out of the cache, and
the copy was a pixel at a time: each pixel looked its block and its place
up in the maps and tested its middle against the tile's edges in floating
point. That is not all of it (the readings below): most of a cached
gradient's time in QEMU is still to be accounted for.

So a row is copied in runs (`blocks_run`): from a column, the columns
that read one block's row in order — up to where the block ends, which is
also where a repeating tile wraps — are one copy, and a run off a tile
that does not repeat is cleared. The tile is placed at whole pixels, so
the test of a pixel's middle that the uncached path makes is the maps'
"off the picture", and the copy makes no other. A rounded box's row is
cut only where its edges cross a pixel (`row_cut`): a pixel the span
covers wholly keeps its alpha, which is what the cut would give it. A
shadow's mask is laid the same way, its values times the colour's alpha
a run at a time. The pixels are perf-2's: the oracle paints every page
with the cache and without it.

The copy found what the per-pixel lookup had hidden: libos64's
`os64_memcpy` and `os64_memset` were byte loops, four passes for every
pixel copied. They copy by size now (#285), and the runs use them.

*The readings.* In QEMU, at Scratch 2000 with every block found, gradients
went from 460.7 ms a paint to 391.8 and laying stayed near 26; QEMU emulates
`rep movsb`, which #285 uses from 2 KiB, so its numbers for copies no longer
rank anything. The P5's (Chris, record 0114, 1920x1080, the middle, against
0107 at the same place on perf-2): 21 paints a second as before, each 29.5
ms instead of 38.2 — gradients 12.3 instead of 19.4, shadows 4.8 instead of
7.5, 4.1 of the two laid — and the main loop 88% busy instead of 93-95%.
At the top of the page the paint is 7.2 ms and handing it to the
compositor (`publish`) 8.9, up to 52, about 33 times a second, half of
them whole views: a full-screen window costs more there to publish than
to paint.

**perf-4: a layout repaints what it changed.** (Reviewed by Fable,
2026-10-17: approved with seven findings, all taken in below.)

*What is wrong.* On the P5 (record 0114, danlegt full screen at
1920x1080), the top of the page paints about 33 times a second, and half
of those paints are the whole view: 2 Mpx painted, published to the
compositor and composited, each time. Nothing in the view needs it. The
cockroaches are GIFs whose frames repaint only their own boxes, and the
rainbow visit counter animates `background-position`, which ticks in
place. A window cut to the roaches' height is smooth, because each of
those whole-view paints is then small. It is the same everywhere on the
page:

| P5 0114, 30 s or so each | layouts | paints | whole views |
|---|---|---|---|
| the top | 349 | 1209 | 620 |
| the middle (Scratch 2000) | 159 | 654 | 386 |
| the bottom | 310 | 605 | 600 |

*Why.* Two links in one chain:

1. **An animation far below the view lays the page out about ten times
   a second.** The QR-code ad below the Scratch 2000 and chat windows
   ("TURN ANYTHING INTO A QR CODE!") animates the
   `text-shadow` of its heading and its button, "chromatic", in
   `steps(2)` across three keyframe intervals, at 1.65 s and 1.45 s:
   about eight changes a second between them. They are laid out more
   often than that: libflow does not schedule a stepped interval by its
   steps. Any interval that is not one held value is due now
   (`f_motion_next_of`), and the pace is left to the face, so the pair is
   laid out at every frame yonder can make, as fast as its cost rule
   allows. On the P5 that was about ten a second at the top. In QEMU it
   was seven on the repro page below and twenty on a lighter one. libflow sorts an
   animation's properties three ways (motion.c). The TICK set repaints
   by a tick, with no layout: what nothing but painting reads and
   nothing inherits. The IN-PLACE class (`repaints_in_place`) needs a
   layout but repaints only where the element already draws, and
   `flow_motion_due` makes it due only while that can be seen (a
   colour, a fade). Anything else may carry what it draws into view
   from anywhere, so it is due wherever its element is. `text-shadow`
   is in neither list, so it is in the third class: due at every
   change, off screen or not.
2. **Every layout repaints the whole view.** `lay_out_shown` ends with
   `os64_ui_mark_dirty(&g.ui, &g.view)`, whatever the layout changed.
   So each of those layouts becomes a whole-view paint,
   a whole-view publish (a copy under `kGuiLock` with interrupts off)
   and a whole-view composite, wherever the page is scrolled.

Reproduced in QEMU with a page whose only animation is that
`text-shadow`, 3400 px below the view (danlegt's own layouts take over
200 ms in QEMU, so yonder's cost rule, `MOTION_COST_MS`, stops its layout
animations there): in 32 s at the top,
237 layouts and 238 paints, 237 of them the whole view, the main loop
32% busy. The same page without the animation painted once.

*The fix, in two parts*, which ship as two slices: perf-4a is part 1 and
the record's layouts by cause; perf-4b is part 2.

1. **`text-shadow` animates in place** (libflow). It moves nothing and
   changes nothing that a layout places; like `color`, which is already
   in the class, it is inherited and changes only how text inside the
   element draws. So it joins `repaints_in_place`, and an off-screen
   chromatic heading costs nothing at all: no layout, no paint, no
   publish. One thing is new for the class: a text shadow's ink reaches
   past the box, by its offset and blur, and that reach changes from
   frame to frame. `flow_motion_due` asks whether the element "can be
   seen" from the ink the current layout gave it (`box_seen`, `b->ink`),
   which holds only the shadow on show now. An element just out of view
   whose next shadow reaches in would then be judged unseen and left
   unpainted. So each element whose animations set `text-shadow` keeps
   its REACH: the farthest any FRAME's shadow can draw from the text. A
   keyframe's value is not the bound: an easing that overshoots (a
   `cubic-bezier` whose control points leave [0, 1]) carries the offsets
   past both ends of an interval, and libflow does not clamp a signed
   length. So each interval's extent is its two ends' farther one, plus
   their difference times how far the easing's output leaves [0, 1] (0 for
   `steps`, linear and every curve that stays inside). It is worked along
   the animations as the mix lays them: a later animation's implicit end
   is whatever the ones before it make, so an overshoot over an overshoot
   is counted. And a keyframe whose shadow list found no room in the lists'
   budget holds the value underneath, which no bound can see past, so its
   reach is unbounded (Codex, #288 round 2, found both). The seen test grows
   the element's inked overflow by that reach: `motion_seen`, and the
   stacked boxes' test, which hands inherited values down. The inked
   overflow already holds the current shadow, so growing it by the full
   reach counts that shadow twice. That is safe, and it is left so: the
   difference would be a bound only if the ink held nothing else. A reach
   of a quarter of what a coordinate can say or more is taken as seen
   from anywhere, since a rect grown that far is past what one can hold
   (Codex, #288 round 1, found the cap that stood there instead).
2. **A layout that changed only values painted in place repaints only
   what they paint** (libflow hands the parts, yonder marks them). When
   yonder lays the page out for its animations alone, with nothing else
   changed since the tree on screen (no script change, no picture or
   face arrived, the same width and zoom, `LAID_FOR_MOTION`), and every
   animated value that changed is drawn where its element draws, then the
   box trees are the same boxes in the same places. Only how some of them
   paint has changed. "Drawn where its element draws" is the in-place
   class and the tick set together. A tick moves a transform between
   layouts, so the tree on screen holds the last tick's and the next layout
   always finds it changed: a layout that refused the tick set would refuse
   every frame on a page that also turns something, as danlegt's ad does.
   An element animating its transform or its opacity is a group for the
   animation's life (`is_group`), and a group's bounds hold all it paints,
   through its matrix, the positioned boxes inside it included. The old tree is still alive when the new one is
   ready (`page_lay_out` keeps it until `page_show`), so libflow can
   hand over, for each animated element whose values changed, what it
   painted in the old tree and what it paints in the new one. That is its
   boxes' inked overflow, or its group's bounds when it opens one, as a
   tick hands them, with one exception: an inline element's pieces draw
   none of its text, which sits beside them on the line, nor a block
   split out of it, which sits on lines of its own — so an inline is
   marked by the nearest block with an element of its own, which holds
   all of them (`pieces_holder`, the box `motion_due` already asks
   about to decide whether an inline can be seen), which is more than the
   inline draws. To that it adds
   the stacked boxes inside it that take what changed, by the rule
   `motion_due` already applies and through the same function, not a copy
   of it. A stacked box takes from the element what is handed down (the
   inherited properties, and a fade), what it and every element between
   them took by keyword (`FStyled.taken`: `background: inherit` takes a
   background that is not inherited), and everything when a
   `background-clip: text` chain holds it. That is the shape
   `flow_motion_tick` already hands a tick's changes in, through the
   same callback, so yonder marks each part with `mark_part`, as it does
   for a tick. If anything else differs, libflow says so and yonder marks
   the whole view as now. Each of these sends a layout to the whole view:
   - a box tree of a different shape;
   - a box at a different place;
   - a changed value that a layout places;
   - the root's or the body's background, which the canvas takes over the
     whole view;
   - an element with no box of its own (`display: contents`), whose
     descendants draw what it hands them;
   - stacked boxes nested so deep inside a changed element that walking
     each one's ancestors (the cost is the square of the depth) would pass
     `PARTS_WALK_MAX` visits in a tree; the repaint is cheaper there.

   As a check that costs one walk, libflow compares the two trees box by
   box and treats any difference as "cannot bound". It compares the count,
   and each box's node, kind, rect, overflow, clip, whether it clips,
   whether it is stacked, positioned or floated, and the scroll of each
   scroll container. It deliberately leaves out each box's ink: shadows
   change a box's ink by design, so a compare that held it would refuse
   every `text-shadow` frame and part 2 would never fire.

   The two trees can then paint differently only where (a) the changed
   elements' own boxes draw, (b) a stacked box inside them takes from them
   by that rule, or (c) the canvas's element changed, which goes to the
   whole view. A propagated decoration colour, a gradient stop in
   `currentColor` or a clip to a changed radius is drawn by a box inside
   (a) or (b) (Fable looked for a fourth door and found none).

   Which layouts may hand parts is derived, not flagged. The rule is that
   every change that is not an animation's reaches the glass through a
   whole-view mark of its own, never through a motion layout. So a layout
   hands parts only when it is for the animations alone
   (`LAID_FOR_MOTION`) and reads the very cascade the tree on screen was
   laid out from (`cascade == p->cascade`), at the same width, height and
   zoom, with the old tree still alive (not the retry that frees it for
   room). The cascade is rebuilt for any change of sheets or of its
   environment (`width`, `height`, `dark`, `reduced_motion`), so those
   cannot slip through. A script's change waits for its own rebuild
   (`page_rebuild_owed` refuses a motion layout until then). A picture's
   size or a face that came in is laid out by the settle, which may wait;
   a motion layout made while one waits takes it in too, so it does not
   ask for parts (`g.pictures_moved`, `s_web.moved`): its parts would
   leave the new face's text unpainted. libui's dirty marks accumulate, so
   parts never undo a
   whole-view mark made before them. Anything that moved a box is caught
   by the compare.

Wherever the ad is out of view, part 1 alone takes its layouts to none;
where it is in view, part 2 takes each of its frames from 2 Mpx to its
heading and its button. What is left there is a layout per frame for a
visible chromatic heading, since a stepped interval is due every frame;
its cost the record will say. Scheduling a stepped interval at its steps
would take that to the steps' own rate (booked below). The finish beyond
that is a tick that pushes an inherited value down to the boxes that do
not override it (`taken`'s logic run the other way), a bigger design not
taken here.

*Why these and not something else.*

- **Not the tick set.** A tick rewrites the animated element's own style
  and nothing below it. `text-shadow` is inherited: the shadow is drawn
  by the text boxes inside the element, whose styles a tick does not
  touch. That is why the tick set leaves inherited properties out, and
  `color` with them.
- **Not "skip a layout whose element is off screen" alone.** That is
  part 1 without the reach, and it draws wrong pixels: the shadow that
  reaches into view from an element just outside it.
- **Not throttling off-screen animations by time** (some browsers slow
  them down). That still lays out and repaints the whole view, only less
  often, and the view would show stale frames of anything that reaches
  in. Due-when-seen is exact.
- **Not a general diff of any two layouts.** Comparing every box's
  painted values after any layout would also cover a script's change
  that keeps the page's shape. It needs a "paints the same" comparison
  over every property of every box, because a script can set any value
  on any element, and the box compare cannot see a value that changes
  paint and not shape. It is the bigger design, and no reading yet says
  those layouts are the cost. Part 2 is that diff, cut to the case where
  libflow already knows what changed (the timeline), with the box walk
  as its proof.
- **Not in the compositor.** Making publish and composite cheaper
  (booked under Publishing below) shrinks the cost of each wasted frame.
  This removes the frames.

*What it does not change.* A layout for any other reason (a script's
change, a picture or a face arriving, a resize, an animation of a
property that moves boxes) still repaints the whole view. Publishing and
compositing a whole-view frame cost what they do.

*State it adds.*

| State | Begun | Changed | Read | Let go |
|---|---|---|---|---|
| An animation's text-shadow reach | when the element's styles are built with that animation (FMotion) | never: the keyframes' values are fixed | by `flow_motion_due`'s seen tests | with the styles |
| A layout's parts (old and new paint of each changed element) | not kept: handed through the mark callback while both trees are alive, between `flow_layout` and `page_show` | — | by yonder's `mark_part`, at once | at the end of the call; the old tree is freed by `page_show` as now |
| (No flag: whether a layout may hand parts is derived from its cause, what waits for the settle, its cascade, its sizes and the old tree, above) | — | — | — | — |
| The record's layouts by cause: for the animations alone, and of those the ones that painted parts | at a page shown, with the other counts | at each layout | at each write of the record | with the paint counts |
| One layout's ask for parts (`s_parts`: the page, the view and its scroll; whether they were marked) | by `lay_out_shown`, before a layout for the animations | by `page_lay_out_run`, when it hands them | by `lay_out_shown`, after the layout | cleared after the layout that read it |

*Tests.*
- libflow:
  - An off-screen `text-shadow` animation is not due.
  - One whose reach crosses into the view is due.
  - One whose overshoot frame crosses into the view is due.
  - A stacked box that inherits it is judged by the same reach.
- The parts: the old and new paint of the changed element. The whole
  view when a box moved, the tree's shape changed, or the canvas's
  element is animated.
- An oracle that does not trust the rule: after a layout whose parts
  are handed, the two trees compared box by box, every box whose style
  differs lies inside a mark, with its ink, where it was and where it
  is.
- yonder: a colour's frame marks its element, not the view, and the
  record counts it; a frame that moves a box marks the whole view.

*How it is measured.* The QEMU page above: no layouts at the top. Then
the P5 at danlegt's top, middle and bottom: the whole-view paints and
the busy main loop are the numbers to watch. The before-numbers are
paced by frames, not by the pair's steps (a stepped interval is due every
frame): the P5's layouts a second are how fast a frame went, 11.6 at the
top and fewer at the middle, where a frame costs more. So the
after-reading is judged by the record's layouts by cause, which name what
still lays out, not by the total alone.

*perf-4a's reading (QEMU).* The repro page at its top, the heading 3400
px below the view: 1 paint and no layouts in 32 s, where perf-3 laid it
out 237 times. With the heading in view (a page of only it), it animates
as before: 668 layouts in 32 s, all of them for animations, against 600
on perf-3. That is a layout and a whole-view paint at every frame, which
is perf-4b's and the stepped schedule's to cut.

*perf-4b's reading (QEMU).* The same page of only the heading, in view:
1,014 layouts in 32 s, every one for animations and every one repainting
only what it changed, and 117 paints, none of them the whole view, against
669 whole-view paints on perf-4a. 117 paints in 32 s is the heading's 3.6
steps a second: a layout whose step changed nothing marks nothing. The
layouts rose because a frame got cheaper; scheduling a stepped interval at
its steps would take them to the steps' rate too.

*Questions for review.*
1. Is the in-place class the right home for `text-shadow`? Its ink is
   the first in that class to change extent, which is what the reach is
   for.
2. Is the box-by-box comparison a sufficient proof that a layout moved
   nothing, or is there a way two trees can match there and still paint
   differently outside the changed elements' parts?
3. Should the parts be handed for a script's layout too, when its change
   touched only in-place values? No (Fable): the box compare cannot see
   a value a script set that changes paint and not shape, so that needs
   the general diff declined above.

**perf-5: the paint's scratch.** In the middle of danlegt on the P5,
yonder's main thread spent about a fifth of a core on `unmap` and another
tenth on page faults (10/17: ~300 unmaps a second at ~660 µs each). The
cause is how libos64 handles a block of 128 KB or more. It gets a region
of its own (heap.c `HEAP_BIG_BYTES`) and is unmapped on free. So a buffer
the painter takes and frees on every paint costs, every paint, a map,
a page fault and a zeroed page for each page it touches, then an unmap
and a TLB shootdown. A probe at every candidate site (QEMU, mid-page,
4 s) counted 43 such regions, which was all of the heap's maps:

| Site | Big buffers | What it holds |
|---|---|---|
| `gradient_paint` | 29 (~500 KB each) | the view's part of a gradient, copied from the cache's tile when the cache holds it |
| `shadow_kept` | 11 (~140 KB) | a cached shadow's alpha |
| `mask_prepare` | 3 | the mask a missing block needs |

The opacity group's copy of what is under it (`glass_group_open`) and
the transform and text-clip sheets (`pane_through_open`,
`pane_clip_open`) stayed under the line in QEMU's window, but they are the
same shape and full screen can carry them over it.

**Fixed at the source, not in malloc** (Chris, 10/18). A cache of freed
big blocks in libos64 would serve every program, but it would disarm
MALLOC.md's ratified tripwire: freed addresses are never reused, so a
stale pointer faults rather than reading the next owner's bytes. Nothing
but yonder has the habit. So the painter owns its buffers instead:

- **`yonder_scratch_t`** (scratch.c): a fixed set of slots, each a buffer
  and its capacity, owned by the window like the paint cache and reached
  through the `Painter` and the glass. `take(bytes)` lends the smallest
  idle buffer that holds `bytes`. If none does, it allocates one with an
  eighth to spare, rounded to whole pages, so a box that grows a little
  between frames keeps its buffer. `give` returns it. A buffer is lent
  DIRTY: every site above writes each byte before it reads one.
- **Small requests are not pooled.** Below `YONDER_SCRATCH_FLOOR`
  (64 KB) a request goes straight to `os64_malloc`. The heap carves those
  from its pools without the kernel, so they cost nothing to free and
  holding them would only take slots.
- **Kept, but not forever.** The paint's end ages the slots, and a
  buffer idle for `YONDER_SCRATCH_IDLE_PAINTS` paints is freed. Showing
  another page frees every idle buffer. Idle buffers are held to
  `YONDER_SCRATCH_KEEP` bytes in all: one given back past that is freed
  instead of kept.
- **One painter.** Only the window's main thread paints. The scratch has
  no lock, and its header says so.

| A slot is | When | It becomes |
|---|---|---|
| empty | `take` finds no idle buffer that holds the request | lent (a new buffer) |
| idle | `take` and it is the smallest that holds the request | lent |
| idle, too small | `take` finds no empty slot and no idle buffer that fits | lent (its buffer freed, a new one made) |
| lent | `give` | idle, last used this paint |
| lent | `give`, and keeping it would pass `YONDER_SCRATCH_KEEP` | empty (freed) |
| idle | the paint ends, `YONDER_SCRATCH_IDLE_PAINTS` paints since its use | empty (freed) |
| idle | another page is shown | empty (freed) |
| any | the window closes (`yonder_scratch_free`) | gone |

A `take` with every slot lent, which only a paint nesting more buffers
than there are slots can cause, falls back to `os64_malloc`, and `give`
frees a buffer no slot holds. A buffer never given back stays lent, the
same leak a missed `free` is today. The host harness asserts that no
slot is lent between paints.

**Proof.** The host harness's `scratch_cases` walk the table above row by
row, with its allocator shim's counts as the oracle: a buffer lent again
is no allocation, and one freed is no longer live. Its oracle paints every
page through one scratch shared by the whole suite, so each page paints
on buffers the pages before it left dirty, and the pixels must match the
painter's without it. `scratch_paint_cases` paints a page with a big
gradient and a blurred shadow twice, with the cache and without, and the
second paint must allocate no buffer of the scratch's size: that is what
catches a site that goes back to the heap. A shadow's mask is prepared
only while its blocks are missing, so the same is asked of a shadow
painted again after the cache was emptied. `test_yonder_scratch_mutants.py`
breaks each rule. In QEMU, the heap's own count of regions taken (4 s):

| | Before | After | The scratch holds |
|---|---|---|---|
| Top of danlegt | 25–35 | 0 | 164 KB |
| The middle | 43 | 0–1 | 1.3 MB |

The P5's user/system split is the reading still owed.

**perf-6: what made a paint whole.** After the kernel's copies were made
cheap (#309, #310), the P5 in the middle of danlegt (1920x1080, 10/19)
read kGuiLock at under 5% of a core and yonder's main thread at 74%,
nearly all of it user time: 44 paints a second, 47% of them whole views,
about 85% of the pixels painted. And the record said that none of the
618 layouts for animations in that minute had repainted only what it
changed, which is perf-4's whole point. Nothing said why.

- **The record says what marked each whole view.** Every site that marks
  the view or the window whole goes through `mark_whole` with its cause
  (`WholeBy`: a layout for the animations that refused its parts, any
  other layout, a script's change, a scroll, a picture or a face that came
  in, the window and its bars). The first cause since the last paint
  names it. A paint of the whole view that no cause asked for was made
  whole by the parts it marked when they reach across the view
  (`s_parts_reach`, the union of `mark_part`'s marks), and by libui's own
  marks otherwise (a hover or focus moved onto the view, the window laid
  out). The `whole views:` line says the counts, and each `d` reading
  carries it.
- **And why a layout refused its parts.** `flow_layout_parts` says why it
  returned false (`flow_parts_said_t`): which test of the box compare
  failed (other layers, a box scrolled elsewhere, a box moved), a value a
  layout places (and which property), an element with no box of its own,
  a background the canvas may take, stacked boxes nested past the walk's
  budget, no memory. yonder adds the reasons decided before libflow is
  asked: a picture's size or a face laid out with it, another cascade,
  another size, the view moved. The last refusal is named as a selector
  would name it (`div#x.a.b`, `text in …`), but only when a record can be
  written (yonder.conf's `diagnostics`): naming costs a string at every
  refusal. The counts cost an increment, like every count here.

**What it found.** In QEMU, every refusal was the same one: *an element
with no box of its own, `span.home-scratch-character-performer`*. That
span is inside the Scratch 2000 shop's `<button>`, and a button is a
replaced element here (boxes.c `replaced`): its widget draws it, and
nothing inside it gets a box. The span's looping `transform` changed its
style at every frame, and "no box of its own" refuses the whole page's
parts, because it was written for `display: contents`, whose descendants
draw what it changes.

**The rule, fixed.** An element that no box stands for, and nothing
inside it either (`draws_none`), shows nothing of what changed, so its
change is skipped rather than refused: a button's content, what is under
`display: none`. `display: contents` still refuses, because its
descendants have boxes. Asking the new tree alone is asking both: the box
compare has already proved the trees' boxes stand for the same nodes.

**Proof.** The libflow motion cases ask each refusal for its reason, its
element and its property: a box that moved, another number of stacked
boxes, stacked boxes reordered, a box scrolled elsewhere, a tree missing,
the walk past its budget, a value a layout places (`z-index` before it
moves anything), the canvas, `display: contents` with and without a box
inside, and an anonymous box named by the element holding it; an element
inside a button and one under `display: none` are bounded. The scripts
suite checks the counts and the named element through a real frame, the
naming gate, the first-cause rule, the parts' reach, and the record line
word for word. `test_motion_mutants.py` breaks each reason and each
check. Readings, the middle of danlegt:

| | Layouts for animations, repainting only what they changed | Whole views | What made them whole |
|---|---|---|---|
| P5 1920x1080, before | 0 of 618 | 47% of paints | (not yet counted) |
| QEMU, before | 0 of 28 | all | 28 refused layouts, 98 parts |
| QEMU, after | 28 of 28 | all | 126 parts |
| P5 2560x1440, after | 597 of 597 | 28% of paints | 656 parts, 2 the toolkit's |

What is left is the parts: the GIF strip, the Scratch panel and the QR
code's text are far apart in the view, and libui's one dirty rectangle
is their union, so even a paint that is not whole averages a third of
the view (1.1 Mpx of 3.3). That is the libui item below.

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
  of rectangles**, so two marks far apart paint two parts. perf-6's
  reading makes it the next one: once the layouts for animations marked
  only their parts, every whole view left on the P5 was parts far apart
  united into one rectangle.
- **Moving pictures that share their ticks.** Pictures of one delay
  started a tick apart stay a tick apart, and each tick is a paint. A
  coarser display step (20-30 ms) would let them share, with each
  picture's exact time kept so its cadence does not drift by the
  rounding; what it costs in jitter and what it saves in paints is for a
  reading to say.
- **A stepped interval due at its steps.** `f_motion_next_of` answers
  "now" for any interval that is not one held value, so a `steps()`
  animation in view is laid out at every frame yonder can make, not at
  its steps. Its next change is the next step's time, which the easing
  and the interval give, as a held interval's far end is given now. For
  danlegt's chromatic pair in view that is about twenty layouts a second
  (in QEMU, on a light page) against their 7.8 steps.
- **Publishing.** At the top of danlegt, full screen on the P5, handing a
  paint to the compositor costs more than the paint (record 0114). Most of
  those paints should not have happened (perf-4), but each frame still
  moves its damage three times: the publish's copy (under `kGuiLock`,
  interrupts off), the composite, and the flush. Read on the P5 after
  #306, #309 and #310 (10/19, the middle at 1920x1080): publish 0.3 ms a
  paint, the compositor's hold of kGuiLock 2.6% of a core, every wait on
  the lock together under half a percent. Nothing is left here to take.
- What the instrument says next: scaled frames kept, warps.

Also owed to this slice (PILE3.md): the paused-animation test, "throttle,
not freeze", and the frame-timing switch.
