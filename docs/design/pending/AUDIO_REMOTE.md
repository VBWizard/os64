# AUDIO_REMOTE.md — the design for A.4: the headphones' volume, their buttons, and a link that dozes

*Fable, 2026-11-14. The design for rows A.4.1 to A.4.3 of
`AUDIO_TRACKER.csv`. AUDIO_A2DP.md (A.3) is what this sits on;
AUDIO_CLASSIC.md is the link; AUDIO_SEAM.md is the seam whose volume
changes hands here. Checked against the tree at `0e0568df`.*

*Status: read once by someone other than the author (a cold Opus read
against the source, Linux's `hci.h`, BlueZ's `avctp.c` and Android's AVRCP
headers, 2026-11-14; its findings are folded in). Chris said "go on A.4,
stack" and is asleep, so where a choice is his, this takes my lean and
§ For Chris says so. A fact marked (source) was read in one of those
sources; one marked (recalled) is from memory and is checked by whoever
builds that part.*

## What A.4 is

Three things a person notices, in the order they are built:

| Part | What a person gets | Rows |
|---|---|---|
| **Z, the link dozes** | Headphones connected and silent stop costing radio time and their own battery; sound still starts cleanly | A.4.2, A.4.3 |
| **R, the remote's protocol** | Nothing yet: AVCTP and AVRCP as pure code, and the SDP records that say we speak them | A.4.1 |
| **W, the volume wired** | `/dev/volume` moves the HEADPHONES' volume; their volume buttons move `/dev/volume`; their play, pause, next and previous presses are heard and counted | A.4.1 |

Three stacked pull requests, Z then R then W, each read cold and sent to
Codex before the next is stacked.

## What the P5 has said that shapes this (2026-11-13 and 14)

- **Sound was quiet at `/dev/volume` 100.** We send full-scale PCM; the
  loudness is the headphones' own setting, which we neither know nor set.
- **The headphones put an idle link into sniff, at 500 ms, and do not take
  it out when a stream starts.** Sniff was refused link-wide before part Z,
  and the refusal held: our link policy is what our controller answers
  their request with (AUDIO_A2DP.md § P, as built).
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
our stream starts.

| The host finds | It does |
|---|---|
| The link `READY`, AVDTP not streaming and no sound waiting, for 10 s | Write Link Policy Settings for this link: role switch and sniff. The headphones may then ask for sniff |
| Sound waiting (the seam has sound), or AVDTP `STARTING`, `STREAMING` or `SUSPENDING` | Write Link Policy Settings: role switch alone. While the link's mode reads sniff: Exit Sniff Mode, again each second up to five times |
| The link being woken (`WAKING`, below) | The pump is told there is no sound yet, so OUR Start waits, until the wake is confirmed or 1 s has passed, whichever is first |

