# BLUETOOTH_AUDIO.md — design notes: sound out of os64, through Bluetooth headphones

*Written 2026-10-10 by Fable, after a morning conversation with Chris. These
are NOTES, not a design: the shape of the problem, what the stack already
has, what each layer would need, and the questions that have to be ruled
before a design is written. Nothing here is sized by anyone who has done
it; the LE HID arc was estimated at weeks and took under two days.*

*Revised 2026-11-12 by Fable, with Chris, the day he chose audio as the next
arc. What changed: the destination is VIDEO IN YONDER (yonder is not his
daily driver until YouTube plays), so the seam is designed for a video
player and not a music player; the only sink for now is Bluetooth, so the
HDA driver left the plan and a small test sink took its place; the xHCI
interrupt moved from last to first; the headphones may call us; the fixed
rate is 48 kHz; and every codec is PORTED, none written here.*

*Revised again the same day after Quinn's review (BLUETOOTH_AUDIO_REVIEW.md,
beside this file). Her four protocol findings were right and are folded in
where they belong: the credit pool may be one pool shared with LE; a
headphone that calls us also starts the profile, so we must answer it;
SDP's answer can come in parts; and RTP's sequence counts packets, not
frames. So are her two contracts to settle before the seam is frozen (what
"position" counts, and what a flush can promise), the transport's 512-byte
send buffer, and the specification facts she checked that these notes had
stated from memory.*

## The tracker

`AUDIO_TRACKER.csv` beside this file is where the arc stands: four slices
(`A.1` the seam with a test sink, `A.2` the Classic transport, `A.3` AVDTP
and the encoder, `A.4` volume, sniff and scanning), their sub-tasks, and
§ Open questions as `Q` rows. `~/tracker.sh audio` draws it;
`tools/TRACKERS.md` is the format. The rules are this arc's:

- Each slice opens with its design row. That design may add, split or drop
  the slice's sub-task rows, in the PR that carries it; a dropped row
  stays, with the reason in its notes.
- A row is flipped by the branch that does its work, in that PR; `done`
  and the PR number land as the PR's last commit. A branch touches its own
  slice's rows.
- A `Q` row is ruled by Chris, or settled by the proof its notes name, and
  the answer is written into its notes.
- `python3 -I tools/tracker.py --csv docs/design/pending/AUDIO_TRACKER.csv
  --quiet` passes before a push that touches the file.

The xHCI interrupt, which § Order puts first, was its own arc and is done
(`XHCI_TRACKER.csv`).

## The ask, and why it is two things

The use case is one: play sound on the P5, and the only sink that matters
for now is a pair of Bluetooth headphones. Where it is going is a page's
`<video>` and `<audio>` in yonder, and that decides what the seam must
offer on its first day (§ The audio seam). It splits into two pieces that
are different in kind:

1. **An audio SEAM** — how a program says "here is PCM, play it", how it
   learns which sample is leaving the speaker now, and who owns the device.
   This is the half every sink shares, and the half the QEMU harness can
   prove.
2. **The A2DP SINK PATH** — Classic Bluetooth from the AX210 to the
   headphones. This is the half QEMU cannot emulate at all (its Bluetooth
   emulation was removed in QEMU 5), so it is proven the way LE HID was:
   host-side protocol models for every decoder, then the P5 with real
   headphones.

The HDA controllers the P5 and the desktop both carry are a THIRD thing,
and this note does not plan them. The seam is proved in QEMU by a test
sink instead (§ Proof); an HDA output is a later sink under the same seam,
for the day the jack or HDMI audio is wanted.

## What the stack has today (BLUETOOTH.md, `kernel/src/driver/system/usb/bt_*.c`)

- HCI over USB on the AX210, firmware loaded, one EP0 command in flight at
  a time, interrupt IN for events, bulk IN/OUT for ACL. ~2,200 lines of
  pure decoders that allocate nothing and wait on nothing, driven from
  `kworker` (`xhci_bluetooth_maintain`).
- `bt_scan.c` already does CLASSIC INQUIRY beside LE scanning —
  `/sys/bluetooth/devices` lists Classic results. So the controller side
  of BR/EDR discovery is not new.
- `bt_le.c`: LE central, legacy SMP, GATT client, HID over GATT, two peer
  slots, bonds on disk, automatic reconnection. ACL credits are spent one
  at a time across both peers and returned by Number Of Completed Packets.
