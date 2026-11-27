# The video plane: a picture shown from the player's own thread

Fable's design, 2026-11-24, revised 2026-11-25 after Quinn's review (§ 10
says where each finding went and what Chris ruled). Kernel GUI concurrency
work, so this is the design note GUI_LOCK.md asks for before code, and its
slices get an outside review.

Chris's rulings it is built on: the shape (a plane under a window's content);
see-through content inside a plane's rectangle; **copy**, not flip; a plane's
stores the smallest of a fixed list of sizes that holds what is asked, backed
up front; frames answered on the thread that asked; the plane's place carried
by the window publish that paints its hole; `/sys/gui` reports planes; effects
that must read the picture fall back to the page's thread at first, and are
counted; the code is shared with gplay; the handle table grows and can be
interrogated. Order: before full screen and before MSE.

Facts cited are from reads of the kernel's GUI, paging, syscall and thread
code and of libmedia's player, gplay and yonder on 2026-11-24 and 2026-11-25.

## 1. The problem, in one paragraph

libmedia's player decodes, keeps time and scales on its own threads, but
choosing the picture that is due and putting it on the window is the caller's:
`os64_media_player_frame` runs on gplay's main thread and on a page's window
thread (VIDEO.md § 3: "the sound never waits on the caller's thread"; the
picture does). In yonder that thread also runs the page's scripts, layouts and
paints, so a 300 ms layout is nine pictures not shown. Every picture also
crosses four copies and marks part of the page for repaint.

## 2. What a plane is

A **plane** is a second pair of pixel stores belonging to a window: a canvas
the owning task draws in and a front copy the compositor reads, exactly the
pair a window has (`window_t.canvas`, `window_t.content`). The compositor draws
the plane where the window's content says it shows through.

- A window has at most `GUI_WINDOW_PLANES` (4) planes. A plane is named by its
  index and a **generation** that changes each time the slot is given out, so
  a name kept from an earlier plane never reaches a later one. A plane is not
  a window: it has no frame, no z-order, no event queue, no handle of its own.
- A plane has a **picture size** (the pixels its last publish held) and a
  **place**, given by the window's side: an **origin rectangle** (where the
  whole picture would sit in the window's content, whether or not all of it
  shows) and a **clip** inside it (the part that shows). The picture's pixel
  (0,0) is always at the origin rectangle's corner, so a video half scrolled
  under a header shows its lower half, not its upper half moved down.
- The clips of a window's planes never intersect. A change that would make
  two meet is refused whole (§ 3.5); the window's side paints one of the two
  itself.
- The window as a whole stays opaque: every pixel of its frame is still
  written by the compositor, from the content or from the plane. The claim
  walk and occlusion (`claim_stack`, `visible_claim`) do not change.

## 3. The kernel

### 3.1 Calls

All take the window's handle and are owner-checked by task, as every GUI call
is. "The creator" is the thread that created the plane.

| call | who may | what |
|---|---|---|
| `plane_create(win, w, h)` → name | any thread of the owner; it becomes the creator | gives the plane stores of the **capacity class** that holds `w` x `h` (§ 3.8), from the task's kept stores or newly allocated, mapped and eagerly backed. No place yet, so nothing shows. Refused by name: no slot, above the largest class, over the task's budget, no memory. |
| `plane_surface(win, plane, &out)` | the creator | the canvas pointer, capacity and pitch. One pointer for the plane's life. |
| `plane_publish(win, plane, w, h)` | the creator | snapshots `w` x `h` pixels canvas to front under kGuiLock (one row copy each, as a window's publish), damages the clip's visible pieces, stamps the publish-to-glass instrument. Allowed with no place (that is how a plane is given its first picture). Refused when `w` x `h` differs from the plane's picture size while its place is not empty (§ 3.5, rule 4). |
| `plane_frame_wait(win, plane, patience_ms, &out)` | the creator | parks until one of: a display frame, when the plane is shown (§ 3.6); a poke; the patience's end. `out` says which, the frame's number, blank and period, and always the plane's **committed state**: whether its place is empty, and the count of window publishes that changed it. |
| `plane_poke(win, plane)` | any thread of the owner | ends the creator's wait, or the next one if none is under way. One poke is owed at most. |
| `window_publish` with plane edits | the window's publishing thread | the window's own publish, carrying up to 4 **edits**: a plane's name with a new place (origin, clip, and up to 8 **overlay rectangles**, § 3.3) or "hide". Edits and pixels are one transaction (§ 3.5). |
| `plane_destroy(win, plane)` | the creator | refused `GUI_ERR_BUSY` while the plane's committed place is not empty. Otherwise takes it out of the window and keeps its stores for the task (§ 3.2). |

There is no separate call to place a plane: the first draft's latched
`plane_place` could be applied by a publish that did not carry the hole's
pixels (review, finding 3).

### 3.2 Who owns a plane's memory, and when it is let go

**A plane's canvas is never unmapped while its task lives.** A creator check
decides who may make plane calls; it cannot take a mapping away from the
task's other threads, whose CPUs may hold the translation after an unmap
until they next reload it, with no acknowledgement the kernel waits for
today (review, finding 1). So this design never puts the question:

- `plane_destroy`, and the replacement of a plane, hand its two stores to the
  task's **kept stores**, still mapped, still the task's pages. The next
  `plane_create` of the same capacity class, or a smaller one, takes them. A thread that kept
  the old pointer can spoil pixels in a later plane of its own task and
  nothing else.
- The pages go back to the allocator at the task's burial (`task_destroy`),
  when every thread of it is retired, not at its exit, where a sibling told
  to die may still be making a pair or drawing in a canvas. They go through
  `gui_canvas_pages_release` like any canvas, after kGuiLock (the owner's
  signalLock, which is what keeps signal delivery from writing a page as it
  is released).
- Capacity classes (§ 3.8) are what bound this: a task keeps at most what it
  once used at the same time in each class.

**The creator** is recorded as the thread itself, not its number. A thread
that ends while it has planes has them taken out by the kernel on its way
out: hidden (the hole then shows the content's pixels as they stand until the
window's side repaints), then destroyed as above. A recycled thread number
therefore never meets a plane.

- A window with planes cannot be destroyed: `GUI_ERR_BUSY`, the rule owned
  popups already have ("destroy an owner's children before the owner").
- **One door for the task's address range.** Window creation and `map` each
  read and advance `task->heapEnd` on their own, and a plane is created on a
  second thread. All three reserve through one function that claims a range
  atomically. This is a fix to a race that exists today (a window created
  while another thread maps), and it is P1's first commit.

Not changed here, and booked: `gui_window_destroy` and `unmap` release a
mapping with a local invalidation only, which the comment at
`gui_window_destroy` accepts because "GUI clients are single-threaded". That
is no longer true of yonder or gplay. The cure is an invalidation the kernel
waits for on every CPU running the task before the pages are reused; it is a
kernel slice of its own (DEBTS), and nothing in this design depends on it.

### 3.3 See-through content, and the overlay rectangles

Inside a plane's clip, the content does not simply cover the plane. Chris
ruled that the content pixel's top byte says how much of the plane shows (0
all plane, 255 all content, between blended, colours premultiplied). Reading
that byte for every pixel of a large rectangle in a kernel built `-O0`
without SSE would cost several milliseconds under kGuiLock at every picture,
so the place says where it is worth reading:

- **Outside the overlay rectangles**, the content is declared wholly
  see-through and the kernel does not read it: the plane's rows are copied.
- **Inside the union of the overlay rectangles**, the kernel reads each
  content pixel's top byte `a`: 255 copies the content, 0 copies the plane,
  anything else is `content + (plane * (255 - a) + 127) / 255` per channel,
  rounded to nearest, which is yonder's own premultiplied rule.
- Overlay rectangles may overlap; a pixel is blended once. The list in an
  edit is the whole list for the place, not what was just painted, so every
  content pixel inside the union must carry a true top byte, the gaps between
  two controls included.
- **The overlay area has a budget** (`GUI_PLANE_OVERLAY_PIXELS`, one number,
  393,216 pixels, about 2.7 ms of blending at the P5's price): an edit whose
  union is larger is refused, and the window's side paints the video
  itself. Eight rectangles bound the list; this bounds the work. The number
  holds a real player's bar and its seek preview (the P5 on 2026-11-26: the
  bar alone 78,896 px, the preview up to 143,616), and was 98,304 (about
  0.7 ms) until those runs showed the compositor's late frames at 0.28%
  with 143,616 blended.
- With no overlay rectangles (a video with nothing over it) the compositor's
  work for a new picture is row copies only.

Outside every plane's clip the top byte is ignored, as today; no existing
program changes.

The blend and the row walk are a pure file (`plane_compose.c`: no kernel
headers, integer only), built with flags of its own at `-O2` as the SBC port
is, driven on the host beside a model and broken by mutants, as
`frame_clock.c` is.

### 3.4 Compositing

`composite_one` blits a window's content into the damaged view by rows. Where
the view meets a plane's clip it calls the plane composer for that part:
plane rows taken from the picture at the offset the origin rectangle gives,
with the overlay union's pixels worked out as § 3.3. Outside it, the content
blit is unchanged. The cursor, the band and the switcher are drawn after, as
now.

- The clip is cut again by the window's content as it stands, so a window
  made smaller never reads past either store.
- Where the clip reaches past the plane's picture (it cannot while the place
  is not empty, by § 3.5's rule 4, except between a window's resize and its
  next publish), the content there is read as opaque.
