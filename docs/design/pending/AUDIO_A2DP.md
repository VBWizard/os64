# AUDIO_A2DP.md — the design for A.3: sound plays

*Fable, 2026-11-13. The design row A.3.1 of `AUDIO_TRACKER.csv`.
BLUETOOTH_AUDIO.md (§ AVDTP and A2DP, § SBC, § Where the encoder lives,
§ Pacing, § Latency) is the argument and carries Chris's rulings and
Quinn's review; AUDIO_SEAM.md is the seam this plugs into; AUDIO_CLASSIC.md
is the link it rides. This file is what gets built, checked against the
tree at `50feceaa`.*

*Status: read once by someone other than the author (a cold Opus read
against the source, BlueZ's `profiles/audio/avdtp.c` and Android's encoder,
2026-11-13; its findings are folded in). Chris ruled the encoder, the
headphones-off behaviour and the three pull requests the same day (§ For
Chris). A fact marked (recalled) is from memory of the specifications and
is checked against public sources by whoever builds that part.*

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

- **They never page us.** We make the LINK on every connection. Who then
  starts AVDTP is a separate matter: many headphones open it themselves a
  moment after any link comes up, so the acceptor's half and the rules for
  both sides starting at once may be needed on this very pair.
- **They hang up two seconds after their last L2CAP channel closes.** So
  AVDTP's signalling channel is opened FIRST, and only then is the SDP
  channel of part S closed. A suspended stream keeps its channels. The one
  place A.3 closes the last channel on purpose is to recover from a session
  that went wrong (§ When a session goes wrong), where their hanging up is
  what brings a clean start.
- **They report AVDTP 1.3, A2DP 1.3, features 0x0001, PSM 0x0019.** 1.3 is
  the version that has Get All Capabilities and delay reporting.
- **Their SDP channel's MTU is 48.** Small MTUs are real; the media
  channel's MTU is read, never assumed.
- **The controller's Classic pool is 4 packets of 1,021 bytes**, and the
  transport has four send slots, one always left for LE.

## Where the code lives

| File | What | Kind |
|---|---|---|
| `kernel/ports/sbc/` | The encoder: `upstream/` (untouched), `port/` (the shim headers and the boundary), `manifest.json`, `README.md`, its own make rules | Ported |
| `kernel/src/audio/sbc_frame.c`, `kernel/include/audio/sbc_frame.h` | Frame length and bit rate from the settings; the settings' validity. Ours, tiny, used by V, P and the port's checks | Pure |
| `kernel/src/driver/system/usb/bt_avdtp.c`, `.h` | AVDTP signalling, both roles | Pure |
| `kernel/src/driver/system/usb/bt_a2dp.c`, `.h` | The pump: the stream's policy, the sample clock, RTP, the seam's sink | Pure (the seam, the clock and the encoder reach it through callbacks) |
| `bt_host.c/.h`, `bt_l2cap.c/.h`, `xhci.c`, `audio.c` | The wiring, and the changes below | Existing |
| `tools/test_sbc_host.*`, `test_bt_avdtp_host.*`, `test_bt_a2dp_host.*`, and their mutant harnesses | Proof | Host |

`kernel/ports/` is new: third-party code in the kernel has a place of its
own, laid out as `userland/libsound/` is, so that what is ours and what is
not can never be confused in a diff. `THIRD_PARTY.md` gains its row.

## Part E: the encoder

### Which one (ruled by Chris, 2026-11-13)

**Android's SBC encoder** (`system/embdrv/sbc/encoder` in the Android
Bluetooth module; Copyright 1999-2012 Broadcom Corporation; Apache License
2.0). Read file by file on 2026-11-13:

- **C, eight source files and four headers**, about 3,000 lines, the
  largest being the analysis filter's macros.
- **Integer-only in its default configuration.** No `float`, `double` or
  `<math.h>` in code. One file, `sbc_dct_coeffs.c`, holds floating-point
  LITERALS (`(int16_t)(0.7071 * 32768)`) in tables the default switch
  compiles out (`SBC_FAST_DCT` true); compiled in, they would be constant
  initialisers the compiler folds, with no floating-point instruction. The
  kernel is built `-mno-sse` but not without the x87, so floating point
  added by mistake would compile; the audit reads the objects for
  floating-point instructions so that it cannot pass.
- **Allocates nothing.** One libc call in all of it, a `memset` at
  initialisation. One compiler builtin, `__builtin_mul_overflow` in
  `sbc_packing.c`, which the cross compiler has.
- **Supports what A.3 uses**: 48 kHz, joint stereo, 16 blocks, 8 subbands,
  loudness allocation.

Its rough edges, and what the port does about each:

