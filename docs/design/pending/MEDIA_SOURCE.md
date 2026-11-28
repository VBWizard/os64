# Media Source Extensions: a video fed in pieces

Fable, 2026-11-27. The design for the arc VIDEO.md Rulings 1 put off
("files first, streams a later arc"). `MSE_TRACKER.csv` beside this file
is its state: one row per slice, and a slice that turns out to need more
than one PR is broken into tasks there (Chris's ruling 4).

## 0. What it is, and what v1 is

A player that streams does not give the browser a file. Its script
fetches the video in pieces and hands them over through
`SourceBuffer.appendBuffer`: one stream of sound and one of picture, each
an init segment (what the stream is) followed by media segments (a run of
coded frames with their times), cut wherever the network cut them. The
browser parses them, keeps the coded frames ordered by time, and plays
from that store; the script may change quality (a new init segment in the
middle of a stream), seek, and remove what has been played.

**Chris's rulings (2026-11-26):**

1. v1 is H.264 and AAC in fragmented MP4. Nothing else answers yes.
2. No DRM.
3. A fixture proves the pipeline on the P5 before the browser work, fed
   through the same entry points `appendBuffer` will call, in the same
   pattern: two streams, init then media segments, pieces cut at arbitrary
   byte counts, a quality switch, a seek, eviction of what was played.
4. A tracker, one row per slice at first.

**Not in v1**, each a row when something asks: VP9, AV1, Opus and WebM;
`MediaSource` in a worker; `ManagedMediaSource`; text tracks; more than
one track of a kind in a SourceBuffer; encrypted media.

## 1. The question the design hung on (M0, answered)

Who turns appended bytes into coded frames: FFmpeg's `mov` demuxer fed
from memory, or a parser of our own. A host spike built both over the
pinned FFmpeg (the report and its programs are kept outside the tree).

- **FFmpeg's demuxer does not resume.** Opened on an init segment and one
  media segment it gives that segment's packets and nothing for any later
  one, whatever the read answers when it comes up short. It can be made to
  work only as a fresh demuxer per complete media segment behind a framer
  of our own: no frame until a whole segment is in, five copies a byte, an
  open and a close under libmedia's one FFmpeg lock on every append, and
  two of the byte stream format's rules wrong (a fragment with no decode
  time is accepted with invented times; a second init segment of another
  kind is accepted).
- **Our own parser** gives each frame when its last byte arrives, under
  every cut down to one byte at a time, allocates nothing per frame, and
  its frames decode bit for bit as the host's ffmpeg decodes the same
  files: straight through, across a quality switch, and appended out of
  order.

**The parser is ours.** FFmpeg stays what libmedia already uses it for
here: the decoders. Nothing of libavformat is on the append path.

## 2. The parts

```
 page script ── MediaSource / SourceBuffer (libdom)
                      │  the source provider (libdom's seam, like media's)
                 yonder: a fed session per attached element
                      │
              libmedia: the fed media
        ┌─────────────┼──────────────────────┐
   the fragment     the track buffers     the player
   parser (pure)    (pure), one a track   (its decode thread pulls coded
                                           frames by time; decoders are
                                           avcodec's, as today)
```

Two pure cores carry every rule and are host-tested beside models, with
mutants, like the picture queue, the clock and the pace before them.

## 3. The fragment parser (pure)

Built in MS.1.1: `os64/media_fragment.h` and `userland/libmedia/media_fragment.c`; the header states every rule below in the code's own terms.

**In:** bytes, in whatever pieces they arrive. **Out,** in arrival order:
an init segment (its tracks, each track's kind, its sample entries with
their codec configurations, its timescale and edit offset, and a
GENERATION that rises with every init segment), coded frames, "more bytes
needed", or an error naming the rule broken. An error stays until a reset
(the spec's reset parser state), which keeps the init segment, so the
media segments after an `abort()` still parse.

**A coded frame carries:** its track and kind; the init generation that
decodes it, and which of its track's sample entries (a track may carry
several); decode time and presentation time in the track's timescale,
WITH THE EDIT LIST APPLIED (so they may be negative: an encoder's
B-frame delay and AAC's priming frame are both written as an edit, and
without it picture and sound sit a frame or two apart); its duration;
whether it is a random access point (neither `sample_is_non_sync_sample`
nor depends-on-others in its flags); and where its bytes are.

