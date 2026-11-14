# AUDIO_YONDER.md — a page's sound: `<audio>`, the media document, and FFmpeg

*Opus, 2026-11-14. Status: DESIGN, for Chris. Nothing here is built.*

*Chris's goal for this arc (2026-11-14): yonder plays a page's sound,
generally — WAV and MP3 first, because MP3 is what an ordinary page links
and embeds, then the gaps as pages show them to us. The mid-term goal
behind it is os64 as his daily driver, and two sites are named there:
YouTube and a streaming radio player. Those want streaming (Media Source
Extensions), MP4 and WebM, AAC and Opus, and pictures; this arc builds
none of that, but it must not make any of it harder. Where a choice below
exists for that reason, it says so.*

*Two earlier documents meet this one.* `AUDIO_SEAM.md` is the kernel side
it writes to (`/dev/audio`, `/dev/audiopos`, `/dev/audioctl`), and was
built with this consumer in mind: its § The write gives yonder "a thread
of its own that does blocking writes", and its position record counts in
the program's own frames so that a player's clock is an index into what
it wrote. `BLUETOOTH_AUDIO.md` § Players put Opus and AAC before MP3
because video sites serve them; Chris's goal above reorders that, since
the first goal is the ordinary page, not the video site.

## What the web asks for

A page makes sound three ways, and all three run on one machinery:

1. **`<audio controls src="song.mp3">`.** The browser draws its own player
   in the page: play/pause, elapsed and total time, a seek bar, volume.
   Every browser draws its own; ours is drawn by yonder in os64's look, in
   Chrome's box (300 by 54 CSS pixels), because pages size their layout
   around the box and never around the look.
2. **`<audio>` without `controls`, driven by script.** `new Audio(url)`,
   `play()`, `pause()`, `currentTime`, `volume`, the events. Custom
   players, sound effects, games.
3. **A link straight to `song.mp3`.** Other browsers show a nearly empty
   page with one player on it (the "media document"). yonder already does
   exactly this for a picture (`image_page`, yonder.c); a sound file gets
   the same trick. Today it is offered as a download instead.

The machinery is HTML's `HTMLMediaElement`, the base `<video>` shares.
Building it as the base, with `HTMLAudioElement` as a thin child, is the
first thing this design does for video.

## The shape

```
   page (libdom, window thread)          yonder (window thread)
   HTMLMediaElement ── provider ──────▶  media element record ── controls widget
          ▲                                  │        ▲
          │ events, at task boundaries       │ ask    │ state (BELL_MEDIA)
          │                                  ▼        │
          └──────────────────────────  libmedia: one DECODER THREAD per element
                                             │  bytes ← yonder's fetch job (Range)
                                             │  FFmpeg (libav.so): demux, decode
                                             │  libsound: resample to 48 kHz
                                             ▼
                                        PCM queue (per element, bounded)
                                             │
                                        THE AUDIO THREAD (one per yonder)
                                             │  mix playing elements, volume
                                             ▼
                                        /dev/audio  (blocking writes)
                                        /dev/audiopos (the clock)
```

### Three new pieces of code

| Piece | What it is | Proof |
|---|---|---|
| `userland/libav/` → `/lib/libav.so` | FFmpeg, ported: libavutil, libavcodec, libavformat, built down to the decoders and readers we name. Its own library, never linked into anything (§ FFmpeg) | Host build of the same configuration; `tools/test_av_audit.py` (licence, exports, imports, the configuration) |
| `userland/libmedia/` → `/lib/libmedia.so` | The pipeline, independent of yonder: a SOURCE (where bytes come from: a callback that reads and seeks), a DECODER over libav, the resampler, and the bounded PCM queue; plus the MIXER. No window, no DOM | Host: decode fixtures against FFmpeg's own CLI output, the mixer and the clock against a model, under ASan/UBSan |
| yonder | The element's record, the provider libdom polls, the audio thread, the controls widget, the media document | QEMU: pages that play, heard at `/dev/audiotap` |

libmedia is a library and not a file of yonder's for two reasons: the
pipeline is the part host tests can drive whole, and `<video>` grows a
picture path inside it rather than inside a program already as large as
yonder. It also
means `play song.mp3` costs a few lines (§ For Chris 5).

## FFmpeg