| Edge | Handling |
|---|---|
| State in file-scope statics and three globals: one encoder in the machine | There is one stream in the machine (AUDIO_SEAM.md). The boundary says so: a second `begin` restarts the one encoder with its settings |
| `SBC_Encode(params, pcm, out)` takes no output capacity | The boundary encodes into its own buffer, sized for the longest frame any setting it ACCEPTS gives (513 bytes, at bitpool 250; the bitpools upstream allows above that are refused, not fitted), with guard bytes after it. A length that is not the one the settings give, or a disturbed guard, fails the sink |
| **Its knob is a bit rate, not a bitpool.** `SBC_Encoder_Init` computes the bitpool from `u16BitRate`, corrects it once, clamps it, and writes it over whatever the caller put there | AVDTP agrees a BITPOOL with the headphones. The boundary takes the bitpool, gives the encoder the bit rate that upstream's own formula turns into it, and refuses to begin unless the bitpool the encoder then holds is exactly the one asked. Whether writing the field after Init is also sound is settled by reading Init when the port is built, and recorded in the README; the check stands either way |
| It reads one buffer through two pointer types (a 16-bit view of 32-bit storage) | Built `-fno-strict-aliasing` |
| An unrecognised sampling-frequency value silently means 48 kHz | The boundary validates every setting before the encoder sees it (`sbc_frame.c`) |
| `SBC_Encode` takes its PCM through a pointer that is not `const` | Read when built: if upstream never writes through it, the boundary casts and says why; if it does, the boundary copies |
| mSBC is half supported | Not offered by the boundary |

**How it builds.** The kernel's own flags are `-O0 -Wall -Wextra -Werror`
against freestanding headers. Upstream code is not ours to make
warning-clean, and `-O0` would make a filter bank several times slower
than it need be. So the port has its own rules, stated in its makefile and
README: the kernel's machine flags (`-mno-sse` and the rest) kept, `-O2`,
`-fno-strict-aliasing`, warnings on but not errors for `upstream/` alone,
and a shim `string.h` in `port/` that declares the kernel's `memset`.
`port/`'s own files are built as the kernel's are. No upstream file is
modified.

**The licence** is Apache 2.0, which os64 (MIT) may carry: its notice goes
in `license/android-sbc-LICENSE` with the files' own copyright lines, and
the README states what was taken and from which commit.

The alternatives: BlueZ's `libsbc` and FFmpeg's encoder are LGPL. os64's
source is public, so LGPL could be honoured; the cost would be a kernel
that is no longer MIT throughout. FFmpeg's encoder is also written against
FFmpeg's own framework and would need that framework replaced, where
Android's is standalone. Both are fine as HOST references, and FFmpeg is
what the proof uses. The boundary below is narrow so that another encoder
can replace this one by changing one directory.

### The boundary