**The edit list.** The first edit with a media time places the media: its
media time is subtracted from every time, and any empty edits before it
(in the movie's timescale, from `mvhd`) are added back. Later edits and a
rate other than one are ignored; the byte stream format asks only for a
single edit at rate one. The decode check proves the result equals FFmpeg's
presentation times, frame for frame.

**Boxes read:** `ftyp`, `moov` (`mvhd`, `trak`/`tkhd`/`edts`/`elst`,
`mdia`/`mdhd`/`hdlr`, `minf`/`stbl`/`stsd` with `avc1` or `avc3`/`avcC`
and `mp4a`/`esds`, `mvex`/`trex`), `moof` (`traf`/`tfhd`/`tfdt`/`trun`),
`mdat`, and everything else (`styp`, `sidx`, `free`, `emsg`, `mfhd`,
`sdtp` ...) skipped by length, at the top level and inside a box it reads.
A track of another kind (text, metadata) is read for its id and its trex
default size, and its trafs for their length (a chained traf after one
begins where its data ends); its frames are not handed out.

**Rules it enforces** (the byte stream format's; each has a host case and
a mutant): a partial box waits; a top-level box whose size is zero or
smaller than its header is an error (there a size of zero means "to the
end of the file", which a byte stream has not got; inside a box it means
to the end of the container, as ISO defines); a media segment before any init
segment is an error; an init segment has `mvex` and no samples of its own
(`stts`, `stsc`, `stco`, `co64`, `stsz` all empty); a later init segment
keeps the number of picture and sound tracks (MSE counts text tracks too,
which v1 does not hand out); at most one track of each kind
(v1), with ids unique across every track; sample entries v1 decodes
(`avc1`, `avc3`; `mp4a` carrying AAC Main, LC, LTP, HE-AAC or HE-AAC v2,
by the AudioSpecificConfig's object type, since MPEG-4 Audio's object type
indication also covers MP3, ALS and the rest), so an encrypted entry is
refused; every `traf` has a `tfdt`, names no base data offset, and its
first `trun` has a data offset (movie fragment relative addressing); a
sample description index names an entry; a fragment marked
duration-is-empty names no frames; a `moof` has a `traf`; every byte a
`trun` names lies inside one `mdat` after its `moof` (checked as soon as
it can be: a frame placed before its `moof` when the `moof` is read, one
before its `mdat` when that `mdat` begins, one running past its `mdat`
when the `mdat` ends, one in no `mdat` when the next `moof` or `moov`
begins; a zero-size frame at an `mdat`'s very end is inside it); a time that does not fit
in 63 bits is an error. Lengths and counts are read through a bounded
reader that answers zero and marks itself bad rather than read past a box,
so no field is used before it is checked against the box it sits in. The
held boxes and the mdats holding frames have caps (4 MB and 128 MB to
begin with, set by the caller), and a moof may name at most 65,536 frames.

**The sink may reset or destroy the parser** from inside a callback (a
track buffer that fails mid-append runs the append error algorithm, which
resets the parser): the append in progress ends there, the rest of its
bytes discarded as reset parser state discards the input buffer.

A base data offset is refused as the format requires. ffmpeg's plain
fragmented output (`frag_keyframe+empty_moov` without
`default_base_moof`) writes one, so it is refused; DASH, CMAF and the
streaming players' own output use default-base-is-moof.

**What a real stream needs that the spike lacked**, built in MS.1.1:
several `mdat`s after one `moof`; `stsd` with more than one entry, the
frame naming which; `avc3` (parameter sets in the frames: the parser does
not look inside frames, so whether they are there is the decoder's to
find); `tfhd`'s duration-is-empty; `trex` defaults under `tfhd`'s and the
`trun`'s own; and a `traf` without default-base-is-moof following another,
whose data begins where the previous `traf`'s ended (ISO 14496-12
§ 8.8.7). **That last is a choice, not the format's rule:** the byte stream
format counts as relative addressing only every `traf` with
default-base-is-moof or a single `traf` without it, so a strict reading
refuses a second chained `traf`. The parser accepts it, because the
addressing is unambiguous and ISO defines it; refusing it is one line.

**Memory.** Appended bytes are copied once, and nothing is moved as boxes
are consumed:
- a `moov` or `moof` into the parser's box buffer, reused (grown only when
  a box is larger than it); an accepted `moov`'s buffer becomes the init segment's by
  exchanging the two buffers, so its configurations stay good until the
  next init segment, without a copy;
