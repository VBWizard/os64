# Video in yonder

*OUTLINE, 2026-11-16, Fable. Not ratified, not complete. It sets down what
the measurements of this morning decided, the shape that follows from them,
and the gaps still open (each marked **GAP**). Chris: "Video is one of the
big bosses. I want to take our time and get it right."*

*Stands on AUDIO_YONDER.md (the element, the sessions, libmedia, libav).
Evidence: `~/os64-opus-kit/VIDEO_DECODE_MEASURE_REPORT.md` and
`VIDEO_SCALE_MEASURE_REPORT.md` (3900X under Linux, not os64, not the P5).*

## 0. What this arc is

**A `<video>` shows its picture.** The first goal is the video file on an
ordinary page: an MP4 holding H.264 and AAC, as a hero clip, a news clip, a
product video, or a link straight to the file.

| In this arc | Named, and left for a later one |
|---|---|
| MP4 / H.264 (8-bit 4:2:0) with AAC or no sound | Media Source Extensions (script-fed streams: the big video sites) |
| the picture in its box, at frame pace, in step with the sound | WebM, VP9; AV1 (a second decoder port, dav1d) |
| `poster`, `videoWidth`/`videoHeight`, `object-fit` | captions drawn over the picture |
| `<video controls>` over the picture; a page for a linked video file | `drawImage(video)` into a canvas, `requestVideoFrameCallback` |
| silent files and muted clips, at their true cost | picture-in-picture, 10-bit and HDR |
| full screen (**GAP**: this arc or the next, § 9) | protected (DRM) streams: not planned |

Nothing here may make the later column harder. Where a choice bears on it,
the section says so.

## 1. What was measured, and what it decided

One thread, 3900X, real footage at the bitrates sites serve. "x" is the
multiple of real time.

| | plain C (libav today) | SSE assembly | with AVX2 |
|---|---|---|---|
| H.264 1080p30 decode | 2.7x; worst frame 61 ms | 6.6x; worst 34 ms | 6.2x |
| H.264 1080p60 decode | 1.5x | 3.6x; worst 42 ms | 3.3x |
| H.264 1440p30 decode | 1.6x | 3.7x | not run |
| 1440p60, any codec (by pixel count) | under 1x | about 2x | 1.9x to 2.5x |
| convert + stretch 1080p to 2560x1440 | 16.3 ms a frame | 4.7 ms | same |
| convert 1440p at natural size | 3.2 ms | 1.45 ms | same |
| copy one 2560x1440 frame (`rep movsb`) | 1.6 ms | | |

What follows from it:

1. **Assembly is required, for the decoder and for the scaler.** Plain C
   cannot fill a 1440p screen at 60 fps: the stretch alone is a whole
   frame's time.
2. **AVX is not required for H.264.** It is worth 10 to 30 % for VP9 and
   AV1 later. Ring 3 may use AVX (AVX.md), and FFmpeg's run-time
   detection decides for itself whether to.
3. **One decoder thread is enough to start; FFmpeg's own threading can
   wait.** On the P5 one thread carries H.264 at 1440p60 (below).
4. **A frame is converted and scaled in ONE step, to the size it will be
   drawn at.** It beat converting small and stretching later in every row.
5. **Average speed is not the test; the worst frame is.** At 1080p60 the
   worst frame takes two and a half frame times. The player decodes ahead
   into a queue or it stutters at every keyframe.
6. **Decoding and scaling want separate threads at 60 fps.** Together they
   are about 10 ms of a 16.7 ms frame on one thread, before anything else.

**On os64, on the P5** (`/tests/avbench`, one pinned thread; evidence
`~/os64-opus-kit/VIDEO_P5_POINT_REPORT.md`). Every run's pictures matched
the host's, checksum for checksum, in every mode.

| H.264 | assembly | plain C | worst frame (assembly) |
|---|---|---|---|
| 1080p30 | 9.3x | 4.2x | 27 ms |
| 1080p60 | 5.1x | 2.3x | 34 ms |
| 1440p30 | 5.0x | 2.3x | 53 ms |
| 1440p60 | 2.8x | 1.3x | 62 ms |

The P5 is about one and a half times the 3900X per thread; assembly buys
2.2 to 2.3 times over C, as on the host; AVX and SSE are equal for H.264.
So § 3's shape stands, **one decoder thread carries 1440p60**, and § 6's
eight decoded pictures cover the worst frame (under four frame times).
The scaling figures above are still the host's.

## 2. The libraries

**libav.so** gains the H.264 decoder and parser, FFmpeg's assembly with
run-time CPU detection (`nasm` returns as a build prerequisite), and
**libswscale**. Lean: use FFmpeg's scaler rather than write one; it is the
same LGPL source, already measured, and its SSE routines are the 4.7 ms
above. The library stays one LGPL library of its own, and the audit's
rules stand.

