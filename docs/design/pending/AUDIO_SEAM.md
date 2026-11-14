# AUDIO_SEAM.md — the design for A.1: sound as a file, with nothing listening

*Fable, 2026-11-13. The design row A.1.1 of `AUDIO_TRACKER.csv`.
BLUETOOTH_AUDIO.md § The audio seam is the argument for the shape and
Chris's rulings (its § Open questions 1 to 6); this file is what gets built,
checked against the tree at `a86b0f18`. It settles Q8 (the position's
contract) and leaves Q7 (what a flush does inside the headphones) where it
was: a proof for A.3 on the real pair.*

*Status: BUILT, the kernel half (the first of the two pull requests). Read
once before code by someone other than the author (a cold Opus read against
the source, 2026-11-13; its fourteen findings are folded in, the largest
being that the seam no longer calls into a sink at all), and cleared by
Chris the same day. § As built says where the code departs from the text
above it; § For Chris records what was put to him.*

## What A.1 is

A program writes PCM to `/dev/audio` and it is consumed at the rate a real
output would consume it, with nothing attached. Everything a player needs
is there and proven sample for sample in QEMU: the write that waits and the
write that does not, the playback position, pause and flush, volume, and
the report. No Bluetooth, no codec, no driver.

The slice lands as **two pull requests**, because they are different kinds
of work and want different reviewers:

1. **The kernel seam** (rows A.1.2 to A.1.9, A.1.11, A.1.12). Kernel
   concurrency and a ring-boundary: Codex reads it.
2. **`play` and the resampler** (A.1.10, A.1.13). Userland; reviewed here.

The three nodes Chris asked about stay in one pull request. They are three
faces of one object (a ring and its counters), each a few dozen lines once
the object exists; reviewed apart, the ring would be read three times and
its invariants never once whole.

## The names

| Name | What it is | Who may use it |
|---|---|---|
| `/dev/audio` | The stream. Write PCM. | One open at a time |
| `/dev/audiopos` | The playback position: one fixed record per read | Anyone |
| `/dev/audiotap` | What the sink was given, as records. The proof's ear | One open at a time |
| `/dev/audioctl` | Text verbs: `pause`, `resume`, `flush`, `format ...`. Reads back the state | Anyone |
| `/dev/volume` | Text, `0` to `100`. Reads back the value | Anyone |
| `/sys/audio` | The report | Anyone |

`/dev/audio` and `/dev/volume` are Plan 9's names (and the first is Sun's,
1989). `audiopos`, `audioctl` and `audiotap` are flat beside them because
devfs is one directory deep (`dev_parse_path`), and a second level is not
worth building for three names.

**Three of these are handle kinds, not files.** A devfs file's read and
write run inside `call_in_kernel_context`, on the core's interrupt stack
under the kernel's page tables, and may not sleep (devfs.h § THE ALIAS).
`/dev/audio`'s write parks and `/dev/audiotap`'s read parks, so both are
answered at open by `devfs_handle_alias`, as `/dev/tty` and `/dev/glass`
are. `/dev/audiopos` never parks, and is an alias for a different reason:
Chris ruled the position a node "provided it costs no more than a syscall
would", and a devfs file read pays a stack switch and two CR3 loads on top
of the syscall. Through a handle kind it pays the syscall, the pin and one
short lock (§ What it costs is measured in A.1.8).

One new tag, `HANDLE_AUDIO`, carries all three. Its object is a small
`audio_open_t`: which face it is (`AUDIO_FACE_STREAM`, `_POSITION`, `_TAP`)
and two counts, kept apart as the bell and the glass view keep them: how
many HANDLES name it (one, plus one for each task it was shared into), and
how many holders keep its memory (each handle, and each operation in
flight; handle.c § The pin). The last handle's close is the CLOSE: it marks
the open closed, does the face's last-close work (§ The stream's states,
or freeing the tap), and wakes an operation parked in it, which answers
"closed" and lets go. The last holder's release frees the memory. There is
one stream in the machine, a static object, so the open carries no pointer
to it.

A stream open is shareable across a spawn, as a pipe end is
(`handle_share`), so `cat song.pcm > /dev/audio` works; the position and
the tap are not, as glass and bells are not. An open the handle table had
no room for is given back through `devfs_handle_alias_abandon`, which does
the same close, or the one-open rule would hold the name until reboot.

