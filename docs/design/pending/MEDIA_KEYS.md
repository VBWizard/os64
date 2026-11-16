# MEDIA_KEYS.md — a press reaches the program it is for

The headphones' play, pause, next and previous presses reach a program.
Several programs can be players at once (yonder windows, `play`), and a
press goes to the one it is for, by the rule other systems use: the one
playing, else the one you were last looking at. This is the audio arc's
A.5 (`AUDIO_TRACKER.csv`) and, for yonder's half, the yonder arc's Y.6
(`YAUDIO_TRACKER.csv`). A.4 left "what a press DOES" undecided on
purpose (AUDIO_REMOTE.md § Their buttons); this is that decision. Fable
reviewed the kernel half (approved with asks, 2026-11-15); the
asks are written in below.

## What the headphones do (P5, 2026-11-14 and 15)

- **One press of the middle button is Play or Pause, chosen by the
  headphones from what we told them.** Our AVRCP target reports a playback
  status (`bt_host.c` `host_avrcp_playback`: PLAYING while A2DP streams
  with sound in it, PAUSED while the stream is paused, STOPPED otherwise),
  and the headphones send PAUSE when told PLAYING and PLAY otherwise.
  Chris counted four Plays pressed in silence, then Pause for every press
  while a song played.
- **Two presses are Forward, three are Backward**, whatever the status.
- Each press arrives as a press and then a release. Volume presses already
  have their meaning (AUDIO_REMOTE.md § Part W) and keep it.

## What Chris asked for (2026-11-15, `AUDIO_TRACKER.csv` Q9)

Shared control, as Chrome on Windows does it with several YouTube windows
and a radio window open: a press goes to whichever is playing, or to
whichever has the focus when none is. And when no program listens at all,
the kernel itself pauses and resumes the sound. (Several programs SOUNDING
at once is a different matter, a kernel mixer, and not asked for: today
one program holds `/dev/audio` and another waits, `OS64_AUDIO_BUSY`.)

## The session

**A program that wants presses opens `/dev/mediakeys` in mode `u` (read
and write), and that handle IS its session** (`HANDLE_MEDIA_KEYS`, in
`handle.h`'s list beside `HANDLE_BELL`, and named in `/proc`'s handle
list). Any other mode is the ordinary refusal. Every open is a session of
its own, up to `MEDIA_KEYS_SESSIONS` (32) machine-wide; one past that is
refused as busy (`OS64_MEDIA_KEYS_FULL`), since the rule runs over every
session with interrupts off.

**What a session remembers of its program, taken once at open** in the
opener's own syscall: its task ID and name (for `/sys/mediakeys`, so the
report never reaches into a task), and its LINEAGE: the task IDs of the
opener and its ancestors, nearest first, up to eight, stopping before
`kKernelTask`, copied under `kDeadChildLock` (the lock the orphanage
rewrites `parentTask` under). The rule matches the focus against this
list as integers, so `kusb` touches no task and takes no task lock. The
snapshot's cost: a program orphaned later still lists its dead ancestors,
which is harmless (a dead task owns no window: its windows go before its
handles, `task_exit_teardown`), and a task ID recycled after the ID space
wraps could match falsely (DEBTS.md).

**Its lifetime follows the house's kernel objects (the bell, the audio
open):** an atomic reference count, one for the handle and one for each
operation in flight; a `closed` flag under the session lock; the memory
freed by the last reference after that lock is released. **Close** (the
handle's last close, which for a dying task is in `task_exit_teardown`,
after its windows are destroyed and its siblings told to die) takes the
session lock, marks it closed, strikes it from the table, so no press can
reach it again, wakes a parked reader and empties its waiter slot. Close
takes a spinlock and nothing else, legal with interrupts off and under
any address space, as every handle closer must be. **A session is not
shareable** (`handle_share` answers false): a session two tasks hold
would have no one program for the rule to match and no one close to clear
its reader on.