- `/dev/glass` readers and the frame clock's copy see the composed back
  buffer, so a screen capture and a remote viewer see the video.

### 3.5 One commit: a place changes with the pixels that go with it

A window publish that carries edits is accepted whole or refused whole, with
the reason; refused, nothing changes. Under one hold of kGuiLock it checks,
copies its damage, and applies the edits. The rules:

1. Every named plane exists with that generation.
2. **The publish's damage covers what the edit changes the reading of**: for
   a place that is shown, hidden or moved, the old clip and the new; for a
   change to the overlay list alone, the new overlay rectangles. (A plain
   `window_publish` with no edits changes no place, whatever it paints.)
3. After the edits, no two of the window's clips intersect, and each overlay
   union is within its budget and inside its clip.
4. A plane is given a place that is not empty only when it has a published
   picture whose size is the origin rectangle's. And while its place is not
   empty its picture size does not change (`plane_publish` refuses): **a
   plane's picture changes size only while it is hidden.**

The compositor can therefore never see a hole without its plane, a plane
without its hole, or a picture at a place made for another size. The old clip
and the new are both damaged.

### 3.6 Frames for a plane

**One test says whether a plane is shown**, and both the frame clock's count
of shown waiters and the answers use it: the window is not covered, the
committed clip is not empty, and the clip, cut to the screen, meets the
window's visible pieces as the compositor's own occlusion walk gives them. The walk is bounded as the
visible-damage walk is; when it runs out of pieces it answers "shown". It is
worked out once a pass for each plane with a waiter.

A plane has its own `frame_wanted`, `frame_epoch`, answer slot and one waiter,
the fields a window has. `fr_answer` serves shown planes in the same pass as
windows, by the same epoch rule (a frame answers only requests made before
its pass).

`plane_frame_wait` is one request for its whole park:

- A plane that is not shown is answered nothing and stays parked. It is not
  sent back at once: the wait ends when the plane becomes shown and its next
  frame comes, at a poke, or at the patience's end.
- The request, and any wake owed it, are cleared when the wait ends for any
  reason, so a frame number from one wait never reaches the next.
- It is a park like the others: it ends for any caught signal
  (`signal_park_must_end`), and clears its own registration at the top of
  every pass.

A shown plane with a waiter keeps the frame clock running exactly as a
window's request does; a parked plane that is not shown does not.

### 3.7 Locks and holds

No new lock. Every plane call takes kGuiLock once, at a new counted site
(`GUI_LOCK_PLANE`), with the same interrupts-off hold a window's publish has.

- **Nothing is allocated, zeroed or mapped under kGuiLock.** `plane_create`
  prepares its stores outside it and installs them under it after checking
  that the window and the slot are still what they were; a loser gives its
  stores to the task's kept stores. Taking a plane out is a few stores under
  the lock; nothing is freed there.
- The plane publish's hold is its copy: VIDEO.md measured 0.3 ms for a 1080p
  publish on the P5. The composer runs inside the compositor's existing hold,
  on the compositor's reserved CPU, and its extra work is bounded by the
  overlay budget.