`/dev/audioctl` and `/dev/volume` are ordinary devfs files: short text,
never waiting, used at a person's pace. Their per-open state gains a read
offset so `cat /dev/volume` prints a line and ends.

**Nobody owns the controls.** os64 has no users to tell apart, and tying
`pause` to the task that opened `/dev/audio` breaks the first time a shell
opens it for a child (`cat song.pcm > /dev/audio`: husk opened it, cat
writes). So `echo pause > /dev/audioctl` from another terminal pauses
whatever is playing, which is the behaviour a person wants from it.

## The format

Signed 16-bit little-endian, two channels, 48,000 frames a second (ruled,
Q3). A FRAME is one sample for each channel: four bytes. The stream reports
its format in `/dev/audioctl` and in every position record, and
`format s16le 2 48000` written to `/dev/audioctl` is accepted; any other
format line is refused. That is the negotiation door: the day a second
format exists, the verb is already how it is asked for, and nothing above
the sink has the number 48 written into it.

A write need not be a whole number of frames. The stream keeps up to three
bytes of a split frame and completes it from the next write, because a
shell redirection chunks wherever it likes. A flush and the last close
discard them.

## The ring, and who empties it

One ring of PCM, sized in time: 200 ms, 9,600 frames, 38,400 bytes,
allocated once at boot. One spinlock (interrupts off) guards the ring and
every counter. Copies happen under it: the largest is the whole ring, a few
microseconds, and the largest a sink takes at once is a few kilobytes.
User memory is never touched under the lock; `syscall_write` copies into a
kernel buffer first, in pieces, as the pipe's write does.

**The sink pulls.** A sink is whatever empties the ring, and it does so
from its own thread at its own cadence by calling `audio_pull`. The seam
never calls into a sink to push sound at it. That is what lets the A2DP
sink take its PCM inside `kusb`'s pass, where Bluetooth's clockwork and
USB's serializing flag already live, with no second thread to hand packets
across to.

```c
// What a sink is, to the seam. (kernel/include/audio.h)
typedef struct audio_sink
{
	const char *name;             // for /sys/audio
	void (*nudge)(void *ctx);     // optional: "the stream changed, pump soon". Any
	                              // context, never sleeps, carries no fact
	void *ctx;
} audio_sink_t;

void audio_sink_attach(const audio_sink_t *sink);   // becomes the sink; one at a time
void audio_sink_detach(const audio_sink_t *sink);   // the null sink takes over
void audio_sink_failed(const audio_sink_t *sink, const char *why);   // fail closed
void audio_sink_delay(const audio_sink_t *sink, uint32_t delay_us);  // how far behind the pull the ear is
bool audio_sink_has_sound(const audio_sink_t *sink);  // it is the sink, the ring holds frames, not paused
                                                      // (A.3: the A2DP pump asks before it starts a stream)

// What a pull tells the sink, read under the same lock as the frames.
typedef struct audio_pulled
{
	uint32_t frames;       // how many of `out` are the ring's; the rest is silence
	uint32_t generation;   // the generation those frames belong to
	bool     paused;       // the stream is paused: `out` is untouched, send nothing
	bool     yours;        // false: another sink is the sink now. `out` is untouched
} audio_pulled_t;

// The sink's one verb. Fills `out` with exactly `frames` frames: the ring's,
// with the volume applied, then silence for what the ring did not have. It
// never crosses a generation: frames of one generation, then silence, and
// the next pull starts the next. Any thread; never sleeps.
audio_pulled_t audio_pull(const audio_sink_t *self, int16_t *out, uint32_t frames);
```

**The seam never calls into a sink with a fact.** A pause, a resume and a
flush are things the sink LEARNS from its next pull: `paused` says stop,
and a `generation` different from the last one it saw says everything it
still holds is retired. The first draft had `suspend`, `resume` and `flush`
callbacks made after the lock was released; two control verbs on two CPUs
could then reach the sink in the wrong order and leave it suspended under a
stream that said playing, and a flush could not name what to drop because
the sink was never told which generation its frames belonged to. A fact
read under the lock, with the frames it describes, cannot arrive out of
order. `nudge` only shortens the wait for that next pull and says nothing.

A pull names its sink, and is answered `yours = false` when that sink is no
longer the one attached: a pump that was already awake when the sink
changed takes nothing from a stream that is not its own.

**A sink change is a flush.** Attaching or detaching empties the ring and
moves the generation on, exactly as the `flush` verb does. Sound queued for
headphones that have gone is not played late into something else, and a
player sees one event it already handles. (A.3 may want better for a
reconnection; it will have the real pair to judge with.)