**Why FFmpeg and not one small port per format** (Chris, 2026-11-14,
"start small with FFmpeg"): every later gap — AAC, Opus, Vorbis, FLAC,
MP4, WebM, Ogg, and the picture decoders — becomes a configure switch and
a rebuild instead of a port with its own pin, review, shims and audit.
The first port pays for teaching FFmpeg to live on libos64; nothing after
it does.

**The licence is the library's shape.** FFmpeg's default build is LGPL
2.1 or later. The LGPL lets a program use the library as long as the user
can put their own build of it in its place, and a separate shared object
is the plain way to give that: os64's loader serves the file at
`/lib/libav.so`, and replacing the file replaces it for every program
started after. So:

- `libav.so` is its own library, linked by `libmedia.so` alone. Never
  static, never merged into libos64 or libmedia.
- Its source and our patches live in the tree (`upstream/`, `patches/`,
  `manifest.json`), which is the "source offered" half.
- `--enable-gpl` and `--enable-nonfree` are never passed, and the audit
  FAILS if the generated configuration carries either (`CONFIG_GPL`,
  `CONFIG_NONFREE`). Every decoder this arc and the video arc want is LGPL.
- `THIRD_PARTY.md` gets its row, naming the licence and the
  separate-library rule so nobody later "simplifies" it into a static link;
  the licence text goes to `/etc/licenses/` like the others.

MP3's patents expired in 2017; it carries no question of that kind.

**The first configuration** (`--disable-everything`, then by name):

| Kind | Enabled | Why |
|---|---|---|
| demuxer | `wav`, `mp3` | the two formats |
| decoder | `mp3float`, `pcm_s16le`, `pcm_s24le`, `pcm_s32le`, `pcm_u8`, `pcm_f32le`, `pcm_alaw`, `pcm_mulaw` | the MP3 decoder and what a WAV file carries |
| parser | `mpegaudio` | MP3 frame sync |
| protocol | none | bytes come through our own read/seek callbacks (`AVIOContext`), never FFmpeg's networking |
| everything else | off | `--disable-network --disable-programs --disable-doc --disable-swscale --disable-swresample --disable-avdevice --disable-avfilter` |

The configuration is generated ON THE HOST by FFmpeg's own `configure`
(cross, `--target-os=none`), and the generated `config.h` and
`config_components.h` are committed beside the manifest, checked by the
audit. os64 cannot run `configure`, and a committed configuration is a
reviewable one.

**Three switches that are off now and must be able to come on** (video's
door):

- **Threads** (`--disable-pthreads`). Sound decodes far faster than it
  plays on one thread. H.264 at a useful size will want FFmpeg's frame and
  slice threads, which means a pthread shim (mutex, condition, once)
  built on `os64_thread`, the lock and bells. Not built now; nothing here
  assumes it will never be.
- **Assembly** (`--disable-asm`). FFmpeg's hand-written x86 needs `nasm`,
  and much of the fast part of it assumes AVX, which os64 leaves off.
  Irrelevant to sound; a measurement for the video arc.