```c
// kernel/ports/sbc/port/sbc_port.h — everything the kernel may call.
// (The settings live in kernel/include/audio/sbc_frame.h, which it includes.)
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

// Starts the one encoder. False: the settings are not ones it takes, or
// the encoder did not settle on the bitpool asked for.
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

`tools/test_sbc_host.sh` builds the port's own sources with the port's own
switches, under the sanitizers, and:

1. **Every setting the boundary accepts**, and a sweep of bitpools for
   each: `begin` settles on the bitpool asked; the frame length is the one
   `sbc_frame_bytes` gives; the header bytes say what was asked; the guard
   is intact.
2. **Decoded by FFmpeg's SBC decoder**, which shares no code with this
   encoder, with decoding errors fatal and the count of frames out equal to
   the count in, so that a frame the decoder had to conceal fails the test
   and is not averaged away. Signals: a sine sweep, white noise, full-scale
   square waves, a few seconds of music-like signal, and silence.
   - The error against the input is measured after aligning for the filter
     bank's delay and leaving out the start-up transient; the alignment is
     stated in the test.
   - The bound is FFmpeg's own ENCODER at the same settings: ours may be
     no more than 3 dB worse. The test asserts from the frame headers that
     FFmpeg's stream really carries the settings under test.
   - Silence has no ratio to compare: it must decode to within a few least
     significant bits of zero.
   - There is no absolute error bound in the specification to assert. An
     encoder's conformance there is a comparison with a reference
     encoder's output; this is that comparison's audible form.
3. **The side information is checked, not only the sound.** A wrong
   joint-stereo decision or a swapped allocation method can leave the
   ratio close. For known inputs (identical channels, opposite channels,
   one channel silent) the test reads each frame's header, join flags and
   scale factors and asserts what they must be.
4. **Random PCM, random valid settings, thousands of frames**, under
   ASan and UBSan. If UBSan objects to an upstream fixed-point idiom (a
   left shift of a negative value is the usual one), the finding is
   recorded in the README with the instruction it compiles to, and that
   one check is turned off for `upstream/` alone; nothing is patched to
   quiet it.
5. **Speed**, at the port's flags: a frame's cost is printed and bounded,
   so the pump's budget (§ The sample clock) rests on a number.
6. **The audit** (`tools/test_sbc_audit.py`): `upstream/` is byte for byte
   the pinned commit's files; the kernel objects contain no SSE or x87
   instruction and no undefined symbol but `memset`.

**The suite FAILS, by name, on a machine with no `ffmpeg`.** Other suites
skip when a tool is missing; this is the only proof the first third-party
code in ring 0 has, so it is not allowed to pass by not running.

### E, as built

`kernel/ports/sbc/README.md` is the record (the commit, every switch, what
was read upstream, what the proof found). Where it differs from the design
above, and why:

- **The pin** is commit `6b3bed84` (2025-01-09), the newest on `main` that
  touches the directory; the files taken last changed in November 2024.
  Files are pinned by git blob id as well as SHA-256, and each directory by
  its tree id, because Gitiles' tarballs carry the time they were made and
  have no stable digest.
- **`sbc_settings_t` lives in `kernel/include/audio/sbc_frame.h`**, which
  `sbc_port.h` includes: V and P need the settings without the encoder.
- **The bitpool's range is 2 to 250**, the range an A2DP configuration can
  name (BlueZ's `SBC_MIN_BITPOOL`/`SBC_MAX_BITPOOL`), inside the frame's own
  limit of 32 × subbands. What a pair of headphones takes is AVDTP's to
  agree; the encoder is proven over the whole range, so the boundary's
  buffer is 513 bytes.
- **A second `begin` restarts the one encoder** rather than being refused:
  a session made again simply begins again, and there is no verb to end
  one. The caller serializes.
- **The bit rate is found by asking `Init`**, halving between 1 and 4,096
  kbit/s, rather than by restating its formula; the exact-bitpool check is
  kept, and proven by a build whose search cannot reach the bitpool asked.
  Writing `s16BitPool` after `Init` would also have been sound: `Init`
  derives nothing else from the bit rate.
- **The PCM is cast, not copied**: upstream only reads it.
- **The switches are written out** in `port/sbc_switches.h`, force-included
  by every build, each at upstream's default; the audit fails if any
  upstream file tests a name in a preprocessor condition that the file does
  not set (include guards and `__cplusplus` aside).
- **Upstream compiles without a warning** under `-Wall -Wextra`, so "shown,
  not fatal" has nothing to show today.
- **UBSan found one idiom**, a left shift of a negative value at five lines
  of `sbc_dct.c`'s fast DCT; that check (`shift-base`) is off for
  `upstream/` alone in the host build, and the README says why it is benign
  under GCC.
- **Against FFmpeg**: the delay through either encoder and FFmpeg's decoder
  is 73 samples at every setting; ours is at worst 1.06 dB below FFmpeg's
  encoder (full-scale squares at bitpool 250, both above 67 dB) and usually
  within 0.3 dB either way. FFmpeg's encoder has no SNR allocation and
  picks joint or plain stereo from the bit rate (`-b:a`), the bitpool from
  `-q:a`; the suite checks its headers say the settings, and holds SNR
  settings against its loudness stream.
- **FFmpeg 6.1's `-xerror` exits 0** after a frame it could not decode. The
  frame count is the check that holds, and the suite proves it fails on a
  stream with one bad CRC.
- **A frame costs about 3 µs** on the build machine at the port's flags
  (0.11% of a core in real time); the bound asserted is 200 µs.
- **The kernel links the objects and calls nothing yet**, so the binary
  carries none of the encoder until P; the licence goes on the images then
  (DEBTS).
- **The audit builds the kernel and reads it**: it proves its instruction
  scan sees x87 and SSE code in a probe built the same way; checks the
  upstream objects' recorded compiler flags (DWARF producer) for the
  kernel's; and enforces "nothing above `bt_a2dp.c` knows an encoder
  exists" from the symbols: no object outside `ports/sbc` may reach a
  symbol upstream defines, and only objects named in its
  `BOUNDARY_CALLERS` may call the boundary (empty until P adds the pump).
- **A second `begin` is proven to restart**: the same PCM after a begin
  gives the same bytes whether other settings or the same ones ran before.
- **The host proof runs the host compiler's build** of the same sources;
  the kernel's objects are audited, not executed, until P runs them.

## Part V: AVDTP (`bt_avdtp.c`)

Pure, one per Classic link, embedded by the host. It reads and writes
whole signalling messages; carrying them on an L2CAP channel is the host's.

### The wire

Checked against BlueZ's `avdtp.c` where marked (BlueZ); otherwise
recalled, and checked when built.

- **A single packet's header** (BlueZ): byte 0 is the transaction label
  (bits 7 to 4), the packet type (bits 3 and 2: single, start, continue,
  end) and the message type (bits 1 and 0: command, general reject,
  response accept, response reject); byte 1 is the signal identifier in
  its low six bits.
- **Fragments** (BlueZ): a START packet's byte 1 is the number of packets
  in the whole message, the start included, and its byte 2 is the signal
  identifier; continue and end packets carry no signal identifier. We
  reassemble what they send. We never SEND a fragmented message: our
  longest is under twenty bytes and the least MTU is 48.
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
  A CAPABILITY sets every bit it supports; a CONFIGURATION sets exactly
  one bit in each field, and a bitpool range.
- **Rejects differ by signal** (recalled): Set Configuration's carries the
  category at fault and an error code; Start's and Suspend's carry the
  endpoint at fault and a code; Open's, Close's and the rest carry the
  code alone; Abort has no reject at all. The host suite compares bytes,
  so each is written out there from the specification.
- **Delay reporting is offered only in the answer to Get All
  Capabilities** (AVDTP 1.3, recalled), so the two answers differ.

### What we are

One local endpoint: identifier 1, an audio SOURCE, SBC. Its capabilities
are what the pump can stream: 48 kHz (the seam's rate; the encoder would
take 44.1 kHz, the seam cannot feed it), joint stereo and stereo, 16
blocks, 8 subbands, loudness, bitpools 2 to 51; plus delay reporting in
the Get All Capabilities answer.

### The session's states (the lifecycle table comes first)

"Sig" is the signalling channel; "media" the second channel to the same
PSM.

| State | Meaning | Leaves by |
|---|---|---|
| `IDLE` | No signalling channel | the host says sig is open: `CONNECTED` |
| `CONNECTED` | Sig up, no stream, nothing outstanding | we begin: `DISCOVERING`. Their Set Configuration, accepted: `CONFIGURED` |
| `DISCOVERING` | Our Discover is out | its answer: the first audio SINK endpoint not in use is chosen; none is a walk that ended (below). Their Set Configuration, accepted: `CONFIGURED`, and ours is abandoned |
| `ASKING` | Get All Capabilities (when their SDP record says AVDTP 1.3 or later) or Get Capabilities is out | its answer: SBC offering 48 kHz, 16 blocks, 8 subbands, joint stereo or stereo, loudness, and a bitpool range that meets ours; else the next sink endpoint; none left is a walk that ended (below). Get All Capabilities REJECTED: Get Capabilities is asked instead. Their Set Configuration, accepted: `CONFIGURED` |
| `CONFIGURING` | Our Set Configuration is out: media transport, one SBC setting in each field, the bitpool range from their least (or 2, if higher) to the one chosen, and delay reporting if they offered it | accepted: `OPENING`. Rejected "in use" or "bad state" while we have just rejected theirs: once, after 1 s, sent again. Any other reject: § When a session goes wrong |
| `CONFIGURED` | We accepted THEIR Set Configuration; they are to open the stream | their Open, accepted: `AWAITING_MEDIA`. 10 s without it: Abort, then `CONNECTED` |
| `OPENING` | Our Open is out | accepted: the host opens media, `AWAITING_MEDIA` |
| `AWAITING_MEDIA` | Open is agreed; the media channel is not up yet (ours being made, or theirs awaited) | the host says media is open: `OPEN`. The media channel refused, or 10 s without it: Abort, then § When a session goes wrong |
| `OPEN` | Configured, both channels up, not streaming | Start, ours or theirs |
| `STARTING` | Our Start is out | accepted: `STREAMING`. Rejected: `OPEN`, counted, and the pump decides when to ask again (§ The stream's policy) |
| `STREAMING` | Media may be sent | Suspend, ours or theirs; Close; Abort |
| `SUSPENDING` | Our Suspend is out | accepted: `OPEN`. Rejected: `STREAMING` still (a reject leaves the stream as it was), counted |
| `CLOSING` | Our Close is out | its answer or its timer: the host closes media; `CONNECTED` |
| `ENDING` | A session went wrong and the host is closing both channels; nothing is taken | sig closes: `IDLE` |
| `FAILED` | A second session went wrong on this connection; why is kept | the link drops |

**A walk that ended is not a failure.** "No sink endpoint", "no sink
endpoint free" and "no settings in common" return to `CONNECTED` with the
reason kept and shown. Their own Set Configuration is still judged on its
merits there, so headphones that would rather configure us can. We walk
again only after "no sink endpoint free", thirty seconds later and for as
long as it stays so (headphones whose sink a phone is holding); after the
other two, not on this link.

From any state from `CONFIGURED` on:

| Event | Result |
|---|---|
| their Close, accepted | the host closes media; `CONNECTED`. We do not begin again by ourselves: they chose to end it, and a new session is theirs to ask for or the next connection's |
| their Abort | answered; media closed; `CONNECTED`, likewise |
| the media channel closes by itself | Abort; `CONNECTED` |
| sig closes, the link staying up | everything cleared; `IDLE`; counted as a session gone wrong only if there was a session (from `CONFIGURING` on) or a command of ours had been sent |
| the link drops | everything cleared, silently; `IDLE` |

**Timers** (BlueZ's figures): a command we send waits 6 s; Suspend 10 s;
Abort 2 s, and Abort is not waited for beyond that.

**Labels.** A response is taken only if its label and signal match the one
command outstanding; anything else is counted as a stray. We send one
command at a time.

**Answers owed and one command may wait.** An L2CAP channel holds one
payload. While our command waits its turn, they may send something that
must be answered. `bt_avdtp.c` keeps up to two answers owed and one
command to send, and hands the host the answers first each time the
channel can take a payload. A peer sends one command at a time, so a
third message while two answers are owed is dropped and counted; except
an Abort, which is always obeyed and takes the newer answer's place.

### When they do it to us

| They send | We answer |
|---|---|
| Discover | Our one endpoint, with whether it is in use |
| Get Capabilities, Get All Capabilities | Our capabilities (the second with delay reporting); a wrong endpoint is rejected (bad ACP SEID) |
| Set Configuration | In `CONNECTED`, `DISCOVERING` or `ASKING`: accepted when it names one SBC setting in each field that the boundary takes, a bitpool range that meets ours, and no category we lack; else rejected with the category and code. In `CONFIGURING` or later: rejected (endpoint in use) |
| Get Configuration | What is configured; rejected (bad state) when nothing is |
| Open, Start, Suspend, Close, Abort | By the state table; a signal the state does not allow is rejected (bad state) |
| Delay Report | Taken in any state from `CONFIGURED` on, under our endpoint's identifier or the sink's: there is one stream, and refusing it would lose the delay (§ The delay) |
| Reconfigure | Rejected (not supported command) |
| Security Control, anything else | General Reject |

### When a session goes wrong

A command unanswered, a reject we cannot act on, a channel that will not
open, or sig closing under us. One rule:

1. Abort if a stream is configured; close media and sig.
2. With these headphones the link then drops within seconds and the
   reconnection ladder brings a fresh connection and a fresh session.
3. With headphones that keep the link: the host opens sig again after
   10 s, once. A second session gone wrong on the same connection is
   `FAILED`, which stays until the link drops.

### Bounds

A reassembled message is at most 256 bytes and four fragments; longer is
dropped and counted. A capabilities list is walked with every length
checked against what remains; an element that overruns ends the walk as
malformed. At most eight endpoints are read from a Discover response.

### Proof

`tools/test_bt_avdtp_host.sh`: a model sink that offers capabilities,
accepts or refuses each step, answers late, never answers, fragments its
answers at every byte, and starts the profile itself, alone and while we
do, at every point in our own sequence; every message built by the model
from the specification and compared byte for byte, each reject's layout
included; every row of the three tables above reached; random bytes under
the sanitizers; a mutant harness.

### V, as built

- **The wire was checked** against BlueZ (`profiles/audio/avdtp.c`,
  `a2dp-codecs.h`) and Android (`stack/avdt/avdt_msg.cc`): every error
  code, the header, the fragment rules, the Discover entry, the codec
  element and SBC's bits, each signal's layout and each reject's. Still
  unconfirmed by either: that a Delay Report counts in tenths of a
  millisecond. The specification's own text was not reachable.
- **Two shapes of general reject are taken**: message type 01 with the
  signal and nothing after (BlueZ), and a reject of one byte with no
  signal (AVDTP 1.0, as Android reads it), which matches the one command
  outstanding by label alone.
- **Collisions are accepted where accepting is harmless**: their Start
  while ours is out, and their Suspend likewise; a reject of ours that
  arrives after theirs already moved the stream is not counted as a
  refusal. Their Set Configuration is taken while ours is queued and not
  yet sent, or waiting for its one retry. Both sending Open is not
  resolved: theirs is rejected (bad state).
- **A command of ours still queued is retired when we accept a command
  of theirs that changes the stream** (their Set Configuration, Open,
  Start, Suspend, Close, Abort): it describes a stream that has since
  changed. One already SENT is theirs to answer, and its answer is taken
  without changing a state it no longer applies to. What that leaves: our
  Start sent, their Start and then their Suspend accepted, and only then
  their accept of ours. Whether they are streaming depends on the order
  they handled the two, which nothing on the wire says (DEBTS).
- **An Abort of ours and their new configuration**: an Abort still queued
  is dropped when their Set Configuration is accepted (it names the
  endpoint they have just configured); one already sent keeps its timer
  and its answer changes nothing.
- **What their Set Configuration is refused with**: a codec, field or
  category we lack, 0x29; an unknown category, 0x17; media transport with
  a length, 0x23; an element past the end of the message, 0x11; a wrong
  endpoint, 0x12. A2DP's own codec codes are not used.
- **Their Abort for another endpoint gets no answer** (an Abort is never
  rejected); in a state with no stream it is accepted and changes
  nothing.
- **What the caller reads**: the state; whether we are to open the media
  channel; whether a media channel should exist at all; whether both
  channels are to be closed; the settings agreed; their delay; and five
  counters that only go up (sessions agreed, their starts, their
  suspends, our rejected starts, our rejected suspends), so a caller that
  polls misses nothing. The counters start again when the link comes up
  or drops. `bt_avdtp_may_begin` says whether a walk may start.
- **Nothing we send is ever fragmented**: every message is built in 16
  bytes, asserted under the least MTU of 48.
- **What it does not handle**: headphones that configure us first with a
  codec we lack and do not fall back to SBC; content protection in their
  configuration (refused); both sides sending Open.
- Proof: `tools/test_bt_avdtp_host.sh`, every message compared with one
  built from the specification, messages fed in storage of exactly their
  length so a read past the end is a sanitizer report, a responsive
  random peer that must reach `OPEN` and `STREAMING`; 324 mutants.

## Part P: the pump (`bt_a2dp.c`) and the wiring

### L2CAP and the host: what changes

- **PSM 0x0019 is admitted** for their calls, and opened by us only on an
  encrypted link (the host's guard, as part S's comment says).
- **Which 0x0019 channel is which is decided by the session's state**, not
  by order (BlueZ does the same): a channel is MEDIA only while the
  session is `AWAITING_MEDIA`; otherwise it is signalling. A second
  signalling channel is closed once it opens. If we and they each begin a
  signalling channel at once, the one that opens first is used and the
  other, ours or theirs, is closed.
- **The channel table is exactly enough**: their SDP, sig, media, and for
  a moment our SDP or a crossing sig. It stays four, and the multi-host
  suite fills it.
- **The SDP channel is closed once sig is open**, not before.
- **AVDTP is tried even when the SDP query was not made.** The PSM is
  fixed by assigned numbers; without their record we ask Get Capabilities,
  as for a 1.2 sink.
- **A refusal for each channel.** L2CAP keeps the last refusal of each
  channel index, what the last channel we asked for there ended with, so
  the SDP attempt's and each AVDTP attempt's are read apart.
- **The Bluetooth probe is allocated.** `xhci_bt_probe_t` is too large for
  a kernel stack, and the pump adds about 9 KB: PCM for the most frames a
  packet may hold (15 frames of 128 stereo samples) and a packet.
  Bring-up allocates the probe before it fills it. This is the first
  commit of part P, alone, because the keyboard depends on bring-up.

### The seam: what changes

- `audio_sink_has_sound(sink)`: true when the ring holds frames and the
  stream is not paused. Opening `/dev/audio` with nothing written is not
  sound.
- The pump's `nudge` rings `kusb`'s doorbell (whose rung bit is the flag),
  and does nothing else: it runs under the stream's lock with interrupts
  off, and asking the seam anything from inside it would take that lock
  twice.

### The stream's policy

The pump is the seam's sink from `OPEN` until the session ends, and then
it detaches and the null sink takes over (ruled by Chris, 2026-11-13: a
song whose headphones are switched off carries on in real time, unheard).
A detach, like an attach, is a flush: the stream goes on under a new
generation, which `play` already rides through.

The pump decides from STATE at every pass, never from a nudge as an event.
A nudge only means "pass soon". (A nudge that arrived during `SUSPENDING`
would otherwise be lost, and a full ring never nudges again.)

| Each pass, the pump finds | It does |
|---|---|
| `OPEN`, and the seam has sound | Start, unless one is being held back (below) |
| `STREAMING`, and a pull brings frames | sends them |
| `STREAMING`, and pulls are dry | sends silence, so the gap between two songs costs nothing |
| `STREAMING`, paused | sends nothing |
| `STREAMING`, with nothing but silence or a pause for 3 s | Suspend |
| a new generation (a flush) | drops nothing of its own. At most one packet, built and waiting for L2CAP's turn, may still go: 13 ms. What is inside the headphones plays out; § Proof measures it |
| the encoder's boundary said false, or eight packets in a row cost more than their budget | `audio_sink_failed`; Close. The sink stays attached and failed, so writes answer `OS64_AUDIO_FAILED`, until the session is made again (a reconnection, or `slot 2 reconnect`) |

**Holding a Start back.** A Start that was rejected is asked again after
1 s, then 2 s, then 4 s: four Starts in all. A fourth reject gives the
session up, and the pump detaches (the null sink plays) until the session
is made again. After THEY suspend a stream, we do not Start it for 2 s:
headphones with a second host suspend us to take its call, and starting
again at once would fight them.

**A rejected Suspend** leaves the session `STREAMING`. The pump sends
nothing while there is nothing to send and carries on when there is.

**The clock is anchored again** whenever sending begins after a stop: a
Start accepted, ours or theirs, sound after a quiet spell without a
Suspend, or a slip. Their Start also begins a new quiet spell.

### The settings

48 kHz, joint stereo (plain stereo if they lack joint), 16 blocks, 8
subbands, loudness, and the bitpool the specification recommends for high
quality at 48 kHz joint stereo, 51, lowered to their maximum if that is
less. A frame is 128 samples a channel, 2.67 ms, 115 bytes at bitpool 51:
345 kbit/s. The bitpool is one constant; if § Proof shows typing suffering
for the air a song takes, the specification's middle-quality figure is the
first thing to try.

### The packet

An RTP header of 12 bytes (version 2, payload type 96, a sequence number
that goes up by one a packet, a timestamp that names the packet's first
sample and goes up by the samples it carries, a fixed source identifier),
one byte whose low four bits say how many SBC frames follow, then the
frames, whole. Frames a packet: as many as the media channel's MTU holds,
at most 15. At an MTU of 672 that is 5 frames: 588 bytes, 13.3 ms, 75
packets a second.

The sequence and the timestamp are computed in `bt_a2dp.c` from counts it
keeps, not by the encoder, and the host suite checks them against a model
packet by packet.

### The sample clock, and the one loop

The headphones give no flow control: they play at their own crystal's
rate, and buffer a hundred milliseconds or more. So the pump sends by the
clock. Packet number k is DUE at the anchor plus the time the frames
before it last, computed in 64-bit integers from the count of frames
handed over, so nothing accumulates.

Each pass of `kusb` (every tick, and every xHCI interrupt), after the LE
peers have had their turn, ONE loop, at most three times round:

1. Stop unless a packet is due, the media channel's payload slot is empty,
   and the send gate allows (the accountant gives the Classic link a
   buffer, a send slot is left for LE, no LE peer holds a packet).
2. Pull one packet's PCM from the seam. Encode it. Queue it on the media
   channel.
3. Give L2CAP its turn. If the packet left, count its frames as sent. If
   L2CAP sent something older first (a signal, an AVDTP message), go
   round again: the packet goes on the next turn.

With passes on time, one packet is due a pass. The second and third times
round are for catching up after a pass that came late.

- **Nothing is pulled that cannot be queued now.** If the controller has
  no buffer or the slots are taken, the sound waits in the ring as PCM,
  the ring fills, and the player's write parks. Sound that cannot leave
  makes the PLAYER late; it does not pile up as delay.
- **Falling behind is a slip, not a burst.** A pump more than 100 ms late
  re-anchors to now and counts a slip. Sending the backlog at once would
  overflow the headphones' buffer.
- **The encode is timed.** `kusb` can be preempted, so one slow packet
  proves nothing and is only counted; eight in a row over 5 ms each is an
  encoder that cannot keep up, and fails the sink.

### The delay

`bt_a2dp.c` KEEPS the delay: their latest Delay Report (units of 100 µs),
else 150 ms until § Proof measures this pair. It tells the seam after
every attach, since attaching sets the seam's figure to zero and their
first report arrives before the stream is open. It is an estimate of what
lies beyond the pull and does not count packets waiting with the
controller.

### What is seen

Slot 2's status gains the session: its state, the endpoint and settings
chosen, both channels, packets and frames sent, slips, slow encodes, dry
pulls, rejects with their codes, starts held back, the delay and where it
came from. `/sys/audio` names the sink `a2dp`. The log gets one line for
each session made or gone wrong, each start and suspend under `DEBUG_USB`,
and one for a failed sink.

### Proof

Host: `tools/test_bt_a2dp_host.sh` drives `bt_a2dp.c` with the real
encoder, a model seam and a clock the test owns: the cadence over an hour
of stream time with passes at ragged intervals (no drift, the sequence and
timestamp exact); buffers withheld (nothing pulled, the ring left to fill,
then a slip); L2CAP sending something older first; a pause and a resume at
every point of a Suspend; a rejected Start, three times; a rejected
Suspend; their suspend with sound waiting; a flush; three seconds of
silence; the delay kept across a reattach; every packet decoded and
compared. The multi-host suite gains the model headphones' AVDTP: `READY`
to `OPEN` with no typing beside a typing keyboard, then sound, with the LE
assertions of part S unchanged, and the channel table full. Mutants for
`bt_avdtp.c` and `bt_a2dp.c`.

P5, by Chris (rows A.3.5 and A.3.6):

1. Boot, headphones on: slot 2 reaches `ready`, the session `open`, and
   the headphones say "connected".
2. `play` a WAV: it sounds right, for its whole length. `/sys/audio`
   after: no underruns, no slips.
3. Switch the headphones off mid-song and on again: the song carries on
   unheard, and sound returns without typing.
4. **The keyboard and mouse during a song, in numbers**: `/sys/usb` read
   before and during (the wait figures and `kusb`'s pass length), and
   whether typing feels different. Then a `scan classic` during a song.
5. **A long song** (ten minutes or more): the headphones' clock against
   ours. Slips, underruns and their Delay Reports are read afterwards.
6. **The flush question** (Q7): `flush` mid-song, then play something
   else at once. Does the old sound's tail play first, and for how long?
   If it is objectionable, a Suspend and Start around a flush is tried,
   and what the pair does is recorded.
7. **The delay**: a click track and a stopwatch on the glass, to put a
   number on what "150 ms" really is for this pair.

### P, as built

The pump's pure core (`bt_a2dp.c`) and its wiring were built by Opus
builders from written briefs (`~/os64-opus-kit/BRIEF_BT_A2DP_WIRING.md` is
the wiring's). Where the code departs from Part P above, or settles what
it left open:

- **Four Starts in all.** Each rejected Start holds the next for 1, 2 and
  then 4 s; a fourth reject gives the session up, and the pump detaches
  until a new one is made. Their Suspend holds our next Start 2 s.
- **Made and sent are separate.** `bt_a2dp_packet` makes at most one
  packet and makes none while it is pending; its frames count toward the
  clock when `bt_a2dp_sent` says it reached the controller, and only under
  the anchor it was made under. The host calls `bt_a2dp_sent` from its
  L2CAP send callback, when the media channel's PDU (found by the peer's
  CID that heads it) is handed over, in whichever tick that is. A packet
  that leaves the payload slot any other way (the peer's new MTU no longer
  takes it, or one controller packet cannot carry it) is let go as sent
  and counted (`a2dp media packets dropped before the controller`), so the
  pump is never left waiting for it.
- **Their Start re-anchors the clock and begins a new quiet spell**: it
  may follow our accepted Suspend between two passes, so the state alone
  never shows the stream stop.
- **A pull that says another sink is the sink** leaves the pump displaced:
  it does not attach again until the session leaves the sink states.
- **The pump holds 7.7 KB of PCM** (15 frames of 128 stereo samples); the
  packet (up to 1,017 bytes) is the host's, and L2CAP copies it into the
  channel's one payload slot.
- **Only with a sink.** The host runs AVDTP and the pump once the
  transport has given it the ops (`bt_host_audio`); without them PSM
  0x0019 is neither admitted nor opened and slot 2 is what part S made it,
  which is how every earlier case of the host suites still runs. The ops
  live in `kernel/src/driver/system/usb/bt_a2dp_seam.c` (the seam's one
  `audio_sink_t`, `a2dp`; `sbc_port_begin` and `sbc_port_encode`;
  `micros_now`), the object `tools/test_sbc_audit.py` names as the
  boundary's caller, so `bt_host.c` still builds in every host suite with
  no kernel header.
- **Two clocks, kept apart.** `bt_host_tick` takes `now_us`
  (`micros_now`) beside the millisecond `now` (`kTicksSinceStart`). The
  pump paces by `now_us`, and AVDTP is handed `now_us / 1000` in every
  call, from the tick and from the receive path alike, so its timers and
  the pump's calls into it share one clock: the tick count loses ticks
  under load, and mixing the two would stretch AVDTP's timeouts by the
  drift. L2CAP keeps the millisecond clock.
- **Roles from state, settled after every ACL packet.** A channel opens
  only inside L2CAP's receive, so the host settles the 0x0019 roles after
  each packet L2CAP takes as well as at each tick: the channel that opened
  first is the one seen first. A role is held by index and CID while L2CAP
  holds the channel connected under that CID, so a peer reconfiguring an
  open channel does not end it. `bt_host.c` § Slot 2's audio has the
  lifecycle table.
- **Signalling at most twice a connection.** We ask once the link is
  `READY` and encrypted, the SDP query has ended and no 0x0019 channel
  exists (one of theirs still being made counts, which makes a crossing
  rarer); and once more 10 s after a signalling channel ended or an attempt
  of ours failed. AVDTP's own count makes a second session gone wrong
  `FAILED`.
- **A refused media request ends nothing it was not needed for**: when
  their media channel opened first, ours is closed when it opens, and a
  refusal of it is not passed to AVDTP.
- **A refusal is the channel index's** (`bt_l2cap_refusal`): what the last
  channel we asked for at that index ended with, started over by the next
  attempt there.
- **The loop** goes round at most three times while the media channel is
  open and the gate allows: a packet is made only when the media slot is
  empty (and one is due), and L2CAP has a turn either way, so a packet the
  gate held goes on the first turn it allows, before another is made.
  AVDTP's messages are queued before the pump's step and again after it,
  so a command the pump asks for leaves in the same tick when the
  signalling slot is free.
- **The probe is allocated** (`kmalloc_try`), filled in place, copied into
  the controller's storage and freed on every way out of bring-up; no
  memory for it releases the slot.
- **A failed controller detaches the pump**, so the null sink plays and a
  player's writes are not held.
- **The nudge rings `kusb`'s doorbell** and sets no flag of its own: the
  doorbell's rung bit is the flag, and the pump decides from state at the
  pass the ring brings.
- **The log's session line** is said once the media channel is up, so the
  frames a packet are known, not at Set Configuration.
- **Locks.** `audio_pull` and every other seam call is made from `kusb`'s
  pass, which holds USB's serializing flag (a test-and-set word nobody
  spins on) and no spinlock: `xhci.c` takes none, and the host is pure.
  The nudge, called under the stream's lock, only rings the doorbell,
  which takes no lock.
- Proof: `test_bt_multi_host` (the model headphones' AVDTP and media
  channel, a model seam, the real encoder: the session from a bond, the
  SDP query not made, ten seconds of sound at the sample clock's pace
  with every frame compared, LE beside a stream, the pool exhausted, a
  packet held by the gate, an AVDTP answer ahead of a packet, crossings
  with the table full, their session, a session gone wrong both ways, a
  drop mid-stream, pause, flush and silence, the encoder failing, no
  bond); `test_bt_l2cap_host` (refusals by channel) and its mutants;
  `test_audio_stream_host` (`has_sound`) and its mutants;
  `test_bt_loader_host` (the probe allocated and freed once in every
  scenario, a refused allocation, the host given the seam's ops);
  `test_sbc_audit` (the caller named, the notice on both images, the
  encoder in the binary).

**What only the P5 can show**: every step of § Proof's P5 list. No host
test runs `bt_a2dp_seam.c` against the real `audio.c` and its lock, `kusb`
on its core with `micros_now` beside the tick, the AX210 carrying media
beside the keyboard, or the Bose pair's own AVDTP.

## What A.3 leaves out, on purpose

- Volume from the headphones and their buttons (AVRCP): A.4.
- Sniff mode when nothing plays; what the manager does about a scan
  during a stream: A.4.
- A flush timeout in the controller, so stale media is dropped on a bad
  radio: it needs signalling and media told apart on the wire, which the
  AX210's feature bits may allow. Booked in DEBTS with the first slip that
  shows it is wanted.
- Any codec but SBC; 44.1 kHz streams (the seam is 48 kHz); a second
  stream.
- Reconfiguring a stream that is open. A change of settings is a Close
  and a new session.
- A way to take back a packet already queued on a channel, which would
  make a flush exact to the packet. One packet is 13 ms.

## For Chris

Ruled 2026-11-13:

1. **The encoder is Android's, under Apache 2.0**, unpatched, in
   `kernel/ports/sbc/`.
2. **Headphones switched off mid-song: the song carries on** in the null
   sink, in real time, under a new generation. An ENCODER that fails is
   the other case: the player is told.
3. **Three stacked pull requests** (E, V, P), each read cold and sent to
   Codex before the next is stacked.

Left for the first P5 run, by agreement: how long a silent stream waits
before it is suspended (three seconds here).