The default policy for a NEW link stays "sniff refused" (the start-up
round's command): a link begins awake, and earns sniff by being idle.

**A stream THEY start is not held.** If the headphones Start the stream
while the link sleeps, media flows at sniff's rhythm until the wake
completes, a second at most. The wrapper holds only our own Start.

**Whether these headphones ask for sniff again is not known.** On the P5
they asked on a link that allowed it from the first moment; here their
first requests are refused for ten seconds. If they do not ask again, the
link simply stays awake, as it does today, and § Proof's first step says
so. The remedy then is to ask for sniff ourselves (Sniff Mode, which a
peripheral may request), which this part does not build.

### The commands (source: Linux `hci.h`)

| Command | Opcode | Parameters | Answer |
|---|---|---|---|
| Write Link Policy Settings | 0x080d | handle, policy (role switch 0x0001, sniff 0x0004) | Command Complete: status, handle |
| Exit Sniff Mode | 0x0804 | handle | Command Status, then Mode Change (0x14: status, handle, mode, interval; mode 0 active, 2 sniff) |

### Where it lives, and the rules that keep the link alive

`bt_classic.c` (pure) gains the two commands. They are NOT like its other
link commands, where an unanswered or refused command ends the link:

- **Each is an owed flag of its own**, not `next_op` (which closing
  overwrites). It is sent at the link's ordinary turn of the command slot,
  after a `next_op` and before the page-scan write, and only while the
  link is `READY` and connected. LATCHED answers go first, as always.
- **Neither can end the link.** A refusal, a malformed reply or no reply is
  recorded (status, a count) and the flag is cleared. `cl_finish` and
  `cl_timeout` treat these two opcodes apart from the rest. That is the
  link's rule; the host's is older and stays: a command the controller
  NEVER answers holds the command credit, and the host fails the
  controller three seconds past its deadline, as it does for an
  unanswered command of any link's. A READY link that sends these makes
  that reachable more often.
- **Each has its own reply shape.** Write Link Policy Settings answers
  status then handle (an eight-byte Command Complete, six of them
  parameters, a shape the link's reader did not know before part Z and
  would have counted as malformed); Exit Sniff Mode is asynchronous, like
  Create Connection.
- **A drop clears everything**: the owed flags and what was confirmed go
  with the connection (`cl_close`, `cl_give_up`, a new session), so
  nothing is sent under a dead handle and `bt_classic_quiescent` (which
  the reconnection ladder and the Classic scan ask) means what it meant.
- **The policy the controller CONFIRMED** is kept; a new link starts at
  "sniff refused, confirmed", since the default is what a new link gets.
  A write is asked when the wanted policy differs from the confirmed one.
  A refused write is asked again only when the wanted value next changes
  (so at most once a song), and is counted.
- **A Mode Change carrying an error is recorded**, not dropped, so a
  failed unsniff is seen.
- Exit Sniff Mode refused "command disallowed" is the ordinary case of the
  headphones having left sniff a moment before: counted, harmless.

`bt_host.c` decides, each tick, from state: the seam's `has_sound` through
the ops it already holds, AVDTP's state, and when the link last had
neither. The "no sound yet" answer is the host's own wrapper around the
ops' `has_sound`; the pump is not changed (it reads `has_sound` only to
decide our Start, so the quiet timer and a stream already running are
untouched).

### Lifecycle (the link's sniff policy, kept by the host)

| State | Meaning | Leaves by |
|---|---|---|
| `AWAKE` | Confirmed: sniff refused. Every new link | idle for 10 s: the allowing write is asked: `ALLOWING`. A stream wanted while the mode reads sniff (added as built): `WAKING` |
| `ALLOWING` | The write allowing sniff is owed or out | confirmed: `DOZING`. Refused or unanswered: `AWAKE`, counted, and not asked again until a stream has come and gone. Sound or a stream arrives first: `WAKING` |
| `DOZING` | Confirmed: sniff allowed. The mode may be active or sniff | sound waiting, or AVDTP in `STARTING`, `STREAMING` or `SUSPENDING`: `WAKING` |
| `WAKING` | The refusing write is owed or out; Exit Sniff Mode is sent while the mode reads sniff | the write CONFIRMED, 100 ms settled since, AND the mode reads active (as built: "Z, as built"; this table first said "answered, confirmed or not"): `AWAKE`. 1 s gone: `AWAKE` all the same, counted (`woke late`); Exit Sniff Mode goes on being retried, each second up to five times, while the mode reads sniff and a stream is wanted |

The link dropping ends every state. If the refusing write is refused,
Exit Sniff Mode is still sent; the link is then awake with sniff still
allowed, which is counted and shown, since it would explain a stream that
turns to clicks later.

### Scanning while streaming (row A.4.3)

Measured, not built: an LE scan and a 20-second Classic scan during six
songs cost no slip and no underrun on the P5. The manager does nothing
about a scan during a stream. The measurement is recorded in the tracker
and in BLUETOOTH.md; if a busier radio shows otherwise, the first thing to
try is refusing a Classic inquiry while AVDTP is streaming, which is one
line where the scan is asked for.

### Proof

