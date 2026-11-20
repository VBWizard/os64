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
| full screen (§ 9; its last slice, Rulings 4) | protected (DRM) streams: not planned |

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

**The conversion on os64, on the P5** (`avbench -x`, one pinned thread,
2026-11-18; evidence `~/os64-opus-kit/video-v3/p5_avbench.txt`). The
call is libmedia's: `sws_scale_frame` on a dynamic context into the
caller's pixels. Milliseconds a frame, average / worst, with FFmpeg's own
choice of routines (AVX2); SSE is the same within a percent.

| source | at its own size | to 2560x1440, bilinear | the same, C routines | 3900X, SSE (above) |
|---|---|---|---|---|
| 480p | 0.09 / 0.13 | 2.78 / 5.25 | 10.1 | 3.74 |
| 720p | 0.21 / 0.28 | 3.10 / 5.82 | 11.5 | 4.32 |
| 1080p | 0.52 / 0.75 | 3.55 / 6.58 | 13.6 | 4.72 |
| 1440p | 1.14 / 1.51 (is 2560x1440) | | 2.54 | 1.45 |

The P5 converts about 1.3 times as fast as the 3900X, and assembly is
worth 3.8 times over C there too: § 1's conclusion 1 holds on the
machine. **1080p60 shown at 2560x1440 costs 6.8 ms a frame, decode and
conversion together, on one thread** (worst decode 34 ms, which the
queue absorbs); 1440p60 at its own size 7.2 ms. A conversion's worst
runs to about twice its average in every row, though its work does not
vary: that is the thread being kept from the CPU, which V4's pacing
should watch. Fast bilinear is not in the table: enlarging, it is plain
bilinear on os64 (§ 2, As built).

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

**As built (V3).** What the outline left open, settled:

- **libswscale is in libav.so**, its routines chosen at run time. Its
  experimental "ops" backend is built and never used (DEBTS: 9.0.2 does
  not link without it). `avbench -x WxH` times decode and conversion on
  the call path libmedia uses; on the 3900X that path costs what the
  table above measured, and on the P5 what its own table (§ 1) says.
- **The header** (`os64/media.h` § THE PICTURE SIDE): `os64_media_open_for`
  with WANT_SOUND and/or WANT_PICTURE. An unwanted stream is discarded at
  the demuxer, so a sound-only probe makes and decodes none of the
  picture's packets (and its analysis is held short, or FFmpeg would read
  the sound for seconds waiting on the discarded picture).
  The open succeeds with whatever wanted kind can be decoded, and the
  info says of each kind whether it is absent, playable or unplayable.
  It also gives the shown size (the pixel aspect applied), the coded size
  (cropped), the stated rate, and the rotation, which is reported but not
  yet honoured (DEBTS). One `os64_media_next` gives sound, a picture or
  the end, of the kinds the caller accepts (§ 3). A picture is a handle with its time, duration, size and
  keyframe flag. There are `os64_media_picture_skip` (the unused pictures),
  `os64_media_picture_resync` (to the next keyframe) and
  `os64_media_counts`.
- **The timeline's zero is the FILE's start**, the earlier of the sound's
  and the picture's starts, the same for every open. A sound that starts
  later is preceded by silence, so it is contiguous from frame 0.
- **A picture is released from any thread and returned on the decoding
  thread**, because FFmpeg's pools have empty locks in this build
  (media.c § THREADS). `tools/test_av_audit.py` follows the calls from
  each off-thread entry point. `tools/test_media_picture_host.sh` runs
  the glue on the host over the pinned FFmpeg, under ThreadSanitizer and
  ASan. Freeing on the wrong thread is caught there, as a race in
  `av_buffer_pool_get`.
- **The scaler** converts and scales in one step into the caller's
  pixels. The X byte is written 0xFF. Colours follow mpv's matrix rule
  where the stream is silent. The primaries and transfer are not
  converted. An interlaced picture is converted field by field. It writes
  exactly the pixels asked for. A conversion at the picture's own size,
  at a width that is not a multiple of eight, goes through a padded
  buffer of the scaler's own: FFmpeg's unscaled converter works eight
  pixels at a time and would otherwise write past the row (media_scale.c,
  THE UNSCALED EDGE).
  FAST is FFmpeg's fast bilinear only when shrinking. Enlarging, that
  path runs x86 code it writes into the heap, and os64's data memory does
  not execute: it was the P5's first fault, `avbench -x 2560x1440`.
