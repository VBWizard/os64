# AUDIO_A2DP.md — the design for A.3: sound plays

*Fable, 2026-11-13. The design row A.3.1 of `AUDIO_TRACKER.csv`.
BLUETOOTH_AUDIO.md (§ AVDTP and A2DP, § SBC, § Where the encoder lives,
§ Pacing, § Latency) is the argument and carries Chris's rulings and
Quinn's review; AUDIO_SEAM.md is the seam this plugs into; AUDIO_CLASSIC.md
is the link it rides. This file is what gets built, checked against the
tree at `50feceaa`.*

*Status: not yet read by anyone but the author. Facts about AVDTP and A2DP
marked (recalled) are from my memory of the specifications and are checked
against public sources (the specification text, Linux's `net/bluetooth`,
BlueZ's `profiles/audio`) by whoever builds that part, as parts T, L and S
were. § For Chris lists what is his to rule.*

## What A.3 is

`play song.wav` on the P5 comes out of the headphones, and they say
"connected" when the link comes up. Three things are built:

| Part | What | Why it is its own pull request |
|---|---|---|
| **E, the encoder** | An SBC encoder ported into the kernel behind PCM-in, frames-out, proven on the host against an independent decoder | The first third-party code in ring 0. It can be read and proven alone, with no Bluetooth in the diff |
| **V, the stream's signalling** | AVDTP (`bt_avdtp.c`, pure): find the headphones' SBC endpoint, configure it, open it; answer when they do the same to us | A protocol that parses a stranger's bytes, host-modelled and fuzzed like L2CAP and SDP |
| **P, the pump** | The A2DP sink behind the audio seam (`bt_a2dp.c`, pure) and its wiring: pull PCM, encode, frame as RTP, send at the sample clock's pace | Touches the host the keyboard and mouse run on, and the seam |

E and V share nothing and may be built side by side. P needs both.

## What the P5 has already said (2026-11-13)

Facts from the Bose SoundLink around-ear II, each of which shapes
something below:

- **They never call us.** We page; so we are AVDTP's initiator on every
  connection, and the acceptor's half exists for other headphones and is
  proven by the host model alone until some pair uses it.
- **They hang up two seconds after their last L2CAP channel closes.** So
  the order is: open AVDTP's signalling channel FIRST, and only then close
  the SDP channel part S leaves open. A stream that is suspended keeps its
  channels; nothing in A.3 closes the last one.
- **They report AVDTP 1.3, A2DP 1.3, features 0x0001, PSM 0x0019.** 1.3 is
  the version that has Get All Capabilities and delay reporting.
- **Their SDP channel's MTU is 48.** Small MTUs are real; the media
  channel's MTU is read, never assumed.
- **The controller's Classic pool is 4 packets of 1,021 bytes**, and the
  transport has four send slots, one always left for LE.

## Where the code lives

| File | What | Kind |
|---|---|---|
| `kernel/ports/sbc/` | The encoder: `upstream/` (untouched), `port/` (the shim and the boundary), `manifest.json`, `README.md` | Ported |
| `kernel/src/audio/sbc_frame.c`, `kernel/include/audio/sbc_frame.h` | Frame length and bit rate from the settings; the settings' validity. Ours, tiny, used by V, P and the port's checks | Pure |
| `kernel/src/driver/system/usb/bt_avdtp.c`, `.h` | AVDTP signalling, both roles | Pure |
| `kernel/src/driver/system/usb/bt_a2dp.c`, `.h` | The pump: the stream's policy, the sample clock, RTP, the seam's sink | Pure (the seam, the clock and the encoder reach it through callbacks) |
| `bt_host.c/.h`, `bt_l2cap.c/.h`, `xhci.c` | The wiring, and the changes below | Existing |
| `tools/test_sbc_host.*`, `test_bt_avdtp_host.*`, `test_bt_a2dp_host.*`, and their mutant harnesses | Proof | Host |

`kernel/ports/` is new: third-party code in the kernel has a place of its
own, laid out as `userland/libsound/` is, so that what is ours and what is
not can never be confused in a diff.

## Part E: the encoder

### Which one

**Android's SBC encoder** (`system/embdrv/sbc/encoder` in the Android
Bluetooth module; Copyright 1999-2012 Broadcom Corporation; Apache License
2.0). Read file by file on 2026-11-13:

- **C, eight source files and four headers**, about 2,500 lines, the
  largest being the analysis filter's macros.