- a box the parser does not read, and an `mdat` holding none of the
  `moof`'s frames, is counted past and never held;
- an `mdat` holding frames is read in place into a SLAB its frames point
  into, one allocation an `mdat`, none a frame (a `moof` whose trafs' data
  lies out of order borrows one scratch array to sort, once a segment).

**A slab's life is counted in holds.** The parser holds a slab while its
`mdat` fills and lets go when it ends, at a reset and at destroy; the track
buffer takes a hold for each frame it keeps and lets go when the frame is
evicted, and the last drop frees the slab. This is "handed over when the
mdat completes" made safe against a reset mid-`mdat`: a frame already
handed out keeps its slab, and a slab no frame was kept from is freed as
soon as its `mdat` ends (one whose `mdat` ends in an error, at the reset
or destroy that follows). The holds are not atomic; the fed media's one
lock covers them.

## 4. The track buffer (pure)

One per track. It is the spec's coded frame processing, as a table of
rules over the frames the parser gives:

| Rule | What it does |
|---|---|
| `timestampOffset` | added to every frame's times as it arrives |
| the append window | a frame outside `appendWindowStart`..`End` is dropped, and the next frames wait for a random access point. A sound frame before the window is still DECODED, its output discarded (preroll): AAC overlaps each frame's transform with the one before, and dropping the priming frame unread changed 291 of the next 563 frames in MS.1.1's decode check |
| need a random access point | after an init segment, a discontinuity or a dropped frame, frames are dropped until one |
| a discontinuity | a decode time that steps back, or forward by more than twice the last duration, starts a new coded frame group |
| overlap | a new frame removes the frames it overlaps in presentation time, and, when it is a random access point, the old frames that depended on what it replaced |
| order | kept by decode time within a coded frame group; found by presentation time |
| `buffered` | the ranges of presentation time held, small gaps (under a frame or two) joined, as other browsers join them |
| `remove(start, end)` | frames in the range go, and those after them up to the next random access point |
| `mode` | `segments` (times from the stream) and `sequence` (each group placed after the last), both in v1: the rule is two lines and a player library may ask for either |
| eviction | when an append would pass the QUOTA, frames well behind the play position go first, then those far ahead; if it still does not fit, the append fails with QuotaExceededError and nothing is taken |
| the group end and the highest end time | kept as the spec keeps them, for `duration` and `endOfStream` |

**The quota** is per SourceBuffer, in bytes of slabs held: 150 MB for
picture and 12 MB for sound to begin with (what another browser allows),
set in one place and reported, to be tuned from the P5's readings in M5.

**What the player asks of it:** the frame at or after a time; the next
frame in decode order; the random access point at or before a time; the
init generation of a frame; whether the play position is inside a
buffered range and how far that range runs ahead.

## 5. The fed media in libmedia

`os64/media_fed.h`: a media whose frames come from track buffers. The
player's decoding thread, which today pulls packets from a demuxer, pulls
coded frames from the track buffers by the play position; everything
after the decoder is the player as it is (the picture queue, the clock,
the pace, the show thread, the plane, the voice).

- **Decoders** are avcodec's, opened from the init segment's
  configuration (`avcC` as the decoder's extra data, the AAC configuration
  from `esds`). A frame of another generation is a decoder reopened with
  that generation's configuration, at that frame; the picture thread
  already takes a picture of any size. Opens and closes stay under
  libmedia's one FFmpeg lock, on the decoding thread, as today.
- **Starvation.** The play position leaving the buffered range holds the
  clock (the sound stops cleanly, the picture holds) and the element says
  `waiting`; frames arriving there start it again. A decoding thread that
  finds nothing parks on the session's bell; it never spins.
- **A seek** flushes the decoders and the queues, and waits for a random
  access point at or before the target; a seek into a gap waits for an
  append there (the element says `seeking` until then).
- **One lock a fed media**, held for a frame's insertion or lookup and
  never across a copy, a decode or a call out. The appending side and the
  decoding thread are its two takers.
- **`readyState`** follows the buffered ranges: nothing before both
  tracks' init segments; current data when the play position has a frame;
  future data and enough data by how far the range runs ahead (the
  thresholds in one place, reported).
- **Nothing that runs per frame allocates**: frame records live in
  blocks a track buffer grows by doubling; a slab is one allocation a
  media segment; the decoder's and the picture's buffers are the player's
  existing pools.