- Everything is polled: `xhci_poll` rides `processSignals`, and the DEBTS
  row on dispatch frequency says what that costs when nothing else keeps
  the kernel task awake. HID tolerates it, though not well: Chris finds
  the keyboard and the trackball slow to connect when switched on, and
  DEBTS records a boot where they were ready after about 118 and 153
  seconds, cause unresolved. Whether the polling is that cause is not
  proven. Audio will not tolerate it at all (§ Pacing).
- Since these notes were first written the tree has gained two things the
  seam uses: `knet`'s doorbell shape for a kernel daemon woken by the tick
  (DOORBELL.md), and a bell a ring-3 thread can wait on (`os64/bell.h`).

**Everything above the HCI transport is LE.** A2DP lives on BR/EDR, and
the two halves of Bluetooth share a controller and almost nothing else:
different discovery, different connection setup, different pairing,
different L2CAP, different everything up to the profile. The DEBTS row
already says "Classic HID and audio need their own profiles"; this is
what that row costs.

## The A2DP path, layer by layer

Bottom up. Each layer is a pure decoder in the house style, host-tested
against a model of the peer before it meets the radio.

### Classic HCI (Core Vol 4 Part E)

- **Setup after reset**, beside what LE already sets: `Write Simple
  Pairing Mode` on, `Write Local Name` (headphones show it on their
  display and some refuse a nameless host), `Write Class of Device`
  (some sinks check the major class), `Write Default Link Policy
  Settings` allowing role switch, and the event mask bits for the
  classic events below. As built the default REFUSES sniff: every link
  begins awake, and the host allows sniff per link while nothing plays
  (AUDIO_REMOTE.md § Part Z).
- **Connection**: `Create Connection` to the inquiry's address, with the
  page scan repetition mode the inquiry reported. The sink may ask to
  ROLE SWITCH to master; allow it, or some sinks drop the link.
- **The headphones call us.** A pair switched on pages the host it was
  last bonded to, and "turn them on and sound comes out of them" depends
  on answering: `Write Scan Enable` with page scan on, the `Connection
  Request` event accepted for an address that has a bond and refused for
  one that does not, and `Link Key Request` answered from the bond. A
  host that only ever calls out has to keep paging a sink that may be in
  a drawer, and feels as slow as the LE keyboard does today. Both
  directions end in the same session. A sink that calls us usually goes
  on to start the PROFILE as well, so accepting its link is half of it:
  see § SDP and § AVDTP for what must answer.
- **Secure Simple Pairing**: `IO Capability Request`/`Reply` (we are
  NoInputNoOutput or DisplayYesNo; headphones have no input, so this is
  Just Works), `User Confirmation Request` auto-accepted, `Link Key
  Notification` — the link key is the bond, stored the way LE's LTK is
  (`bt_bond_store.c`), and `Link Key Request` answers from it on
  reconnect. Then `Authentication Requested` and `Set Connection
  Encryption` on; A2DP data over an unencrypted link is refused by most
  sinks.
- **Sniff mode** while nothing plays, or the radio and the headphones'
  battery pay for a link that is idle. As built (AUDIO_REMOTE.md § Part
  Z) sniff is allowed on a link idle ten seconds and refused, the link
  taken out of sniff, before our stream starts: the P5's headphones kept
  a link in sniff through a stream, and a link in sniff carries media
  only at its anchors. The sink asks; we never do (DEBTS.md), and `Sniff
  Subrating` is not used.
- **Read Buffer Size** (0x1005) is already issued for LE's sake; the
  CLASSIC half of its reply — ACL packet length and the number of
  packets — is what audio's credits come from. The AX210 reports 1021
  bytes and a handful of packets. **Whether that pool is audio's alone
  depends on the controller.** When `LE Read Buffer Size` answers zero
  buffers, LE and Classic share the one pool this command reports, and
  `bt_le.c` already handles that reply (`BT_LE_SHARED_BUFFER`). Giving
  audio "the whole Classic pool" and LE one packet more would then spend
  more than the controller has. So the credits are the CONTROLLER's to
  account, not a session's: shared or separate decided from both
  replies, packets outstanding tracked by connection handle, and in the
  shared case room kept for the keyboard, the trackball and control
  traffic inside the total (Core Vol 4 Part E §§ 4.1.1, 7.4.5).

### Classic L2CAP (Core Vol 3 Part A)