**The null sink is the first sink, and the default.** It is what plays when
nothing else is attached: it takes frames at the rate the clock says a real
output would, and discards them. So `play` on a machine with no headphones
runs for the length of the song and ends, as it would with them. It is the
"test sink" of the notes, named for what it is on a real machine.

Its pump is **`kaudio`**, a kernel daemon in `knet`'s shape: pinned to the
BSP, parked on a doorbell, rung by the tick. The tick rings it only while
`kAudioClocked` is set: the null sink is the sink, and the stream has work
(frames queued, or a handle open and not paused), so an idle machine pays
nothing. That flag is one word, written under the stream's lock whenever
the facts behind it change and READ WITHOUT IT, because the tick's ringers
run inside the scheduler's pass with the queue lock held, and the stream's
lock is taken before the queue lock everywhere else (a wake is made under
it). Work that appears between two ticks waits for the next one, at most 10
ms, which the null sink's clock absorbs.

Each wake `kaudio` asks the clock how many frames are due since its anchor
(`elapsed_us * 48000 / 1000000`, less what it has taken) and pulls that
many through the same `audio_pull` every sink uses. The anchor is set to
now, with nothing taken, each time `kAudioClocked` goes from clear to set
and at each resume, so a wake never measures from an earlier stream. A wake
that finds more than the ring's worth due (the daemon was held off for over
200 ms) takes one ring's worth, moves the anchor to now, and counts a
`clock_slip`. A wake that is answered `paused` or `yours = false` takes
nothing.

`kaudio` exists on every boot. When a real sink attaches, the tick stops
ringing it.

**Volume is applied at the pull**, not at the write, so a change is heard
within what the sink holds and not 200 ms of ring later. `/dev/volume`'s 0
to 100 becomes a gain of `(v/100)^2` in Q15 (a square is the cheapest curve
that makes the slider's middle sound like half), multiplied per sample and
rounded toward zero (a gain of at most one cannot overflow). 100 is exactly
unity and copies without arithmetic. The value
is the kernel's until reboot; keeping it across boots is a conf question
for whoever builds the slider.

**The tap** is the same pull seen from the side. While `/dev/audiotap` is
open, each pull also appends a record to a capture ring (one second,
allocated at the open, freed at the close): a header and the frames exactly
as the sink received them.

```c
// abi/include/os64/audio.h
typedef struct os64_audio_tap
{
	uint32_t generation;   // the stream generation these frames belong to
	uint32_t frames;       // how many follow this header
	uint64_t first;        // index of the first, counting every frame any sink was given since boot
	uint64_t micros;       // when the pull happened
	uint32_t silence;      // 1: these frames are fill, the ring was empty
	uint32_t dropped;      // records lost to a full capture ring before this one
} os64_audio_tap_t;
```

A record carries one generation and is all sound or all silence, at most
1,024 frames (4,096 bytes and the header); a larger pull is cut into
several, and a pull that is part ring and part silence makes two. A read
returns as many whole records as its buffer holds and refuses a buffer too
small for one of full size. It waits under read's patience when there are
none, is woken by the pull, and ends for a signal like every other wait. A
full capture ring drops the new record whole and counts it; so is a record
taken for a reader whose buffer turned out not to be writable. The close
frees the capture ring under the stream's lock, which a pull appending to
it holds. The tap is at the pull, so it hears
whichever sink is attached; on the P5 it will record what the headphones
were sent.

## The position (Q8, settled here)

```c
// abi/include/os64/audio.h — one read of /dev/audiopos returns exactly this.
typedef struct os64_audio_position
{
	uint32_t size;         // sizeof this struct, so it can grow
	uint32_t generation;   // changes when the numbers below start again
	uint32_t state;        // OS64_AUDIO_IDLE, _WAITING, _PLAYING, _STARVED, _PAUSED, _DRAINING, _FAILED
	uint32_t rate;         // frames a second
	uint64_t accepted;     // frames taken from the program, this generation
	uint64_t taken;        // of those, frames the sink has pulled
	uint64_t heard;        // of those, an ESTIMATE of how many have reached the ear
	uint64_t micros;       // the clock at which `taken` and `heard` were true
	uint32_t queued;       // frames in the ring ahead of the next one written
	uint32_t delay_us;     // what the sink says lies beyond the pull
	uint64_t underruns;    // times the ring ran dry while playing, this generation
} os64_audio_position_t;
```

A read is served the first `length` bytes of it, up to its size, and never
refused for being short: a program built against today's record keeps
working the day the record grows, and `size` says how much of a larger one
the kernel filled.

**The clock** is `micros()`'s own. The body of `syscall_micros` (the TSC
against the boot anchor, folded through its high-water mark so it never
runs backward) becomes a kernel function the syscall and the stream both
call. A stamp taken any other way would not be comparable with the `now` a
player reads.