- **Integer-only in its default configuration.** No `float`, `double` or
  `<math.h>` in code. Two files hold floating-point LITERALS in tables that
  the default switches compile out (`SBC_FAST_DCT` true, `SBC_IPAQ_OPT`
  true); the port's audit checks the objects for floating-point
  instructions, so a switch flipped by mistake cannot pass.
- **Allocates nothing.** One libc call in all of it, a `memset` at
  initialisation. One compiler builtin (`__builtin_mul_overflow`), which
  the cross compiler has.
- **Supports what A.3 uses**: 48 kHz, joint stereo, 16 blocks, 8 subbands,
  loudness allocation.

Its rough edges, and what the port does about each:

| Edge | Handling |
|---|---|
| State in file-scope statics and two globals: one encoder in the machine | There is one stream in the machine (AUDIO_SEAM.md). The boundary says so and refuses a second user |
| `SBC_Encode(params, pcm, out)` takes no output capacity | The boundary encodes into its own buffer, sized for the longest frame ANY setting gives, with guard bytes after it. A length that is not the one the settings give, or a disturbed guard, fails the sink |
| It reads one buffer through two pointer types (a 16-bit view of 32-bit storage) | Built `-fno-strict-aliasing`, stated in the port's makefile and README |
| An unrecognised sampling-frequency value silently means 48 kHz; the bitpool is clamped, not refused | The boundary validates every setting before the encoder sees it (`sbc_frame.c`), and compares the bitpool the encoder settled on with the one asked for |
| mSBC is half supported | Not offered by the boundary |

**The licence** is Apache 2.0, which os64 (MIT) may carry: its notice goes
in `license/android-sbc-LICENSE` with the files' own copyright lines, and
the README states what was taken and from which commit. Apache 2.0 § 4
asks that modified files say so; no file is modified.

The alternatives, and why not: BlueZ's `libsbc` is LGPL, which a kernel
linked as one image cannot honour without relinking terms; FFmpeg's encoder
is LGPL as well. Both are fine as HOST references, and FFmpeg is what the
proof uses.

### The boundary

```c
// kernel/ports/sbc/port/sbc_port.h — everything the kernel may call.
typedef struct sbc_settings
{
	uint32_t rate;        // 44100 or 48000
	uint8_t  channels;    // 2
	bool     joint;       // joint stereo; false is plain stereo
	uint8_t  blocks;      // 16
	uint8_t  subbands;    // 8
	bool     snr;         // false: loudness
	uint8_t  bitpool;
} sbc_settings_t;

// Starts the one encoder. False: the settings are not ones it takes.
bool   sbc_port_begin(const sbc_settings_t *settings);
// One frame: exactly sbc_frame_samples(settings) frames of interleaved PCM
// in, exactly sbc_frame_bytes(settings) bytes out. False: the encoder
// produced something else, and must not be used again until begin.
bool   sbc_port_encode(const int16_t *pcm, uint8_t *out, size_t capacity);
```

Nothing above `bt_a2dp.c` knows an encoder exists, and `bt_a2dp.c` knows
only this.

### The rule it has to meet (BLUETOOTH_AUDIO.md, ruled 2026-11-12)

Integer-only: audited from the objects. Allocates nothing: audited from
the objects' undefined symbols, which may be `memset` and nothing else.
Keeps no pointer but its own state: by reading, and recorded in the README.
Parses nothing a stranger sent: it reads our PCM and our settings, both
validated. Host-tested against a reference before the kernel links it:
below.

### Proof

`tools/test_sbc_host.sh` builds the port's own objects (the same sources,
the same switches) with the host compiler under the sanitizers, and:

1. **Every setting the boundary accepts**, and a sweep of bitpools for
   each: the frame length is the one `sbc_frame_bytes` gives, the header
   bytes say what was asked, the guard is intact.
2. **Decoded by FFmpeg's SBC decoder** (an implementation that shares no
   code with this one), for a sine sweep, white noise, silence, full-scale
   square waves and a few seconds of music-like signal: the decoded signal's
   error against the input is within a bound set by FFmpeg's own ENCODER
   at the same settings (ours may be no more than 3 dB worse). The
   specification's conformance procedure for an encoder is a comparison of
   its output with a reference encoder's; this is that comparison's
   audible form, and the design says so plainly: there is no absolute
   error bound in the specification to assert.
3. **Random PCM, random valid settings, thousands of frames**, under
   ASan and UBSan. If UBSan objects to the upstream code's fixed-point
   idioms (a left shift of a negative value is the usual one), the finding
   is recorded in the README with the instruction it compiles to, and that
   one check is turned off for the port's files alone; nothing is patched
   to quiet it.