- That 0.3 ms is one publisher with no overlay. P2 and P3 measure the rest
  and put it in their records: the longest kGuiLock holds by site, publish to
  glass, and display frames missed, with the plane on and off, with an
  overlay at the budget, and with several planes publishing. Time a plane is
  deliberately not shown is kept out of the missed count.
- **The P5's figure for a publish's copy** (planetest, 2026-11-25): the
  plane site's worst hold of kGuiLock was 0.14 ms for a 640x360 picture,
  0.32 ms for 1280x720, 0.64 ms for 1920x1080, 1.5 ms for 2560x1440 and
  3.4 ms for 3840x2160. The copy is affordable through the 1080p class and
  not above it, which is the measured case for flipping the stores instead
  of copying when the larger classes are used.

### 3.8 Memory

- **Capacity classes.** A plane's stores are one of a short fixed list of
  sizes (640 x 360, 1280 x 720, 1920 x 1080, 2560 x 1440, 3840 x 2160), the
  smallest that holds what was asked. Two stores of `cap_w * cap_h * 4`
  bytes: 16.6 MB for the 1920 x 1080 class, which is what a 1780 x 1001
  picture takes (14.2 MB if it were sized exactly). The classes are what let
  § 3.2 keep a destroyed plane's stores and hand them to the next plane.
- Dimensions, pitch and bytes are checked before anything is allocated.
- **A budget**: a task's planes and kept stores together, counting stores
  being prepared, stay under `GUI_PLANE_BYTES` (one number). Over it,
  `plane_create` is refused and the caller shows the video as it does
  without a plane.
- Allocation is the fallible kind (`allocate_memory_try`), never the path
  that panics when memory is short: a plane is optional.
- `window_bytes` in `/sys/gui` counts planes; kept stores are a line of
  their own.

### 3.9 `/sys/gui`

Per plane: its window, generation, capacity class, picture size, place
(origin and clip), overlay count and area; whether it is shown by § 3.6's
test; publishes; how long a published picture waited to reach the glass (the
existing instrument, kept apart for planes); composed rows copied against
pixels read and blended; frames asked for, answered, and waits ended by a
poke and by patience. Per task: stores kept and their bytes against the
budget. Refusals by reason: not the creator, no such plane or an old
generation, above the largest class, over budget, no memory, busy, and each
of § 3.5's rules.

### 3.10 Kernel proof

- `plane_compose.c` on the host: against a model compositor over generated
  contents, pictures, places (origins partly outside their clips) and overlay
  lists (overlapping, with gaps), every pixel equal; mutants for each rule (a
  blend rounded the other way, an overlay's edge off by one, the content read
  outside the union, a pixel blended twice, the picture taken from the clip's
  corner instead of the origin's).
- In-kernel tests, through the calls and not the composer alone, since the
  commit is the lock hold, the copy and the edits together: create, publish,
  place, hide, destroy; the creator rule; an old generation refused; each of
  § 3.5's four rules refusing whole; a publish that paints elsewhere leaving
  every place as it was; a window with planes refusing destroy; destroy
  refused while placed; a creator thread ending with a plane placed; task
  death with planes; stores kept and reused by class; the budget; create
  racing a window's destroy.
- Frames: a covered plane in a partly visible window is not answered and does
  not keep the clock running; it is answered after an uncover with the clock
  stopped; a poke before a wait and during one; patience and a caught signal
  with a frame in flight; no frame number carried from one wait to the next.
  All of it under the frame clock and with `NOFRAMECLOCK`.
- `/tests/planetest`: a ring-3 fixture with a window thread and a picture
  thread publishing a moving test pattern under a see-through hole with
  overlapping translucent overlays, the hole scrolled partly out of its clip,
  an overlay moved and removed across a damage edge, checked through
  `/dev/glass`; and the same under the frame clock's tearing count.
- `make run-sparse` as well, since a second thread publishes.

### 3.11 As built (P1a to P3a)

- **P1a**: `task_va_reserve` (task.c) is the one door; `map` and a window's
  canvas take their ranges through it, and so does a plane's canvas.
- **P1b**: `plane_compose.c` as § 3.3 and § 3.4 say, at `-O2`, host-tested
  beside a model (`tools/test_plane_compose_host.sh`, its blend cases every
  byte against every byte) with 22 mutants. The blend works on all four
  channels in 16-bit lanes of one 64-bit word; the P5 priced it at 6.6 to
  7.0 ns a blended pixel and about 2 ns one read and found opaque or clear
  (2026-11-25), which `GUI_PLANE_OVERLAY_PIXELS` rests on (about 2.7 ms at
  its 393,216 pixels).
- **P1c**: the calls (syscalls 71 to 75: create, surface, publish, destroy,
  and `window_publish_edits`), `kernel/src/gui/plane.c`, the ABI in
  `os64/gui.h` § Planes. A pair of stores and its record are made outside
  kGuiLock and stay on the task's list (`task->planeStores`) until its
  burial, a kept pair serving the next plane of its class or a smaller one;
  the creator's exit is taken at the top of `syscall_thread_exit`, which a
  thread's returning entry reaches through libos64's exit stub; every other
  way a thread ends ends its task. The compositor draws the content around
  one clip in up to four bands and the clip's part through the composer, so
  no pixel is drawn twice; with two or more clips in one damaged view it
  blits the content and draws each clip over it.
- **P1d**: frames (syscalls 76 and 77, `plane_frame_wait` and `plane_poke`).
  § 3.6's one test of "shown" (`plane_shown_locked`) is judged in the
  compositor's pass for each plane with a request, beside the windows'
  requests, and the shown ones keep the frame clock running; the answer
  pass serves them by the same epoch rule and wakes the creator, kGuiLock
  then the scheduler's queue lock. The wait is P1's design-state table: its
  request at entry, the event wait's park, and every end clearing the
  request, the answer and the poke it took. Proof:
  `kernel/test/test_planes.c` (late phase; the wait on a plane that cannot
  be shown: a poke owed, one owed at most, the patience, nothing left set,
  the creator rule) and `/tests/planetest`: every pixel of the clip checked
  through `/dev/glass` after each phase (a picture scrolled partly out of
  its clip under overlapping overlays, one moved and one removed with damage
  on the moved one alone, among them); paced by frames with the numbers
  only rising; covered and answered nothing, then answered once uncovered;
  a poke during a wait and one before; prices (copied, blended, read and
  found opaque or clear, and the content pixel a plane replaces) and the
  plane site's worst hold for a publish at each class. Run under the frame
  clock, with `NOFRAMECLOCK`, and on the sparse layout.
- **Departures from § 3.8 to § 3.10**: `/sys/gui` gives the machine's pairs
  (in use and kept) and bytes, not each task's; a plane's publish starts the
  window's publish-to-glass instrument, not one kept apart for planes;
  `window_bytes` does not count planes; a caught signal ending a wait is in
  the code (`signal_park_must_end`, as every park) but no case makes one.
- **P2a**: libmedia's show thread (`media_player.c`), the pace's one owner
  while a plane is on, with the pass of P2's design state; its decisions are
  `media_show.c`, pure (`tools/test_media_show_host.sh`, 17 mutants): a
  publish owed, stale and ready, a recreate past the class, closing and its
  bound. `os64_media_player_plane` waits for the create's verdict (a tripwire
  bound only); `shown` pins a frame (two at most); a refused recreate hands
  the pace back to `frame`; the decoding thread joins the show thread before
  `finished`. Proof: `/tests/playertest` step 10 (the plane made and `frame`
  refused, published hidden then placed, `shown` pinned while the pace moves,
  a new size stale then ready on a new plane, closed placed and finished
  after the bound with the plane taken out, refused with no slot).