LE's channels are fixed and skip configuration. Classic channels are
DYNAMIC: `Connection Request` to a PSM, then a `Configuration
Request`/`Response` exchange in BOTH directions before a byte flows (MTU
at least; a sink may propose flush timeout or a retransmission mode,
which we decline back to Basic mode). Two channels per headphone: one for
AVDTP signalling (PSM 0x0019) and one for the media stream, opened by the
same PSM after the stream endpoint is configured. Plus one more for AVRCP
(PSM 0x0017) if volume is wanted.

### SDP (Core Vol 3 Part B)

A `Service Search Attribute Request` for the Audio Sink UUID (0x110B)
answers the AVDTP version and the sink's supported features, in a binary
data-element grammar that needs a careful bounded parser. Some sinks will
accept AVDTP without being asked; do it anyway, the version decides what
AVDTP may send.

**It is not one request and one response.** A legal answer may carry part
of the attribute data and a continuation token, and a single data element
may be cut across two answers (Core Vol 3 Part B § 4.7). The client hands
the token back until the answer is whole, and bounds the whole: bytes in
all, transactions, nesting, time. It parses the assembled result, or keeps
bounded parser state across the parts.

**And we are asked, too.** A headphone that called us looks up OUR
service before it configures anything. So os64 publishes an Audio Source
record and answers SDP queries for it: a small responder beside the
client, with the same grammar and the same bounds.

### AVDTP and A2DP (the profile specs)

- `Discover` the stream endpoints, `Get Capabilities` on the sink ones,
  pick the SBC endpoint, `Set Configuration` with the chosen SBC
  parameters inside the capabilities the sink offered, `Open` (which is
  when the media L2CAP channel is created), `Start`. `Suspend` on pause
  and `Close` at the end; `Abort` on anything confusing.
- **Both roles.** That list is the INITIATOR's. A source must also be
  the ACCEPTOR (A2DP §§ 5, 5.1.1.1): a headphone that called us sends
  `Discover`, `Get Capabilities` and `Set Configuration` itself, opens the
  channels itself, and stalls on an encrypted link if nothing answers. So
  there is an acceptor state machine beside the initiator, incoming L2CAP
  channels are admitted for the two PSMs, and two setups begun at once
  from both ends resolve into one session. (That a given pair of
  headphones does this is inferred from the specification, not observed.)
- Media packets are RTP headers (12 bytes) plus a one-byte A2DP payload
  header (the number of SBC frames) plus the frames. **The two counters
  count different things** (RFC 3550 § 5.1, A2DP § 4.3.3.1): the SEQUENCE
  goes up by one for each packet; the TIMESTAMP names the packet's first
  sample and goes up by the samples the packet carries. Six frames of 128
  samples: the next packet's sequence is one more and its timestamp 768
  more. (These notes first said both advance per frame, which a sink
  would hear as five packets lost out of every six.)
- **Delay reporting** (AVDTP 1.3) is how the sink tells us its own
  buffering, and current A2DP requires a sink to support it (§ 5.1.1.2,
  by Quinn's reading; these notes had said most sinks do not). So it is
  negotiated first, and § Latency's measured constant is for an older
  sink that does not offer it.

### SBC, the codec

Mandatory in every sink, so it is the only one a first slice needs. A
published spec, and the encoder is PORTED, not written here (Chris,
2026-11-12: the codecs are ports): a fixed-point encoder is about 1,500
lines of integer C with no floating point, which matters because of
§ Where the encoder lives. Which one is the port's first question; its
licence and whether it builds freestanding decide it. At 44.1 kHz, joint
stereo, 16 blocks, 8 subbands, bitpool 53 (the "high-quality" settings
every sink accepts) it is 328 kbit/s, 128 PCM samples a frame, 2.9 ms a
frame, and 5 to 7 frames fit in one ACL packet, so one packet every 15 to
20 ms. At 48 kHz, which § The audio seam now chooses and which every
sink must accept (A2DP § 4.3.2.1), the specification's recommended
high-quality joint-stereo setting is bitpool 51: 115 bytes a frame, 345
kbit/s (its Table 4.7; Quinn checked both against the specification, where
these notes had them from memory). A frame is still 128 samples, 2.7 ms.
How many fit a packet follows from the media channel's MTU and is worked
out in the design. AAC and aptX are optional codecs and optional work.

### AVRCP, for volume

Headphones expect to be TOLD their volume (absolute volume, AVRCP 1.4)
and will report the buttons on the cup as commands back. AVRCP rides
AVCTP on its own L2CAP channel and is a slice of its own, after sound
plays. Without it the headphones' buttons work on the headphones and
nothing else, which is a fine first day.

### Not this: LE Audio

The AX210's firmware does LE Audio on Linux (LC3, isochronous channels,
BAP). It is the future, the stack is larger (ISO data paths are a new HCI
transport kind), and headphone support is still a minority in 2026.
Classic A2DP is where every pair of headphones is. Booked, not planned.

