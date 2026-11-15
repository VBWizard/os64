# AUDIO_YONDER.md — a page's sound: `<audio>`, the media document, and FFmpeg

*Opus, 2026-11-14. Status: DESIGN, cleared by Chris the same day
(§ For Chris), and being built: `YAUDIO_TRACKER.csv` says what is, and
each slice's "As built" section below what departs from the text.*

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
| `userland/libav/` → `/lib/libav.so` | FFmpeg, ported: libavutil, libavcodec, libavformat, built down to the decoders and readers we name. Its own library, never linked into anything (§ FFmpeg) | `/tests/avtest` (decodes in ring 3); `tools/test_av_libc_host.sh` (the shims against glibc); `tools/test_av_audit.py` (licence, exports, imports, the configuration) |
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
- Its source lives in the tree (`upstream/`, unpatched, and
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
`config_components.h` are committed beside the manifest:
`tools/import_av.py --check` proves them the pinned `configure`'s, and the
audit checks `config.h`'s licence and overrides on every run. os64 cannot
run `configure`, and a committed configuration is a reviewable one.

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
and QuickJS already use: a name libos64 has is mapped onto libos64, and
the rest is a small pure file host-tested against glibc plus a file of
doors that stay shut (no file opens, no environment, no clock but a
stopwatch). `userland/libav/README.md` § The C library is the inventory.
Aligned allocation is NOT shimmed: with FFmpeg's assembly off nothing in
it asks for more than the 16 bytes `os64_malloc` gives, so the
configuration says it has no `posix_memalign` and FFmpeg uses `malloc`.
The day assembly comes on, that changes.

**Pin**: the newest FFmpeg release on the day of the import, its tarball's
SHA-256 in `manifest.json`, every shipped file hashed. A release, not a
git snapshot: the monthly scan then reads a release's security notes.

**Size**: the library slot is 64 MiB (`app_bases.py`). This configuration
is a megabyte or two; every audio and video decoder the daily-driver goal
names is still well inside the slot.

## The pipeline (libmedia)

`userland/libmedia/include/os64/media.h` is the interface: open a
source, decode, seek, the position, close, and a source over a file
handle. A source is a `read` that may block, a `seek` (NULL when the
source cannot), and the byte length when known.

**Everything leaves libmedia in the stream's format** (s16le, two
channels, 48 kHz) and on the OUTPUT's timeline: every count and position
is in 48 kHz frames from the start of the media, so the element's clock
and the audio thread speak one unit. The resampler's start-up delay is
counted off and its tail drawn out, so a seek to a frame returns that
frame first and a whole decode is the media's length. Mono is doubled;
more than two channels are folded to two with BS.775's weights,
normalised so the fold cannot clip (a file that names no layout is read
as the usual one for its count, 5.1 for six); a rate that is not 48 kHz
goes through `libsound`'s resampler.

**The decoder thread** (one per element that is loading or playing):
reads the source, decodes, resamples, and pushes into the element's PCM
queue, which holds two seconds and makes the thread wait when full. The
queue is Y.2.1's, with its first consumer: `play` needs none, since its
loop decodes and writes in one thread. It
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
The mixer is in libmedia, pure, host-tested against a model, and it is a
slice of its own (Y.3, ruled 2026-11-14: a slice, not an arc). Until it
lands, the thread sounds one element at a time, and an element that
starts while another sounds waits exactly as § When the stream is busy
says, by the same code.

**It is not what a kernel mixer would be.** A kernel mixer (DEBTS since
A.1) mixes PROGRAMS: two yonder windows, or yonder and `play`. yonder's
mixer mixes one program's own sounds into its one stream, which is what
other browsers do too: they mix every tab into one output and hand the
system one stream. With a kernel mixer in place, yonder's would still be
the right shape, so neither duplicates the other.

**Two yonder windows are two programs**, and the stream has one owner. The
second window's play waits (§ When the stream is busy) until there is a
kernel mixer; this arc does not need one.

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