- **P2b**: gplay on a plane, as § 6 says. Its window is painted only when
  something but the picture changes (the readout, refreshed every fifteen
  display frames while shown; a resize; the plane placed or hidden), and
  the facts' READY at the hole's own size places the plane, asked at each
  look and right after a resize's hide (a hole the plane's picture already
  fits changes neither ready nor stale, so the show thread rings no look). Between the hide
  and the place the window holds `shown`, painted once: a still for the
  pictures queued at the old size. `--no-plane` asks for none, for § 8's
  measurements with the plane off. Proof: `/tests/gplaytest`, where
  `--check` now also judges the path (a plane on to the end with `frame`
  never called, or none with `--no-plane`; steps 10 and 11 are the
  window's path, 11 covered) and step 5's script resizes on a plane.
- **P3a**: yonder's videos on planes with nothing over the picture
  (`apps/yonder/yonder.c` § Videos on planes, `plane_place.c` pure with
  `tools/test_plane_place_host.sh` and 22 mutants). Each paint decides each
  video's place around the render: what the render drew over a picture
  is every cut a glass verb made after the video's own box, every widget
  painted after the view, and every pixel that differs from `shown`, which
  the render painted the picture from (the tile map); a video with nothing
  over it has its clip set to zero, the hole, in the publish that places
  it. From the ask until a frame call answers again after `frame` is
  handed back, the page paints the box from a pinned `shown`. P3b brings the two paints and the overlays. The ask is
  `yonder_media_plane_ask`, on a thread of its own, a close during the ask
  carried out by that thread when it returns; `--plane=off` and
  `planes = off` ask for none; facts.plane_serial is libmedia's for the
  serial rule. Proof: the host cases in `tools/test_yonder_planes.inc` (a
  real render placed, drawn over, not ready, an older picture, out of view,
  closed placed with its picture painted back, the first ask) and on QEMU
  `video-held.html` (through a three-second hold the plane published 100
  pictures and dropped none; with `--plane=off` 28 shown and 77 dropped),
  `video-leave.html` and `video-five.html`.
- **P3b**: what the page draws over a picture rides the plane as a layer
  (`apps/yonder/yonder.c` § Videos on planes, THE LAYERS). Where a render may
  have drawn over a mapped video's picture, the part's bounds are painted
  twice more from the video up (`plane_layer_paint`: `yonder_paint_from`
  with the glass's clip empty until `pane_video` for that video, which puts
  it back and draws nothing, so the box's own background beneath the
  picture is skipped; then the scroll boxes' bars and the widgets libui
  paints after the view, `os64_ui_paint_after`, each cut to the part) over
  black and over white, with the panes' own verbs so a pass counts nothing
  of a paint's, and `yonder_plane_layer` takes the layer apart; the passes'
  time is a clause of its own in the reading, taken out of the render's
  cost. The map is inked from the layer's coverage and gives the overlays
  (`yonder_plane_map_overlays`: rows of inked tiles as runs, grown down,
  joined by least added area to eight). The hole holds the layer, zero
  where there is none. The place's edit carries the overlays, and a change
  of them is a change of place; a refused publish whose places changed only
  their overlays keeps its holes and asks again, counted, and overlays the
  kernel refused as too large take the video off its plane for the budget
  instead of asking again. New reasons: not a layer (a pixel's channels
  disagreeing by more than one, or a layer that could not be made: the part
  is forgotten in the map until a paint can say), the overlays' budget (the
  kernel's budget, § 3.3), not yet painted. The non-source-over readers (a backdrop filter, a blend
  other than normal) were already P3a's "a backdrop reader"; every other
  verb composes source over. `--plane-check[=N]` composes the hole with the
  kernel's own `plane_blend` (built into yonder by `plane_blend.c`) against
  the page's pixels, counted per video in the record. Proof: the host cases
  in `tools/test_yonder_planes.inc` (an opaque box, black over black, a
  filtered box, half-red controls and a caption, a pill-shaped widget the
  picture's colour, a scroll box's bar, the budget, a layer that cannot be
  made, a wrong layer the check finds) with hand
  mutants, the core's in `tools/test_plane_place_host.sh` (the layer
  arithmetic against a reference, the overlays against a per-pixel count,
  the union's area, forgetting); on QEMU `video-controls.html`,
  `video-header.html` and `video-pair.html` placed through their overlays,
  `video-backdrop.html` leaving and coming back with its reader.
