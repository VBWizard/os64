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
   arrow) whose list opens in a list mode of the menu session (§ Step 2).
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
| `yonder_event_batch_next`: at most `YONDER_EVENT_BATCH_MAX` events a batch, idle moves merged | the same function, unchanged, as yonder's window's poll in the dispatcher's backend (`os64_ui_dispatcher_native`, new, with `poll` replaced): the batch's end reads as an empty queue for the rest of the turn, and a lookahead it holds is the next turn's first event, so no sleep while one is held. Except during the dispatcher's quarantine (below) |
| per event: hover, coverage, Settings, bar, zoom, address, keys, `os64_ui_dispatch`, `inputs_run` | the same chain, in yonder's event callback |
| after the batch: relayout, the stream, coming, scripts, storage, cookies, pictures, title, paint, bells re-rung | after the turn, unchanged; the dispatcher's paint for yonder's window does nothing |
| the bar armed only when the queue is empty, a lookahead event held | while the bar is disarmed the next turn does not sleep: the events it finds go to the disarmed bar, and a turn that finds none (and holds none) arms it |
| the Settings dialog: a relay thread waits on its window and rings `BELL_SETTINGS`; the bell pumps it | no relay: `os64_ui_settings_attach` (new) registers the dialog's window, its events go to `os64_ui_settings_event` (new, the pump's body), and a dialog asked to close destroys its window through the dispatcher in the turn |
| a wait that answers anything but 1 ends yonder | so does a turn that answers below 0; the "leaving:" line says either |

The relay thread has to go, not just become redundant: `event_wait_any`
answers BUSY while another thread of the task waits on any of its windows.

**The cap must not lift the quarantine** (Fable's review, DROPDOWN_REVIEW.md,
found it in step 1 as built). After a window is destroyed the dispatcher
quarantines its handle (`can_create` false) until every live entry's poll
has answered empty since (Roam's D015, explorer/DECISIONS.md: drain
surviving queues after destruction before new creation). A batch that ends at its cap answers empty with
events still queued, so the quarantine lifted early, and the next popup
could be made with the dead one's handle while a stale `WINDOW_FOCUS`
naming it was still queued; the session would read that as focus
returning from the family and dismiss the new list at birth. While the
quarantine holds, the cap starts a new batch instead
(`yonder_event_batch_draining`, events.h, a scripted-page host case).

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

## Step 2: a list mode in the menu session