- **Cover art** (an attached picture) is never the media's picture, nor
  part of its zero.
- **`canPlayType`**: `video/mp4` naming `avc1` or `avc3` answers
  probably, and a codec string that does not parse answers no.

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
| **decoder** (the session's existing one) | the demuxer, the sound decoder, the picture decoder; fills the PCM queue and the decoded-picture queue | both queues are full; or one is full and libmedia answers `GOT_FULL` for that kind; or the media is decoded to its end |
| **picture** (new, one per playing video) | conversion and scaling into ready frames at the box's size | the ready queue is full, the video is not visible, or nothing is due |
| **audio** (the one for all of yonder) | `/dev/audio`, the mix, the heard position | as today |
| **main** (yonder's loop) | choosing the frame that is due, painting it, the page | as today |

One demuxer feeds both decoders, because an MP4 interleaves them. **The
sound is never hostage to the picture decoder**: the decoder thread asks
`os64_media_next` only for the kinds it has room for (`accept`), and
libmedia reads on past the other kind, keeping its packets undecoded on
a list of their own (media.c § THE PACKET LISTS), so the sound queue
fills to its own depth whatever the pictures are doing. Once a list's
packet data reaches eight MiB (some five seconds of a 1440p60 picture)
the answer is `GOT_FULL`, naming the kind to take first, and the thread
parks on that kind's queue. It parks when both queues are full, too.
Without this the sound queue never held more than the picture queue's
lead plus one interleave chunk, so a picture decoder falling behind ran
the SOUND dry. `tools/test_media_picture_host.sh` check 12 is the proof.

**A door left open for later:** FFmpeg's frame threads (needs a pthread
shim) replace nothing here; they would live inside the decoder thread's
calls.

**As built in the standalone player (V4, `/bin/gplay`).** The four threads
above, plus a keys thread owning the `/dev/mediakeys` session, under ONE
lock never held across a libmedia call, a conversion, a device call or a
window call that draws or waits (the window's doorbell, which never
blocks, is rung under it, as bells are); pictures to release are
collected under it and released after. What the player adds to the
table, most of it proven by `/tests/gplaytest` (Fable's changes to the
tables sent, `~/os64-opus-kit/VIDEO_V4_TABLES.md`):

- **A covered window keeps the sound playing.** It is answered no frames,
  so with no frame call for 100 ms the player's picture thread retires
  overdue pictures by the clock (V.5.2's S1; it was main's, rung by
  another thread, in V4); the queues keep moving. Their conversions are
  wasted (DEBTS).
- **Nothing reaches the device before the clock plays**: after an open or
  a seek the picture's run-in from a keyframe can take most of a second,
  and sound heard against a held picture would be a lip-sync error.
- **Buffering is a state**: after the open, a seek while playing, or the
  sound queue running dry, the clock is paused and nothing is written,
  until a picture is ready and half a second of sound is queued (twice
  the pace's floor, so leaving buffering never opens the far-behind door
  at once). A sink stalled for a second (it stops advancing with sound
  queued, or a start, an unmute or a regained device hears nothing
  within a start's hold bound) is a refused device instead: monotonic,
  retried every two seconds.
  Monotonic is for sound that cannot be had, never for sound that is late.
- **A resize while paused or ended converts the shown picture again**:
  each converted frame keeps its decoded picture until it is retired, so
  up to 12 decoded pictures are held where the queue alone would hold 8.
  Mid-play, frames already converted are drawn at their own size,
  centred, until the new size arrives (at most three).
- **GOT_END ends the kinds asked for**: asked for sound alone, the end of
  the sound leaves pictures listed, so each kind ends on its own.
- **A report is advancing only when the ear has moved.** The stream says
  PLAYING once its ring holds sound, but `heard` stays put until the
  sink's delay has passed (after a start, a seek's flush, a resume), so
  a PLAYING stream whose ear has not moved sends the clock nothing, and
  any other state is reported as stopped at once (a wait refuses it, so
  a start keeps its protection). An ear that stops while PLAYING (a sink
  no longer pulling, or pulling late) is the clock's to notice, by the
  report's trust (§ 4), whose state is STILL. gplay counts a still ear as
  a stalled sink (refused after a second), whatever its queues hold: at
  the end the sound the device still holds is still to come, so the clock
  goes monotonic only on a device that has said it stopped (starved).
  gplay derives the trust as the pump's packet (13.3 ms), its pass up to
  a tick late (10 ms), the output's write (20 ms) and the period
  main reads ahead (16.7 ms), plus 20 ms: 80 ms at 60 Hz.
  The readout's `ear:` and `report ages:` lines say how often it held,
  how often the bound fired, how long the start's first report took, and
  how old reports grew, so the trust can be checked against a P5 run.
  Buffering also waits until the sink's
  delay has been read, since the start's hold bound is made from it. A
  stream generation the output did not start (a sink attached or
  detached) loses what the device held, and the sound carries on from
  its queue, the output dropping what the clock has passed (D2, as built
  in V.5.1); when the output itself lets go of sound still in the device
  (a long pause, another sound starting while paused), the player seeks
  to where its clock stands (the output's `rewind`).

**In the library (V.5.2, `os64/media_player.h`).** Everything above but
the window is libmedia's player now, which gplay and yonder share: the
decoding and picture threads, the queues, the clock, the pace, the state
and its moves, its sound a voice of the program's output
(`os64/media_output.h`, V.5.1). The caller (gplay's main, a page's
window thread) asks at each display frame for the picture due at that
blank, reads the facts when told to look, and does nothing else: **the
sound never waits on the caller's thread.** What that rule moved:

- The state moves where it changes (S2): buffering's end, a stalled
  sink and the end are decided under the player's lock by whichever
  thread made them true, and again in each of the caller's calls.
- A caller that stops asking for frames for 100 ms (a covered window, a
  box out of view, a page busy laying out) has the clock's overdue
  pictures retired by the picture thread, on a timed wait while playing
  (S1); a picture owed after a seek or the open lands unshown, and at the
  end the last picture goes once its time is over, so the end comes with
  nobody watching. `/tests/gplaytest` steps 2, 7 (covered and muted) and
  8 (covered, no sound) are the proof.
- The media is opened on the decoding thread, never the caller's (a
  probe may read the network); the player is OPENING until it has
  probed.
- The player frees itself: close returns at once, and `finished` says
  when the source will never be read again.
- When the output lets go of the player's sound still in the device (a
  long pause, another sound starting while paused), the decoding thread
  seeks to where the clock stands, so the sound is queued again.

