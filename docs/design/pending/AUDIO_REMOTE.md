# AUDIO_REMOTE.md — the design for A.4: the headphones' volume, their buttons, and a link that dozes

*Fable, 2026-11-14. The design for rows A.4.1 to A.4.3 of
`AUDIO_TRACKER.csv`. AUDIO_A2DP.md (A.3) is what this sits on;
AUDIO_CLASSIC.md is the link; AUDIO_SEAM.md is the seam whose volume
changes hands here. Checked against the tree at `0e0568df`.*

*Status: not yet read by anyone but the author. Chris said "go on A.4,
stack" on 2026-11-14 and is to be asked nothing before morning, so where a
choice is his, this takes my lean and § For Chris says so. A fact marked
(recalled) is from memory of the specifications and is checked against
public sources (BlueZ's `profiles/audio/avctp.c` and `avrcp.c`, Android's
`stack/avrc` and `stack/avct`) by whoever builds that part.*

## What A.4 is

Three things a person notices, in the order they are built:

| Part | What a person gets | Rows |
|---|---|---|
| **Z, the link dozes** | Headphones connected and silent stop costing radio time and their own battery; sound still starts at once | A.4.2, A.4.3 |
| **R, the remote's protocol** | Nothing yet: AVCTP and AVRCP as pure code, and the SDP records that say we speak them | A.4.1 |
| **W, the volume wired** | `/dev/volume` moves the HEADPHONES' volume; their volume buttons move `/dev/volume`; their play, pause, next and previous presses are heard and counted | A.4.1 |

Three stacked pull requests, Z then R then W, each read cold and sent to
Codex before the next is stacked.

## What the P5 has said that shapes this (2026-11-13 and 14)

- **Sound was quiet at `/dev/volume` 100.** We send full-scale PCM; the
  loudness is the headphones' own setting, which we neither know nor set.
- **The headphones put an idle link into sniff, at 500 ms, and do not take
  it out when a stream starts.** Sniff is refused link-wide today
  (AUDIO_A2DP.md § P, as built).
- **They are the link's central** after connecting either way, and they
  call us back once they know us as an audio host.
- **An LE scan and a Classic scan during a song cost no slip** (14,918
  packets, none late). That is row A.4.3's measurement, already made.
- **The buttons on the cup**: volume up, volume down, and between them one
  button for play, pause, next and previous (Chris).

## Part Z: the link dozes

### The rule

Sniff is ALLOWED while nothing is streaming and REFUSED while something
is, and a link found in sniff when sound arrives is taken out of it before
the stream starts.

| The host finds | It does |
|---|---|
| The link `READY`, AVDTP not streaming and no sound waiting, for 10 s | Write Link Policy Settings for this link: role switch and sniff. The headphones may then ask for sniff, as they did on the P5 |
| Sound waiting (the seam has sound), or AVDTP `STARTING` or `STREAMING` | Write Link Policy Settings: role switch alone. If the link's mode is sniff: Exit Sniff Mode |
| Sound waiting and the link still in sniff | The pump is told there is no sound yet, so it does not Start, until the link's Mode Change says active or 1 s has passed, whichever is first |

The default policy for a NEW link stays "sniff refused" (the start-up
round's command): a link begins awake, and earns sniff by being idle.

### Where it lives

- `bt_classic.c` (pure) gains the two commands as requests of the link,
  beside the ones it has: `bt_classic_policy(c, sniff_allowed)` and
  `bt_classic_wake(c)`. Each is a command at the link's next turn of the
  one command slot, by the rules the link's other commands follow (LATCHED
  is for answers the controller is waiting on; these are not, and take
  their turn). It records what policy the controller last confirmed, so a
  write is made only when the wanted policy differs, and never repeated
  after a refusal until the wanted value changes (the page-scan rule).
- The Mode Change event is already read (`mode`, `mode_interval`).
- `bt_host.c` decides, each tick, from state: the seam's `has_sound`
  through the ops it already holds, AVDTP's state, and a timestamp of when
  the link last had neither.
- The "no sound yet" answer is the host's own wrapper around the ops'
  `has_sound`; the pump is not changed.