**libmedia.so stays the one door to FFmpeg.** It grows a picture side
beside the sound side: what the picture is (size, frame rate, the pixel
aspect), "give me the next decoded picture and its time", and "convert
this picture into these pixels at this size". Times are on the same output
timeline as sound. yonder, and any other player, see no FFmpeg type.

**Pure cores, host-tested with mutants**, in the pattern of
`media_queue.h` and `media_mix.h`:

- the picture queue (what is kept, what is dropped, generations across a
  seek);
- the choice of which picture to show at a given time, and what to do
  when late;
- the clock (§ 4).

## 3. Threads for one playing video

| Thread | Owns | Parks when |
|---|---|---|
| **decoder** (the session's existing one) | the demuxer, the sound decoder, the picture decoder; fills the PCM queue and the decoded-picture queue | both queues are full, or the media is decoded to its end |
| **picture** (new, one per playing video) | conversion and scaling into ready frames at the box's size | the ready queue is full, the video is not visible, or nothing is due |
| **audio** (the one for all of yonder) | `/dev/audio`, the mix, the heard position | as today |
| **main** (yonder's loop) | choosing the frame that is due, painting it, the page | as today |

One demuxer feeds both decoders, because an MP4 interleaves them. The
decoder thread parks on whichever queue fills first only when BOTH have
enough, or a starved one deadlocks the other.

**A door left open for later:** FFmpeg's frame threads (needs a pthread
shim) replace nothing here; they would live inside the decoder thread's
calls.

## 4. The clock

An element's time comes from the audio position today, and a file with no
sound is refused. Video needs a clock that does not depend on sound.

| The element | Its clock | Sound decoded? | Holds a mixer voice? |
|---|---|---|---|
| has sound, audible | the heard position (as today) | yes | yes |
| has sound, muted or volume 0 | the monotonic clock (`micros()`), anchored at play | **GAP** | no |
| has no sound track | the monotonic clock | n/a | no |
| sound refused (`/dev/audio` busy) | as today: waits | | |

Moving between rows (mute, unmute) must not jump the time: the new clock
is anchored at the old one's reading.

**GAP (muted sound).** Decoding sound nobody hears is cheap, and keeps
unmute instant; not decoding it is what the booked debt asks for. Lean:
decode it but hold no voice, and measure.

**The headphones' delay comes due here.** Pictures follow `heard`, which
rests on the sink's reported delay (150 ms from the P5's pair, never
measured: DEBTS). The first player is where it is measured.

## 5. From a decoded picture to the page

1. Layout gives the video box a content size in device pixels. The page
   tells the session that size and the `object-fit` rectangle inside it.
2. The picture thread converts each decoded picture straight to XRGB at
   that size (libswscale's `bgr0` is the window canvas's format).
3. When a ready frame comes due, the main loop marks the box's visible
   part dirty, as a GIF's frame does today. No layout.