gplay is the window, the letterbox, the keys, the title and the readout.

## 4. The clock

An element's time comes from the audio position today, and a file with no
sound is refused. Video needs a clock that does not depend on sound.

| The element | Its clock | Sound decoded? | Holds a mixer voice? |
|---|---|---|---|
| has sound, audible | the heard position (as today) | yes | yes |
| has sound, muted | the monotonic clock (`micros()`), anchored at play | **GAP** | no |
| has sound, volume 0, not muted | the heard position: a silent voice, still mixed | yes | yes |
| has no sound track | the monotonic clock | n/a | no |
| sound refused (`/dev/audio` busy) | as today: waits | | |

Moving between rows (mute, unmute) must not jump the time: the new clock
is anchored at the old one's reading.

**The clock as built** (`os64/media_clock.h`, a pure core;
`tools/test_media_cores_host.sh` and its mutants). It answers one
question, the media frame showing at wall time `t`, and the player asks
it for the blank its publish will be shown at. Its sources: HEARD (the
player reports "frame H reached the EAR at wall Th", the sink's delay
already taken off; the clock does no delay arithmetic) and MONOTONIC.
A reading never moves backward and never steps, but for a seek and the
bound's one step forward (below): it rises with `t`, an event leaves it
where it was at the event's moment, and where the two views disagree the DRIFT is worked off by running a
twentieth fast or slow (50 ms in a second) down to the last 10 ms, which
go at a 480th (4.8 s). The P5 set the second rate: the headphones'
reports scatter by up to a tick (each is stamped at the sink's pull,
which waits for kusb's pass), and at a twentieth the reading swung with
them enough to leave a hundred pictures a minute unshown at 1080p60,
against six muted; a model of the scatter gave 114 and 5, and 6 to 10
with the near rate. A reading taken ahead, for a blank or a conversion's reach past the
next event, is a forecast that event may revise down (a starve freezes
at its own moment); a picture taken on it is simply gone.

| State | Event | The reading | Next |
|---|---|---|---|
| paused | play, heard source | holds where it was | **starting** |
| paused | play, monotonic | runs at real time from where it was | running |
| any running | pause | frozen at its reading then | paused |
| any | seek to `m` | `m` exactly; generation + 1, reports forgotten | paused stays paused; else starting (heard) or running |
| **starting** | (the hold's bound passes, no advancing report) | runs at real time from where it held, so a silent device never freezes the picture | starting, late (counted at its end) |
| **starting** | advancing report, current generation | unchanged; the difference from the ear becomes drift, the bound applied (rows below) | following |
| starting, awaiting | report not advancing, or of sound heard before the wait began | unchanged (refused) | same |
| running | source → heard (unmute) | unchanged, and keeps running: a picture already moving does not stop | **awaiting** |
| awaiting | advancing report | unchanged; the difference becomes drift, the bound applied | following |
| **following** | (reading) | the ear's view (`H` + the wall time since `Th`) plus the drift, worked toward 0 | |
| following | (the last report older than its trust, still_us past `Th`) | holds where that left it, drift and all: the ear is taken as stopped; the state reads **still** (a device that stopped says so at once, and is starved) | following |
| following | advancing report, the reading within the bound of the new view | unchanged at that moment; drift recomputed against the new report | following |
| following | advancing report, the reading more than the bound AHEAD | holds until the ear is within the bound, then the slew (counted once a hold) | following |
| following | advancing report, the reading more than the bound BEHIND | steps forward to the bound behind (the pace drops what it passes; counted) | following |
| following | report not advancing (the sound ran dry, or was held) | frozen, drift and all: the sound is the master | **starved** |
| starved | advancing report | carries on from where it froze; the difference becomes drift, the bound applied | following |
| starved | source → monotonic | carries on from where it froze, at real time | running |
| any heard | source → monotonic (mute, sound refused or ended) | unchanged | running |
| paused | source → either | unchanged; takes effect at play | paused |
| any | report from an earlier generation | refused, counted | same |

**THE BOUND** (`OS64_MEDIA_CLOCK_BOUND_US`, 100 ms, about where lip sync
becomes visible): following, the reading is never more than the bound
from the ear's view at a report, whatever the sound does. The slew
bounds how fast drift is corrected; this bounds how much there can be.
The P5's round 2 is why: its A2DP pump slipped 211 times in a burst, most
gaps under the old 200 ms still-ear rule, so the clock ran through each
and slewed back at 50 ms a second, and they came faster than that: a
maximised run's picture ended 9 s ahead of its sound. Fable's ruling:
hold a still ear sooner (THE TRUST, `os64_media_clock_still`, the
player's own arithmetic, `OS64_MEDIA_CLOCK_STILL_US` = 80 ms by
default), and bound the error. Between reports a gap can still carry the
reading the trust past where the ear actually stopped, so inside a hold
it is ahead by at most the bound plus the trust, shrinking at real time.
`tools/test_media_cores_host.c`'s storm (a 150 ms gap every 3 s for a
minute, then every second for a minute, reports scattered as the pump's
are): never more than 124 ms ahead of the true ear, the picture's end
within a millisecond of the sound's, every gap held through; the bound
fired 68 times, in the once-a-second minute, where each gap leaves 80 ms
and the slew takes 50 a second. **In a dense storm the bound is the
mechanism, not a guard:** with gaps closer than about 1.6 s it fires at
nearly every one, so a nonzero `held ahead` count is the clock doing its
job, not a fault. Below that density it does not fire.

**The hold's bound is the player's** (`os64_media_clock_hold`): the first
advancing report cannot come before the sink's delay has passed, so the
bound is the larger of a quarter second and that delay plus 150 ms. The
headphones' delay is about 150 ms and unmeasured, which a fixed quarter
second would cut too close.

**A starved hold lasts as long as the clock is told**: a fresh advancing
report or the monotonic source ends it, and the PLAYER owns the rule
that it does not last for ever (V4 answers it). Inside, a media time is
counted in 2500ths of a frame, so a microsecond is exactly 120 units and
the slew exactly 6: a reading's rise over any microsecond is 114 to 126
units, never less, where whole frames could round it back by one. The
playback rate is one, in one place (`media_clock.c`'s `run`; DEBTS).

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
   that size (the window canvas's XRGB8888 is libswscale's `bgra` in
   memory, its X byte written 0xFF; `bgr0` is the same bytes with that
   byte undefined).
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

**As built (V.5.3).** A `<video>`'s session is a picture session over
libmedia's player (`apps/yonder/media.c`): an opener thread makes the
bytes ready as a sound session's decoder does (`source_prepare`, one
function for both), then the player, whose decoding thread reads them
through `store_read` and `store_seek`; the window drives the player.
`apps/yonder/yonder.c`'s videos section is the rest:

1. `videos_turn` reads each shown video's facts after the media turn: the
   picture's size once known sets the box's natural size
   (`replaced_size`) and lays the page out once; whether it plays.
2. `videos_next` makes a video that plays (or whose facts moved while
   paused) and can be seen a customer of the frame planner;
   `videos_tick`, in each open frame, asks the player for the picture at
   the answer's blank plus one period and marks the box's visible part
   when it changed, leaving the kept drawings' stamp alone (no group
   holding a video is kept).