What each number means, exactly:

- **A count is in the program's own frames.** `accepted` is how many frames
  the program has had taken from it, so the program knows which moment of
  its media frame N is. Silence the seam inserts is never counted. That is
  the choice that makes the record useful to a video player: `heard` IS an
  index into what it wrote.
- **`taken`** counts at the pull, under the lock, with `micros` stamped by
  the same pull. It is exact.
- **`heard`** is `taken` less `delay_us` worth of frames, floored at zero.
  It is an estimate and named as one, for two reasons the record cannot
  remove: `delay_us` is the sink's own claim (a delay report, or a measured
  constant), and for `delay_us` after an underrun ends the frames inside
  the sink include silence the subtraction does not know about, so `heard`
  runs ahead by up to the length of that silence. An underrun is already an
  audible fault; the estimate is exact again once the sink's delay has
  passed.
- **Between readings a player extrapolates**, and only in state `PLAYING`:
  `heard + (now - micros) * rate / 1000000`, never past `taken`. In every
  other state it uses `heard` as read.
- **`queued` is delay; the ring's size is not.** A full ring puts 200 ms
  between a write and its pull, an empty one nothing. `queued` also counts
  frames an earlier owner left (§ Close), which no subtraction of this
  generation's numbers would show.
- **`generation`** goes up by one at: a flush (the verb, a sink change, or
  the last close of a paused stream, which are all the same act); and an
  open of `/dev/audio`. `accepted`, `taken`, `heard` and `underruns` start
  from zero with it. It does not change at a pause, a resume or an
  underrun. A player holds the generation of the stream it is feeding and
  discards a reading that carries another.
