# DROPDOWN.md — a one-line select, and the libui dropdown under it

Opus, 2026-10-26. Chris's rulings the same day: the open list is a
**native popup**, through libui's existing popup machinery (the menu
session's owned popups, Roam's D013), not an in-window layer; and the
control is a **libui widget**, `os64_ui_dropdown`, not yonder's alone,
because other programs will want one.

## Why

yonder draws every `<select>` as a listbox. A select without `size` is a
one-line select in HTML, which a browser draws as a box showing the chosen
option with a list that drops from it; yonder gives it as many rows as it
has options (up to `SELECT_ROWS`), and a page's CSS that sizes it one line
high leaves one row visible and nothing to open. The React shop demo's sort
control is that: four options, one row, no way to see the others.

## Three steps, in order

1. **yonder's loop onto `os64_ui_dispatcher`** (this file's § Step 1). A
   native popup is a second window, and yonder waits on one. The dispatcher
   waits on all of a task's windows (`os64_gui_event_wait_any`) and is what
   the menu session runs on, so yonder must turn through it before it can
   own a popup.
2. **`os64_ui_dropdown`** in libos64: a closed box (the chosen label and an
   arrow) whose list opens as an owned popup. Waiting on a ruling
   (§ Step 2's question).
3. **yonder's one-line selects** (`multiple` absent, `size` 1 or absent)
   become dropdowns; a listbox stays a listbox.

## Step 1: yonder's loop through the dispatcher

yonder keeps its own loop. What changes is how a turn gets its events: the
wait on `g.win` and the batch read from it become yonder's own wait on all
of its windows, then one `os64_ui_dispatcher_turn` that does not sleep,
with yonder's window registered for its events. The wait stays yonder's
so that its sleep is measured as before (the record's busy share and
longest wait). Everything yonder does after a batch it still does after
the turn. It does NOT take the dispatcher's boundary hook: a menu session
claims its owner window's hook, and refuses an owner whose hook is taken.

| Before | Through the dispatcher |
|---|---|
| `os64_gui_event_wait(g.win)` | `os64_gui_event_wait_any` over yonder's window, the Settings dialog and (step 2) popups, then `os64_ui_dispatcher_turn(false)` |
| `yonder_event_batch_next`: at most `YONDER_EVENT_BATCH_MAX` events a batch, idle moves merged | the same function, unchanged, as yonder's window's poll in the dispatcher's backend (`os64_ui_dispatcher_native`, new, with `poll` replaced): the batch's end reads as an empty queue for the rest of the turn, and a lookahead it holds is the next turn's first event, so no sleep while one is held |
| per event: hover, coverage, Settings, bar, zoom, address, keys, `os64_ui_dispatch`, `inputs_run` | the same chain, in yonder's event callback |
| after the batch: relayout, the stream, coming, scripts, storage, cookies, pictures, title, paint, bells re-rung | after the turn, unchanged; the dispatcher's paint for yonder's window does nothing |
| the bar armed only when the queue is empty, a lookahead event held | while the bar is disarmed the next turn does not sleep: the events it finds go to the disarmed bar, and a turn that finds none (and holds none) arms it |
| the Settings dialog: a relay thread waits on its window and rings `BELL_SETTINGS`; the bell pumps it | no relay: `os64_ui_settings_attach` (new) registers the dialog's window, its events go to `os64_ui_settings_event` (new, the pump's body), and a dialog asked to close destroys its window through the dispatcher in the turn |
| a wait that answers anything but 1 ends yonder | so does a turn that answers below 0; the "leaving:" line says either |

The relay thread has to go, not just become redundant: `event_wait_any`
answers BUSY while another thread of the task waits on any of its windows.

Unchanged: the bells (`os64_gui_event_ring` delivers an event to
`g.win`, which wakes `wait_any` as it woke `wait`), the work pool, the
ticker, and every turn after the events.

**Proof.** The host suites compile yonder whole but never run its main
loop (the scripted-page host collects it away), so the loop's proof is
QEMU's, over the wire, and as built: TodoMVC's Svelte build loaded and
typed into; the Settings dialog opened, Dark pages ticked and applied
(yonder's page repainted dark behind it), closed, opened again, closed
with Escape; httpbin's order form scrolled, submitted, its reply reloaded,
the question bar asked and answered No; and the window closed, the log
saying `leaving: the window was asked to close`.

## Step 2's question: what carries a list longer than a menu

The menu session cannot carry a select's list as it stands. Its sizes are
a menu's: a model holds `OS64_UI_MENU_ENTRIES` (64) entries in fixed
arrays (`entries[64]`, `labels[64][128]`), a popup `rows[64]`, and a
registry `OS64_UI_ACTIONS_MAX` (256) actions, sized by arrays on the
stack (`ui_action.c`). A select of countries or birth years has 200 or
more options, and they are data, not commands. What the session does
that a dropdown needs is the hard part: the popup family's focus rules,
swallowing the click that dismisses, lost input, geometry, keyboard and
scrolling (`ui_menu.c`'s `focus_event`, `owner_event`, `panel_event`).

| Option | Cost | What it gives up |
|---|---|---|
| A. Through the session as it is | none | any select over 64 options (a fallback for longer lists means two controls) |
| B. Raise the session's caps | allocation in place of the fixed arrays, through the model, popup and registry | nothing, but a menu system sized for menus is resized for data, and it is Roam's (Quinn's) |
| C. The dropdown's own popup: a libui listbox in an owned popup window | small; the listbox has no cap | the session's dismissal rules, written a second time |
| D. **A list mode in the session** (lean): `os64_ui_menu_open_list` with rows from a callback (label, enabled), a selected row, and a pick callback; no registry, no model, no cap | a second content kind in the session's popup, which draws rows it is handed | nothing; the dismissal rules stay in one place |

The lean is D: one popup system, one set of dismissal rules, and the
data-sized part (the rows) is the dropdown's. It changes `ui_menu.c`,
which is Roam's, so it is Chris's to rule and Quinn's to see first.