4. **The audit** (`tools/test_sbc_audit.py`): `upstream/` is byte for byte
   the pinned commit's files; the kernel objects contain no SSE or x87
   instruction and no undefined symbol but `memset`.

The suite SKIPS, by name, on a machine with no `ffmpeg`, as the suites that
need `tinycss2` do.

## Part V: AVDTP (`bt_avdtp.c`)

Pure, one per Classic link, embedded by the host. It reads and writes
whole signalling messages; carrying them on an L2CAP channel is the host's.

### The wire (recalled; checked when built)

- **Header**: byte 0 is the transaction label (bits 7 to 4), the packet
  type (bits 3 and 2: single, start, continue, end) and the message type
  (bits 1 and 0: command, general reject, response accept, response
  reject); byte 1 is the signal identifier in its low six bits. A message
  longer than the channel's MTU is fragmented, and a start packet carries
  the number of packets after its header.
- **Signals**: Discover 0x01, Get Capabilities 0x02, Set Configuration
  0x03, Get Configuration 0x04, Reconfigure 0x05, Open 0x06, Start 0x07,
  Close 0x08, Suspend 0x09, Abort 0x0a, Security Control 0x0b, Get All
  Capabilities 0x0c, Delay Report 0x0d.
- **Capabilities** are a list of (category, length, bytes): media
  transport 1, reporting 2, recovery 3, content protection 4, header
  compression 5, multiplexing 6, media codec 7, delay reporting 8. The
  media codec element is the media type, the codec type (SBC is 0), and
  for SBC four bytes: sampling frequencies and channel modes; block
  lengths, subbands and allocation methods; the least bitpool; the most.

### What we are

One local endpoint: identifier 1, an audio SOURCE, SBC. Its capabilities
are what the encoder's boundary accepts.

### The session's states (the lifecycle table comes first)

| State | Meaning | Leaves by |
|---|---|---|
| `IDLE` | No signalling channel | the host says the channel is open |
| `DISCOVERING` | Our Discover is out | its answer: an audio SINK endpoint not in use is chosen, the first such; none is `FAILED` (no sink) |
| `ASKING` | Get All Capabilities (peer AVDTP 1.3 or later, by its SDP record) or Get Capabilities is out | its answer: SBC with 48 kHz, 16 blocks, 8 subbands, joint stereo or stereo, loudness, and a bitpool range that meets ours; else the next sink endpoint; none left is `FAILED` (no settings in common) |
| `CONFIGURING` | Set Configuration is out: media transport, the SBC settings chosen, delay reporting if they offered it | accepted; a reject is `FAILED` with its category and code |
| `OPENING` | Open is out, then the host opens the MEDIA channel (a second L2CAP channel to the same PSM) | the host says the media channel is open |
| `OPEN` | Configured, both channels up, not streaming. The seam's sink is attached from here on | Start (ours or theirs) |
| `STARTING` | Our Start is out | accepted; a reject returns to `OPEN` and is counted |
| `STREAMING` | Media may be sent | Suspend (ours or theirs), Close, Abort |
| `SUSPENDING` | Our Suspend is out | accepted or rejected: `OPEN` either way (a sink that rejects a suspend is not streaming to anyone) |
| `CLOSING` | Our Close is out | its answer or its timer: `IDLE`, and the host closes the media channel, then, if nothing else is open, leaves the signalling channel as the link's last |
| `FAILED` | The session cannot be made on this connection; why is kept | the link drops |

Every state is left for `IDLE` by the link dropping or the signalling
channel closing, silently, with everything cleared.

**Timers.** Each command we send has one (5 s); an unanswered command
sends Abort for a configured stream and ends in `FAILED` (not answered),
never touching the link. Abort itself has no reject and is not waited for
longer than its timer.

**Labels.** A response is taken only if its label and signal match the one
command outstanding; anything else is counted as a stray. We send one
command at a time.

### When they do it to us

The acceptor's half, complete and small:

| They send | We answer |
|---|---|
| Discover | Our one endpoint, with whether it is in use |
| Get Capabilities, Get All Capabilities | Our capabilities; a wrong endpoint is rejected (bad ACP SEID) |
| Set Configuration | Accepted when it names SBC settings the boundary takes, a bitpool range that meets ours, and no category we do not have; else rejected with the category and the code the specification gives. Accepted, the session is `CONFIGURING` as acceptor: they open the stream and the media channel |
| Get Configuration | What is configured |
| Open, Start, Suspend, Close, Abort | By the state table; a signal the state does not allow is rejected (bad state) |
| Delay Report | Taken (§ The delay) |
| Reconfigure, Security Control | Rejected (not supported) |
| Anything else | General Reject |