4. The paint copies that frame's rows into the window canvas, clipped.
   Whatever lies over the video (its controls, the page's own overlay) is
   painted after it by the ordinary walk of the dirty part.

**The plain path and the general path.** Step 4's row copy applies when
the box is drawn plainly. A video under a transform, an opacity, a rounded
clip or a filter is drawn by the existing picture path from the same
frame (per-pixel, slower, correct). Same for the moment between a resize
and the first frame at the new size.

**What is not shown is not converted.** Covered window, scrolled out of
view, a page that is not the shown one: the picture thread parks. **GAP:**
whether the decoder also stops decoding pictures then (it must resume at a
keyframe, so the picture returns late) or keeps going (a steady cost for
nothing seen). Lean: keep decoding in this arc, book the saving.

**Late policy** (to become a lifecycle table before code): a ready frame
whose time has passed is skipped, never shown late; a picture thread that
is behind skips conversion of pictures already due; a decoder that is
behind asks FFmpeg to skip pictures nothing depends on; a decoder far
behind lets the sound run and resumes the picture at the next keyframe.

## 6. Queues and memory

| Queue | Holds | Depth (first guess) | At 1080p | At 1440p |
|---|---|---|---|---|
| decoded pictures (YUV 4:2:0) | decoder's output | 8 | 25 MiB | 44 MiB |
| ready frames (XRGB at the box's size) | picture thread's output | 3 | up to 44 MiB when filling a 1440p screen | same |

Eight decoded pictures cover a 42 ms worst frame at 60 fps with room. The
ready queue is kept short because its frames are the large ones.

**GAP (how many at once).** A page may hold many videos. Other browsers
play every visible one. A budget is needed (pictures decode only for
visible videos; a ceiling on total queue memory, beyond which a video
shows its poster or last frame), with numbers from the P5.

## 7. The path to the glass (the part that is the kernel's)

What a frame costs after yonder has it, from the code as it stands:

- **Four copies of the frame's pixels**: ready frame to window canvas
  (yonder), canvas to the compositor's copy (publish), that to the back
  buffer (the compositor's pass), the back buffer to the framebuffer. At
  2560x1440 that is 14.7 MB each time.
- **Publish holds `kGuiLock` with interrupts off for its whole copy.**
  Measured at 0.3 ms for a 1080p paint on the P5; a full-screen video
  takes that lock sixty times a second.
- **A publish does not wake the compositor.** Its damage waits for the
  next interrupt, in practice the 10 ms tick. A 16.7 ms frame cadence on a
  10 ms grid shows frames for 10 and 20 ms by turns: visible judder. This
  is booked (DEBTS: "no wake of its own").
- **There is no vertical sync and no report of when a frame was shown.**
  Tearing is possible, and a player cannot pace itself to the display.

**Measured on the P5** (`/tests/glassbench`, 2560x1440;
`~/os64-opus-kit/GLASS_MEASURE_REPORT.md`):

- one full-screen frame costs about 6 ms of CPU end to end, of a 16.7 ms
  budget, across three threads;
- a published frame waits 5 to 7 ms on average and up to 16, on the
  10 ms tick; a program's own sleep is on the same tick;
- `kGuiLock` is held with interrupts off for about 3 ms of every
  full-screen frame;
- **every moving edge tears, at every window size.**

**What this arc asks of the kernel is FRAME_CLOCK.md**: the compositor
paced by the display (the P5's graphics chip can be read for the line the
display is on), which cures the tearing and the 10 ms grid and gives a
player a "next frame" to wait for. This arc's players pace by that frame.

**GAP (one copy fewer).** Publishing into the back buffer when the window
is unobscured, or handing the compositor the canvas, would save about
1.4 ms a frame and as much interrupts-off lock time. GRAPHICS.md rejected
page flipping once, for reasons to re-read. To be weighed after the frame
clock, with sound playing: whether a fifth of every second with
interrupts off is heard.

## 8. The element

- `videoWidth`, `videoHeight` once metadata is known; the box takes the
  picture's ratio then (one layout), unless the page fixed it.
- `poster`: fetched as an image, shown until the first frame is.
- Readiness counts pictures as well as sound: `loadeddata` means a first
  frame exists.
- `canPlayType`: `video/mp4` and `avc1` codecs answer for what is built.
- `<video controls>`: the existing player, along the bottom, over the
  picture. **GAP:** hide when idle, as other browsers do.
- A link straight to a video file opens a page showing it
  (`WAY_BODY_MEDIA`'s video twin).
- **A file whose index is at the END** (common for MP4s not prepared for
  the web): today's loader refuses the seek FFmpeg makes while opening.
  With ranges, the open fetches the tail; without, the file must arrive
  whole. Needs a fixture; nothing in the tree exercises it.

## 9. Full screen (**GAP**: scope)

There is no full-screen window in os64: no client call to drop the
decorations or cover the screen after creation, and no `requestFullscreen`
in libdom. It needs a small window-manager call plus the DOM's Fullscreen
API. Lean: a late slice of THIS arc, since a video player without it is
not one, and since 2560x1440 full screen is the worst case everything
above was sized for.

## 10. Slices (a first cut, to be argued)

| | Slice | Proves |
|---|---|---|
| V0 | libav: H.264 + assembly, `/tests/avbench`; `os64_malloc_aligned` beneath it | the P5 point (measured) |
| V1 | the glass fixture and its measurement (done) | § 7's numbers |
| V2 | the frame clock (FRAME_CLOCK.md, its own slices) | no tearing, even pacing, a frame to wait for |
| V3 | libswscale in libav; libmedia's picture side; the pure cores | pictures and times, host-tested |
| V4 | **a standalone player** (`play`'s sibling: a window, a file, picture and sound in step) | the whole pipeline on the P5, before any page code |
| V5 | yonder: the picture in the box, the clock, silent files, `videoWidth` | a video on a page |
| V6 | poster, controls over the picture, the video page, index-at-end | the ordinary web |
| V7 | visibility, the budget for many videos, the muted-clip debt | what it costs |
| V8 | full screen | |

V4 is the lean worth arguing: a player outside the browser lets sync,
pacing and the glass be judged with nothing else in the way, and gives
os64 a video player in its own right.

`VIDEO_TRACKER.csv` beside this file is the state of each slice and of
the GAPs, as questions (`tools/tracker.sh video` renders it). Its ids put
a dot in the slice names (`V.3` is V3), as the tracker format asks.

## Rulings (Chris)

1. **Scope:** files with H.264 first, streams a later arc (2026-11-16).
2. **FFmpeg's scaler** (libswscale) in libav, rather than one of our own
   (2026-11-17).
3. **A standalone player before the browser work**, V4 (2026-11-17).
4. **Full screen in this arc**, as its last slice (2026-11-17).

Still open: the GAPs marked above (muted sound, pictures while out of
view, the budget for many videos, one copy fewer).

## For Chris to know

- **H.264 and patents.** FFmpeg's decoder is LGPL code and freely
   usable; the format itself is covered by patents in some countries,
   many now expired. For a personal operating system I see no obstacle,
   but it is yours to know about.
