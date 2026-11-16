# MEDIA_KEYS.md — a press reaches the program it is for

The headphones' play, pause, next and previous presses reach a program.
Several programs can be players at once (yonder windows, `play`), and a
press goes to the one it is for, by the rule other systems use: the one
playing, else the one you were last looking at. This is the audio arc's
A.5 (`AUDIO_TRACKER.csv`) and, for yonder's half, the yonder arc's Y.6
(`YAUDIO_TRACKER.csv`). A.4 left "what a press DOES" undecided on
purpose (AUDIO_REMOTE.md § Their buttons); this is that decision.

## What the headphones do (P5, 2026-11-14 and 15)

- **One press of the middle button is Play or Pause, chosen by the
  headphones from what we told them.** Our AVRCP target reports a playback
  status (`bt_host.c` `host_avrcp_playback`: PLAYING while A2DP streams
  with sound in it, PAUSED while the stream is paused, STOPPED otherwise),
  and the headphones send PAUSE when told PLAYING and PLAY otherwise.
  Chris counted four Plays pressed in silence, then Pause for every press
  while a song played.
- **Two presses are Forward, three are Backward**, whatever the status.
- Volume presses already have their meaning (AUDIO_REMOTE.md § Part W)
  and keep it.

## What Chris asked for (2026-11-15)

Shared control, as Chrome on Windows does it with several YouTube windows
and a radio window open: a press goes to whichever is playing, or to
whichever has the focus when none is. And when no program listens at all,
the kernel itself pauses and resumes the sound. (Several programs SOUNDING
at once is a different matter, a kernel mixer, and not asked for: today
one program holds `/dev/audio` and another waits, `OS64_AUDIO_BUSY`.)

## The session

**A program that wants presses opens `/dev/mediakeys`, and that handle IS
its session** (`HANDLE_MEDIA_KEYS`, a handle kind in `handle.h`'s list
beside `HANDLE_BELL`). Every open is a session of its own; one program may
hold one per player it shows. The session ends when its handle closes,
including at the task's burial.

**Write: the session's state**, one word a write: `playing`, `paused` or
`stopped` (a fresh session is `stopped`). Anything else is refused (the
write answers an error and changes nothing), never guessed at. The state is
the PROGRAM's word for itself, because only it knows: yonder mixes every
element of a window into one stream, so the stream cannot say which window
plays, let alone which element.

**Read: the next press for this session**, one record a read, waiting with
read's patience as `/dev/audio`'s write does (forever, some milliseconds,
none), ended early by a caught signal (CLAUDE.md § Signals: the parked
reader asks `signal_park_must_end`, and clears its own waiter slot at the
top of every pass):

```c
// abi/include/os64/mediakeys.h
typedef struct {
    uint32_t key;        // OS64_MEDIA_KEY_PLAY .. _STOP
    uint32_t serial;     // this session's presses, counted from 1
    uint64_t at_us;      // micros() when the press arrived
} os64_media_key_t;
#define OS64_MEDIA_KEY_PLAY      1
#define OS64_MEDIA_KEY_PAUSE     2
#define OS64_MEDIA_KEY_NEXT      3
#define OS64_MEDIA_KEY_PREVIOUS  4
#define OS64_MEDIA_KEY_STOP      5
```

Each session keeps the presses not yet read in a ring of eight; a ninth
drops the oldest and counts the loss (a person cannot press faster than a
player reads, so a full ring means a player that stopped reading).

## The rule: which session a press is for

Worked out once a press, by a pure function (`media_keys_pick`, in
`kernel/src/audio/media_keys.c`, no kernel headers, host-tested):

1. **A session that says `playing`.** If several do, the one that said it
   most recently (the newest player wins, as starting a second video in
   Chrome moves the keys to it).
2. **Else the session of the program you are looking at**: the one whose
   task owns the focused window, or whose task's ANCESTOR does, so a `play`
   started from a gterm counts as focused while that gterm is (the window
   belongs to gterm, `play` is its grandchild through husk). The nearest
   ancestor wins when two sessions qualify.
3. **Else the session most recently active**: the last to change its state
   or to be given a press.
4. **Else no session**: the kernel answers itself. Play and Pause toggle the
   stream through `audio_control` (`pause` / `resume`, the verbs
   `/dev/audioctl` already has, safe from any context); Next, Previous and
   Stop are counted as unanswered.

**Play and Pause are one toggle, decided by the target's own state.** A
press of either is delivered as PAUSE to a session that says `playing` and
as PLAY to any other. The headphones choose between the two from the
STREAM's status, which can disagree with the target's word (two players,
a playing element that is silent), and a literal Pause sent to a paused
player would be a button that does nothing, pressed forever. Next,
Previous and Stop go through unchanged.

