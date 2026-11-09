# DROPDOWN_REVIEW.md — Fable's read of DROPDOWN.md

Fable 5.1, 2026-10-26, against `2ef671ee` in the dropdown worktree. Read:
DROPDOWN.md, `ui_menu.c`, `ui_menu_widget.c`, `ui_menu_internal.h`,
`ui_dispatcher.c`, `ui_list.c`, `ui_settings.c`, yonder's loop and form
plumbing, `GUI_POPUPS.md` and D013–D015.

## Verdict

**D, and the doc's reasoning for it is right — but D is bigger than the
table says, and the strongest argument for it is missing from the table.**
One correction to step 1 to carry into step 2. Three things the design
should say before the list mode is built, because each one is a lifetime or
contract question and cheaper on paper.

## Why D, stated the way the code states it

The doc argues D from taste: one popup system, one set of dismissal rules.
The code argues it from structure, and that argument closes the question:

- **An owner window has ONE predispatch slot and ONE boundary hook, and the
  menu session owns both.** `os64_ui_menu_session_create` installs
  `ui->menu_event` and claims the owner entry's `boundary`; it refuses an
  owner whose boundary is already taken, and a `ui` that already has a
  `menu`. So a second popup system on the same window (option C) cannot
  coexist with a menu session. yonder has no menubar today, so C would
  work in yonder — and fail the day Roam or Scribe wants a dropdown in a
  dialog, which is exactly the "other programs will want one" Chris ruled
  on. C is not "the rules written twice"; it is "a window may have a menu
  or a dropdown, never both."
- **The kernel contract already names the dropdown as the session's
  consumer.** GUI_POPUPS.md § Ownership: "Cascades name the parent level; a
  dropdown names the document." A list mode in the session is what E1K was
  written to carry.
- **B is the wrong vocabulary, not just the wrong size.** The registry is
  actions; options are data. Raising the caps would also let the menu
  model grow to 200 rows, which nothing wants.

Put the first bullet in the doc's table under C. It is the reason that
survives a reader who disagrees about taste.

## What D actually costs — say it

The table calls D "a second content kind in the session's popup, which
draws rows it is handed." Two places where that undersells it:

1. **The popup widget lays out EVERY row at open.** `os64_ui_popup_create`
   runs `fit_text` (a run layout) for each of the model's rows and keeps a
   `menu_row` (two 128-byte buffers, two runs, an icon lease, a copied
   spec — about 250 bytes) for each in `rows[64]`. A select of 200
   countries would lay out 200 runs on every open. The listbox
   (`ui_list.c`) already has the house rule for this: runs for the rows
   the box can SHOW, re-laid as they scroll in. The list panel must stage
   by viewport, not by count. That is the real work in D: the popup
   widget's row storage becomes a viewport window for the list kind, with
   the menu kind keeping its whole-model array. Geometry (`row_h`,
   `viewport`, `os64_ui_popup_row/hit/select/scroll`) is shared as is.
2. **"A second content kind" is a seam, not a flag.** The session reads
   `p->model` and `s->view` at the sites a list has neither: `menu_selectable`
   (panel_select, activate, select_step, select_page, reconcile_selection,
   the paint), the mnemonic scan in `key_event` (`p->model->entries[i]`),
   `help` (`p->rows[].spec.help`), `child`/`invoke` (model entries, the
   view), `os64_ui_menu_open` (refuses `!v`), `os64_ui_menu_close`
   (releases the view). `os64_ui_action_shortcut_id` tolerates a NULL view;
   the rest do not. Cut it as a per-panel content seam — count,
   selectable(i), label(i), activate(i) — rather than `if(list)` at a dozen
   sites, or the next content kind (a colour picker, a font list) pays the
   dozen again. The mnemonic scan becomes first-letter type-ahead for a
   list for free: a row's "mnemonic" is its label's first letter, and the
   scan's `next` already cycles — which is exactly HTML's select behaviour.

Neither changes the verdict. Both change the estimate from "small" to "a
day", and the doc should carry the honest number.

## Three contracts the doc should state

**1. Who closes the list when its anchor goes away.** The session closes on
owner RESIZE/CLOSE (`OS64_UI_MENU_GEOMETRY`), INPUT_LOST, and focus loss;
it does not know when the WIDGET under the list moves, hides or dies. In
yonder that happens without a resize: `forms_build` destroys a widget
whose node a script replaced, `forms_place` hides one the page scrolled
out of view (a script `scrollTo`, a layout shift as a stream arrives), and
`relayout` moves them all. The menubar's answer is the model:
`os64_ui_menubar_destroy` refuses to proceed until `os64_ui_menu_close`
succeeds. The dropdown widget's `destroy`, its hide, and a bounds change
while open each close the list (GEOMETRY), and `destroy` refuses when the
close is refused — the BUSY-destroy rule the session already has for its
own panels. Without this, a replaced select leaves a popup owned by a
widget that no longer exists, and the pick callback fires into freed
memory.