3. The paint reaches the box through the painter's retained-pixels verb
   (`paint.h` `canvas`, which names both), and `pane_video` draws the
   latest picture fitted by `yonder_object_rect` (object-fit and
   object-position, as an image's), the rest of the box the element's
   own background, nothing before the first picture. The fitted size is
   told to the player there, where it is known; until a paint tells one,
   the player converts at the picture's own size (held to 1920 a side),
   so a video never painted (below the fold, hidden, not in the document,
   past the registry's 32) still has pictures to be ready with, plays and
   ends.
4. **The test that picks the path:** the pane draws straight to the
   window (one target, not a group's sheets: no opacity, transform,
   filter, mask or blend above the box) and the picture was converted at
   the fitted size: the rows are copied (`video_plain`). Otherwise it is
   drawn as a picture is, scaled (`video_general`). yonder's `d` reading
   prints both, with the frames asked and the new pictures. A
   border-radius does not take a video off the plain path: yonder does
   not round a replaced box's content.
5. **Not seen** (the window covered, the box hidden or scrolled away, a
   page not shown): no frame is asked, so the player retires what the
   clock passes and the sound plays on (its S1). The pictures are still
   converted (V7's saving, DEBTS).

**What is not shown is not converted.** Covered window, scrolled out of
view, a page that is not the shown one: the picture thread parks. **GAP:**
whether the decoder also stops decoding pictures then (it must resume at a
keyframe, so the picture returns late) or keeps going (a steady cost for
nothing seen). Lean: keep decoding in this arc, book the saving.

**The late policy as built** (`os64/media_pace.h`, a pure core beside
the clock, reading `os64/media_pictures.h`'s queues: bounded by count,
in the order pushed, a generation per seek so a picture made before a
seek is refused after it, and an end; same suite and mutants). Three
answers:

- **At each paint**: with `C` the clock's reading for the paint's blank,
  SHOW the latest ready picture due (`at <= C`) and DROP every ready one
  before it, unshown and counted: a late picture is never shown late.
  Nothing due: the screen keeps what it has. Nothing ready and the shown
  picture past its end (its duration, else a period): STARVED, counted.
- **Before each conversion**: SKIP the front decoded picture, counted,
  when the one after it is due at the blank this conversion could reach,
  which is the first blank at or after now plus the last conversion's
  cost, read on the clock. The successor decides, not the duration, which
  a file may not state; with no successor decoded it is converted.
- **The producers' state**, judged once a paint from the decoder's LEAD
  (the newest decoded picture's time minus `C`), against the decoded
  queue's DEPTH in periods:

| State | Condition | Action on entry | Next |
|---|---|---|---|
| on time | lead below 3/8 of the depth, for 125 ms | leave out the pictures nothing depends on (skip UNUSED) | decoding late |
| decoding late | lead 3/4 of the depth or more, for 1 s | decode every picture (skip NONE) | on time |
| on time, decoding late | lead more than 30 periods behind, or the sound queue under the player's floor (its door armed), at once | a resync asked: drop to the next keyframe, the sound running on | far behind |
| far behind | the first picture after the resync is decoded | (UNUSED stays) | decoding late |
| any | a seek, or a new media | skip NONE; a resync not yet taken withdrawn; the counters kept | on time |

The gap between the doors and the out door's long hold keep a burst of
cheap pictures after a keyframe from flapping it. Every hold is WALL
time, so it means the same at 60 Hz and 144, and a covered window, which
paints nothing, holds nothing. **No verdict without something to
judge:** the lead also falls to nothing at the end of the media, while
buffering after a seek, and while paused, so the player says `judging`
false then, no door moves, no hold runs, and no paint counts as starved.
The sound's floor is the twin of the 30 periods because the sound is
what is protected; the player passes 0 when there is none to protect.
Its door fires once and re-arms only when the sound has been back at
its floor: a shortfall a resync did not cure is not the picture
decoder's, and another would only cost the pictures again. A resync is
ASKED by the pace and TAKEN by the decoding thread under the player's
lock, so a seek between the two withdraws it.
**What a resync costs:** the screen holds its last picture until the next
keyframe, commonly two to five seconds away on the web. That is the
design, not a defect, and `far_behind_us` with the entries counted says
how often V4 sees it. The constants (3/8, 3/4, 125 ms, 1 s, 30 periods)
are to tune on the P5.

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

**As built (V.5.3).** The element's table is VIDEO_V5_DESIGN.md § 3,
with Fable's answers, in libdom (`media.c`, `video.c`) over the host's
facts, which gained the picture (`picture`, `width`, `height`) and
`running`:

- `resize` is queued between `durationchange` and `loadedmetadata`, and
  again whenever the size changes; `videoWidth` and `videoHeight` are 0
  until metadata and again at a new load.
- `timeupdate` follows the clock while it runs, sounding or not.
- The host reports readyState from the player: its queues' readiness
  while paused; at least FUTURE while it plays (queues that drain and
  refill as it plays are no wait), and ENOUGH held once reached until it
  buffers; a buffering player is CURRENT, which is libdom's `waiting`. A
  seek leaves readyState as it was until it lands.
- `loop` is a seek to the start inside the host, playing on, with no
  `ended` and no seek events (as a sound session's decoder loops).
- Muted is told to the host apart from the gain: the voice goes and the
  clock is monotonic; a volume of 0 unmuted is a silent voice.
- The device refused: a `<video>`'s player plays on, unheard, and retries.
- `/tests/pages/video.html`, `video-events.html` (the table's events in
  order, by a script), `video-fit.html`, `video-two.html`,
  `video-overlay.html` and `video-silent.html` are the pages.

## 9. Full screen (this arc's last slice, V8)

There is no full-screen window in os64: no client call to drop the
decorations or cover the screen after creation, and no `requestFullscreen`
in libdom. It needs a small window-manager call plus the DOM's Fullscreen
API. Ruled into THIS arc, as its last slice (Rulings 4): a video player
without it is not one, and 2560x1440 full screen is the worst case
everything above was sized for.

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