**Where an append is parsed.** On the appending thread, inside the
append's own task: the copy and the box walk cost about a millisecond a
megabyte, and it keeps the DOM's state on one thread. `appendBuffer`
copies the bytes and returns; the task that follows parses them, hands
frames to the track buffer and fires `updateend`. If the P5's readings in
M4 show that task holding a busy page, the parse moves to the session's
own thread behind the same entry points; the fixture already feeds from a
thread of its own, so both shapes are proven by M2.

## 6. The fixture (M2), before any browser code

`/tests/msetest`, and `gplay --fed <directory>` for eyes: sound and
picture as two streams from local fragmented files, an init segment then
media segments, appended in pieces of arbitrary sizes on a schedule a
streaming player would keep (a few seconds ahead of the play position),
with a quality switch between two sizes, a seek forward into nothing
buffered and back into what is, and eviction as it goes. Its readout is
gplay's plus the track buffers' (ranges, bytes held against the quota,
frames dropped by each rule, starvations, decoder reopens). It passes in
`testrun` muted and covered, like gplaytest, and on the P5 it is the
proof of 1080p60 fed in pieces, smooth, before a page is involved.

The live-player factor applies to every forecast here: what a stage costs
in a playing video on the P5 has measured about twice its benchmark.

## 7. The web's side (M3)

libdom: `MediaSource` (`readyState` closed, open, ended; `duration`;
`addSourceBuffer`, `removeSourceBuffer`, `endOfStream`,
`setLiveSeekableRange`, `clearLiveSeekableRange`, `isTypeSupported`,
`sourceBuffers`, `activeSourceBuffers`, and `sourceopen`, `sourceended`,
`sourceclose`), `SourceBuffer` (`appendBuffer`, `remove`, `abort`,
`changeType`, `updating`, `buffered`, `mode`, `timestampOffset`,
`appendWindowStart`, `appendWindowEnd`, and `updatestart`, `update`,
`updateend`, `error`, `abort`), `SourceBufferList`,
`URL.createObjectURL(MediaSource)` and the element's attach and detach
through `src`. The state machines are tables in the slice's design state
before code, with every transition's events in order.

- **`isTypeSupported` is honest.** Yes only for `video/mp4` with an
  `avc1` or `avc3` codec string whose profile and level the decoder takes,
  and `audio/mp4` with `mp4a.40.2` (other AAC object types only if the
  slice proves the built decoder plays them). Players pair each sane
  question with an absurd one to catch a browser that says yes to
  everything.
- **A provider seam** to yonder, in the media provider's shape: libdom
  keeps the objects, states, events and promises; yonder owns the fed
  session and its threads.
- **An error** in an append is the spec's: the parser is reset, `error`
  then `updateend` fire, and the source ends with a decode error.
- **Test pages** in `tools/pages`, fed by script from local fragmented
  files through `fetch` and by ranges, with no site's bytes.

## 8. Slices

| | Slice | Proves | Review |
|---|---|---|---|
| M0 | host spike | the parser is ours (done, § 1) | Fable |
| M1 | the two pure cores, their models, mutants and a fuzzer | the rules, on the host | a cold reader, then Codex |
| M2 | libmedia's fed media, `msetest`, `gplay --fed`, a P5 run | 1080p60 fed in pieces with a switch, a seek and eviction | a cold reader, then Codex |
| M3 | libdom's API, yonder's provider, pages | a page feeding a video | a cold reader, then Codex |
| M4 | real players: honest answers, playback quality, replies that stream as they arrive | the first P5 run on a real page | Codex |
| M5 | the quota and eviction under a long watch, `changeType`, the live range | what an evening of watching costs | Codex |

M1 is likely two PRs (the parser, then the track buffer) and its tracker
row is split when the builder says so.

## 9. Known before starting

- **Replies that stream.** A player reads its fetches as they arrive
  (`Response.body`); DEBTS has "Whether the player's streamed requests
  stream". M4 needs it true.
- **A page's thread held by its own script** starves an append task as it
  starves everything else on that thread; the plane keeps the picture
  moving only while frames are buffered ahead. The layout arc is the cure
  for the cause.
- **A shrunk picture's width.** An odd destination width was four times
  the cost until 2026-11-27 (libmedia's scaler converts one narrower
  now); a fed video in a page's box meets the scaler as a file's does.
- **H.264 only** means a streaming site's top size is whatever it offers
  in H.264, commonly 1080p.