- **P3c**: P3b as Chris's P5 runs found it, five things fixed. Layers only
  where the page changed something (`planes_take_changes`: marks kept at
  `mark_part` with the video whose own picture they marked, since libui
  folds a page mark inside the box into the picture's rectangle; libui's
  damage entries no mark made, for a widget's own invalidation), per
  changed part (`yonder_plane_join`, shared with the overlays), waiting for
  a video no layer can place and a quarter second apart for one off for no
  layer or the budget. Buffers kept: the layers', a pass's and the check's
  row (`plane_buf`). The edit publish's damage is what rule 2 asks (an
  overlays-only change, its overlays) plus the damage rectangles meeting a
  changed place's clip, each whole; the rest goes plain, first. The record:
  no layer by cause, the disagreement's spread (2, 3, 4, 5 or more) and its
  first pixel with the element there; the passes waited for; the buffers
  grown; the heap's regions taken and given back; the largest overlay area
  asked for and the largest the budget refused; a plane's end with the
  frame wait's answer, which is also said on the console, and the asks
  after it. The rate line's bridge to a run's first picture runs from the
  run before's last publish when that is later than the last fold.
- **Open**: kept pairs of smaller classes can fill
  a task's budget for its life, so a larger plane is refused until it dies
  (beside one 4K pair in use, two kept 1080p pairs leave no room in 128 MiB
  for a second 4K one, which would fit without them).

## 4. libmedia

### 4.1 The show thread, and one owner of the pace

A player given a plane starts one more thread, the **show thread**, and from
then on **the show thread alone picks**: it reads the clock, asks the pace for
the picture due, judges and services, which is what `os64_media_player_frame`
does today for whoever calls it. Two threads sharing that call would each
move the pace and each retire the other's borrowed pixels (review, finding
4). The picture thread keeps converting into the ready queue as now; the
decoding thread is unchanged.

Each pass:

1. `plane_frame_wait`, with a patience of the time until the next picture is
   due (capped).
2. Under the player's lock: pick, by the clock at the answer's blank plus one
   period when a frame answered, by the clock now otherwise; take a reference
   to the picked picture; note what the wait said of the place.
3. Outside the lock: if a publish is owed (below), copy the picture's rows
   into the canvas and publish. No copy and no GUI call is made under the
   player's lock, so the sound's thread is never held by either.
4. Update the facts (§ 4.2) and ring the caller's bell if one the caller acts
   on changed.

**When a publish is owed.** While the plane is placed: every newly picked
picture of the plane's size. While it is not placed: the first picture after
the plane is made, and after that only while the caller has said it **wants**
the plane (it is about to place it); then every pick, at the clock's pace, so
the plane is current whenever the caller's publish lands. A plane that is not
wanted and not placed (the video is out of view) picks and lets pictures go
by the clock, as the picture thread's `retire_unasked` does, and copies
nothing.

**What wakes it.**

| event | how the wait ends |
|---|---|
| a display frame while the plane is shown | the frame's answer |
| play, pause, a seek, a new size, wanted or not wanted, close | the player's own call pokes the plane |
| the window uncovered, the place given | the next frame's answer, the plane now being shown |
| the next picture due while not shown | the patience's end |

Display frames missed (gaps in the answers' numbers while playing and shown)
are counted here, where gplay counts them on its main thread today.

### 4.2 The API

- `os64_media_player_plane(p, win)`: deliver to a plane of this window. The
  show thread creates the plane and owns it. If the thread cannot be started
  or the plane is refused, the call says so and the player behaves as it does
  without one.
- `os64_media_player_plane_want(p, yes)`: the caller means to place the plane
  (or no longer does).
- The facts gain: the plane's name; its picture size and serial; whether it
  holds a picture of the size the player is converting to (**ready**);
  whether it is **stale** (a picture of another size is due and the plane
  cannot show it while placed); the last refusal's reason; pictures
  published; display frames missed; waits ended by patience.
- **`os64_media_player_shown(p, &view)` / `os64_media_player_shown_done`**:
  any thread takes the picture the show thread last picked, by serial, as a
  reference that keeps its pixels unchanged until it is given back. This is
  what a caller paints from when it paints the video itself beside a plane.
  It moves nothing in the pace.
- Without a plane the player behaves as today and `os64_media_player_frame`
  is the caller's. With one, `os64_media_player_frame` is refused: there is
  one owner of the pace.

### 4.3 Starting and stopping

- **Starting.** The kernel's thread call can today report failure with the
  thread already running (the handle could not be had after the thread was
  submitted). H0 changes that: a thread's handle is reserved before the
  thread exists, so an error means no thread. libmedia relies on it.
- **Stopping.** `close` marks the player closing and pokes the plane. The
  show thread stops publishing and waits, by pokes and a short patience, for
  its place to be empty (the caller's next publish hides it); then destroys
  the plane and ends. **`finished` comes after the plane is destroyed and
  after the show thread's last touch of the player.**
- A caller that is leaving (gplay at exit) publishes once with "hide" and
  waits for `finished` with a bound. If the bound passes it ends the program
  without destroying the window; task death releases both. It does not call
  `gui_window_destroy` on a window that may still have a plane.

### 4.4 A new size

A plane's picture changes size only while it is hidden (§ 3.5), and there is
never a second plane staged beside the first, so a full set of four planes
cannot block a resize:

| step | who | what |
|---|---|---|
| 1 | the caller | asks the player for the new size, as today |
| 2 | the show thread | picks the first picture of the new size; does not publish it; the plane keeps its last picture on the glass; facts say **stale**; bell |
| 3 | the caller | publishes with "hide", painting the video itself from `shown` |
| 4 | the show thread | sees its place empty; if the new size is beyond the plane's class, destroys it and creates one of the right class (the old stores are kept for the task); publishes the current pick; facts say **ready**; bell |
| 5 | the caller | says it wants the plane, then publishes with the new place |

If step 4's create is refused, the facts carry the reason and the caller goes
on painting the video itself; nothing is left half made. The first picture of
a new player is steps 4 and 5 alone.

## 5. yonder

### 5.1 When a video is on its plane

Exactly when it is a **cover** today (`video_cover_rect`: the walk opens no
group around its draw, the picture is at the fitted size, it is visible and
painted) and no backdrop reader meets its box (`backdrop_read_meets`), its
clip meets no other plane candidate's clip, what is painted over it fits the
overlay budget, and the player's plane is **ready** at the fitted size and
holds **`shown`'s picture** (facts.plane_serial is `shown`'s serial at the
paint that places it): ready alone, after time spent hidden, could place an
older picture, and the glass would step back one for a frame. Otherwise the
page's thread paints it as now, plain or general, from `shown`.

Each reason is counted, in this order (`plane_place.h`): out of view, not a
cover, a backdrop reader, another plane's clip, not a layer, the
overlays' budget, not yet painted, not ready, a refused publish. The plane is
**wanted** (`os64_media_player_plane_want`) for every reason but out of
view, so the picture is current when the reason goes; that costs the show
thread a copy and a publish of every picture to a plane nobody sees, for as
long as a page keeps its video disqualified, so the record counts the time
wanted and not placed by reason, and that number decides whether the long-
lived reasons should drop `want` later.

The plane is asked for at a video's **first cover**, from a thread of its
own (the ask waits for the plane to be made), and kept until the session
closes: a video that stops qualifying keeps its plane hidden, painted by
the page from `shown`. A plane that **ends** while its session plays (the
show thread's frame wait answered an error; the answer is in the record
and on the console) is not meant to happen; it is asked for again, no
sooner than a second after the end (and only once it is a cover in view
again), three times a session, each counted. A refused plane is not: that
answer does not change.

### 5.2 What the page paints instead of the picture

- The video's box, where the picture would go: see-through pixels (top byte
  0), and nothing else. Letterbox bars are the box's background and stay
  opaque content.