**Write: the session's state**, one word a write with optional trailing
whitespace: `playing`, `paused` or `stopped` (a fresh session is
`stopped`). It is parsed whole and applied under the lock, or refused
whole (-1, the system's refusal), never guessed at. A write never waits,
so a finite patience on it is refused as for any write. The state is the
PROGRAM's word for itself, because only it knows: yonder mixes every
element of a window into one stream, so the stream cannot say which window
plays, let alone which element.

**Read: the next press for this session**, one record a read:

```c
// abi/include/os64/mediakeys.h
typedef struct {
    uint32_t key;        // OS64_MEDIA_KEY_PLAY .. _STOP
    uint32_t serial;     // this session's presses, counted from 1
    uint64_t at_us;      // micros_now() when the press was routed
} os64_media_key_t;
#define OS64_MEDIA_KEY_PLAY      1
#define OS64_MEDIA_KEY_PAUSE     2
#define OS64_MEDIA_KEY_NEXT      3
#define OS64_MEDIA_KEY_PREVIOUS  4
#define OS64_MEDIA_KEY_STOP      5
```

- A buffer smaller than one record is refused as invalid, never given
  part of one.
- The read waits with read's patience (forever, some milliseconds, none),
  as the bell's does. Its park registers in the session's one waiter slot
  under the session lock and parks after releasing it, woken by
  `scheduler_wake_or_owe`. At the top of every pass it clears its own slot
  and calls `scheduler_forget_owed_wake`. It asks `signal_park_must_end`,
  and answers `OS64_INTERRUPTED` through `current_thread_will_catch` at
  the syscall's edge. A second thread reading the same session while the
  first is parked is refused as busy.
- A reader woken by its session's close answers 0, as the bell's and the
  tap's do.
- A record taken from the ring and then not delivered (a bad user
  pointer) is counted as lost, not vanished.

Each session keeps its unread presses in a ring of eight. A ninth drops
the oldest and counts the loss. A full ring means a player that stopped
reading, so the rule passes it over (below).

## The rule: which session a press is for

Worked out once a press, under the session lock, by `media_keys_press` in
`kernel/src/audio/media_keys.c`, a pure file with no kernel headers that
also holds the ring, the Play/Pause rewrite, the state-word parser and the
sequence stamp (the `audio_stream.c` split: the glue keeps the lock, the
wait, the wake, the focus load and the fallback call, in
`media_session.c`):

1. **A session that says `playing`.** If several do, the one that said it
   most recently (the newest player wins, as starting a second video in
   Chrome moves the keys to it).
2. **Else the session of the program you are looking at**: the one whose
   lineage holds the focused window's owner, so a `play` started from a
   gterm counts as focused while that gterm is (the window is gterm's,
   `play` its grandchild through husk). The nearest wins (the smallest
   place in its lineage); among sessions equally near (each yonder window
   is a task of its own, and everything `gui.conf` starts is a child of
   `/bin/desktop`), the most recently active.
3. **Else the session most recently active.**
4. **Else no session**: the kernel answers itself. Play and Pause are
   `audio_control`'s `pause`, and if that is refused, `resume` (pause
   refuses when nothing is open; resume unless paused). Both refused, and
   Next, Previous and Stop, are counted as unanswered. The call is made
   after the session lock is released (it takes the stream's lock and
   wakes under it).

**Active** is a sequence number stamped under the session lock at each
state write and each delivery, never a clock: two cannot tie.

**A session whose ring is full is passed over** at every rule, falling
through to the next and at last to the fallback, and `/sys/mediakeys`
shows the pass. A program that hangs or stops reading must not swallow
the button while its sound plays on.

**Play and Pause are one toggle, decided by the target's own state.** A
press of either is delivered as PAUSE to a session that says `playing` and
as PLAY to any other. The headphones choose between the two from the
STREAM's status, which can disagree with the target's word (two players,
a playing element that is silent), and a literal Pause sent to a paused
player would be a button that does nothing, pressed forever. Between a
delivered PAUSE and the player's `paused` write a second press is
delivered as PAUSE again: a player treats PLAY and PAUSE as idempotent
(Play while playing, Pause while paused, do nothing). Next, Previous and
Stop go through unchanged.

**Decided here, at the edge of the ruling:** the fallback runs only with
NO session at all. With one `stopped` session open and sound coming from
a program that holds none, the press goes to the session (rule 3) and the
sound is not paused. Every shipped player holds a session once Y.6 lands,
so this does not arise with them.

The status the headphones are told stays what it is (`host_avrcp_playback`,
from the real sound): with the toggle, it no longer decides what a press
does, only which word the headphones send.

## Where it runs, and its locks

- **Presses enter from `kusb`.** `bt_host`'s consumer ring
  (`bt_host_button_take`) holds every press and release of every
  operation. The transport takes from it AFTER `xhci_bt_log_trouble` has
  logged that pass's presses (the log reads the same ring by number, and a
  take before it would blind the `DEBUG_USB` lines), keeps only `pressed`
  entries of PLAY, PAUSE, STOP, FORWARD (Next) and BACKWARD (Previous)
  (`bt_avrcp.h`), and hands each to `media_key_pressed(key)`. Releases and
  volume keys stay `bt_host`'s. That is a thread, not an interrupt; the
  press is routed and delivered there.
- **The session table has a spinlock of its own**, taken irqsave (`kusb`
  arrives with interrupts on, a sysfs writer may arrive with them off),
  held for the pick, the ring's append and the wake, never across a wait
  or an allocation. The wake takes the queue lock under it: session lock,
  then queue lock, the audio stream's order (`audio.h`). Nothing that runs
  under the queue lock (the tick, `processSignals`) ever takes the session
  lock; `/sys/mediakeys` copies under the lock and formats after it.
  `media_key_pressed` may run in `kusb` and in a sysfs writer at once: the
  session lock is the only serialization, so the last-press record and the
  unanswered count live under it, and nothing assumes USB's `s_poll_busy`.
