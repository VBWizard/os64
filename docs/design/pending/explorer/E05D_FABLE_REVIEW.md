# E5D — Fable's architectural review of E05D_WINDOW_PLACEMENT.md (revision 1)

2026-10-23. Read against the design, `kernel/src/gui/event_queue.c` and its
header, the compositor's gesture paths (`compositor.c`: the drag and band
state, the button-up commit, `gui_grab_release`, `gui_cancel_resize`,
`gui_decoration_changed`, the glass-loss cancel), `window.c`'s `wm_move`,
`wm_resize` and their automatic callers, the public event layout in
`abi/include/os64/gui.h`, and the file dialog's existing size memory.
**Verdict: APPROVED to build, with the rulings below.** The boundary is
right, the kernel part is small and in the queue's existing shape, and
nothing here needs a syscall or a lock the GUI does not already hold.

## The five questions

**1. The masked, coalesced completion event is the right boundary.** The
compositor is the only thing that knows a geometry change was a person's
(a gesture that ended) and not the system's (fitting, decoration,
minimum size, maximize), and `wm_move`/`wm_resize` are exactly the wrong
place to publish because their callers are both kinds (`window.c:196`,
`:546`, `:568`, `:583`, `:726` are all automatic). Publishing at gesture
completion, with a per-component `changed` mask, keeps policy out of the
kernel entirely: the kernel says "the person did this", libos64 decides
what to keep. Rulings on the edges:

- **Cancellation as written is right, and the code agrees with it.** A
  live move has already applied its frame, so whatever ends the gesture
  (release, `gui_decoration_changed` clearing `s_drag_window`, glass
  loss) the position on screen IS the person's: publish it, once, at
  that moment. A band cancelled (`gui_cancel_resize`) applied nothing:
  nothing. `gui_grab_release` runs from `wm_destroy` on a window still
  linked: nothing is published to it, and the pending record dies with
  the queue. One addition: the drag today keeps only its grab offset
  (`s_drag_dx/dy`); the start-frame snapshot the design needs for "a
  no-op gesture emits nothing" is new state beside it, taken at the
  press that starts the drag, under `kGuiLock` like the band's
  `s_band_origin`. Say in the compositor's comment that the snapshot is
  what makes "unchanged" decidable, so nobody later "simplifies" it away.
- **The completion helper is one function**, called from the four ends
  of a gesture (band commit, drag release, `gui_decoration_changed`,
  the glass-loss path) and nowhere else. The invariant it enforces, in
  its comment: an automatic geometry change never reaches it. The proof
  is a mutant that calls it from `wm_resize` and a case that must then
  fail on create fitting.
- **Maximized at gesture start → all four components**: right, and the
  band commit already clears `GUI_WINDOW_MAXIMIZED` before `wm_resize`,
  so the order is: clear, resize (which may refuse), then publish only
  on success with the clamped actual frame.

**2. The scheduling slot is sound, but not as a fourth pair of yield
flags.** The queue today has three coalesced sources (appearance,
doorbell, pointer) plus loss and the ring, and bounded progress is held
by two pairwise flags (`doorbell_yield_input`, `lost_yield_control`).
The design's rules for placement are correct one by one and would be the
third and fourth such flag, each reasoned about against each other. The
ruling: **express the pop order as ONE function with ONE stated
invariant**, "between two deliveries of any coalesced source, every
other pending source is delivered at least once", and let the flags be
whatever that function needs (a last-served cursor over
appearance → doorbell → placement → ring/loss → pointer is the simplest
shape that satisfies it; the existing two flags fold into it). The
existing orderings the comments promise (appearance first, loss before
retained input, doorbell alternating with the ring, pointer after the
ring's drain) are cases, not code paths, and must all still pass.
`event_queue.c` has no kernel headers and already compiles into
`tools/test_appearance_host.sh`, so this is host-testable: **a queue
harness with the replenishment patterns the design lists and a mutant
per rule** is the required evidence, not an in-OS observation. Lifetime
and locks: the record is inline, zeroed at creation, dies with the
window, written only under `kGuiLock` from paths that already hold it,
delivered through `wm_deliver_event`'s wake-or-owe. No new hazard. Add
placement to `gui_event_queue_pending` so `poll`, `event_wait` and
`event_wait_any` agree, as the design says; a wait that sleeps through a
pending placement is the one bug this slot can have, and the harness
pins it.

**3. The per-component merge rules are sufficient, with one rule stated
louder.** The stored record is a PREFERENCE and the live window is a
FIT; a clamp never writes, a move on a small screen writes X and Y only,
a one-axis resize writes one axis. The queue's record is "explicit edits
not yet consumed", merged by component, never a snapshot of live state,
which is what makes "resize, then maximize before the app runs" keep the
normal size. Right. The louder rule: **a resize committed on a small
screen writes the clamped actual size**, because the person dragged the
corner to it, and that replaces a larger remembered size on purpose. The
design says "actual clamped dimensions"; say beside it that this is the
deliberate case and the clamp-at-restore is the automatic one, so a
reader does not file the first as a bug. Frame x/y with content w/h is
the create call's own vocabulary (`os64_gui_window_create_content`);
keep it and assert it once in the helper's comment.

**4. The initializer's partial-failure ownership is clear enough, given
one table.** "Preserve a native handle whose cleanup was refused in
`*out_window`; retain the app-owned storage for retry" is the file
dialog's contract and the right one. Put it in the initializer's header
comment as a table, step by step (options invalid → nothing created;
create failed → nothing held; UI init failed after create → the handle
is the caller's to close or retry with), because that table is what the
next consumer reads. Dispatcher interception before command filters and
before the widget/root guards in direct dispatch covers a window whose
input is disabled by a modal chooser, and consuming the event there is
what prevents double persistence; both are right. One check the builder
makes: the file dialog is an ordinary window (`ui_file_dialog.c:657`
creates it with `FIT_SCREEN` and no popup flag), so the event's "exclude
owned popups" does not exclude the slice's own third consumer; the
as-built says which windows are excluded by flag, by name.

**5. Last complete publication wins, and no serialization.** One person
on one machine, two instances of one role saving the same file in the
same instant: the same question slice 1a's localStorage asked, and the
same answer. Read, merge the explicit components, publish whole through
the checked atomic writer; no lock, no exit-time save, an old instance
closing writes nothing. The DEBTS row says exactly what can be lost (the
same component written by two windows in one instant, the later file
wins) and the trigger (a day two instances of one role are meant to
share live state).

## Three things the design has right that are worth keeping on purpose

- No persistence in `wm_move`/`wm_resize`, no polling, no save at close.
  Each of those is the path a later "simplification" will reach for;
  the comments say why not.
- The 20-byte payload inside the 32-byte event with the offsets asserted
  on both sides. Event 17 if free; assert it as the others are.
- The first-publication path through NOREPLACE rather than "a failed
  open means absent": the writer's assumption was fine for a file a
  person edits and is not fine for one two windows create at once.

## What is required of the PR beyond the design's evidence table

- The queue harness (question 2) with its mutants, run by the PR.
- The compositor's completion helper proved by HMP mouse press and
  release in QEMU, as the design says: titlebar move, chord move, the
  four corners, a cancelled band, a decoration change mid-drag, and a
  window destroyed mid-drag; each one observed as the event the app
  receives, or as no event.
- Every touched comment re-read: `WINDOW_RESIZE` is no longer the only
  geometry observation, and the dialog's size memory no longer saves on
  its own path.
- Codex: yes. This is kernel concurrency (a gesture's end racing a
  destroy, a decoration change, a VT switch) and a queue ordering
  change, which is the risk the review budget is for.