**Both at once.** If their Set Configuration arrives while we are
`DISCOVERING` or `ASKING`, theirs stands and we stop asking. If it arrives
after ours was sent, ours stands and theirs is rejected (endpoint in use).
A Start from them in `OPEN` is accepted and is `STREAMING`: the pump then
sends only if it has sound (§ The pump).

### Bounds

A reassembled message is at most 256 bytes and four fragments; longer is
dropped and counted. A capabilities list is walked with every length
checked against what remains; an element that overruns ends the walk as
malformed. At most eight endpoints are read from a Discover response.

### Proof

`tools/test_bt_avdtp_host.sh`: a model sink that offers capabilities,
accepts or refuses each step, answers late, never answers, fragments its
answers at every byte, and starts the profile itself, alone and while we
do; every message built by the model from the specification and compared
byte for byte; random bytes under the sanitizers; a mutant harness.

## Part P: the pump (`bt_a2dp.c`) and the wiring

### L2CAP and the host: what changes

- **PSM 0x0019 is admitted** for their calls, and opened by us only on an
  encrypted link (the host's guard, as part S's comment says).
- **Two channels to one PSM.** The first is signalling; the one opened
  after Open is accepted is media. The host tells them apart by order, for
  ours and for theirs.
- **The SDP channel is closed once AVDTP's signalling channel is open**,
  not before: the link is never left without a channel.
- **More than one L2CAP PDU a pass.** The core hands over at most one PDU
  a tick. Media needs two in some passes (a packet is due every 13 ms and
  a pass comes every 10). The host calls the tick again, up to three times
  a pass, while L2CAP has something to send and the gate allows. The gate
  is unchanged: the accountant, a send slot left for LE, no LE peer
  holding a packet.
- **A refusal for each channel.** L2CAP keeps one "last refusal" for the
  link, enough while SDP was the only channel we opened. It becomes the
  channel's.
- **The Bluetooth probe leaves the stack.** `xhci_bt_probe_t` is 25 KB,
  built as a local at bring-up (DEBTS). The pump adds PCM and packet
  buffers of about 9 KB. Bring-up allocates the probe before it fills it.
  This is the first commit of part P.

### The stream's policy

The pump is attached as the seam's sink while AVDTP is `OPEN` or beyond,
and detached when the session ends; the null sink takes over, so a `play`
with the headphones off still runs its length. Attaching is a flush
(AUDIO_SEAM.md), which is right: sound queued for nobody is not played
late.