- **The focus is read without the GUI's lock.** `s_focused` is assigned at
  one place, `focus_window` (`window.c`), which every birth, raise,
  minimize and death passes through. Beside that assignment, under the
  lock it already holds, the window manager stores the focused window's
  owner task ID into one word, and `wm_focus_owner()` is an atomic load of
  it: the owner's task ID (`window_t.owner`; a popup's is its parent's),
  0 when nothing is focused or no task is behind the window. 0 matches no
  session, so on a boot with no desktop rule 2 never fires. `media_keys`
  never takes `kGuiLock`.

## What a player does

- **`play`** opens a session, says `playing` as it starts and `paused`
  while paused, and looks for a press between two pieces of sound without
  waiting (a piece is under a tenth of a second). Play/Pause pause the
  stream at once (`/dev/audioctl`) and resume it; Next and Previous move
  through the files it was given (Previous within its first three seconds
  of a file goes back a file, later restarts the file, as players do; Next
  after the last ends it); Stop ends it. Without a session (all taken) it
  plays on, deaf to the keys.
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

The sessions (task ID and name as copied at open, state, when last
active, presses delivered and lost, and whether the last press passed it
over for a full ring), the focus word as last read, the last press and
which rule picked its target (or that the kernel answered it), and the
unanswered count. Read it FIRST when a press seems to go nowhere.

**Writing `press play|pause|next|previous|stop` routes a press** as if
the headphones had sent it, a line at a time: the line is carried out by
the write that brings its newline, because `echo` writes its words, the
spaces between and the newline separately. Never at close, which can run
with interrupts off: text left without a newline when the handle closes
is refused there. Anyone may write it, as anyone may write
`/dev/audioctl`. That is how QEMU, which has no
headphones, tests the rule, and how a keyboard's media keys could arrive
one day without the rule changing.

## Proof

- `tools/test_media_keys_host.sh`: the rule against a model, every order
  of states, focus, lineage and ties, the full-ring pass, the toggle, the
  fallback, the ring and the parser; mutants of each rule
  (`mutants_parallel.py`).
- `/tests/mediakeystest` in QEMU: two sessions, presses written to
  `/sys/mediakeys`, each landing where the rule says; a session's ring
  filling, counting its loss and being passed over; a reader parked
  through a signal and through a sibling's close; a session's end at close
  and at a kill; a short buffer refused; a bad state word refused.
- `play` and yonder by hand in QEMU through `/sys/mediakeys`, then the P5
  with the headphones: two yonder windows, one playing, the press pauses
  that one; nothing playing, the press goes to the window with focus.

## The comments this makes false

The slice changes them in the same commit: "A PRESS DOES NOTHING"
(`bt_host.c`), "A press does nothing yet" (`bt_host.h`), "or taken by a
consumer" (`xhci.c`, true now), and CLAUDE.md's "and do nothing yet".

## Slices

- **A.5.1, the kernel and `play`**: the session handle, the rule and its
  pure core, `wm_focus_owner`, the `kusb` hookup, the fallback,
  `/sys/mediakeys`, `play`'s session, the proof above. Codex reviews it.
- **Y.6, yonder**: `navigator.mediaSession` (MediaSession, MediaMetadata,
  the action handlers) in libdom, and yonder's session per window.

## As built (A.5.1)

- The rule, the ring, the toggle, the state word and the stamps:
  `kernel/src/audio/media_keys.c` (pure), `tools/test_media_keys_host.sh`
  beside a model, `tools/test_media_keys_mutants.py` (35 mutants, all
  caught).
- The session handle, its wait, the routing entry, the kernel's answer and
  the report: `kernel/src/audio/media_session.c`; the lineage at open:
  `task_lineage` (task.c); the focus word: `wm_focus_owner` (window.c); the
  take: `xhci_bt_route_buttons` (xhci.c); the ABI: `os64/mediakeys.h`
  (refusals -30 FULL and -31 BUSY).
- Opening counts as activity, so two sessions never tie at rule 3 (a fresh
  session is stamped when listed).
- `/tests/mediakeystest` (nine steps, in `testrun`; it skips itself when
  another program holds a session or the sound is busy, since its presses
  would reach them) and `play` driven through `/sys/mediakeys` in QEMU:
  pause, resume, next, previous both ways, stop.

## Left out, on purpose

- Telling the headphones the song (AVRCP track metadata): they show none.
- Keyboard media keys: the door is `/sys/mediakeys`' press verb (or
  `media_key_pressed` from a driver); no keyboard sends them yet.
- Two programs sounding at once (a kernel mixer).