Host: `bt_classic`'s suite gains the two commands (their bytes, their turn
in the slot behind a LATCHED answer and a `next_op`, each reply shape,
refused, unanswered, a failed Mode Change, the link dropping under each:
the link is never ended by them) with mutants; the multi-host suite gains
the lifecycle end to end, with model headphones that ask for sniff once it
is allowed and never leave it by themselves, as the real ones do: idle,
dozing, sound, awake before our first media packet, the song ends, dozing
again; a second pair that never ask; a stream they start while dozing; and
the keyboard typing through all of it with part S's LE assertions
unchanged.

P5: connected and silent for a minute: does slot 2 read `mode: sniff`? (If
not, they did not ask again, and that is the finding.) `play` starts
cleanly, with no clicks, and the mode reads active during the song; a
minute after it ends, sniff again.

### Z, as built

Built as above, in `bt_classic.c` (the two commands) and `bt_host.c`
(§ Slot 2's sniff: the lifecycle table, the code beneath it). What the
design left open, and what differs:

- **A write is owed when the value asked for differs from the confirmed
  one AND from a write already out.** A change asked while a write is out
  is written after it, unless that write's answer leaves the controller
  holding the value now wanted. A refused or unanswered write is not
  asked again until the value asked for changes; the host's own bar (not
  until a stream has come and gone) sits on top of that.