## The audio seam

This is the part that outlives the headphones, so it deserves the
argument.

- **`/dev/audio`**: a program WRITES PCM to it. Sun put sound in the tree
  that way in SunOS 4.0 (1989), Plan 9 kept it (`/dev/audio`, with
  `/dev/volume` beside it), and it is the shape os64's doctrine already
  says: one handle type, read/write/close dispatch on the tag, and the
  routing can change under a contract that does not. devfs (`devfs.h`)
  has the live fops this needs. The FORMAT is fixed in the first slice —
  signed 16-bit, two channels, 48 kHz — and a consumer that wants another
  resamples. 48 and not the CD's 44.1 because the consumer this is built
  for is a web page's sound, which is 48 kHz nearly everywhere (Opus is
  48 kHz by definition, and it is what YouTube serves), and making the
  main consumer resample everything to spare a WAV player is the wrong
  way round. A2DP requires a sink to accept 48 kHz SBC as it does 44.1,
  as I remember the specification; that is checked before this is ruled
  (§ Open questions). A format negotiation door can come when a consumer
  asks.
- **Backpressure is the clock for a program that only plays.** The write
  parks when the device's ring is full and returns when it drains, which
  is how `play` stays in time without any API for it. The park is a
  blocking wait, so it obeys `signal_park_must_end` and voids its waiter
  slot like every other park (CLAUDE.md § Signals).