The status the headphones are told stays what it is (`host_avrcp_playback`,
from the real sound): with the toggle above, it no longer decides what a
press does, only which word the headphones send.

## Where it runs, and its locks

- **Presses enter from `kusb`.** Each pass, after `bt_host` drains AVRCP,
  the transport takes the presses from the consumer ring that exists for
  this (`bt_host_button_take`, the ring A.4 built and nothing reads yet)
  and hands each to `media_keys_press(key)`. That is a thread, not an
  interrupt; the press is routed and delivered there.
- **The session table has a spinlock of its own** (interrupts off, held for
  the pick and the ring's append, never across a wait or an allocation).
  A delivery wakes the session's parked reader under it, so it is taken
  before the queue lock and never under it, as the audio stream's is.
- **The focus is read without the GUI's lock.** The window manager
  publishes the focused window's owner task as one word
  (`wm_focus_owner()`, an atomic load of a value stored wherever
  `s_focused` is assigned, under the lock that assignment already holds).
  `media_keys` never takes `kGuiLock`, so no ordering between the two locks
  exists to get wrong. The ancestor walk (rule 2) reads each session's
  task's parent IDs; how that walk stays safe against a task dying beside
  it is the first question for review (§ For review).

## What a player does

- **`play`** opens a session, says `playing` as it starts and `paused`
  while paused. Play/Pause pause and resume it; Next and Previous move
  through the files it was given (Previous within its first three seconds
  of a file goes back a file, later restarts the file, as players do).
- **yonder (Y.6)** holds one session per window and says `playing` while
  any element of its page sounds, `paused` while one it played is paused,
  `stopped` otherwise. A press goes to the page through the web's own door,
  the **Media Session API** (`navigator.mediaSession`): a page that set an
  action handler for `play`, `pause`, `nexttrack`, `previoustrack` or
  `stop` has it run, as podcast and music sites do. With no handler, Play
  and Pause act on the element that last played, as other browsers do,
  and the others do nothing. `MediaMetadata` (title, artist, artwork) and
  `playbackState` are kept and reported, and change nothing here.

## Seeing it: `/sys/mediakeys`

The sessions (task, name, state, when last active, presses delivered and
lost), the focused window's owner as `media_keys` last read it, the last
press and which rule picked its target (or that the kernel answered it),
and the unanswered count. Read it FIRST when a press seems to go nowhere.
Writing `press play|pause|next|previous|stop` to it routes a press as if
the headphones had sent it: that is how QEMU, which has no headphones,
tests the rule, and how a keyboard's media keys could arrive one day
without the rule changing.

## Proof

- `tools/test_media_keys_host.sh`: the rule against a model, every order
  of states, focus and ancestry, the toggle, the fallback; mutants of each
  rule (`mutants_parallel.py`).
- `/tests/mediakeystest` in QEMU: two sessions, presses written to
  `/sys/mediakeys`, each landing where the rule says; a session's ring
  filling and counting its loss; a reader parked through a signal; a
  session's end at close and at a kill.
- `play` and yonder by hand in QEMU through `/sys/mediakeys`, then the P5
  with the headphones: two yonder windows, one playing, the press pauses
  that one; nothing playing, the press goes to the window with focus.

## For review (Fable: the kernel half)

1. **The ancestor walk** (rule 2): reading `parent` IDs up from a session's
   task while tasks may be exiting beside it. My lean: the walk runs on
   task IDs through the task table's existing lookup under its lock,
   bounded in depth, never holding a `task_t *` across a step.
2. **`wm_focus_owner()` as a published word** instead of taking `kGuiLock`
   from `kusb`: is there any focus assignment that does not go through the
   one place the store would live?
3. **Waking the reader from `kusb`** under the session table's lock: the
   wake's lock order against the queue lock, as `audio_stream`'s is.

## Slices

- **A.5.1, the kernel and `play`**: the session handle, the rule and its
  pure core, `wm_focus_owner`, the `kusb` hookup, the fallback,
  `/sys/mediakeys`, `play`'s session, the proof above. Codex reviews it.
- **Y.6, yonder**: `navigator.mediaSession` (MediaSession, MediaMetadata,
  the action handlers) in libdom, and yonder's session per window.

## Left out, on purpose

- Telling the headphones the song (AVRCP track metadata): they show none.
- Keyboard media keys: the door is `/sys/mediakeys`' press verb (or
  `media_keys_press` from a driver); no keyboard sends them yet.
- Two programs sounding at once (a kernel mixer).