**2. Highlight is not pick.** An open dropdown's Up/Down move the
HIGHLIGHT; the value changes on Enter, Space or a click, and `change`
fires once, at commit (Chrome and Firefox both). The session already
separates these: `panel_select` is the highlight, `activate` the commit,
and `invoke` closes the family BEFORE the command runs. The list mode's
pick callback must fire from the same place `invoke` does — after close,
with the index — so yonder's `list_changed` → `os64_page_set_chosen` →
`IN_LIST` runs once per commit, never per arrow. Say it, because the
listbox widget yonder uses today fires `on_change` on every arrow, and the
natural first cut reuses it.

**3. The closed box's keys.** The doc describes the open list and says
nothing about the box. A focused closed select in a browser: Up/Down
change the value directly (with `change` on each), Space/Enter/Alt+Down
open the list, a letter jumps. v1 needs at least Space/Enter/Alt+Down
open and Up/Down change; the letter jump can wait for a consumer. This
is the widget's `event`, not the session's, and it decides whether a
keyboard user can use the React shop's sort control at all.

## A step 1 finding to carry into step 2

**yonder's batch cap defeats the dispatcher's quarantine.** The
dispatcher clears its tombstones when every live entry's poll has answered
0 since the destroy (`drained`), and only then does `can_create` go true —
D015's "drain surviving queues after destruction before new creation."
yonder's `window_poll` answers 0 at the batch's END, and
`yonder_event_batch_next` ends a batch at `YONDER_EVENT_BATCH_MAX` (32)
WITHOUT setting `batch.drained`. So a turn that hits the cap marks
yonder's entry drained while its queue still holds events, the tombstones
clear, and the next create may reuse the dead handle — with a stale
`WINDOW_FOCUS` naming that handle still queued. Today that needs a
Settings close under 32 non-mergeable events. In step 2 it is every
dropdown: close one under a held button or a key run, open the next, and
`focus_event` reads the old popup's focus-return as "the owner gained
focus from a live family member" and dismisses the new list at birth.

The fix is one condition, and it is yonder's: while
`!os64_ui_dispatcher_can_create(&g.dispatcher)` the batch ignores its cap
(the quarantine is rare and must finish before anything else can happen;
the cap exists for paint latency, which a quarantine turn does not owe).
Or `window_poll` keeps answering 1 past the cap while `dead_count` is
set, which is the same thing said in the backend. Either way, the comment
on `window_poll` ("the batch's end reads as an empty queue") stops being
the whole truth and should say the exception.

## Smaller notes

- **Face and zoom.** yonder's form widgets wear the page face at the page's
  zoom (`os64_ui_widget_app_face`, `select_size` measures with
  `OS64_UI_FONT_APP`). The popup draws in `OS64_FONT_ROLE_UI`, the owner's
  interface face, at no zoom. So the closed box will be at 150% and its
  list at 100%. Browsers differ here (Chrome scales the popup, Firefox's
  GTK popup does not). Acceptable for v1; write it down so nobody files it.
- **200 rows scroll by arrow strips and wheel**, which is how the popup
  scrolls today; a select's list has a scrollbar. Fine for v1 — the strips
  are what a classic menu used — but it is a visible difference on a long
  list, and worth a line in DEBTS when step 3 lands.
- **`disabled` is per option and inherits from `optgroup`**
  (`os64_page_option_t.disabled`), which the (label, enabled) row callback
  carries. An optgroup HEADING has no row in libpage's option list, so
  v1 shows a flat list. Fine; say so.
- **Step 1's settings attach is sound.** The entry is copied by value in
  the turn, so a destroy from inside `attached_event` is safe; the paint
  pass re-reads the table. `os64_ui_font_release` after
  `os64_ui_dispatcher_destroy` is fine: the binding is a userland text
  context with no window dependency.
- **The Settings dialog and the list.** A click on the dialog while a
  list is open is focus-loss to a non-family window → LOST → close; the
  dialog gets the click once. Correct, and GUI_POPUPS.md's "same task is
  insufficient" is what makes it so. No change needed.