| The seam says | The pump does |
|---|---|
| a nudge, and the stream has work | Start, if `OPEN` |
| pulls bring frames | sends them |
| pulls are dry (no handle, or a player that fell behind) for 3 s | Suspend. Until then it sends silence, so a gap between two songs does not cost a suspend and a start |
| `paused` | stops sending at once, and Suspend |
| a new generation (a flush) | nothing of ours to retire: no packet is ever built that is not sent in the same pass (§ The sample clock). What is inside the headphones plays out; § Proof measures it |
| the sink failed (the encoder's boundary, or a session that died) | `audio_sink_failed` with the reason; writes answer `OS64_AUDIO_FAILED` until the link comes back |

The seam grows one question, `audio_sink_has_work`, so a suspended pump
can tell a nudge that means sound from one that means a pause, without
pulling.

### The settings

48 kHz, joint stereo (plain stereo if they lack joint), 16 blocks, 8
subbands, loudness, and the bitpool the specification recommends for high
quality at 48 kHz joint stereo, 51, lowered to their maximum if that is
less (BLUETOOTH_AUDIO.md § SBC; Quinn checked the figure). A frame is 128
samples a channel, 2.67 ms, 115 bytes at bitpool 51.

### The packet

An RTP header of 12 bytes (version 2, payload type 96, a sequence number
that goes up by one a packet, a timestamp that names the packet's first
sample and goes up by the samples it carries, a fixed source identifier),
one byte saying how many SBC frames follow, then the frames, whole
(recalled, with the payload type; checked when built). Frames
a packet: as many as the media channel's MTU holds, at most 15. At an MTU
of 672 that is 5 frames: 588 bytes, 13.3 ms.

The sequence and the timestamp are computed in `bt_a2dp.c` from counts it
keeps, not by the encoder, and the host suite checks them against a model
packet by packet (Quinn's finding: they count different things).

### The sample clock

The headphones give no flow control: they play at their own crystal's
rate, and buffer a hundred milliseconds or more. So the pump sends by the
clock.

- The stream's time starts when Start is accepted: an anchor from
  `micros_now()`. Packet number k is DUE at the anchor plus the time the
  frames before it last, computed in 64-bit integers from the count of
  frames sent, so nothing accumulates.
- Each pass of `kusb` (every tick, and every xHCI interrupt): while a
  packet is due and the gate allows and fewer than three have gone this
  pass, pull one packet's PCM from the seam, encode it, send it.
- **Nothing is pulled that cannot be sent now.** If the controller has no
  buffer or the slots are taken, the packet waits in the ring as PCM, the
  ring fills, and the player's write parks. Sound that cannot leave makes
  the PLAYER late; it never piles up as delay.
- **Falling behind is a slip, not a burst.** A pump more than 100 ms late
  (held off, or starved of buffers) re-anchors to now and counts a slip.
  Sending the backlog at once would overflow the headphones' buffer.
- The encode is timed. A packet's worth is expected to cost a fraction of
  a millisecond; one over 5 ms fails the sink (the ruling's "an encode
  that outruns its time budget").

### The delay

`audio_sink_delay` is told what lies beyond the pull: their Delay Report
when they send one (units of 100 µs), else a constant, 150 ms until § Proof
measures the real one for this pair.

### What is seen

Slot 2's status gains the session: its state, the endpoint and settings
chosen, both channels, packets and frames sent, slips, dry pulls, rejects
with their codes, the delay and where it came from. `/sys/audio` names the
sink `a2dp`. The log gets one line for each session made or failed, each
start and suspend under `DEBUG_USB`, and one for a failed sink.

### Proof

Host: `tools/test_bt_a2dp_host.sh` drives `bt_a2dp.c` with the real
encoder, a model seam and a clock the test owns: the cadence over an hour
of stream time with passes at ragged intervals (no drift, the sequence and
timestamp exact); buffers withheld (nothing pulled, the ring left to fill,
then a slip); a pause, a flush, three seconds of silence, a start by the
headphones; every packet decoded and compared. The multi-host suite gains
the model headphones' AVDTP: `READY` to `OPEN` with no typing beside a
typing keyboard, then sound, with the LE assertions of part S unchanged.
Mutants for `bt_avdtp.c` and `bt_a2dp.c`.

P5, by Chris (rows A.3.5 and A.3.6):

1. Boot, headphones on: slot 2 reaches `ready`, the session `open`, and
   the headphones say "connected".
2. `play` a WAV: it sounds right, for its whole length. `/sys/audio`
   after: no underruns, no slips.
3. Switch the headphones off mid-song and on again: the song carries on
   in the null sink meanwhile (or the player is told the sink failed;
   § For Chris), and sound returns without typing.
4. A `scan classic` during a song; typing and the mouse during a song.
5. **The flush question** (Q7): `flush` mid-song, then play something
   else at once. Does the old sound's tail play first, and for how long?
   If it is objectionable, a Suspend and Start around a flush is tried,
   and what the pair does is recorded.
6. **The delay**: a click track and a stopwatch on the glass, to put a
   number on what "150 ms" really is for this pair.

## What A.3 leaves out, on purpose

- Volume from the headphones and their buttons (AVRCP): A.4.
- Sniff mode when nothing plays; the scan-while-streaming measurement:
  A.4.
- A flush timeout in the controller, so stale media is dropped on a bad
  radio: it needs signalling and media told apart on the wire, which the
  AX210's feature bits may allow. Booked in DEBTS with the first slip that
  shows it is wanted.
- Any codec but SBC; 44.1 kHz streams (the seam is 48 kHz); a second
  stream.
- Reconfiguring a stream that is open. A change of settings is a Close
  and a new session.

## For Chris

1. **The encoder is Android's, under Apache 2.0**, unpatched, in
   `kernel/ports/sbc/`. My lean, for the reasons in § Part E. It is the
   first third-party code in ring 0, so it is yours to refuse.
2. **Headphones switched off mid-song.** Two honest behaviours. (a) The
   seam's sink fails: the player's next write answers an error and `play`
   stops with a message. (b) The pump detaches and the null sink takes
   over: the song carries on unheard and is at the right place if the
   headphones come back. My lean is (b): it is what a player expects of a
   speaker that was unplugged, the position stays true, and (a) is still
   what happens when the ENCODER fails, which is the case the ruling was
   about.
3. **Three stacked pull requests** (E, V, P), each read cold and sent to
   Codex before the next is stacked, as A.2 was. E and V can be built at
   the same time by two builders.
4. **Three seconds of silence before a suspend** is a guess that covers
   the gap between songs. It is a constant, and § Proof's first run will
   say whether these headphones mind either way.