- **A video player needs four things more, from the first slice**, since
  it is the consumer the seam exists for:
  - **The playback clock.** Pictures are kept in step with sound by
    asking "which sample is leaving the speaker now?" and showing the
    frame that belongs to it. A latency constant alone cannot do this:
    it says how late sound is, not where playback is after an underrun
    or a stall. So the seam answers a POSITION, and the design fixes
    exactly what it counts before anything is built on it (Quinn: the
    first wording said "samples played" in one place and "samples the
    sink has taken" in another, which are different numbers). The
    contract to start from, hers: a count of frames SUBMITTED to the sink
    (a frame being one sample for each channel), the monotonic time of
    that reading, a separately named ESTIMATE of the frame being heard,
    and a stream GENERATION all three belong to, which changes at a
    flush, a seek or a reconnection so that an old reading is never
    mistaken for a new one. The design says what each does on pause and
    on underrun. What is queued in the ring is delay; the ring's size is
    not, and the test sink proves the difference.
  - **A write that does not park**, and a bell that rings when the ring
    has room. yonder runs a page on one loop and may not sleep in a
    write; `os64/bell.h` is what it waits on beside everything else.
  - **Pause and flush.** A pause stops the pump and tells the sink to
    suspend; a seek throws away what is queued. What a flush retires on
    OUR side is ours to define and prove: the ring, packets built and not
    yet sent, packets with the controller, and how a completion that
    belongs to the old generation is refused. What it does to the sound
    already inside the HEADPHONES is not established: these notes first
    said a suspend and a start discard it, and nothing found so far in
    the specifications promises that. It is an open proof (§ Open
    questions), settled on the real headphones with a seek and an
    immediate resume, and if a suspend does not do it, by closing the
    stream and making it again. The test sink must not be written to
    pretend the answer is known.
  - **Volume in the seam** (`/dev/volume`, Plan 9's name): a gain applied
    to the PCM on its way to the sink. A player has a slider on its
    first day, and the headphones' own volume (AVRCP) is a later slice.
- **`/sys/audio`**: the report. Which sink, its state, the ring's fill,
  underruns since boot and since last read (the number that says the
  player fell behind), credits, the position, and the LATENCY ESTIMATE
  (§ Latency). Read it FIRST when sound stutters, in the house tradition.
- **One owner at a time**, at first. A second open of `/dev/audio` while
  one is open is refused, with its own result code. A mixer is a later
  thing and probably a ring-3 daemon that owns `/dev/audio` and offers
  something else, the way logd owns the log sink. Not now.
- **Sinks are a seam below the node**: the node knows "a sink with a
  ring, a rate, a position and a latency"; the test sink of § Proof is
  the first, the A2DP source the second, an HDA stream can be a third,
  and the node does not change.

## Where the encoder lives (the question to rule first)

Two honest shapes:

1. **The kernel encodes.** `/dev/audio` takes PCM; the A2DP sink pulls
   PCM out of the ring, SBC-encodes it, frames it, and sends. Every
   consumer stays a PCM writer; the sink can be swapped under it. SBC
   fixed-point is integer arithmetic, so the `-mno-sse` kernel can run it,
   and it costs about one percent of a core. The encoder is a port
   (`upstream/` + `patches/` + `port/`, pinned and hash-checked like the
   userland ones), which makes it the first third-party code inside the
   kernel: it must build with no libc and no floating point, and it is
   host-tested against a reference decoder before the kernel links it.
2. **Ring 3 encodes.** A daemon opens a raw A2DP door, encodes with a
   libsbc, hands frames down. The kernel stays smaller; every consumer
   now depends on a daemon being up, and the seam is no longer "write
   PCM here".

**RULED 2026-11-12 by Chris: (1), the kernel encodes**, for the timing
(the encode and the send in one place the tick wakes, so one thing can be
late and not two), on these terms, which he asked for because sound must
not be what takes the machine down:

- **What may be ported into ring 0.** Code that is integer-only,
  allocates nothing, keeps no pointer but its own state, parses nothing a
  stranger sent, and is host-tested against a reference before the kernel
  links it. The SBC encoder is the first; the rule is written so that the
  second has to meet it too.
- **Behind a narrow boundary**: PCM in (a fixed-size block), frames out
  (into a buffer whose size it is told). Nothing above the sink knows an
  encoder exists, so it can move to ring 3, or gain a ring-3 neighbour
  for a codec that needs floating point, by changing one sink.
- **What can be detected fails closed.** The sink marks itself failed, a
  write to `/dev/audio` answers an error, `/sys/audio` says why, and
  nothing panics:
  - a frame that is not the one length its settings give (for a given
    SBC configuration every frame has the same length, known beforehand);
  - one of the encoder's own checks, compiled to fail the sink and not
    to stop the kernel;
  - an encode that outruns its time budget;
  - settings outside what the sink negotiated, refused before the
    encoder sees them.
- **What cannot be detected is prevented.** A stray write in ring 0 is
  not contained by any check made afterwards. That is what the host
  proof is for: the sanitizers over every parameter combination and
  random input, and the output compared with a reference decoder.

## Pacing, and the transport we have

A2DP has no flow control from the sink. ACL credits come back when the
CONTROLLER has sent a packet over the air, not when the headphones have
played it, and the headphones buffer a hundred milliseconds or more. So
the source paces itself by WALL CLOCK: a packet every N ms at the
sample rate, a little ahead, drift-corrected from `micros()` and the
sample count. Send too slow and the sink underruns (a click, then
silence); too fast and its buffer overflows (a skip).

That makes the pump a periodic job with a real deadline, and today's
transport is polled from `kworker` at scheduler-pass cadence. The DEBTS
row on dispatch frequency says passes can arrive SECONDS apart on an
idle GUI boot. Audio cannot ride that. Two things follow, both are
infrastructure the row already asked for, and both now come FIRST
(§ Order): sound needs them without question, and they are the first
thing to try against the slow connection of the LE keyboard and
trackball, which is worth having whether or not a note ever plays:

- **The pump is a kernel daemon woken by the tick** — `knet`'s shape
  (DOORBELL.md): park, wake on the bell, do bounded work, park. The tick
  ringing it at 100 Hz is enough for a 15 to 20 ms packet cadence with
  headroom.
- **xHCI needs its interrupt** (MSI-X or INTx, the e1000 precedent) so
  the ACL credit return and the bulk IN completions arrive when they
  happen rather than at the next pass. The A2DP slice is the natural
  neighbour the DEBTS row was waiting for.

**The transport as it stands is not ready for this, and making it ready
is part of the Classic transport slice, not an afterthought.**
`xhci_bt_acl_send` copies a packet into a 512-byte region of the transmit
page, and the adapter's command storage begins right after it: an audio
packet larger than that, sent through today's helper, would overwrite the
commands. One packet is in flight at a time (`tx_used`), and receive
assembly is sized by `BT_LE_ACL_MAX`. Audio wants larger packets and
several with the controller at once, so the buffers, fragmentation and who
owns each are designed and reviewed. Two completions must not be confused:
the USB transfer finishing gives the buffer back; the controller's Number
Of Completed Packets gives the credit back.

The ring between `/dev/audio` and the pump is sized in time, not bytes:
about 200 ms of PCM (38 KB at the fixed format) covers a scheduler
hiccup without adding to the latency the headphones already impose.

## Latency

Bluetooth audio arrives 150 to 250 ms late: SBC framing, the sink's
jitter buffer, its own decoder. A music player does not care. A VIDEO
player must show each picture that much after it hands over the sound
that goes with it, or the mouths move before the words. The seam's
position (§ The audio seam) says what the SINK has taken; the latency
estimate is what lies beyond the sink: its reported delay when it reports
one, else a per-sink constant that a fixture measures once with a clap
and a timestamp. A player subtracts one from the other to know what is
being heard. It asks through a door and not by reading `/sys/audio`,
since it asks for every frame it shows; the report carries the same
numbers for a person. A number that is honest about being an estimate
beats no number.

## Coexistence with LE HID

The keyboard and the trackball are LE peers on the same controller. A
Classic ACL link is a third handle through the same HCI, and the
controller schedules the radio between them. Three things to watch,
each already a shape in `bt_host.c`:

- Commands stay one-owner-at-a-time; a Classic session is a new owner
  kind beside the scan and the LE peers.
- ACL credits are the CONTROLLER's, in one pool or two as its two Read
  Buffer Size replies say (§ Classic HCI). With two, audio has the
  Classic pool and LE its own. With one, audio is scheduled inside it
  with room kept for the keyboard and the trackball, or typing stalls
  while music plays.
- Inquiry and page both cost radio time; a scan while audio streams may
  stutter it. Linux's answer is to shorten the inquiry window while an
  A2DP stream is up. Note it; measure it on the P5.

## Players (the consumers, and whose they are)

The seam is mine. The first thing that writes to it is a `play` for WAV
(the format with no decoder, so the seam is proven before any codec is).
The consumer it is built for is yonder: a page's `<audio>` and `<video>`,
which need the playback clock, the write that does not park, pause and
flush, and volume. That work is a yonder arc of its own (the media
element), designed after this seam exists and against it.

Every decoder is a PORT in the `upstream/` + `patches/` + `port/` pattern,
as libwebp and QuickJS are; none is written here. For sound the web's
two are Opus (libopus, C) and AAC, and those come before MP3 or FLAC
because they are what a video site serves. AAC is the awkward one: which
decoder, under which licence, is a question for the porting conversation
and is not answered here. The pictures (VP9, AV1 through dav1d, H.264)
and the containers they arrive in are that conversation too, and whether
one large port (ffmpeg) or several small ones carries them is its first
ruling.

## Proof

- **Every protocol layer is a pure decoder with a host model of the
  peer**, the way `bt_le.c`'s SMP and ATT are tested: an SDP responder,
  an AVDTP sink state machine that offers capabilities and accepts or
  refuses configurations, an L2CAP peer that proposes modes we decline.
  Fuzzed, because every one of these parses bytes a stranger sent over
  the air. From Quinn's review, by name: both credit layouts (one pool
  and two) with completions interleaved and a disconnection while packets
  are outstanding; a peer that STARTS the profile, not only the link, and
  one that starts it while we do; an SDP answer cut inside an attribute's
  value, and one that never finishes; and RTP's sequence and timestamp
  checked apart from the encoder.
- **The SBC encoder against a reference decoder** on the host: encode a
  known signal, decode it with the reference, assert the error bound the
  spec allows.
- **The seam in QEMU with a test sink**: a sink with no hardware, which
  takes PCM from the ring at the rate the clock says a real one would
  and writes it to a file the harness reads back. It proves `/dev/audio`,
  the ring, the park and the bell, pause and flush, volume, underrun
  counting, the position and the latency bookkeeping sample for sample,
  with no radio and no driver. It is chosen by name where the A2DP sink
  is absent, so it is also what a machine with no headphones plays into.
  (The first notes proposed a minimal Intel HDA output here, since QEMU
  emulates one. It is a few thousand lines for a jack nobody is waiting
  for; it can be a sink later without changing anything above it.)
- **The P5 with real headphones**: pair, play a WAV, pull the battery
  out of the headphones mid-song (the LE HID arc's favourite test), put
  it back, and read `/sys/audio` afterwards. Then a scan during
  playback. Then the keyboard typing during playback.
- **Captures**: btsnoop from the laptop's Linux pairing with the same
  headphones (the `keyboard-pairing.btsnoop` precedent) tells us which
  optional steps THIS sink insists on before the code guesses.

## Order, if it were started

1. xHCI's interrupt, and Bluetooth's maintenance on a daemon the tick
   wakes. Measured on the P5 against the LE keyboard and trackball before
   and after: how long from switching one on to its first key. It pays
   for itself there whatever happens to the rest.
2. The seam with the test sink, in QEMU. `/dev/audio`, `/dev/volume`,
   `/sys/audio`, the ring, the pump daemon, the position, pause and
   flush, the bell; `play` for WAV.
3. The Classic transport: the send and receive buffers and the
   controller's credit accounting made fit for it (§ Pacing, § Classic
   HCI), then Classic HCI + SSP + L2CAP configuration and SDP in both
   directions, host-modelled, then a P5 connection to the headphones that
   goes nowhere (encrypted link up, SDP asked and answered), made from
   either end. The bond on disk.
4. AVDTP + the ported SBC encoder: sound plays. The latency constant
   measured.
5. AVRCP volume. Sniff mode. The scan-while-streaming measurement.

Steps 1 and 2 share nothing and can be built side by side.

## Open questions for Chris

1. ~~Kernel-side SBC or a ring-3 encoder~~ RULED 2026-11-12 by Chris: the
   kernel, on the terms in § Where the encoder lives (what may be ported
   into ring 0, the narrow boundary, and failing closed).
2. ~~Is an HDA output worth its lines as the harness's sink?~~ Settled
   2026-11-12 by "for now it's just BT audio": no HDA; the test sink of
   § Proof is the harness's.
3. ~~The fixed format~~ RULED 2026-11-12 by Chris: fixed at 48 kHz for
   the first slice, built with negotiation in mind, since sites and
   sources will vary and it has to start somewhere. So the format is a
   property the open states and the node reports, with one value accepted
   today, and nothing above the sink assumes 48. The fact it rested on
   is confirmed: a sink must accept 48 kHz SBC (A2DP § 4.3.2.1, checked
   by Quinn).
4. ~~Where the headphone session lives~~ RULED 2026-11-12 by Chris: in
   the same place, with the same words (`connect`, `forget`, `reconnect`,
   reconnection at boot). Whether a device is Classic or LE is something
   a person reads in `/sys/bluetooth/connection` and nowhere has to
   choose. The design says how one list holds both kinds of peer.
5. ~~How a player asks for the position~~ RULED 2026-11-12 by Chris: a
   node beside `/dev/audio`, provided it costs no more than a syscall
   would. It costs the same kernel entry (a read IS a syscall) plus the
   handle's dispatch, which is tens of nanoseconds at sixty asks a
   second; the design states the measured figure. If a consumer ever
   asks often enough for that to matter, the answer is a page the kernel
   keeps current and the player maps, which needs no entry at all.
6. ~~Whether the xHCI interrupt goes first~~ RULED 2026-11-12 by Chris:
   first, for every USB device and not only Bluetooth, as its own arc
   with its own agent, apart from audio. Its brief is XHCI_INTERRUPT.md,
   and § Order's step 1 is that arc, not a slice of this one.
7. OPEN, a proof and not a ruling: what a flush does to sound already in
   the headphones (§ The audio seam, Pause and flush). Settled on the P5
   with the real pair before the seam's flush is called done.
8. OPEN, for the design: the position's exact contract (§ The audio seam,
   The playback clock). Quinn's is the starting point.

## Lineage, for the commit message

A2DP's media packets are RTP (1996, the protocol under every VoIP call
and video conference), carried over L2CAP, which is Bluetooth 1.0's
(1999) answer to "a packet protocol over a radio that only speaks
slots". SBC was written for Bluetooth by Philips because every other
codec of 2003 carried a patent; it is deliberately simple enough to run
on a headphone's chip of that year, which is why it is still mandatory
and still everywhere. And `/dev/audio` is 1989 Sun: a sound card as a
file you write to, the idea Plan 9 kept and Linux buried under three
generations of sound APIs.