- **An open is the one generation change that keeps frames in the ring**
  (an earlier owner's, still playing out; § Close). They stay the old
  generation's: the stream remembers how many there are, pulls take them
  first and count them toward nothing, and the tap shows them under the old
  number. `taken` for the new generation starts at its own first frame.
  Until then the new owner sees `taken` 0 and the old frames in `queued`,
  which is the truth a player needs: that much sound is ahead of mine.

What each event does to it:

| Event | `accepted` | `taken` / `micros` | `heard` | `generation` |
|---|---|---|---|---|
| write | grows | | | |
| pull | | grow, stamped | follows | |
| pull finds the ring dry | | `micros` stamped | follows | (`underruns` +1, once, if this generation was ever written and the dry spell is not already counted; a write ends the spell) |
| pause | | frozen at the last pull | frozen | |
| resume | | resume | resumes | |
| flush, sink change | 0 | 0 | 0 | +1 |
| open | 0 | 0 until the earlier owner's frames are gone | 0 | +1 |

On a pause `heard` freezes at `taken` less the delay. What the far end does
with the sound it already holds when it is told to suspend is not known
(it is Q7's neighbour), so the record claims nothing about it: the state
says `PAUSED` and a player does not extrapolate.

## The stream's states

One stream. Its state is derived from five facts, never stored beside
them: is a `/dev/audio` handle open; is it paused; has the ring frames; has
this generation ever been written to; has the sink failed. Every "to" in
the second table is what those facts then say, and where a row names a
state it is naming the usual case, not overriding them.

| State | Handle | Paused | Ring | Meaning |
|---|---|---|---|---|
| `IDLE` | no | no | empty | Nothing to do. The tick does not ring `kaudio` |
| `WAITING` | yes | no | empty, never written this generation | Open, no sound yet. A dry pull is not an underrun |
| `PLAYING` | yes | no | has frames | |
| `STARVED` | yes | no | empty, was written | The player fell behind. Counted once on entry |
| `PAUSED` | yes | yes | any | No pulls. Writes are accepted until the ring is full |
| `DRAINING` | no | no | has frames | The owner closed; what it queued plays out |
| `FAILED` | any | any | any | The sink failed closed. Outranks the rest |

| Event | From | To | What happens |
|---|---|---|---|
| open `/dev/audio` | `IDLE` | `WAITING` | generation +1 |
| open | `DRAINING` | `PLAYING` | generation +1; the old frames stay the old generation's and play first |
| open | `FAILED`, no handle | `FAILED` | accepted; its writes answer `OS64_AUDIO_FAILED` until the sink is replaced |
| open | any state with a handle | (unchanged) | refused, `OS64_AUDIO_BUSY` |
| write, room | `WAITING`, `STARVED` | `PLAYING` | the dry spell, if one was counted, is over |
| write, no room | `PLAYING`, `PAUSED` | (unchanged) | parks, or answers by its patience (§ The write) |
| pull, frames | `PLAYING`, `DRAINING` | by the facts | `STARVED` if it emptied a written generation, `WAITING` if the new owner has not written yet, `IDLE` with no handle; writer woken |
| pull, dry | `STARVED`, `WAITING` | (unchanged) | all silence; an underrun counted by the rule above |
| pull | `PAUSED` | (unchanged) | takes nothing and leaves the sink's buffer alone |
| pull | `FAILED`, `IDLE` | (unchanged) | all silence |
| `pause` | `WAITING`, `PLAYING`, `STARVED` | `PAUSED` | sink nudged |
| `pause` | `IDLE`, `DRAINING`, `PAUSED`, `FAILED` | (unchanged) | refused: nothing is open to pause, or it already is |
| `resume` | `PAUSED` | by the facts | sink nudged; null sink's clock anchored. No underrun is counted for being empty at a resume: only a dry pull counts one |
| `resume` | any other | (unchanged) | refused |
| `flush` | any but `IDLE` | by the facts | ring emptied (an earlier owner's frames too), split frame dropped, generation +1, sink nudged, a parked writer woken to `OS64_AUDIO_FLUSHED` |
| `flush` | `IDLE` | `IDLE` | accepted, and changes nothing |
| last close | `PLAYING`, `STARVED`, `WAITING` | `DRAINING` or `IDLE` | split frame dropped |
| last close | `PAUSED` | `IDLE` | a flush (generation +1), and the pause is lifted: a pause belongs to an open, and sound nobody can resume is not kept |
| a close while a thread is parked in a write | any | as the last close | the writer wakes, answers what it had placed (or -1), and lets go |
| sink attach, detach | any | by the facts | a flush; a failure belonging to the departing sink is cleared |
| sink failed | any | `FAILED` | ring emptied; a parked writer woken to `OS64_AUDIO_FAILED` |

**Close never waits.** A close can come from a task's burial, where nothing
may park. So the last close leaves the queued sound to play out
(`DRAINING`) and returns. `play a.wav; play b.wav` therefore does not clip
the first song's last fifth of a second, and does not need a "drain" verb:
the second open is accepted while the first drains, and its frames follow.
A player that is killed leaves at most 200 ms to play; one that wants
silence at once writes `flush` first.

**What a flush retires on our side** is the ring, the split frame, and,
once the sink's next pull shows it the new generation, whatever the sink
has built and not sent. Nothing completes later against the seam: a frame is counted when it
is pulled, under the lock a flush takes, so there is no old completion to
refuse at this layer. The A2DP sink has its own (packets built, packets
with the controller), and refusing those by generation is A.3's design.

**The handle is shared the way a pipe's is.** A spawn redirection is the
same open: one `audio_open_t`, one more handle counted on it. The "one
open" rule counts opens, not tasks, and the last close is the last of those
handles.

## The write

`write(audio, bytes, length, timeout_ms)`, with write's patience (arg3,
`os64/syscall_numbers.h`), which today only TCP honours and the audio
stream now does too:

| Patience | Ring has room | Ring full |
|---|---|---|
| `OS64_WAIT_FOREVER` (plain `os64_write`) | takes what fits, parks for the rest, returns `length` | parks |
| N ms | takes what fits; waits up to N ms for more | returns what it took, or `OS64_ERR_TIMEOUT` if none |
| 0 | takes what fits and returns the count | `OS64_ERR_TIMEOUT` |

The other answers: `OS64_INTERRUPTED` when a caught signal ends the wait
with nothing taken (the count taken so far otherwise; an uncaught one ends
the program as it does in a pipe write); `OS64_AUDIO_FLUSHED` when a flush
landed while the call was parked or between its pieces, and then nothing
this call wrote survives, whatever it had been told so far;
`OS64_AUDIO_FAILED` on a failed sink; `OS64_AUDIO_BUSY` when another thread
is already parked in a write on the stream.

A write remembers the generation it began under. Its bytes cross from user
memory in pieces, with the lock released between them, so each piece is
placed only if the generation is still that one; otherwise the call answers
`OS64_AUDIO_FLUSHED`, and what it had placed went with the flush.

The park is the kernel's ordinary one, bell.c's shape, and obeys its rules
(CLAUDE.md § Signals): the writer registers in the stream's one waiter slot
under the lock; at the top of every pass it voids its own slot and its own
owed wake (`scheduler_forget_owed_wake`), then looks for closed, flushed,
failed and `signal_park_must_end` before it looks for room; and on the way
out it asks `current_thread_will_catch`. The pull wakes it with
`scheduler_wake_or_owe`, so a wake that lands between the registration and
the park is not lost (WAKE_OWED.md). It is woken when a pull leaves at
least a quarter of the ring free, not at every pull: a writer woken to
place 480 frames a hundred times a second is all overhead.

The seam guarantees a flush against a write IN FLIGHT. It cannot tell a
write that arrives after the flush with stale sound from a fresh one, and
does not try: a program that flushes from one thread while another writes
stops its own writer first.

**No kernel bell.** The notes promised "a bell that rings when the ring has
room". Nothing in the kernel rings a ring-3 bell today (a bell is rung by a
write from its own program), and making the stream hold a reference into
some task's handle table to ring one is a lifetime problem bought for a
convenience. The same need is met two ways without it: a zero or finite
patience on the write itself, and, for a program with one loop that may not
wait on audio at all (yonder), a thread of its own that does blocking
writes and rings the program's bell, which is the shape libfetch's HTTP/2
work already has. Row A.1.5 becomes the write's patience. This is the one
departure from the notes, and it is Chris's to overrule (§ For Chris).

## Result codes

`abi/include/os64/audio.h`. Negative, distinct from each other and from the
system-wide ones a caller already tests for (`OS64_INTERRUPTED` -4,
`OS64_ERR_TIMEOUT` -10, and -1 / -2, which keep their meanings: refused,
and unreadable user memory).

| Name | Returned by | Means |
|---|---|---|
| `OS64_AUDIO_BUSY` | open of `/dev/audio` or `/dev/audiotap`; write | Already open; or another thread is parked in a write |
| `OS64_AUDIO_FLUSHED` | write | A flush retired this call's sound |
| `OS64_AUDIO_FAILED` | write | The sink failed closed; `/sys/audio` says why |

The alias hook grows one out-parameter so an open can be refused with a
code of its own; today a refused alias can only say -1.

## `/sys/audio`

Text, one fact a line, in the house's `key: value` shape. The sink's name,
whether it failed and why; the format; the state, the generation and the
name of the task that opened the stream; the ring's fill in frames and
milliseconds against its size; `accepted`, `taken`, `heard`, `delay_us`;
silence frames given; underruns since boot and since this file was last
read; `clock_slips`; the volume; the writer's parks and its longest wait;
`kaudio`'s wakes and its longest gap between pulls; whether the tap is open
and what it dropped. Read it first when sound stutters.

## Where the code lives

| File | What | Proof |
|---|---|---|
| `kernel/src/audio/audio_stream.c` | PURE (no kernel headers): the ring, the split frame, every counter, the state, the gain, the position record, the tap's record framing. The caller gives it storage and the clock | `tools/test_audio_stream_host.sh` under ASan and UBSan against a plain model; `tools/test_audio_stream_mutants.py` |
| `kernel/src/audio/audio.c` | The lock, the park, the sink table, the handle faces, the control and volume verbs, `/sys/audio`'s text | QEMU fixture |
| `kernel/src/audio/kaudio.c` | The daemon's loop. The null sink's pump is `audio_null_pump` in audio.c, under the stream's lock; its clock arithmetic is in the pure file | QEMU fixture |
| `devfs.c`, `handle.c`, `procfs.c`, `syscall.c`, `sysfs.c`, `signals.c`, `kernel.c` | The names and the abandon arm; the tag's rows (pin, unpin, close, share and unshare for the stream face, its name in `/proc/<pid>/handles`); write's patience and read's for the new tag; `micros` as a kernel function; the report; the tick's ring; the daemon's birth | |
| `abi/include/os64/audio.h` | The two records, the states, the result codes | Size and offsets static-asserted both sides |
| `userland/tests/audiotest/` | The fixture | |

The pure file is the slice's centre of gravity, as `xhci_service.c` was for
the interrupt arc: every rule in § The position and § The stream's states
is a function there, and each has a mutant that breaks it.

## Proof

**Host.** A model that is obviously right (an unbounded list of frames and
a few integers) run beside the ring through random sequences of every
event, comparing the pulled sound, every field of the position, and the
state after each step; then the named cases: wrap at every offset, a write
split inside a frame at each of its three places, a pull that is part ring
and part silence, a flush with a split frame held, an open while draining,
pause across an underrun, the gain at 0, 1, 99 and 100 with full-scale
samples of both signs, `heard` with a delay larger than `taken`, the clock
arithmetic across a slip.

**QEMU**, `/tests/audiotest`, in the ring-3 suite. It writes a ramp it can
recognise (each frame's left sample is its index, the right its
complement), with the tap open:

- the tap's frames are the ramp, in order, none missing, none twice, and
  the tap's `first` indices leave no gap;
- they arrive at 48,000 a second by the tap's own `micros`, within a tick;
- a blocking write of two seconds of sound takes about 1.8 seconds, and a
  zero-patience write to a full ring answers `OS64_ERR_TIMEOUT`;
- a signal ends a parked write with `OS64_INTERRUPTED`;
- a second open answers `OS64_AUDIO_BUSY`;
- `pause`: the tap goes quiet, the position's `taken` stops; `resume`: the
  ramp continues at the next index;
- `flush`: the generation changes, the tap never shows the frames that were
  queued, and a thread parked in a write wakes to `OS64_AUDIO_FLUSHED`;
- a deliberate stall: `underruns` goes up by one, the tap shows silence
  records, and the ramp resumes unbroken;
- `/dev/volume` at 50: every tapped sample is the ramp's times the gain,
  exactly;
- close with sound queued, then open: the first owner's tail and the second
  owner's frames both appear, in order, under two generations, and the
  second owner's `taken` counts only its own;
- `cat` of a raw file with the stream as its redirected output plays (the
  shared handle), and an open refused for a full handle table does not
  leave the name busy;
- a close from one thread ends another's parked write, and a parked tap
  read;
- the position read 100,000 times against `micros()` read 100,000 times,
  for A.1.8's figure.

Booted with 8 CPUs and with `make run-sparse`'s 24, since the writer, the
daemon and the control verbs meet on one lock from different cores.

**Not proven in QEMU:** a second sink. Attach, detach, a pull answered
`yours = false`, and `audio_sink_failed` are driven on the host, where the
pure file holds their rules, and meet a real sink in A.3.

## What A.1 leaves out, on purpose

Booked in DEBTS.md by A.1.12: a mixer (one stream, one owner); any format
but the one; recording; a volume that survives a reboot; the estimate's
error after an underrun; `/dev/audiotap` for more than one listener.

## `play` and the resampler (the second pull request)

`play file.wav`: reads a RIFF/WAVE file of 16-bit PCM, one or two channels,
any rate (4 to 384 kHz as built); writes it to `/dev/audio`; prints nothing unless asked; on
Ctrl+C writes `flush` and exits. Mono is doubled. A rate that is not the
device's goes through a resampler (§ As built says where it lives), which yonder's media element
will need for the same reason (a great deal of the web's sound is 44.1
kHz).

The resampler is a PORT, by the arc's rule that signal-processing code is
ported and host-tested and not written here. The lean is speexdsp's
`resample.c` (BSD, one file, the resampler inside Opus's own tools), in the
`upstream/` + `patches/` + `port/` pattern, pinned and hash-checked. Its
proof is a host test: a sine through 44.1 to 48 kHz comes out a sine at the
same frequency within a stated error. Chris approved that port on
2026-11-13; nothing in the kernel seam depends on it.

## As built (the kernel seam)

What the code does that the text above does not say, or says differently:

- **The result codes** are -20 (`OS64_AUDIO_BUSY`), -21 (`_FLUSHED`), -22
  (`_FAILED`). A write on a handle closed under it, a read of the stream, a
  write to the position or the tap, and a tap read with a buffer smaller
  than one full record all answer -1.
- **`nudge` is called with the stream's lock held** and interrupts off, not
  after it is released: it may set a flag or ring a doorbell and nothing
  else. That is what keeps two nudges in the order of the changes they
  announce, though a nudge carries no fact for the order to matter to.
- **A pull's silence is counted as time.** The null sink's clock advances by
  every frame it was given, sound or fill, so a dry stream is still paced.
- **The bytes that would finish a split frame are not taken while the ring
  is full**; the write waits with them. So the stream never holds a whole
  frame outside the ring, and what a close or a flush drops is at most
  three bytes.
- **A write looks for a pending signal before it looks for room.** A write
  made with a caught signal pending places nothing and answers
  `OS64_INTERRUPTED`; the handler runs and the program writes again.
- **Bytes cross from the program 16 KiB at a time** (85 ms of sound), each
  piece copied in before the lock is taken.
- **A tap read takes at most sixteen full records** in one hold of the lock.
- **The frames of two earlier opens**, both still in the ring when a third
  opens, are both shown at the tap under the later one's generation.
- **`/sys/audio` also gives** the writer's parks and longest wait, pulls
  refused to a sink that was not the sink, and the longest gap between two
  of `kaudio`'s wakes since boot and since the file was last read.
- **`micros()` moved** into `kernel/src/micros.c` as `micros_now()`; the
  syscall calls it.

Measured on the P5 (Chris, 2026-11-13, `/tests/audiotest` step 11): a
position read costs 392 ns against 277 ns for `micros()`, which is the
syscall floor. Q5's "no more than a syscall would" holds to within the
handle's pin and one lock: 115 ns, or seven microseconds a second for a
player asking sixty times a second. (QEMU under TCG says 5.2 microseconds
against 3.5: the same ratio.) The whole fixture passed there too.

A second cold read, of the code, found that a flush could lose to a signal
or a close that landed with it (the write answered a count for sound that
was gone), that the writer's slot could name a thread of the last open
when the next one first wrote, and that a split frame finished against a
full ring was counted and could then be dropped. All three are fixed, and
the fixture now proves the last handle's close frees the name.

Proven: `tools/test_audio_stream_host.sh` (twelve named cases and random
rounds beside the model), `tools/test_audio_stream_mutants.py` (111
mutants), and `/tests/audiotest` (eleven steps, in `testrun`) on 8 CPUs and
on the sparse 24.

## As built (`play` and the resampler)

- **The resampler is speexdsp's**, 1.2.1, `resample.c` alone and unpatched,
  as `libsound.so` (`userland/libsound/README.md` has the inventory, the
  build switches, and a review of upstream's branches made at Chris's ask:
  nothing since the release fixes the resampler's C code). Not in libos64,
  as this file first said: it needs `sin`, and libos64 does not load
  libmath. The interface is `os64/resample.h`.
- **`play [-v] file.wav`**: 16-bit PCM, one or two channels, any rate from
  4 to 384 kHz; a WAVE_FORMAT_EXTENSIBLE header is read. It does not print
  unless asked or refused. Ctrl+C flushes and exits 130. A flush from
  somebody else is played through. It does not wait for its last fifth of
  a second before exiting; the close leaves it playing.
- **Proof**: `tools/test_resample_host.sh` and `/tests/resampletest` (one
  body, `checks.h`: a sine in at one rate is the same sine out at another,
  to within the rounding to 16 bits), `/tests/playtest` (`play` on a
  44.1 kHz mono file and a 48 kHz stereo one, heard at the tap, and three
  refusals), `tools/test_sound_audit.py`.

On the P5 (Chris, 2026-11-13): `/tests/resampletest` and `/tests/audiotest`
pass, and `play` on a 22.05 kHz file of 12.8 s ran for 12.65 s by the
`time` utility, which is the song less the last of the ring that the close
leaves playing.

## For Chris

*Put to Chris 2026-11-13 with the leans below; he cleared the design to
proceed on them.*


1. **The bell.** § The write drops the kernel-rung bell and gives the write
   patience instead. Lean: drop it. Its cost was a kernel object reaching
   into a program's handle table; yonder gets a small audio thread, as
   libfetch has.
2. **The names** `/dev/audiopos`, `/dev/audioctl`, `/dev/audiotap`. Lean:
   as written. Plan 9 has `audioctl` and `audiostat`; the rest are mine.
3. **Anyone may pause, flush or set the volume.** Lean: yes (§ The names).
4. **Close lets queued sound play out, and a new open may follow it.**
   Lean: yes. The other choice is a close that cuts the last 200 ms of
   every song.
5. **Whose `play` is.** A.1 needs something that plays a file, and the
   fixture is not that. Lean: I write the small one above now, since it is
   what you will type on the day the headphones work, and it is yours to
   replace.