### Lifecycle (the link's sniff policy)

| State | Meaning | Leaves by |
|---|---|---|
| `AWAKE` | Policy confirmed: sniff refused. Every new link | idle for 10 s: the policy write is asked: `ALLOWING` |
| `ALLOWING` | The write allowing sniff is owed or out | confirmed: `DOZING`. Refused: `AWAKE`, counted, not asked again on this connection. Sound arrives first: `WAKING` |
| `DOZING` | Policy confirmed: sniff allowed. The link may be active or in sniff | sound arrives, or AVDTP leaves `OPEN` for a stream: `WAKING` |
| `WAKING` | The write refusing sniff is owed or out, and Exit Sniff Mode after it if the mode is sniff | policy confirmed and the mode active: `AWAKE`. 1 s without the mode going active: `AWAKE` all the same, counted (`woke late`), and the stream starts in sniff's rhythm, which is audible and so is seen |

The link dropping ends every state. A controller that refuses the wake's
policy write leaves the link `AWAKE` in name with sniff still allowed:
counted and shown, since it would explain a click track.

### Scanning while streaming (row A.4.3)

Measured, not built: an LE scan and a 20-second Classic scan during six
songs cost no slip and no underrun on the P5. The manager does nothing
about a scan during a stream. The measurement is recorded in the tracker
and in BLUETOOTH.md; if a busier radio shows otherwise, the first thing to
try is refusing a Classic inquiry while AVDTP is streaming, which is one
line where the scan is asked for.

### Proof

Host: `bt_classic`'s suite gains the two requests (their bytes, their turn
in the slot, refused, unanswered, the link dropping under each) with
mutants; the multi-host suite gains the lifecycle end to end with the model
headphones asking for sniff once it is allowed and never leaving it by
themselves, as the real ones do: idle, dozing, sound, awake before the
first media packet, the song ends, dozing again; and the keyboard typing
through all of it with part S's LE assertions unchanged.

P5: connected and silent for a minute, slot 2 reads `mode: sniff`; `play`
starts cleanly (no clicks) and the mode reads active during the song; a
minute after it ends, sniff again.

## Part R: AVCTP and AVRCP

### What the two are

- **AVCTP** carries the remote-control messages: its own L2CAP channel on
  PSM 0x0017 (recalled), a three-byte header (a transaction label, whether
  the message is a command or a response, and the profile it belongs to),
  then the message. Messages too long for the channel are fragmented; ours
  never are.
- **AVRCP** is the messages. There are two families:
  - **Pass through**: "this button was pressed", "released". One byte of
    operation (play 0x44, stop 0x45, pause 0x46, forward 0x4b, backward
    0x4c, volume up 0x41, volume down 0x42; all recalled), its top bit
    saying released.
  - **Vendor dependent**, under the Bluetooth SIG's company identifier,
    carrying numbered PDUs. The three this needs (recalled): Get
    Capabilities 0x10, Register Notification 0x31 (for the volume-changed
    event, 0x0d), Set Absolute Volume 0x50. Volume is seven bits, 0 to
    127.

### Who is which

Both ends play both roles, for different things:

| For | The headphones are | We are |
|---|---|---|
| Their buttons (play, pause, next, previous) | the CONTROLLER: they send pass-through commands | the TARGET: we answer each, and hand the press on |
| Absolute volume | the TARGET: they hold the volume, obey Set Absolute Volume, and announce changes | the CONTROLLER: we set it, and register to hear it change |

Headphones with volume buttons and absolute volume do not send "volume up"
as a pass-through: they change their own level and announce the new one.
Headphones WITHOUT absolute volume change their own level and tell nobody,
or send the pass-through; both are handled (below).

### What we say we are (SDP)

Our SDP responder answers for one record, the Audio Source. It gains two,
and so becomes a responder for a small table of records:

| Record | Service class | Says |
|---|---|---|
| Audio Source | 0x110A | as today |
| AV Remote Control Target | 0x110C | AVCTP over L2CAP PSM 0x0017; AVRCP 1.4 (the first with absolute volume); features: category 1 (player) |
| AV Remote Control | 0x110E and 0x110F (Controller) | the same protocols and version; features: category 2 (amplifier: the volume's controller) |

(Classes, versions and feature bits recalled; checked when built.)
Headphones read these before they offer absolute volume to a source, so
the records land with the protocol, in the same pull request. Service
Search and Service Search Attribute answer for every record the pattern
matches, with the continuation rules the one record has today.

### `bt_avctp.c` and `bt_avrcp.c` (pure)

One file each, or one if that reads better: the builder's call. Pure, one
per link, embedded by the host; whole messages in and out; carrying them
on L2CAP is the host's, with the take-then-commit shape AVDTP has
(`next` gives the head message and a serial, `sent` commits it), because
the same parked-message hazard exists.

**As the target** (their buttons): every pass-through command is answered
ACCEPTED for the operations in the table above and NOT IMPLEMENTED for the
rest; each accepted press and release is appended to a small ring of
events the host reads (operation, pressed or released, a count that only
goes up). Unit Info and Subunit Info are answered (panel). Get
Capabilities is answered (company identifier: the SIG's; events: none we
announce). Any other vendor-dependent PDU is answered with the AVRCP
reject its PDU calls for.

**As the controller** (their volume), a small state machine:

| State | Meaning | Leaves by |
|---|---|---|
| `IDLE` | No AVCTP channel | the host says it is open: `ASKING` |
| `ASKING` | Get Capabilities (events) is out | the answer lists volume-changed: `REGISTERING`. It does not, or the command is rejected or not implemented: `PLAIN` (no absolute volume) |
| `REGISTERING` | Register Notification (volume changed) is out | INTERIM, carrying their volume now: `READY`, and the volume is known. Rejected: `PLAIN` |
| `READY` | Absolute volume works. A registration is standing | CHANGED arrives: the new volume is known, and the registration is made again (each lasts one change). A Set Absolute Volume of ours is answered: the volume they actually took is known |
| `PLAIN` | These headphones have no absolute volume | the link drops |
| `FAILED` | A command went unanswered (2 s, recalled as the specification's order of magnitude; the builder checks) twice | the link drops |

`bt_avrcp_set_volume(a, 0..127)`: allowed in `READY`; one Set at a time,
the latest value wins if another is asked while one is out.

Everything it reads came over the air: every length is checked against
the bytes that remain; a fragmented message from them is reassembled to a
bound (512 bytes) or dropped and counted; an AVCTP message for a profile
that is not AVRCP is answered with the invalid-profile bit, as the
specification asks (recalled).

### Proof

`tools/test_bt_avrcp_host.sh`: a model pair of headphones in both roles,
every message built from the specification and compared byte for byte;
headphones with absolute volume and without; a volume change announced
while our Set is out; every reject; fragments at every byte; random bytes
under the sanitizers; a responsive random peer that must reach `READY`;
mutants. `tools/test_bt_sdp_host.sh` gains the table of records: each
found by its class, all three by the public browse root, continuation
across records, the cross check with the client's walker.

## Part W: the volume wired

### L2CAP and the host

- **The channel table grows from four to six.** Signalling, media, their
  SDP and AVCTP are four at rest, and an SDP query of ours or a crossing
  channel needs a fifth for a moment. Each channel carries a 1 KB payload
  buffer; the host is on the heap since A.3.
- **PSM 0x0017 is admitted** for their calls and opened by us, on an
  encrypted link, once AVDTP's signalling channel is open (the same guard
  and the same "decided from state" rule AVDTP's channels have). One
  AVCTP channel: a second is closed, the one that opened second.
- AVRCP is fed, ticked and drained each tick beside AVDTP, under the same
  send gate, after the LE peers.

### Who owns the volume

**When the headphones have absolute volume, `/dev/volume` IS their
volume.** One number, one place to set it, and the buttons on the cup move
the same number:

| Event | What happens |
|---|---|
| AVRCP reaches `READY` | The sink tells the seam it owns the volume. The seam stops applying its own gain (full-scale PCM is sent) and hands each pull the volume it holds, 0 to 100. The sink sets the headphones to that (0 to 127, rounded), so what `/dev/volume` says is what is heard |
| A write to `/dev/volume` | The seam's number changes; the sink's next pull sees it and sends Set Absolute Volume. What the headphones answer they took is NOT written back unless it differs by more than rounding: a volume of 50 must read back 50 |
| The volume buttons on the cup | They announce CHANGED; the sink tells the seam, and `/dev/volume` reads the new number |
| The session ends, or AVRCP is `PLAIN` | The seam owns the volume again and applies its gain, as today |

**When they have none** (`PLAIN`): today's behaviour. `/dev/volume` is the
seam's gain, and their buttons are theirs. If they send volume up or down
as pass-through presses, each moves `/dev/volume` by 5.

Why not keep both (a gain AND their level): two volumes multiplied
together is the thing everybody has fought with on some other system,
where one is at 30% and the other at 100% and neither is loud. And full
scale into the encoder is the best signal SBC can be given.

The seam grows two calls and one field, keeping its rule that it never
calls into a sink with a fact:

- `audio_sink_owns_volume(sink, bool)`: while true, the seam's pull applies
  no gain and reports `volume` in what the pull tells.
- `audio_sink_volume(sink, 0..100)`: the sink says the volume changed at
  its end; the seam takes it as a write to `/dev/volume` would be taken.
- `audio_pulled_t` gains `volume`. A sink that is not pulling (suspended)
  still needs to hear a volume write: the seam's `nudge` already rings the
  sink's bell on a control write, and the sink asks
  `audio_sink_volume_now(sink)` when nudged.

`/sys/audio` says who owns the volume.

### Their buttons

Play, pause, forward and backward presses are ANSWERED by part R and reach
the host as events. This part shows them (slot 2's status: the last press,
and a count of each) and logs each under `DEBUG_USB`. **What a press DOES
is not decided here**: Chris and I talk about that on 2026-11-14, and it
has consumers this arc does not own (`play`, yonder's media element). The
events are kept in a form a consumer can be given without touching the
protocol again.

### Proof

Host: the multi-host suite's headphones gain AVCTP and both AVRCP roles:
`READY` to a known volume with no typing; a write to `/dev/volume` reaches
them as Set Absolute Volume and reads back unchanged; their button moves
`/dev/volume`; ownership returns to the seam when the session ends and
when the link drops; headphones with no absolute volume leave everything
as it is; presses arrive, are answered and counted; all beside a typing
keyboard with the LE assertions unchanged, and the channel table's peak
asserted against six. The audio-stream suite gains the ownership rule with
mutants.

P5, by Chris:

1. With the headphones connected, `cat /dev/volume`, then
   `echo 30 > /dev/volume` during a song: it gets quieter at the
   headphones. `echo 100`: louder than A.3 ever was.
2. Press volume up and down on the cup during a song: it changes, and
   `cat /dev/volume` follows.
3. Press the middle button (play/pause; twice for next; three times for
   previous, as these headphones do it): nothing changes yet, and slot 2's
   status counts each press under its name.
4. Type and move the mouse through all of it.

## What A.4 leaves out, on purpose

- **What play, pause, next and previous DO.** Heard and counted here;
  given a meaning after the conversation that decides it.
- Metadata (the song's title on a headset's display), browsing, the
  player-application settings, cover art: AVRCP has all of these and
  headphones use none.
- Our own announcements as a target (track changed, playback status):
  headphones do not ask for them; a car would.
- Initiating sniff ourselves, sniff subrating, and choosing the interval:
  the headphones ask, as they did unprompted.

## For Chris

Taken as leaned, since you asked to be left to sleep; each is one place to
change:

1. **`/dev/volume` becomes the headphones' own volume** when they support
   being set, and the seam's gain steps aside. One number. § Who owns the
   volume has the reasoning.
2. **A link earns sniff after ten idle seconds**, and is woken before a
   stream starts, waiting at most a second for it.
3. **AVRCP 1.4 is what we claim**, the lowest version with absolute
   volume, and nothing above it.
4. **Button presses are answered and counted, and do nothing yet.**