- **Inline assembly** (`--disable-inline-asm`). FFmpeg writes it in AT&T
  syntax; userland compiles `-masm=intel`. libav will compile with its own
  flags (as the kernel's SBC port does), so turning this on later is a
  flag, not a rewrite.

**What libos64 must supply.** There is no libc, and FFmpeg expects one.
The shims live in `libav/port/compat/`, in the pattern libwebp, libjpeg
and QuickJS already use: `stdlib.h`, `string.h`, `stdio.h` (an opaque
`FILE` and `snprintf`; logging routed to a callback that stays silent
unless asked), `errno.h` (FFmpeg's own `AVERROR` codes travel through it),
`inttypes.h`, `limits.h`, `math.h` from libmath, `time.h` (only what
libavutil asks), and aligned allocation (FFmpeg wants 64-byte alignment;
`os64_malloc` gives 16, so the shim over-allocates and stores the offset). The exact list is what the
compiler says on the first build; this is the expected shape, and the
port's README will hold the real inventory.

**Pin**: the newest FFmpeg release on the day of the import, its tarball's
SHA-256 in `manifest.json`, every shipped file hashed. A release, not a
git snapshot: the monthly scan then reads a release's security notes.

**Size**: the library slot is 64 MiB (`app_bases.py`). This configuration
is a megabyte or two; every audio and video decoder the daily-driver goal
names is still well inside the slot.

## The pipeline (libmedia)

```c
// os64/media.h (sketch; names will settle in the code)
typedef struct media_source {
    void *ctx;
    // Read up to `cap` bytes at the current position. Blocks (it runs on
    // the decoder thread). Returns bytes, 0 at the end, negative on error.
    int64_t (*read)(void *ctx, uint8_t *out, size_t cap);
    // Move to `offset`; false when the source cannot (no Range support).
    bool (*seek)(void *ctx, uint64_t offset);
    int64_t size;            // -1 when unknown
} media_source_t;

media_decoder_t *media_open(const media_source_t *src, media_info_t *info);  // probe: format, duration, rate, channels
int media_decode(media_decoder_t *d, int16_t *out, uint32_t frames);        // 48 kHz s16 stereo, resampled
bool media_seek(media_decoder_t *d, uint64_t frame);                         // to a frame of the media's own timeline
void media_close(media_decoder_t *d);
```

**Everything leaves libmedia in the stream's format** (s16le, two
channels, 48 kHz) and is counted in the MEDIA's frames converted to 48
kHz, so the element's clock and the audio thread speak one unit. Mono is
doubled, more than two channels (a multichannel WAV) are folded to two
in libmedia with the ITU downmix weights, and a rate that is not 48 kHz goes through `libsound`'s
resampler (the same one `play` uses).

**The decoder thread** (one per element that is loading or playing):
reads the source, decodes, resamples, and pushes into the element's PCM
queue, which holds two seconds and makes the thread wait when full. It
can block on the network as long as it likes: it is not the window, and
it is not the audio thread. A seek, a new `src`, or the element going
away tells it to stop and is answered at its next turn. Video will add a
picture queue beside the PCM queue, fed by the same thread or a sibling.

**Where bytes come from.** The source is yonder's, not libmedia's: a read
through **yonder's fetch job** (`fetch_job.h`), streamed, so a media
request has the same cookies, cache, HTTP/2 connection reuse, referrer
and CORS rules as every other request a page makes. A media request is
`no-cors` unless the element says `crossorigin`, as the HTML standard
says. A seek past what has arrived restarts the request with `Range:
bytes=N-`; a server that ignores Range (answers 200) makes the source
unseekable, which the element reports as `seekable` empty, and the
controls disable the bar — other browsers do the same. The body has no
cap: a two-hour podcast is a stream, never a whole buffer, and the PCM
queue's bound is what bounds memory.

**The format is FFmpeg's to decide, the type is a hint.** Servers send
`application/octet-stream` for MP3s often enough that a media load
probes the bytes rather than trusting the header — this is what the
standard's media sniffing allows. A response that is plainly not media
(an HTML error page) fails the load with `MEDIA_ERR_SRC_NOT_SUPPORTED`.

## The audio thread and the mixer

**One thread per yonder process owns `/dev/audio`.** It opens the stream
when the first element starts playing and closes it when none has played
for a few seconds (the close lets the queued tail play out, AUDIO_SEAM.md
§ Close never waits). It writes with blocking writes, which is the clock
AUDIO_SEAM.md built: the write parks while the ring is full.

**Several elements in one window play at once**, as in every other
browser (a game's sound effects over its music; two players on one page
both started). The thread MIXES: each playing element's queue, times that
element's `volume` (and zero when `muted`), summed and clamped to 16 bits.
That is a mixer in yonder, not in the kernel, and it is enough: os64 has
one stream (AUDIO_SEAM.md § What A.1 leaves out), and inside one program
a mix is twenty lines and has one owner. The mixer is in libmedia, pure,
host-tested against a model.

**Two yonder windows are two programs**, and the stream has one owner. The
second window's play waits (§ When the stream is busy). A kernel mixer is
the cure for that, booked in DEBTS since A.1; this arc does not need it.

**`/dev/volume` is the system's, an element's `volume` is the page's.**
The element's gain is applied in the mix; yonder never writes
`/dev/volume`.

**The clock.** Each element's `currentTime` comes from `/dev/audiopos`:
the mixer records, for each element, the stream frame at which that
element's next frame enters the mix, and its position is then

```
element_frame = start_frame_in_media + (heard - stream_frame_when_it_started)
```

extrapolated between readings exactly as AUDIO_SEAM.md § The position
says (only while `PLAYING`, never past `taken`). So the seek bar and
`currentTime` show what you HEAR, not how far the decoder has run ahead.
This is the second thing the design does for video: a picture is shown
when its time is `currentTime`, and `currentTime` is the ear's.

**Pause.** Pausing an element stops the mixer taking from its queue.
What is already in the ring (up to 200 ms) still plays, so when the
paused element was the ONLY one playing, the thread also writes `flush`
to `/dev/audioctl` — the pause is then immediate — and records the
element's position at the flush. (The stream is yonder's own while it
holds `/dev/audio`, so the flush cannot reach another program's sound.)
With others still playing, the paused element's last fifth of a second
plays out; the same tail other browsers have.

**Loop, end.** `loop` makes the decoder thread seek to zero at the end
and keep going; without it the element's queue runs dry, the mixer sees
the end, and the element fires `ended`.

## When the stream is busy

Another program (`play` in a terminal, another yonder window) holds
`/dev/audio`. The open answers `OS64_AUDIO_BUSY`.

**Lean** (§ For Chris 1): the element does not fail. It behaves as a
stream that is not yet ready: `play()` is accepted, the element fires
`waiting`, its time does not move, the controls say "Sound is busy:
another program is playing", and the audio thread tries the open again
twice a second until it is granted, when playback starts from where the
element stood. A page cannot tell this from a slow network, which is a
state every page already handles. Failing the element instead
(`MEDIA_ERR_...`) would leave a page's player broken for a reason that
goes away by itself.

## Autoplay

Other browsers refuse `play()` with `NotAllowedError` until the person
has interacted with the page (a click, a key), unless the element is
muted, and a media document plays at once because the person navigated
to it. yonder matches: sound a page starts by itself is the single most
complained-about thing a browser does. yonder tracks a pointer
activation for forms already (`s_pointer_activation`); the page's
STICKY activation (has the person ever interacted with this page) is the
fact play() needs, and is recorded on the page beside it.

## The element in libdom

**Interfaces**: `HTMLMediaElement` (the base), `HTMLAudioElement`,
`MediaError`, `TimeRanges`, the named constructor `Audio`. Installed
behind the Modern Web switch like the rest of the modern surface, in a
row of their own as `HTMLIFrameElement`'s is (`modern.c`).

**Prototypes**: `D_PROTO_MEDIA` and `D_PROTO_AUDIO`. The prototype table
has a wall of 64 and 59 are used; `D_PROTO_VIDEO` will need a sixth of
the five that remain after these two, so the video arc either fits in the
last three or widens the table. Recorded here so the day it bites is not
a surprise.

**The members**, by the standard's own grouping:

| Group | Members | This arc |
|---|---|---|
| error | `error` (`MediaError`: `code`, `message`, the four constants) | yes |
| network | `src`, `currentSrc`, `crossOrigin`, `networkState`, `preload`, `buffered`, `load()`, `canPlayType()` | yes |
| ready | `readyState`, `seeking` | yes |
| playback | `currentTime`, `fastSeek()`, `duration`, `paused`, `played`, `seekable`, `ended`, `autoplay`, `loop`, `play()`, `pause()` | yes |
| rate | `defaultPlaybackRate`, `playbackRate`, `preservesPitch` | stored and reported; a rate other than 1 is a DEBTS row (it plays at 1) |
| controls | `controls`, `volume`, `muted`, `defaultMuted` | yes |
| tracks | `audioTracks`, `videoTracks`, `textTracks`, `addTextTrack()` | no: video arc |
| `<source>` children | resource selection over `<source src type>` | yes: the algorithm walks them and takes the first whose `type` `canPlayType` does not answer `""` |

`play()` returns a promise, settled later (the pattern `fetch()` uses);
a pause or a new load rejects the pending ones with `AbortError`, marked
handled so an unawaited `play()` is not reported as an unhandled
rejection (`webanimation.c`'s `mark_handled`).

**`canPlayType`** answers from the configuration, never from a list
written elsewhere: `audio/mpeg`, `audio/mp3`, `audio/wav`, `audio/wave`,
`audio/x-wav` → `"maybe"` (`"probably"` when a `codecs` parameter names
a codec we decode); everything else `""`. The day AAC is configured in,
`audio/mp4` and `audio/aac` answer without anyone editing yonder.

**The events**: `loadstart`, `progress`, `suspend`, `abort`, `error`,
`emptied`, `stalled`, `loadedmetadata`, `loadeddata`, `canplay`,
`canplaythrough`, `playing`, `waiting`, `seeking`, `seeked`, `ended`,
`durationchange`, `timeupdate` (every 250 ms while playing, as other
browsers do), `play`, `pause`, `ratechange`, `volumechange`. Twenty-two.
`addEventListener` hears any type today; the `on…` handler properties
come from a bit per event type in a 64-bit mask with 13 bits free. **The
mask is widened to 128 bits** in this arc — the alternative, `on…`
properties for some media events and not others, is a hole a page finds
the first time it writes `audio.onended = …`.

**How events reach the page**: the image path's shape. yonder registers a
media PROVIDER with libdom (as `os64_dom_set_images` does); the decoder
and audio threads post state changes to the element's record and ring
the window's doorbell (`BELL_MEDIA`, bit 7); a media turn on the window
thread folds them into the element's state and queues the events, which
fire at the next task boundary. No page script ever runs off the window
thread, and no thread but the window's touches the DOM.

## The element's states

HTML gives the element two numbers, and they are the state; nothing else
is stored beside them except `paused`, `seeking` and `ended`, which the
standard also names.

| `networkState` | Means |
|---|---|
| `NETWORK_EMPTY` (0) | No source chosen |
| `NETWORK_IDLE` (1) | A source chosen, not fetching now (done, or held by `preload`) |
| `NETWORK_LOADING` (2) | Fetching |
| `NETWORK_NO_SOURCE` (3) | No source it could play |

| `readyState` | Means here |
|---|---|
| `HAVE_NOTHING` (0) | Not probed yet |
| `HAVE_METADATA` (1) | Probed: duration, rate, channels known |
| `HAVE_CURRENT_DATA` (2) | Decoded frames for the current position exist |
| `HAVE_FUTURE_DATA` (3) | More than the current position is queued |
| `HAVE_ENOUGH_DATA` (4) | The PCM queue is full, or the whole file has arrived |

| Event | From | To | Fired |
|---|---|---|---|
| `src` set, `load()`, element inserted with a `src` | any | network LOADING (or IDLE if `preload=none`), ready NOTHING | `emptied` if it had a source, `loadstart`; pending `play()` promises rejected `AbortError` |
| probe succeeds | ready NOTHING | ready METADATA | `durationchange`, `loadedmetadata` |
| probe fails, or the response is not media | ready NOTHING | network NO_SOURCE | `error` (`MEDIA_ERR_SRC_NOT_SUPPORTED`) |
| first frames decoded | METADATA | CURRENT_DATA, then FUTURE/ENOUGH as the queue fills | `loadeddata`, `canplay`, `canplaythrough` |
| `play()`, allowed | paused | playing | `play`; `playing` once frames are flowing, `waiting` if not (network, or § busy) |
| `play()`, no activation | paused | (unchanged) | promise rejected `NotAllowedError` |
| `pause()` | playing | paused | `timeupdate`, `pause`; promises rejected `AbortError` |
| queue runs dry while playing, not at the end | ENOUGH/FUTURE | CURRENT_DATA | `waiting` |
| `currentTime` set | any with METADATA | seeking | `seeking`; then `timeupdate`, `seeked` when the decoder lands |
| end reached, no `loop` | playing | paused, `ended` | `timeupdate`, `pause`, `ended` |
| network fails mid-stream | LOADING | IDLE | `error` (`MEDIA_ERR_NETWORK`); what is queued plays out first |
| decode fails | any | as above | `error` (`MEDIA_ERR_DECODE`) |
| element removed from the document | playing | paused | `pause` (the standard's "removed from a document" step) |
| page unloaded | any | gone | none; threads told to stop, the fetch cancelled |

## The controls

**An `os64_ui` widget laid over the element's box**, as yonder's form
controls are (`FormWidget`, `forms_place`): a play/pause button, the time
(`1:12 / 3:45`), a seek bar (the existing `os64_ui_slider`), and a mute
button with a volume slider. A widget gives hit-testing, dragging, focus
and keys for free, and forms already prove the placement, the clipping
and the "covered by something" rule. The bar shows `buffered` behind the
thumb when the stream says how far it has arrived. Space toggles play
while the control has focus.

The control is driven by the element, never the other way round: a click
on play is `play()` with the person's activation, a drag on the bar is a
`currentTime` set, so a page listening for `play` or `seeking` hears a
click on the native control exactly as it hears its own buttons.

`<audio>` WITHOUT `controls` has no box (`f_boxless`, as today), and
gains one when `controls` is set, as other browsers do.

## The media document

libway already sorts a navigation's response into page, text, picture or
"offer as a download". It gains a fourth: a type in the client's AUDIO
list (`leg.sounds`, beside `leg.pictures`; yonder fills it from the same
table `canPlayType` reads) is `WAY_BODY_MEDIA`, and yonder writes the
same kind of page it writes for a picture:

```html
<!doctype html><title>song.mp3</title>
<body style="(the picture page's dark centred body)">
<audio controls autoplay src="URL"></audio>
```

It plays at once (the person navigated to it, which is the activation).
A type yonder cannot decode is still offered as a download, unchanged.

## Proof

- **Host, libmedia**: fixtures (WAV at each sample width, mono and
  stereo, 8 to 96 kHz; MP3 CBR and VBR, with and without a Xing header,
  with ID3v2) decoded by libmedia and by the host's own `ffmpeg` of the
  same release to s16 48 kHz stereo; equal to within the resampler's
  rounding. Seek lands within one MP3 frame. A truncated file, a file of
  random bytes, an HTML page served as audio: each ends in its named
  error and never a crash, under ASan/UBSan. The mixer and the per-element
  clock against a model. The host test FAILS without `ffmpeg`, as
  `test_sbc_host.sh` does.
- **Host, the port**: `tools/test_av_audit.py` — manifest hashes, the
  configuration committed equals the one the pinned `configure` generates,
  no `CONFIG_GPL`/`CONFIG_NONFREE`, `libav.so` imports only libos64 and
  libmath, exports only the `av*` interface libmedia calls.
- **QEMU**: a local test page set under `/tests` (served by yonder's
  `file://`): `<audio controls>` with a WAV, a script playing an MP3
  through `new Audio()`, two elements at once, a pause, a seek, a loop,
  an `ended`. `/dev/audiotap` hears the frames, in order, at 48,000 a
  second; the mix of the two elements is their sum. A navigation to an MP3
  shows the media document and plays. A screendump shows the controls.
- **The P5**: Chris, with the headphones, on the pages that sent him
  looking last night.

## Slices

1. **M1 — FFmpeg and libmedia.** The port (audio-only configuration
   above), libmedia's decoder, source, queue and mixer, host-tested.
   Proven in the OS by `play` learning MP3 (§ For Chris 5) or by a
   `/tests` fixture if not. No yonder change.
2. **M2 — the element, its controls, the media document.** libdom's
   interfaces, the provider, the audio thread, the widget, `leg.sounds`.
   This is the slice that makes the pages Chris saw play.
3. **Then the gaps the pages show**, each its own small slice: a decoder
   switched on, a member a page needed, `playbackRate`.

M1 is a third-party port with no kernel change: one cold Opus read
before it is pushed. M2 is yonder, large, with threads meeting the
window: Codex.

## What this arc leaves out, on purpose

`<video>`; Media Source Extensions; MP4, WebM, Ogg; AAC, Opus, Vorbis
(each a configure switch when a page asks); text tracks; Web Audio
(`AudioContext`); a playback rate other than 1; picture-in-picture;
DRM (EME), which os64 will never have: a licensed decryption module is
not something one person's browser can obtain. The headphones' play and
pause buttons reaching a page are the audio-buttons slice's question
(how a press reaches a program); when it has an answer, yonder's audio
thread is the natural listener.

## For Chris

1. **The stream is busy** (another program, another window): the element
   waits and the controls say why (§ When the stream is busy). Lean: as
   written.
2. **Several elements in one window mix in yonder**; two windows contend
   for the one stream. Lean: as written; the kernel mixer stays in DEBTS
   until two programs at once is a real need.
3. **Pause flushes the stream when it was the only thing playing**, so a
   pause is immediate instead of a fifth of a second late. Lean: yes.
4. **Autoplay matches other browsers** (sound needs an interaction first;
   muted may autoplay; a media document plays at once). That is web
   behaviour, so I am telling rather than asking, unless you object.
5. **`play` learns MP3** through libmedia, as M1's in-OS proof. `play` is
   Fable's, written as yours to replace (AUDIO_SEAM.md § For Chris 5), so
   it is your call: lean yes, it is a few lines and it is what you will
   type to test the headphones with a real song. The alternative is a
   `/tests` fixture that proves the same thing and that nobody types.
6. **A tracker** (`AUDIO_YONDER_TRACKER.csv`), since this is the start of
   an arc that runs to video. Lean: yes, from M1.