**Ruled D** (Chris, with Fable's review, 2026-10-26): the list is a second
content kind in the menu session, cut as a seam in `ui_menu.c`, and the
session's popup controller stays where it is. Quinn sees the cut before it
is built: the file is Roam's. **The menu session is the owner window's
popup controller, and a menu is one thing it carries; a dropdown's list is
the second.** The name stays. DEBTS.md books the extraction of the
controller for the day a third content kind arrives: today's second
consumer earns the seam, a third would earn the split, and the seam makes
that split cheap.

### Why not the others

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
| C. The dropdown's own popup: a libui listbox in an owned popup window | small; the listbox has no cap | more than the rules written twice: an owner window has ONE predispatch slot (`ui->menu_event`) and ONE boundary hook, and a menu session takes both and refuses an owner whose are taken. So a window could have a menu or a dropdown, never both; yonder has no menubar and would not notice, and the first dialog in Roam or Scribe with a dropdown would |
| **D. A list mode in the session** (ruled): rows from a callback, a selected row, a pick callback; no registry, no model, no cap | about a day (below) | nothing; the dismissal rules stay in one place |

The kernel's contract already names this consumer: GUI_POPUPS.md
§ Ownership and focus, "Cascades name the parent level; a dropdown names
the document." B is the wrong vocabulary as well as the wrong size: the
registry is actions, options are data, and raised caps would let a menu
grow to 200 rows, which nothing wants.

### What D costs

- **A content seam, not a flag.** The session reads the model and the
  action view where a list has neither: `menu_selectable` (selection,
  activation, stepping, paging, reconciling, painting), the mnemonic scan
  in `key_event`, `help`, `child` and `invoke`, `os64_ui_menu_open` (which
  refuses no view) and `os64_ui_menu_close` (which releases it). It is cut
  as a per-panel seam, count, selectable(i), label(i) and activate(i),
  rather than an `if (list)` at a dozen sites, which the next content kind
  would pay again. The mnemonic scan's matching and cycling give a list
  first-letter type-ahead, a row's mnemonic being its label's first
  letter, but for a list a match only HIGHLIGHTS, a unique one included:
  the menu's scan activates on a unique match, which in a list would
  commit and close on one keystroke (Quinn's review, 2). A menu's
  activation is unchanged.
- **Rows staged by viewport.** The popup widget lays every row of its
  model out at open (`fit_text`, a `menu_row` of about 250 bytes each, in
  `rows[64]`). The list kind keeps a window of rows the panel can show,
  laid out as they scroll in, the listbox's rule (`ui_list.c`); the menu
  kind keeps its whole-model array. The geometry (`row_h`, the viewport,
  `os64_ui_popup_row`, `_hit`, `_select`, `_scroll`) is shared as it is.

### Three contracts

1. **The list closes when its anchor goes.** The session closes on the
   owner's resize and close, lost input and focus; it cannot know when the
   WIDGET under the list moves, hides or dies, and in yonder that happens
   with no resize: `forms_build` destroys the widget of a node a script
   replaced, `forms_place` hides one scrolled out of view, `relayout` moves
   them all. The dropdown's destroy, its hide, and a change of its bounds
   while open each close the list (GEOMETRY). A destroy cannot refuse, as
   the menubar's does, because yonder's `widget_destroy` cannot hear a
   refusal: it frees the `FormWidget` whatever its class says (Quinn's
   review, 1). So the list borrows nothing of the caller's: the session
   COPIES the rows at open, and the destroy first DETACHES the caller
   (`os64_ui_menu_list_detach`, which cannot fail: no pick is called after
   it), then asks for the close. A close the native destroy refuses leaves
   a popup that holds its own copy and calls no one, and the session
   retries the close at its next boundary. A pending list (below) is
   cancelled by the same detach.
2. **The highlight is not the pick.** In an open list Up and Down move the
   highlight; the value changes on Enter, Space or a click, and `change`
   fires once, at the commit, as Chrome and Firefox do. The session already
   separates them (`panel_select` the highlight, `activate` the commit,
   `invoke` closing the family BEFORE the command runs), and the list's
   pick callback fires where `invoke` does, after the close, with the
   index. So yonder's `list_changed`, `os64_page_set_chosen` and `IN_LIST`
   run once a commit, never once an arrow (the listbox yonder uses today
   fires `on_change` on every arrow, which the natural first cut would
   reuse). And a commit is not a change: the session reports every pick,
   and the dropdown tells yonder only when the picked row is not already
   the value, so Enter on the current value, or moving away and back and
   committing, closes with no `input` or `change`, as the listbox's
   `choose` already suppresses an unchanged index (Quinn's review, 4).
3. **The closed box's keys.** Focused and closed: Up and Down change the
   value directly, `change` each time; Space, Enter and Alt+Down open the
   list. A letter's jump waits for a consumer. These are the widget's,
   not the session's, and they decide whether a keyboard can work the
   shop's sort control at all.

### The cut (read by Quinn, DROPDOWN_REVIEW_QUINN.md; her four findings are folded in)

**Public** (`ui_menu.h`), one call beside `os64_ui_menu_open`:

```c
/* A list the session carries in place of a menu: one level of rows, no
 * actions, no cascades, no help line. The rows are COPIED at open. */
typedef struct {
    const char *const *labels;         /* `count` labels, read only during the call */
    const bool *enabled;               /* `count` flags, read only during the call */
    size_t count, selected;            /* the row highlighted at open; count or more: the first enabled */
    int32_t min_width;                 /* the anchor's: a list is at least as wide as its box */
    void (*pick)(void *user, size_t row);   /* after the list has closed, once per commit */
    void *user;
} os64_ui_menu_list_t;
os64_ui_status_code_t os64_ui_menu_open_list(os64_ui_menu_session_t *, const os64_ui_menu_list_t *,
                                             os64_gui_rect_t anchor);
/* No pick reaches `user` after this, and a list still pending is cancelled;
 * then the caller asks for the close. Cannot fail. */
void os64_ui_menu_list_detach(os64_ui_menu_session_t *, void *user);
```

The session keeps its own copy of the labels and flags for the list's
life, so a caller may change or free its rows once the call returns; a
caller whose rows change while the list is open closes it (contract 1),
because the list shows what they were. A failed open reports as
`os64_ui_menu_open` does, through the error hook.

**A pending list** (Quinn's review, 3). An open that meets the
dispatcher's quarantine is deferred: `request_panel` holds the menu
model in `s->pending` and the boundary hook opens it after the drain.
A list has no model, so the pending request carries either kind: the
model, or the session's copy of the list (made at the call, so nothing
is borrowed while it waits). Replacing a menu with a list is the
ordinary way to meet the quarantine (the menu's popup was just destroyed),
so this is the common path, not a corner. A detach, or the anchor's
going, cancels a pending list as it closes an open one.

**Inside**, the seam is the popup's content, not the session's state.
`os64_ui_popup_t` gains a content kind and four questions every site that
reads the model asks instead: `count`, `selectable(i)`, `label(i)` and,
in the session, `activate(level, i)`. For a menu they answer from the
model and the action view exactly as today; for a list, from the
session's copy of its rows. Each site that reads the model is one of
those, and so is the pending request:

| Site | Menu (unchanged) | List |
|---|---|---|
| `menu_selectable` | separator, child, action state | the copied `enabled[i]` |
| `height`, `final_top`, `viewport`, `select_step`, `select_page` | model count, separators | `count`, no separators |
| `popup_paint` | check or radio, icon, label, legend, arrow | the label; the row that is the value marked as a radio is |
| `key_event`'s mnemonic scan | declared mnemonics; a unique match activates | the label's first letter; any match only highlights |
| `help` | the spec's help | none |
| `activate` | `child` or `invoke` | close the family, then `pick(i)` unless detached, as `invoke` closes before it runs |
| `os64_ui_menu_open` / `_close` | retain and release the view | no view; the close frees the copy |
| `s->pending`, `request_panel`, `boundary` | the retained model | the copied list, cancelled by a detach |

The list's rows are staged by viewport: the popup keeps runs only for
the rows it can show, laid out as they scroll in and released as they
leave; the menu kind keeps its whole-model `rows[64]`. Its width is the
widest label measured once at open (measured, not kept), at least
`min_width`, at most the screen's. Nothing else in the session changes:
the owner hooks, focus family, dismissal, suppression, quarantine and
placement are the same code for both kinds, and the boundary hook gains
only the pending list and the retried close of a detached one.

**Acceptance** (Quinn's checks, to be the cases): a native destroy
refused during a select's replacement and during navigation leaves no
callback reaching freed storage, and the close is retried; a menu
replaced by a list during the quarantine opens after the drain, and a
dropdown removed before the drain leaves no popup and no callback; a
unique or repeated initial moves only the highlight, and Escape then
keeps the value; Enter, Space and a click commit; Enter or a click on
the current value, or away and back, fires no `input` or `change`, and a
different value fires each once.

### Accepted for v1

- The closed box wears the page's face at the page's zoom, as yonder's
  form widgets do; the list draws in the interface face, unzoomed, as the
  session's popups do. At 150% the box is larger than its list; Chrome
  scales its popup and Firefox's does not.
- A long list scrolls by the popup's arrow strips and the wheel, a classic
  menu's way; a browser's select list has a scrollbar (DEBTS.md § GUI).
- libpage's option list has no row for an optgroup's heading, so the list
  is flat; an option a disabled optgroup holds is disabled (the row's
  enabled flag carries it).

## Steps 2 and 3, as built

**The session's list** (`ui_menu.c`, `ui_menu_widget.c`):
`os64_ui_menu_open_list` and `os64_ui_menu_list_detach` as the cut says,
the popup's content answered through `menu_count`, `menu_separator`,
`menu_selectable`, `menu_label` and `menu_mnemonic`, and a list's runs
staged by viewport (`stage_list`). The session's boundary hook opens a
pending list after the drain, retries a detached list's refused close,
and closes a list whose anchor widget was hidden or moved. A label is
kept to 127 bytes, cut at a character boundary, which is what a row lays
out.

**The widget** (`ui_dropdown.c`, declared in `ui_menu.h`): the closed box
in the field colours with its arrow, its caption staged with the face. It
reads a HID keyboard's arrows and Enter as the session reads its named
keys, as well as from an ESC [ burst: `os64_ui_decode_key` reads only the
burst. `os64_ui_dropdown_set` closes an open list only when the rows'
count or the value changes, so yonder syncs from its model as often as it
likes without closing the list under the person's pointer.

**yonder**: a one-line select (`select_one_line`) is an `FW_DROPDOWN`,
sized one row and twenty pixels wider for the arrow; its pick and its
keys reach the page through the listbox's own path (`option_chosen`:
libpage's choice, then `IN_LIST`'s `input` and `change`). yonder makes its
window's menu session after registering the window, and disposes of it
first as it leaves. The hosts have no popups: `yonder_host_menus.c` keeps
a record of the list asked for and plays the person's pick into it.

**Proof.**
- The menu host (`tools/test_ui_action_host.py --menu --real`), 164,831
  checks, 0 failures (150,444 before, unchanged for menus): the list's
  cases, Quinn's acceptance checks under the path invariants (which now
  know a list), and the widget over a real session (a click, Space and
  Alt+Down open it; a commit of the value and of another; the closed
  box's keys; a set that changes nothing and one that does; destroyed
  while open). Eight hand mutants of the rules (a letter that commits, a
  detach that keeps the pick, an unwatched anchor, a list that never
  waits, rows borrowed, a same-value commit that notifies, a destroy that
  keeps the list, no named keys) were each caught.
- The scripted-page host, 8,110 checks: a one-line select is a dropdown
  and one with a size a listbox; a click opens its list on the value with
  the disabled option refused; a pick of another option fires `input` and
  `change` once, and a pick of the same, nothing.
- QEMU, over the wire, the shop demo's inventory page (its sort control):
  a click opens the native popup with its four options, the value marked;
  a pick re-sorts the products (React heard the change); Escape closes it
  with no change; Down and Up on the closed box change the value and
  re-sort. After a change the select loses focus to the page, which is
  the page's: react-burger-menu's effect blurs `document.activeElement`
  whenever the page re-renders with its menu closed, as it would in
  Chrome.