- Everything the ordinary walk paints after the video inside that box (the
  page's own controls, captions, overlays) is stored into the window canvas
  as premultiplied pixels with their coverage in the top byte. **yonder's
  groups keep no coverage** (a group is laid over a copy of what lay beneath
  it), so the coverage comes from **two paints** of the covered part from the
  video up, once over black and once over white, the video's own verb
  drawing nothing: B = C·a and W = C·a + 255·(1 − a), so the pixel is B with
  a = 255 − (W − B) in its top byte, exact at 0 and 255 and within § 5.6's
  two between. Both paints are made only where the page CHANGED something
  over the picture at this paint (a view mark other than the video's own
  picture's, another video's picture, a change of place, a rectangle the
  render adds itself, and a widget's own invalidation, unless libui folds it
  into the video's own picture's rectangle: that one is lost, which delays
  only what an off-plane video's map knows, since a placed video marks no
  picture of its own), and there only where the render may have drawn over
  it (a draw noted, or a pixel not `shown`'s): a video's own new picture
  repaints its box and everything over it, and asks for no layer. Each
  changed part is its own layer, grown to the map's whole tiles so the map
  learns every tile it meets, and joined past four a video by least added
  area, so a moving dot costs its strip. A failed layer leaves its area
  forgotten, and the next change that meets it at all is layered with all
  of it. They wait for a video no layer can place (out of view, a backdrop
  reader, another plane's clip), the changed parts forgotten and learned
  again whole at the paint that places it; and they are made a quarter
  second apart at most for one off its plane for no layer or the budget,
  the parts between owed and marked again when the wait is over, while its
  forgotten tiles keep it off as not yet painted. A placed video never
  waits. **No pass, hole or check allocates**: their buffers are kept
  across paints, grown when needed, let go at a paint with no video mapped
  or no page shown (a buffer of 128 KiB or more is a kernel map and unmap
  under libos64's one heap lock, which the player's threads wait on too;
  allocating three a pass ran the sound dry on the P5).
- **What the two paints rest on, as a rule**: everything painted over the
  part is, pixel by pixel, content plus what lies beneath times (1 − a).
  Anything else that meets a clip (a backdrop filter, a blend mode, any
  filter or group whose result at a pixel depends on what lies beneath its
  neighbours) is a reason of its own, counted: P3a's "a backdrop reader",
  since the backdrop filter and a blend other than normal are the only verbs
  that read what lies beneath; every other composes source over. A pixel
  whose a, taken from each of the three channels, disagrees by more than 1
  makes that part no layer, as does a layer that cannot be made: the part
  is forgotten in the map, so the video is painted by the page, counted as
  not a layer, until a paint of that part can say what lies there.
  `--plane-check` stays the proof on real pages; it names the video and the
  pixel, not the element over it.
- Each area with coverage is an overlay rectangle, from **ink, not boxes'
  rectangles**: a 16-pixel tile map per video says which tiles the page
  painted over the picture, kept across partial repaints (a tile a paint
  covers only in part can be inked, never cleared) and rebuilt by a scroll,
  which repaints the whole view. A real player's transparent box laid over
  the whole video to take clicks paints nothing, and costs nothing. More
  than 8 rectangles are joined.
- **Every in-window toolkit widget over a clip** (yonder's `<video controls>`
  strip, a form control placed over a box) is in the same two paints: the
  layer pass paints, after the page's walk and the scroll boxes' bars, the
  widgets libui paints after the view, each cut to the part
  (`os64_ui_paint_after`), so a widget's coverage comes from the two
  backdrops like the page's, never from its colours. A rounded strip is
  opaque at its body and see-through at its corners.
- The place is the fitted box as the origin rectangle, and that box cut by
  the ancestors' clip and the view as the clip. It is an edit of the publish
  that paints the hole: one `window_publish_edits` whose damage covers every
  rectangle rendered and every clip its edits change, counted with its area
  in the record, so a page publishing whole views because of edits shows.

A new picture then marks nothing on the page and costs the page's thread
nothing. The page repaints over the video only when something over it changes.

### 5.3 Scroll, resize, pause, seek, end

- Scroll: the place moves as an edit of the publish that paints the scrolled
  page.
- A new box size: § 4.4. yonder paints the video itself (the general path,
  from `shown`) from step 3 until the plane is ready at the new size.
- Paused, ended: the plane keeps its last picture; the show thread's waits
  end only by patience, and it publishes nothing.
- A seek while paused: the player pokes; the show thread publishes the owed
  picture when the player has it.
- Out of view: the page hides the plane and says it is not wanted; the show
  thread lets pictures go by the clock. Covered: the kernel stops answering
  its frames (§ 3.6) and the same happens.

### 5.4 Moving between the plane and the page's own paint

Both directions are one publish: the hole and the edit together (§ 3.5).

- **Onto the page's paint**: the page takes `shown`, paints it and hides the
  plane. The show thread may have published one picture more in between, so
  the glass may repeat a picture or step back by one, for one display frame.
- **Onto the plane**: the page says it wants the plane, and when the plane is
  ready publishes the hole with the place. The plane is kept current while
  wanted, so the glass never steps back.
- A pause during the page's paint changes nothing: the show thread still
  owns the pick, and the page paints `shown`.

The record counts each move and why (a backdrop reader, a group around the
video, a new size, another video's clip, the overlay budget, a refusal by
reason).

### 5.4.1 No black hole where a video was

From a session's close to the publish that hides its plane, the glass shows
the plane's last picture. The publish that hides a closed session's plane
comes in the **same turn of the loop as the close**, and paints the last
picture back into the clip, from a copy the page took at the close, in the
box's own turn of the paint (so what the page paints above the video lies
over it), when the page that held it is still the one shown, laid out and
scrolled as it was; otherwise the page paints the place as it now is. On a navigation the old page's
sessions close when the new page is installed, so the hides ride the new
page's first paint, which repaints the whole view. A hide refused because
the plane is already gone (the show thread's closing bound passed and the
kernel took it out) is dropped and counted.

### 5.5 What a script can still do

`drawImage(video)` is refused today for any video and is not part of this
design. When it is built it reads `shown` on the window thread, which the
plane does not prevent.

### 5.6 Switch and proof

- `--plane=off` (and a setting) paints every video on the page's thread.
- A checking mode: at chosen frames yonder composes, in userland, what the
  kernel would show for the plane's clip (plane picture under its content, by
  § 3.3's rule) and compares it with what its own plain or general paint
  gives for the same picture. **The rule for a difference**: a pixel whose
  content top byte is 0 or 255 must be equal; one between may differ by 1 in
  a channel, because several see-through paints flattened into one layer and
  blended once round differently from the same paints blended in turn (a
  channel of 192 under two blacks of alpha 64 and 192 is 36 one way and 35
  the other), and by 2 where the coverage, measured from green, is one off
  that channel's own (the disagreement § 5.2 allows), whose share of the
  picture is then one off at most. Differences of 1 and of 2 are counted
  apart; anything more, or coverage outside every overlay, is wrong, and the
  first wrong pixel of a paint is said with where it is. Why the two: the
  coverage is measured from one channel, and each blend rounds on its own.
  It is measured, not proven: on QEMU's fixtures the 2s are a few in a
  million, all at partial coverage, but each see-through layer nested in a
  stack adds a rounding of its own, so a deep enough stack may exceed it
  (the host case of three stacked see-through boxes over a video prints the
  largest difference it finds).
- Fixtures: a video while a script holds the page's thread
  (`video-held.html`); a video's session closed while its page stays, and a
  navigation that takes seconds to arrive (`video-leave.html`,
  `tools/pages_serve.py`'s `?delay=`); five videos on a long page, scrolled,
  one resized while playing (`video-five.html`); a video with nothing over
  it; with page-drawn controls,
  see-through and opaque, overlapping; with captions; under a fixed header
  while scrolling, its top cut by the header; an overlay that moves and one
  that goes away across a partial repaint; two videos that overlap, with a
  translucent overlay over both; with a backdrop reader shown and hidden
  while playing; resized while playing; five at once (`video-controls.html`,
  `video-header.html`, `video-pair.html`, `video-backdrop.html`).
- The record: per video, pictures published by the plane, display frames
  missed, time on the plane against time on the page's paint and why, the
  page thread's paints the video caused (expected: none while on the plane).
- **What reached the glass is the measure, not the pace.** With a plane on,
  the pace's picks are the show thread's, and they keep the clock's pace
  whether or not a picture reaches the glass: while a video is on the page's
  paint and the page's thread is held, the picks go on and the glass does
  not change. So `--video-rate`'s rate and the record's reading count the
  pictures that reached the glass: the page's paints of a new picture in the
  video's box that the publish left off its plane, and the plane's publishes
  while it was placed and shown and the clock ran
  (facts.plane_placed_published). A page's paint that draws the picture is
  counted even when the page then paints over all of it (an opaque box over
  the whole video): what survives would have to be judged by its colours,
  which § 5.2's rule forbids, or by an analysis of the paint's order out of
  scale with a measuring line. Beside it, the longest the glass went
  without a new picture of that video while it was WATCHED: its clock
  running, its box on the view, the window not wholly covered, on its plane
  or off it, judged at every turn of the loop. A window partly behind
  another, its video hidden, still counts and reads as a gap: whether one
  box is seen would be kernel surface for a measuring line, and a plane's
  frames cannot say it, since a wait that runs out of patience is a display
  stall as often as a hidden plane, and a stall must stay inside the watch
  (ruled 2026-11-26). Judged every turn, so a pause or a scroll away is no gap while a page thread
  held for seconds (which runs no turn) is; a watch's end counts what it
  saw before it closes. The gaps come from the page's paints that drew some
  of the picture, from the player's own gaps between placed publishes
  (`os64_media_player_plane_gap_take`), from the glass's last picture to a
  plane run's first when that run is the only one begun since the last fold
  (the plane is folded every turn), and from a gap still open when a line
  or a reading is written. An open gap is never cut: each line and each reading counts the
  longest gap that ended or is open in its interval, and the picture ending
  a hold counts it whole. The line's fields are labelled
  (`video-rate,t=…,v=…,scr=…,gap=…,pick=…,drop=…,plane=…`: the glass's rate
  `scr`, the pace's rates `pick` and `drop`, each a second, `gap` in ms), and
  a legend says each label once; the running totals are the record's.

## 6. gplay

gplay asks the player for a plane, paints its window once (black bars opaque,
the picture's rectangle see-through), places the plane, and repaints only for
a resize or the readout. The readout is an overlay rectangle. Its main thread
then handles keys and nothing per picture. A resize is § 4.4, gplay painting
`shown` itself for the steps between. The same libmedia code as yonder;
nothing written twice. gplay is also the first consumer: it proves the kernel
slice and the libmedia slice on the P5 before a page is involved.

## 7. The handle table

A plane needs no handle, but each playing video now has one more thread, and a
thread is a handle; yonder was measured at 55 of the 64 a task could hold
(the DEBTS row H0 paid). Chris ruled a table that grows,
with a way to interrogate it:

- `task_t.handles` becomes a table grown in blocks under `handleLock`, with a
  ceiling that is a number in one place; existing handle numbers never move.
- `/proc/<pid>/handles` gains a first line: held, room, ceiling, the most ever
  held, and failures to allocate.
- **A thread's handle is reserved before the thread exists**, so the thread
  call's failure leaves no thread running (§ 4.3).
- Its own slice, first, with the kernel's own review rules: it touches every
  handle lookup.

### 7.1 As built (H0)

Block 0 is the task's own (`OS64_TASK_HANDLES`, 64); blocks of 64, 128, 256,
512, 1024 and 2048 follow as needed, to `TASK_HANDLES_CEILING` (4096,
`kernel/include/handle_blocks.h`). A block is allocated outside `handleLock`
and installed under it; its pointer is written once and read without the
lock; blocks are freed only by `task_destroy`. Entries are still claimed by a
CAS on their type, as before.

| state | who reads entries | who writes entries | who writes a block pointer | a lookup racing it sees |
|---|---|---|---|---|
| block 0 only | any thread, lock-free (pin, share and close's claim take `handleLock`, as before) | CAS claims | a grow, under `handleLock` | a number past 63: no entry, invalid; no such number was handed out |
| growing | as above | as above | the grower, once, NULL to the zeroed block, release, under `handleLock` | no block (invalid, as above) or the whole block, every entry free |
| grown | any thread, the pointer read with acquire | CAS claims in any installed block | the next grow only | every number below the room names one entry for the task's life |
| at the ceiling | as grown | closes free entries, claims take them again | nobody | as grown; an open fails with its usual code and `failed` counts it |
| tearing down | as grown | `handle_close_all`; a sibling's late `handle_alloc` lands in an installed block (DEBTS: a late alloc after the sweep is never closed) | nobody: the install reads `tearingDown` under `handleLock`, and `handle_close_all` takes that lock once before its walk | as grown |
| buried | nobody | nobody | `task_destroy` frees the blocks, then the task | n/a |

The thread call reserves its handle, then makes the thread, then commits: a
refused reservation makes no thread, and a failed make cancels the
reservation. A reservation is its owner's alone — a close refuses one, and
teardown's sweep passes it by — so the commit cannot be refused; a commit
after the teardown began closes the handle it published, as every commit
does, in a task that is dying.

## 8. Slices

| | slice | proves itself by |
|---|---|---|
| H0 | the handle table grows; a thread's handle before the thread | the kernel's handle tests, a fixture that holds a thousand, `/proc/<pid>/handles`, a thread call refused with no thread left |
| P1 | one door for the task's address range, then the kernel's planes (§ 3) | § 3.10 |
| P2 | libmedia's show thread and gplay (§§ 4, 6) | gplay on the P5: `i` shows pictures published by the plane, display frames missed, and the main thread idle; a covered window and a muted one hold their counts; § 3.7's measurements with the plane on and off |
| P3 | yonder (§ 5) | § 5.6; on the P5, a video's pictures shown while the page's thread is held busy by a script, and a real player's page Chris has played before, same media and window size with the plane on and off, with captions and controls showing: time on the plane, fallbacks by reason, pictures dropped, sound and picture in step |
| later | effects on the show thread from a recipe (a blur over a region), by the fallback's count; a flip in place of the publish's copy, by a measured 1080p60; an unmap the kernel waits for on every CPU (DEBTS) | |

Each kernel slice is reviewed before code is merged; each ships with its
record and `/sys/gui` lines; nothing site-specific anywhere.

## 9. What this does not do

- It does not make a page's own repaint faster; it stops that repaint from
  costing pictures.
- It does not show a video that something must read on the plane: those stay
  on the page's thread until § 8's "later".
- It does not show two videos that overlap, or one with more over it than the
  overlay budget, on planes: the page paints one of them.
- It does not change the sound, the clock or the pace.
- It does not close the gap an `unmap` or a window's destroy leaves on a
  sibling thread's CPU today; it avoids adding to it (§ 3.2).

## 10. Quinn's review, 2026-11-25: where each finding went

All eight hold, and all eight changed the design.

| finding | answer |
|---|---|
| 1. A creator check cannot revoke a task-wide mapping; thread end; the address range | § 3.2: the canvas is never unmapped while the task lives, stores are kept and reused by capacity class (§ 3.8); the creator is the thread itself and its end takes its planes out; one door for the address range, P1's first commit. The existing gap at `unmap` and a window's destroy is booked, not widened. |
| 2. The first picture and a replacement need handshakes | § 3.1 (publish with no place; the committed state in every wait; poke), § 4.1 (when a publish is owed, what wakes the thread), § 4.4 (the five steps; no second plane staged; destroy refused while placed). |
| 3. The place and its pixels need one commit | § 3.5: edits ride the window's publish, accepted whole or refused whole, the damage covering what changes, generations checked. `plane_place` is gone. |
| 4. A stable frame and one owner of the pace; startup | § 4.1 (the show thread alone picks; no copy or GUI call under the player's lock), § 4.2 (`shown` as a kept reference; `frame` refused beside a plane), § 4.3 (a thread's handle before the thread, in H0; `finished` after the plane is gone; gplay's bounded wait), § 5.4 (what a move may show, stated). |
| 5. Clip without moving the origin; overlapping planes | § 2 and § 3.4 (origin rectangle and clip; clips never intersect), § 5.1. |
| 6. Integer alpha and retained damage | § 3.3 (the rounding; the list is the whole list; a pixel blended once), § 5.6 (the checking rule, with her own example), § 3.10 and § 5.6 fixtures. |
| 7. Budgets for allocation and for work with interrupts off | § 3.7 (nothing prepared under the lock; what P2 and P3 measure), § 3.8 (checked sizes, a budget, the fallible allocator), § 3.3 (the overlay area's budget). |
| 8. One visibility decision | § 3.6: one bounded test for the waiter count and the answers; a wait is one request; hidden parks. |
| P5 acceptance | § 8, P3's row. |

**Ruled by Chris, 2026-11-25:**

1. **Stores kept by class instead of sized exactly** (§ 3.2, § 3.8): "the
   smallest of five sizes that holds it", about 17% more memory for a
   1780 x 1001 picture, in exchange for never unmapping a canvas under a
   running task. The other way was to build the waited-for invalidation
   first and keep exact sizes.
2. **The place rides the window's publish** (§ 3.5) instead of a call of its
   own.
3. **A thread's handle before the thread** goes into H0 (§ 7).
4. **The gap at `unmap` and a window's destroy is booked** in DEBTS as it
   stands (§ 3.2), with the waited-for invalidation as its cure.