- **Both commands are taken by `cl_finish` and `cl_timeout` before
  anything else**, so none of the paths below them that end the link (a
  refused or unanswered command's `cl_fail`, `cl_give_up`) is reachable
  from them.
  A confirmation counts only for the attempt and handle still up. Exit
  Sniff Mode's 0x0c is counted with its other failures; the last status
  says which. Neither command arms the phase wait, whose expiry would end
  a READY link.
- **The mode reads awake when it is active OR unknown**: a link begins
  active and headphones that never sniff send no Mode Change. Hold counts
  as not awake.
- **WAKING ends only on the refusing write CONFIRMED, 100 ms settled
  since, with the mode awake**, or at 1 s (woke late). A sniff request the
  controller accepted under the allowing policy can complete after the
  refusing write is confirmed, its Mode Change in flight; ending on
  "answered, mode unknown" let our Start go into a link about to sleep.
  The settle is long enough for that Mode Change to arrive, and Exit
  Sniff Mode then answers it. A refusing write that is refused never
  confirms, so its wake runs the whole second: sniff is still allowed and
  no settle can say the headphones will not sleep.
- **Our Start is decided only on a tick whose state says the link is
  awake: AWAKE, with the mode not reading sniff** (or this stream's wake
  having run out its second). The seam's `has_sound` is live, written by
  a player on another CPU, so it is read ONCE a tick; the sniff decision
  and the pump's policy both use that reading. Two readings could
  disagree, the second seeing sound the first did not, and Start the
  pump into a link that was being allowed to sleep.
- **A stream wanted while AWAKE and the mode reads sniff goes to
  WAKING**: the refusing write was refused earlier, sniff is still
  allowed, and the headphones went back to sleep during the idle count.
  Not again for a stream whose wake already ran out its second, which
  would hold our Start for ever behind headphones that never leave sniff.
- **Exit Sniff Mode is asked whenever a stream is wanted and the mode
  reads sniff, in WAKING or AWAKE**: at once, then a second after each
  ask (the first goes on the wire behind the refusing write), five asks a
  stream. The count starts over when the link is idle again.
- **"Idle" reads the seam's `has_sound`, which answers only for the
  pump's own stream**: a player writing into the null sink before AVDTP
  is open is idle to the host. Sound becomes visible when the pump is the
  seam's sink, which is when our Start could follow it.
- **The hold is a second ops table**, given to `bt_a2dp_step` alone, on
  every tick: every op forwards to the seam's but `has_sound`, which
  answers the tick's one reading on a tick that may decide our Start and
  no on any other.
- **It runs on any READY link with an audio sink**, a `connect` session
  among them; with no bond there is no link, so nothing runs (the
  multi-host suite's ten thousand ticks with no bond send neither
  command). The decisions run in slot 2's services, before the pump's
  policy; what they ask goes at the link's next turn of the command slot.
- **Counts are kept from start-up**, the link's and the host's; the
  per-connection state goes with the connection. Slot 2's status: the
  link's `sniff:` line follows its `mode:`; the host's `sniff manager:`
  line (state, time idle, dozes, wakes, woke late, retries, refusals, and
  "awake with sniff allowed", counted once a wake) follows the link's
  text.
- **A sniff command that is never answered at all** is recorded by the
  link like any unanswered sniff command; in the host, a command with no
  reply leaves the slot held with no credit, and the controller is called
  failed three seconds after its deadline, as for an unanswered command
  of any link's (`BT_HOST_CLASSIC_REPLY_GRACE_MS`); a READY link sending
  these makes that reachable more often than the link's setup alone did.
  The multi-host case of "the wake unanswered" is therefore the Command
  Status with no Mode Change after it, which is what a controller that
  keeps its credits does.

Proof, as built: `tools/test_bt_classic_host.sh` (both commands' bytes
and reply shapes, the turn behind a latched answer, behind `next_op` and
ahead of page scan, refused, refused by status, malformed, for another
handle and unanswered with the link still READY, a failed Mode Change,
the link dropping with each owed and each out, a late answer for a dead
link, the value changing while a write is out; 36 mutants, 214 in all)
and `tools/test_bt_multi_host.sh` (the model controller takes both
commands, the model headphones either sniff as the P5's pair does or
never, and the model link in sniff delivers and completes a Classic
packet only at an anchor, so the controller's Classic buffers fill and a
stream started asleep is late: idle to dozing, sound woken before our
Start and first packet, dozing again after the song, the wake unanswered,
each write refused, headphones that never sniff, a stream they start, the
link dropping in each state, a READY link with no sink idle twenty
seconds sending neither command, the seam's answer changing between two calls
in one tick (a model seam that counts its calls: the Start does not go on
that tick, and no tick reads it twice), a sniff request in flight across
the refusing write (sound while the allowing write is out, and just after
it is confirmed), the link asleep again while AWAKE, and the whole cycle
beside a typing keyboard and a moving mouse with part S's LE assertions
at every step).

## Part R: AVCTP and AVRCP

### What the two are

- **AVCTP** carries the remote-control messages: its own L2CAP channel on
  PSM 0x0017 (recalled), a three-byte header (source: a transaction label
  of four bits, a packet type of two, command-or-response, an
  invalid-profile bit, then a 16-bit profile identifier), then the
  message. Messages too long for the channel are fragmented; ours never
  are. A browsing channel (PSM 0x001b) exists for features we do not
  claim; it is not admitted, so L2CAP refuses it.
- **AVRCP** is the messages (all source: Android's `avrc_defs.h`, BlueZ's
  `avctp.c`):
  - **The AV/C frame**: a command or response type (CONTROL 0, STATUS 1,
    NOTIFY 3; NOT IMPLEMENTED 8, ACCEPTED 9, REJECTED 10, STABLE 12,
    CHANGED 13, INTERIM 15), the subunit (panel: byte 0x48), an opcode
    (vendor dependent 0x00, unit info 0x30, subunit info 0x31, pass
    through 0x7c).
  - **Pass through**: one byte, the operation in its low seven bits (play
    0x44, stop 0x45, pause 0x46, forward 0x4b, backward 0x4c, volume up
    0x41, volume down 0x42) and RELEASED in its top bit, then a zero
    length byte.
  - **Vendor dependent**, under the Bluetooth SIG's company identifier
    (0x001958): a PDU number, a packet type, a parameter length, the
    parameters. Get Capabilities 0x10 (capability 0x02 company, 0x03
    events), Register Notification 0x31 (an event and a four-byte
    interval, zero for ours), Set Absolute Volume 0x50; the volume-changed
    event is 0x0d. Volume is seven bits, 0 to 127; the top bit is reserved
    (sent clear, masked on receive).

### Who is which

Both ends play both roles, for different things:

| For | The headphones are | We are |
|---|---|---|
| Their buttons (play, pause, next, previous) | the CONTROLLER: they send pass-through commands | the TARGET: we answer each, and hand the press on |
| Absolute volume | the TARGET: they hold the volume, obey Set Absolute Volume, and announce changes | the CONTROLLER: we ask what they support, set it, and register to hear it change |

Headphones with volume buttons and absolute volume do not send "volume up"
as a pass-through: they change their own level and announce the new one.
Headphones without absolute volume change their own level and tell nobody,
or send the pass-through; both are handled.

### What we say we are (SDP)

Our SDP responder answers for one record, the Audio Source. It gains two,
and so becomes a responder for a small table of records:

| Record | Service class | Says |
|---|---|---|
| Audio Source | 0x110A | as today |
| AV Remote Control Target | 0x110C | L2CAP PSM 0x0017; AVCTP 1.3; profile AV Remote Control (0x110E) 1.4, the first with absolute volume; features: category 1 (player) and category 2 (amplifier), so that headphones without absolute volume may send us volume presses |
| AV Remote Control | 0x110E and 0x110F (Controller) | the same protocols and version; features: category 2 (the volume's controller) |

(Versions and feature bits recalled: AVCTP 0x0103, AVRCP 0x0104, category
1 = 0x0001, category 2 = 0x0002; checked when built.) Headphones may read
our controller record before they honour a registration for their volume,
so the records land with the protocol, in the same pull request. Every
request kind answers for every record the pattern matches (Service Search
returns each matching handle; Service Attribute takes any of the three
handles), with the continuation rules the one record has today, and the
responder's buffers grow to hold three records.

### `bt_avrcp.c` (pure)

One file holds both AVCTP and AVRCP. Pure, one per link, embedded by the host; whole messages in and out; carrying them
on L2CAP is the host's, with the take-then-commit shape AVDTP has (`next`
gives the head message and a serial, `sent` commits it), because the same
parked-message hazard exists. At least two transaction labels of ours are
live at once: a notification's registration keeps its label until the
change it waits for.

**As the target**, what we answer:

| They send | We answer |
|---|---|
| Pass through: play, stop, pause, forward, backward, volume up, volume down | ACCEPTED, and the press or release is an event for the host. A press repeated with no release between is one hold, not new presses (they repeat it while the button is down), and a press with no release is released by us after 2 s (BlueZ's figure) |
| Pass through: anything else | NOT IMPLEMENTED |
| Unit Info, Subunit Info | the panel |
| Get Capabilities (company) | the SIG's |
| Get Capabilities (events) | playback status changed, track changed: the two a player of this version must offer (recalled) |
| Register Notification: playback status changed | INTERIM with the status now (playing, paused or stopped, as the host says), and CHANGED when it changes |
| Register Notification: track changed | INTERIM with "no track selected"; we have no notion of a track |
| Get Play Status | the status; length and position unknown |
| Get Element Attributes | no attributes |
| any other PDU, or a notification we do not offer | the AVRCP reject its PDU calls for (invalid command or invalid parameter) |

The playback status exists because headphones use it to choose what their
one button sends: PAUSE to a player that is playing, PLAY to one that is
not. Without it they guess.

**As the controller** (their volume), a small state machine:

| State | Meaning | Leaves by |
|---|---|---|
| `IDLE` | No AVCTP channel | the host says it is open: `ASKING` |
| `ASKING` | Get Capabilities (events) is out | the answer lists volume-changed: `REGISTERING`. It does not, or the command is rejected or not implemented: `PLAIN` |
| `REGISTERING` | Register Notification (volume changed) is out | INTERIM, carrying their volume now: `READY`, and the volume is known. Rejected or not implemented: `PLAIN` |
| `READY` | Absolute volume works; a registration is standing | CHANGED: the new volume is known and the registration is made again (each lasts one change); the INTERIM that answers it carries their volume again, so a change made between the two is not lost. The re-registration REJECTED: `PLAIN`. A Set of ours answered ACCEPTED: the volume they took is known. Answered INTERIM: go on waiting. Answered REJECTED: counted, the volume stays what it was. Never answered: the Set alone ends, counted, and `READY` stands |
| `PLAIN` | These headphones have no absolute volume | the channel closes |
| `FAILED` | Get Capabilities or Register Notification went unanswered through two periods of 2 s (sent, then sent once more) | the channel closes; or the playback status becomes playing, which asks again from `ASKING`, at most three times a channel |

Every state returns to `IDLE` when the AVCTP channel closes, the link
staying up or not; the host says when.

`bt_avrcp_set_volume(a, 0..127)`: allowed in `READY`; one Set at a time,
the latest value wins if another is asked while one is out. Our Set and
their CHANGED may cross: whichever arrives last is believed, and the next
INTERIM settles it.

Everything it reads came over the air: every length is checked against
the bytes that remain; a fragmented message from them is reassembled to a
bound (512 bytes) or dropped and counted; an AVCTP message for a profile
that is not AVRCP is answered with the invalid-profile bit.

**Timing** (recalled): a target should answer a pass-through within 100 ms
and a control command within 200. While the link sleeps (part Z), our
answer leaves at the next sniff anchor, up to half a second later. A press
on a sleeping link is therefore answered late; it is still answered and
still counted. Waking the link for a press is not built: a press that
means "play" starts sound, and sound wakes the link.

### Proof

`tools/test_bt_avrcp_host.sh`: a model pair of headphones in both roles,
every message built from the specification and compared byte for byte;
headphones with absolute volume and without; a volume change announced
while our Set is out, and one announced between a CHANGED and our
re-registration; a Set answered INTERIM then ACCEPTED; every reject; a
held button and a button never released; the playback status registered
and changed; fragments at every byte; random bytes under the sanitizers; a
responsive random peer that must reach `READY`; mutants.
`tools/test_bt_sdp_host.sh` gains the table of records: each found by its
class, all three by the public browse root, each handle's attributes, a
handle that is none of ours, continuation across records, the cross check
with the client's walker.

### R, as built

`bt_avrcp.c` and its header are the code; `tools/test_bt_avrcp_host.sh`
(201 mutants) and `tools/test_bt_sdp_host.sh` (156) the proof. Nothing
calls it until part W. What differs from the tables above, or was left
open by them:

- **A Set is sent once.** A Set Absolute Volume that gets no answer waits a
  second period of 2 s and is not sent again: sending a level twice can
  undo a button pressed between the two. One that is never answered ends
  alone, counted (`silent`); the registration that stands is evidence
  enough that the headphones are alive. Get Capabilities and Register
  Notification ARE sent a second time, and it is those going unanswered
  that is `FAILED`.
- **An answer to either copy is the answer.** A command sent once takes its
  answer while its second copy still waits to leave, and that copy is then
  withdrawn.
- **`sent` finds the message by its serial**, not by its place: an answer
  owed to a command of theirs that arrives while a message of ours is
  parked with the carrier goes ahead of it, and the parked one is still
  committed when the carrier says it left. Nothing goes out twice.
- **A Set answered INTERIM** waits a fresh 2 s for the answer proper.
- **A second INTERIM** (no CHANGED between) is taken as their level.
- **Answers owed are bounded at eight**, one of them kept for a CHANGED of
  ours; a command of theirs beyond that is dropped and counted.
- **Lengths**: a message shorter than its PDU needs is malformed; one that
  carries more than it needs is read for what is needed. Headphones that
  pad are not failed for it.
- **`FAILED` is retried when sound starts**: the playback status becoming
  playing asks again from `ASKING`, at most three times a channel, since
  headphones busy at connect are the likely cause.
- **Unit Info** names the Bluetooth SIG as the company. **Get Element
  Attributes** answers zero attributes. **The track identifier** is 0
  while playing and all ones otherwise, and no track CHANGED is sent when
  playback starts.
- **Presses** reach the host through a ring of sixteen; a press enters
  only when there is room for its release, and loss is counted.
- **`bt_avrcp_channel_open` comes before the first `receive`**: a message
  received while `IDLE` is dropped unread and counted.
- **The SDP responder** answers for three records, handles 0x00010001
  (Audio Source), 0x00010002 (target, features 0x0003) and 0x00010003
  (controller, classes 0x110E and 0x110F, features 0x0002), AVCTP 1.3 and
  AVRCP 1.4 in both, as Android's own records are laid out.

Known and left: a NOT IMPLEMENTED or REJECTED echo carries at most 26
operands of theirs; a pass-through for a subunit that is not the panel is
REJECTED; BlueZ waits 10 s for a control answer where we wait 2 s twice.

## Part W: the volume wired

### L2CAP and the host

- **The channel table grows from four to six.** Signalling, media, their
  SDP and AVCTP are four at rest, and an SDP query of ours or a crossing
  channel needs a fifth for a moment. Each channel carries a 1 KB payload
  buffer; the host is on the heap since A.3.
- **PSM 0x0017 is admitted** for their calls, on an encrypted link, with
  the same guard and the same "decided from state" rule AVDTP's channels
  have. One AVCTP channel.
- **Who opens it.** They are the link's central and open what they want
  promptly, so we wait 2 s after AVDTP's signalling channel is open for
  theirs, and ask ourselves only if none exists by then, theirs still
  connecting included. If both open, THEIRS is kept and ours closed: "the
  one that opened second" is judged differently at each end and can leave
  none. If the channel ends with the link up, we ask once more, a second
  later.
- AVRCP is fed, ticked and drained each tick beside AVDTP, under the same
  send gate, after the LE peers.
- **The playback status** the target answers with is the host's to say,
  from state: playing while AVDTP is streaming and the seam has had sound
  within the quiet rule; paused when the seam says paused; stopped
  otherwise.

### Who owns the volume

**When the headphones have absolute volume, `/dev/volume` IS their
volume.** One number, one place to set it, and the buttons on the cup move
the same number.

**The rule that comes first: connecting never makes anything louder.**
When ownership begins, `/dev/volume` takes THEIR level; nothing is sent to
them. The first design set the headphones to whatever `/dev/volume` held,
which is 100 on a machine nobody has touched: full volume, in someone's
ears, the moment the link came up, with no hand on anything. After this
rule, the headphones get louder only when a person writes a larger number
or presses their own button.

Ownership is a STATE, decided every pass, never an edge: the sink owns the
volume while AVRCP is `READY` and the pump is the seam's sink (it is the
sink only from the stream's `open` to its end, and the seam ignores a sink
that is not the sink). The seam itself drops ownership whenever a sink
attaches or detaches, so no later sink can inherit a seam that applies no
gain.

| Event | What happens |
|---|---|
| Ownership begins | `/dev/volume` becomes their level (their INTERIM's 0 to 127, as 0 to 100). THEN the seam stops applying its own gain. Headphones that say 0 read 0 |
| A write to `/dev/volume` | The seam's number changes; the host, which reads the seam's volume every pass while it owns it, sees the change and sends Set Absolute Volume |
| The volume buttons on the cup | They announce CHANGED; the host puts their level in the seam; nothing is sent back |
| Ownership ends (the stream's session ends, AVRCP leaves `READY`, the link drops) | The seam applies its gain again, at the number it holds. That can only be quieter or the same |
| AVRCP is `PLAIN` | Today's behaviour: `/dev/volume` is the seam's gain, their buttons are theirs. If they send volume presses, each moves `/dev/volume` by 5 |

**One raw value is tracked, so the two ends never chase each other.** The
host keeps the last 0-to-127 value both ends are known to share.

- 0 to 100 becomes 0 to 127 by rounding `v × 127 / 100`, and back by
  rounding `x × 100 / 127`. Every one of the 101 values survives the round
  trip, so a volume written as 50 reads back 50. The reverse is not true:
  27 of their 128 levels do not survive. So:
- a change at our end sends a Set of the converted value;
- a CHANGED or INTERIM from them sets the seam to the converted value and
  sends NOTHING (sending it back would nudge them a step, they would
  announce it, and so on);
- what a Set's answer says they took is written back only if it differs
  from what the seam's number converts to.

**The curve changes hands with the number.** The seam's own gain is the
square of the fraction, chosen so that a slider feels even. Owned, the
number goes to the headphones as it is and their own curve applies. So
"30" owned and "30" plain are not the same loudness. That is the cost of
one number.

Why not keep both (a gain AND their level): two volumes multiplied
together is the thing everybody has fought with on some other system,
where one is at 30% and the other at 100% and neither is loud. And full
scale into the encoder is the best signal SBC can be given.

The seam grows three calls, reached by the host through three new ops
beside the ones the pump has (the host is pure and knows the seam only
through them), each taking the stream's lock and none called from a
nudge:

- `audio_sink_owns_volume(sink, bool)`: while true, the pull applies no
  gain. A write to `/dev/volume` while owned changes the number and leaves
  the gain at unity; releasing ownership recomputes the gain.
- `audio_sink_volume_set(sink, 0..100)`: the sink says the volume is this
  at its end.
- `audio_sink_volume_now(sink)`: the number, read by the host each pass
  while it owns the volume. A write to `/dev/volume` also nudges the sink,
  only so that pass comes sooner; the pass decides from what it reads, not
  from having been nudged.

`/sys/audio` says who owns the volume.

### Their buttons

Play, stop, pause, forward and backward presses are ANSWERED by part R and
reach the host as events. This part shows them (slot 2's status: the last
press, and a count of each) and logs each under `DEBUG_USB`. **What a
press DOES is not decided here**: Chris and I talk about that on
2026-11-14, and it has consumers this arc does not own (`play`, yonder's
media element). The events are kept in a form a consumer can be given
without touching the protocol again.

### Proof

Host: the multi-host suite's headphones gain AVCTP and both AVRCP roles:
`READY` to a known volume with no typing, and `/dev/volume` reading THEIR
level with nothing sent to them; `/dev/volume` at 100 and headphones at 40
at connect: they stay at 40; a write reaches them as a Set and reads back
unchanged; their button moves `/dev/volume` and nothing is sent back; a
written level they round differently does not ping-pong; ownership returns
to the seam when the session ends, when AVCTP closes with the stream up,
and when the link drops, and begins again on a second session under a
standing AVRCP; headphones with no absolute volume leave everything as it
is; presses arrive, are answered and counted; both ends opening AVCTP; all
beside a typing keyboard with the LE assertions unchanged, and the channel
table's peak asserted against six. The audio-stream suite gains the
ownership rule with mutants.

P5, by Chris:

1. Connect the headphones, then `cat /dev/volume`: it reads their level,
   not 100, and nothing got louder.
2. During a song, `echo 30 > /dev/volume`: quieter at the headphones.
   `echo 100`: their maximum.
3. Press volume up and down on the cup during a song: it changes, and
   `cat /dev/volume` follows.
4. Press the middle button (play/pause; twice for next; three times for
   previous, as these headphones do it): nothing changes yet, and slot 2's
   status counts each press under its name.
5. Type and move the mouse through all of it.

## What A.4 leaves out, on purpose

- **What play, pause, next and previous DO.** Heard and counted here;
  given a meaning after the conversation that decides it.
- Metadata (the song's title on a headset's display), browsing, the
  player-application settings, cover art: AVRCP has all of these and
  headphones use none.
- A notion of a track: track-changed answers "none".
- Asking for sniff ourselves, sniff subrating, choosing the interval:
  named as the remedy if these headphones do not ask again.
- Waking a sleeping link to answer a button sooner.

## For Chris

Taken as leaned, since you asked to be left to sleep; each is one place to
change:

1. **`/dev/volume` becomes the headphones' own volume** when they support
   being set, and the seam's gain steps aside. One number.
2. **Connecting never raises the volume.** `/dev/volume` adopts THEIR
   level when ownership begins. So after connecting it will read what the
   headphones were at, not 100.
3. **A link earns sniff after ten idle seconds**, and is woken before our
   stream starts, waiting at most a second for it.
4. **AVRCP 1.4 is what we claim**, the lowest version with absolute
   volume, with the two notifications a player of that version owes.
5. **Button presses are answered and counted, and do nothing yet.**