**Pause.** When the paused element is the ONLY one sounding, the thread
writes `pause` to `/dev/audioctl` and stops writing; `play()` writes
`resume`. The stream's own pause stops the pulls at once and keeps the
ring, so nothing queued is lost and `heard` freezes where the ear is
(AUDIO_SEAM.md § The stream's states; Chris drove both verbs by hand on
2026-11-14). The stream is yonder's own while it holds `/dev/audio`, so
neither verb can reach another program's sound.

What is queued is DISCARDED (`flush`) only when it no longer belongs at
the front: a seek, a new `src`, or another element starting while this
one is paused. The decoder then starts again from the element's `heard`
position.

A pause that lasts gives the stream back: after 30 seconds paused, the
thread closes `/dev/audio` (a paused close is a flush, AUDIO_SEAM.md
§ The stream's states), so a song paused in yonder does not keep `play`
in a terminal busy for the rest of the evening. Resuming after that opens
the stream again and is a seek to where the element stood.

With others still sounding (Y.3), pausing one stops the mixer taking
from its queue, and its last fifth of a second plays out under the
others: the same tail other browsers have.

**Loop, end.** `loop` makes the decoder thread seek to zero at the end
and keep going; without it the element's queue runs dry, the mixer sees
the end, and the element fires `ended`.

## When the stream is busy

Another program (`play` in a terminal, another yonder window) holds
`/dev/audio`. The open answers `OS64_AUDIO_BUSY`.

**Ruled** (§ For Chris 1): the element does not fail. It behaves as a
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

**Prototypes**: `D_PROTO_MEDIA` and `D_PROTO_AUDIO`, with `D_PROTO_AUDIO`
inheriting `D_PROTO_MEDIA`, so `D_PROTO_VIDEO` is one more kind under it.
(The table was full when Y.2.1 came to it: § As built (Y.2.1).)

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
- **The port**: `tools/test_av_audit.py` — manifest hashes, no GPL,
  nonfree or version-3 parts in the arguments or in `config.h`, `libav.so`
  imports only libos64 and libmath and exports only FFmpeg's public
  prefixes, and no other program carries FFmpeg's code;
  `tools/import_av.py --check` — the committed configuration is the one
  the pinned `configure` generates; `tools/test_av_libc_host.sh` — the
  shims against glibc; `/tests/avtest` — an MP3 and a WAV decoded in
  ring 3.
- **QEMU**: a local test page set under `/tests` (served by yonder's
  `file://`): `<audio controls>` with a WAV, a script playing an MP3
  through `new Audio()`, two elements at once, a pause, a seek, a loop,
  an `ended`. `/dev/audiotap` hears the frames, in order, at 48,000 a
  second; the mix of the two elements is their sum. A navigation to an MP3
  shows the media document and plays. A screendump shows the controls.
- **The P5**: Chris, with the headphones, on the pages that sent him
  looking last night.

## As built (Y.1.1, the port)

`userland/libav/README.md` is the record; what departs from the text
above:

- **FFmpeg 9.0.2**, unpatched, and the configuration is `configure`'s own
  output with the C-library answers overridden in `manifest.json`
  (`config_overrides`), so an upstream change to what `configure` writes
  fails the import instead of slipping through.
- **The whole public interface is exported** (FFmpeg's own prefixes), not
  only what libmedia calls: the LGPL's replaceable library is FFmpeg's
  interface, and a narrower one would make a user's own build of it a
  different library.
- **A build without threads is safe from several threads only under a
  rule**: every open and close under one lock, and a thread-safe log
  callback (README.md § Switched off). That is libmedia's rule in Y.1.2.
- **`snprintf` is the port's own** (`av_format.c`), not libos64's: a
  formatter that does not know a conversion cannot step over its argument,
  and FFmpeg uses `%zu` with more arguments after it. A cold read found
  that before anything printed from the wrong place.
- **The shims' own names are libav's alone**: a program that includes
  FFmpeg's headers sees only the compat names that resolve to libos64
  (README.md § How it is built), or libmedia would have failed to link.
- **Two userland flags are dropped for FFmpeg's sources**: `-masm=intel`
  (a symbol spelled like a register does not assemble in Intel syntax) and
  `-Wredundant-decls`.
- **`/tests/avtest`'s reference is a measurement**: the level the host's
  FFmpeg decodes the fixture's MP3 to with its assembly off. The port
  matched it to six digits on the first boot; the level written into the
  first draft (the sine's own) was 5% high, because LAME's encode does
  not land there.

## As built (Y.1.2, libmedia and `play`)

- **The interface** is `os64/media.h`, described above. Results are named
  (`OS64_MEDIA_NOT_MEDIA`, `_UNSUPPORTED`, `_NO_SOUND`, `_DECODE`, `_IO`,
  `_NO_MEMORY`, `_CANNOT_SEEK`) and `os64_media_error_text` gives libav's
  last error line for the media.
- **The lock covers less than libav's general rule** (every open and
  close): in this configuration FFmpeg's one-time setups are libavutil's
  CRC tables (which a demuxer can first ask for while reading a header)
  and the decoders' tables (made when a decoder opens, by libmedia and by
  `avformat_find_stream_info`, which opens decoders of its own). libmedia
  makes every CRC table at its first open and holds its lock across
  `find_stream_info` and its decoder's open. Opening the input, reading
  and decoding take none; two media probing at once take turns at
  `find_stream_info`. `tools/test_av_audit.py` pins every one-time setup
  in the build, templates included, so a configuration that adds one fails
  until it is looked at. (The cold read caught the first draft locking only
  the decoder's open, which missed `find_stream_info`'s.)
- **The size is given to FFmpeg even when the source cannot seek**: an
  MP3's end padding is trimmed only by a demuxer that knows where the file
  ends (`mediatest` found it: 48,952 frames for one second, until then).
- **Only libmedia loads libav.so** (beside libav's own fixture): the audit
  fails on any other caller, so a player cannot tie itself to the decoder
  libmedia hides, and the decoder can change behind os64/media.h.
- **The host test is of the pure core** (`media_pcm.c`: every sample
  format, planar or not, one to eight channels, against a model), not
  libmedia against the host's FFmpeg as the proof section first said: the
  host has no FFmpeg development files, and the glue is proven in ring 3
  instead (`/tests/mediatest`). The host test found that a NaN on one
  side of a fold silenced the other side.
- **`play` is libmedia's first caller**: anything libmedia reads (MP3,
  and the WAV forms `configure_args` name), its messages the result's
  name. A 44.1 kHz file now plays without the
  resampler's few frames of leading silence, and an 8-bit WAV plays where
  it was refused (`playtest` changed with it, and gained an MP3).

## As built (Y.2.1, the element)

- **The prototype table was full** (64 kinds, the last `D_PROTO_SOURCE`),
  not 59 of 64 as this design was written against, and the handler mask's
  51 event kinds plus the media element's 21 passed its 64 bits. Both sets
  are GCC's 128-bit integer now (`d_kinds_t`, and `os64_dom_handler_mask_t`,
  which yonder's handler walk reads too), so a set is still one value.
  `error` is the existing kind; the media element's prototype has its
  `onerror`, the first element the binding fires `error` at (the rest of
  HTML's per-element `onerror` stays as LIBDOM.md § Events has it).
- **The kernel refused nothing, silently**: an object's `DT_NEEDED` list
  was capped at 16 names, and a 17th was dropped without a word, so yonder,
  which names 17 libraries with libmedia, died at its first call into
  libos64. The cap is 32 (`ELF_MAX_NEEDED`), and an image naming more is
  refused at load (`elf_parse_dynamic`).
- **libdom keeps HTML's half; yonder the sound** (`dom/media.h`, the
  provider): libdom has the states, the events and the promises, and asks
  the host for facts from its media turn; the host never decides an event.
  `canPlayType` is libmedia's `os64_media_type_supported`, which asks
  libav.so for the reader and decoder each MIME type needs.
- **Two threads per element, one for yonder.** A DECODER thread (the bytes,
  libmedia, the PCM queue) and, for a network address, a LOADER thread that
  runs the fetch job itself (`yonder_fetch_media_run`, a native consumer
  that streams its body down the pipe and keeps an opaque body's bytes):
  off the pool, so a song left paused half-loaded holds no worker. One
  AUDIO thread owns `/dev/audio` (yonder/media.c says which thread owns
  what, and the lock order). A `file:` address is read through
  `os64_media_file_source` with no loader.
- **A page of many players costs no handle per thread, and fetches four
  sounds at a time.** No media thread is joined: their handles are
  closed at birth, a session is freed once each of its threads has said
  it is done, and yonder's end waits for the audio thread to say so too.
  A start that answers an error is treated as
  one that may be running, because out of handles the kernel starts the
  thread detached and still answers an error; freeing the fetch job under
  such a loader was a crash on a page with eighteen players. The fetches
  queue for four loader slots, below libfetch's six connections to a host
  (DEBTS.md § A page's sound has what a slot held by a long sound costs).
- **The source keeps every byte that has arrived**, up to 256 MiB, so a seek
  back is free; a seek past what has arrived waits for it. While the media
  is being opened such a seek fails at once instead, because the MP3 reader
  probes the file's end for its tags and would otherwise wait for the
  whole download. A `Range` request for a seek far ahead is not made yet
  (YAUDIO_TRACKER.csv's Y.2.3).
- **`preload` is honoured, by the decoder**: with metadata (the default)
  it stops taking bytes after the probe, decodes nothing, and the fetch runs
  on into the pipe until its backpressure stops it, 16 MiB on; `play()`,
  `preload=auto` or `autoplay` takes the rest. `none` waits for `play()` or
  `load()`. So a song smaller than the pipe's window still downloads whole
  (DEBTS.md § A page's sound).
- **An element done with gives its session back**: one that has ended or
  failed, is paused and is in no document closes its session (the host's
  threads and buffers with it), and `play()` or a seek opens a new one. A
  page that plays `new Audio(x)` per sound effect keeps one small record
  each, and no decoder.
- **Only the shown page's elements turn.** A page loading behind the shown
  one does not advance its media until it is shown.
- **`playing` fires when the element can play** (readyState reaches
  HAVE_FUTURE_DATA with play asked), as the standard has it; the audio
  thread starts writing as soon as frames are queued, so the sound itself
  never waits for the window.
- **Proven in QEMU** by `/tests/pages/audio.html` (`tools/pages`): before
  any key, `play()` is refused `NotAllowedError`; the parsed WAV element
  loads by itself (duration 1.000 at 22.05 kHz, readyState 0 to 4); `new
  Audio()` plays the MP3 (duration 2.000, LAME's padding trimmed) and its
  `ended` comes 2,243 ms after `play()`, with `/sys/audio` showing all
  96,000 frames accepted, taken and heard; a seek lands with sound queued.
  `tools/test_dom_media.inc` plays every step against a fake host;
  `tools/test_media_queue_host.sh` holds the queue to a model.

## As built (Y.2.2, the controls and the media document)

- **The player is one widget** (`apps/yonder/player.c`), not a panel of
  libui's: a play button, the time, a seek bar, a mute button and a
  volume bar, each a share of the box's height so it zooms with the page,
  painted in the theme's colours and the page's face. It is 300 by 54 CSS
  pixels, Chrome's size, and that size has no ratio: a page that widens it
  leaves it as tall (libflow's `f_audio_controls`). The volume bar joins
  only where the seek bar keeps two buttons' width beside it, so Chrome's
  size shows play, time, seek and mute; there the wheel over the speaker
  and Up and Down set the volume, which is Chrome's pop-out slider's job.
  Space and Enter play and pause, Left and Right seek five seconds, `m`
  mutes. Busy and a failed load are said where the bar is ("Sound is
  busy", "Cannot play this sound").
- **It shows the element and asks the element**: libdom's
  `os64_dom_media_view` is what a script's members would answer, and
  `os64_dom_media_control` is a script's call (play, pause, a seek, the
  volume, muted), through the same steps the members take, so a page
  hears the same events. A control's play is a task of its own, and its
  promise's rejection is the element's state, never an unhandled one.
- **A press is acted on at the loop's turn**, not in the widget's
  handler: yonder replays a press to libui from inside the page's pointer
  task, where no task of the control's own can begin. The press has
  already been the page's activation (yonder_scripts_pointer).
- **The players are built and placed with the form controls**, after the
  same layouts and scrolls (`forms_build`, `forms_place`), threaded after
  them in the root's list (`page_widgets_chain`), so a rebuild of either
  keeps the other.
- **A page with scripts off has no player**: the element lives in
  script's runtime, so its audio stays boxless, as a canvas falls back.
  DEBTS.md § A page's sound books what that costs.
- **The media document**: libway sorts a reply whose type is in the
  leg's `sounds` (libmedia's `os64_media_type_list`, the table
  `canPlayType` reads) into `WAY_BODY_MEDIA`; the list is not added to a
  navigation's Accept. yonder writes `<audio controls autoplay>` on the
  picture page's dark body, and the navigation is the page's activation.
- **Proven** by `tools/test_dom_media.inc` (the view and each control,
  events and all, and a refused play no page sees),
  `tools/test_libflow_layout.inc` (the box with scripting, its height
  kept, none without), `tools/test_way_fetch_host.c` (a sound is a page
  for a face that plays it, never a POST's reply, and Accept unchanged),
  and in QEMU by `/tests/pages/audio-controls.html`: each part clicked
  plays, pauses, seeks, sets the volume and mutes, with the page's log
  hearing `play`, `playing`, `seeking`, `seeked`, `volumechange`, `pause`
  and `ended`; the link to the MP3 plays it on its own page at once, and
  plays it again from its button.

## Slices

`YAUDIO_TRACKER.csv` beside this file is the state of each one
(`~/tracker.sh yaudio` renders it). A slice that lands in one pull
request is one row; a slice of several has a task row per pull request.

1. **Y.1 — FFmpeg, libmedia, and `play` learns MP3.** Two pull requests:
   - **Y.1.1** the port: `libav.so` in the audio-only configuration
     above, its shims, manifest and audit, proven by `/tests/avtest`.
   - **Y.1.2** libmedia (source, decoder, resampling, the output
     timeline) and `play` reading MP3 through it (ruled 2026-11-14: Chris
     wants `play` to play his MP3s). No yonder change.
2. **Y.2 — the element.** Two pull requests:
   - **Y.2.1** libdom's interfaces, the provider and events, the audio
     thread, `play()`/`pause()`/seeking from script. One element sounds
     at a time.
   - **Y.2.2** the controls widget and the media document. This is the
     pull request that makes the pages Chris saw play.
3. **Y.3 — the mixer.** One pull request: several elements in one window
   sounding at once.
4. **Then the gaps the pages show**, each its own small slice: a decoder
   switched on, a member a page needed, `playbackRate`.

Y.1 is a third-party port and a library with no kernel change: one cold
Opus read before each push. Y.2 is yonder with threads meeting the
window: Codex. Y.3 is small and pure: reviewed here.

## What this arc leaves out, on purpose

`<video>`; Media Source Extensions; MP4, WebM, Ogg; AAC, Opus, Vorbis
(each a configure switch when a page asks); text tracks; Web Audio
(`AudioContext`); a playback rate other than 1; picture-in-picture;
DRM (EME), which os64 will never have: a licensed decryption module is
not something one person's browser can obtain. The headphones' buttons
(one press is play, two next, three previous) reaching a page wait on
the audio arc's answer to how a press reaches a program; when there is
one, yonder's audio thread is the natural listener.

## For Chris

*Put to Chris 2026-11-14 with leans; his answers the same day:*

1. **The stream is busy** (another program, another window): the element
   waits and the controls say why. RULED: yes, it is about the same work
   as failing.
2. **Several elements in one window mix in yonder.** RULED: if it is
   straightforward and not duplicative of a kernel mixer, as a slice and
   not an arc. It is both (§ The audio thread and the mixer): Y.3.
3. **Pause.** RULED: my lean, which became the stream's own `pause` and
   `resume` (Chris tested both verbs by hand), with a flush only when
   queued sound no longer belongs at the front.
4. **Autoplay matches other browsers.** RULED: yes.
5. **`play` learns MP3.** RULED: yes, Chris wants it.
6. **A tracker.** RULED: yes, `YAUDIO_TRACKER.csv`, slices, with a task
   per pull request where a slice has more than one.
