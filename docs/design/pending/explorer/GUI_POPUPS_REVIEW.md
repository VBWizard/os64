# E1K review — native popup sessions (Fable, 2026-10-11)

Reviewed: `GUI_POPUPS.md` as drafted 2026-10-11, against the kernel at
`6d988565` (`kernel/src/gui/gui_client.c`, `window.c`, `compositor.c`,
`event_queue.c`, `abi/include/os64/gui.h`), `userland/apps/grootmenu`,
`userland/libos64/ui.c`, GRAPHICS.md and the DEBTS/DECLINED ledgers. Also
read: `E01_MENUS.md`, `E01A_HANDOFF.md`, `E02_ICONS.md`, the umbrella, the
decision log.

**Verdict: the four gaps are real and correctly identified. The proposed cure
is the wrong size.** Every one of the four gaps closes with a small, general,
additive change to a contract the GUI already has, and each of those changes
has a second consumer standing in the tree today. The seven-call session
object with staged candidates, input epochs, a private 128-record queue,
a 64 MiB raster budget and a fused owner/panel publication barrier is a
transaction manager for menus. Nothing in the gap list needs one, and D013's
own boundary ("do not add a general modal-dialog, docking, drag/drop or IPC
subsystem here") argues against building one. The rest of this note is the
argument, the replacement, and what it means for E1B/E1C.

## 1. What the gaps actually need

The design's own list, read against the code:

| Gap | What the code does today | Smallest change that closes it |
| --- | --- | --- |
| 1. `focus.sibling` names a task, not a popup family | `focus_window` (window.c) compares `owner` task ids and sends 1 bit | The focus event carries the **handle** of the other party when it belongs to your task, else 0. Two bytes in a union with sixteen spare. A toolkit then knows "focus went to my cascade" from "focus went to my settings window" exactly. |
| 2. Popup birth steals focus; destroying a focused popup picks the heir by stacking (`wm_destroy`: `w->below ? w->below : s_top`); `gui_window_focus` refuses popups | Every window is a stranger to every other | An **owner link**: a popup is attached to an owner window of the same task. When a popup dies *still holding focus*, focus returns to its owner. That is the whole of "focus restoration", it is conditional by construction (a popup dismissed because the user clicked elsewhere no longer holds focus, so nothing is restored), and it is `WM_TRANSIENT_FOR` / Win32's owned-window rule, both older than this reviewer. |
| 3. No atomic "where is my content on screen"; insets are dynamic | `get_state` reports the frame; insets come from the installed decoration under `kGuiLock` | One read: **content rect on screen** for an owned window. Placement, flipping and clamping stay in userland, where grootmenu already does them against `screen_info`. |
| 4. `event_wait` waits on one window; grootmenu polls at 30 Hz | `win->waiter` is one slot per window | **Wait on any owned window**: block until some window of the caller has a pending event and return its handle; the caller polls that handle. This is `select()` for windows and it is the gap that matters most, because it is not a menu gap at all: Appearance, Frame Studio and Mouse Settings each own a second window today and cannot sleep on both. |

Note what is *not* on that list. There is no requirement that keyboard focus
stay at the owner, no requirement that the kernel route input by panel id,
and no requirement that the panels of a menu be published atomically. Each of
those is a consequence of choosing the X11 grab model, and the design chose
it without saying why the simpler model — popups are windows, they take focus
like windows, and the two things windows cannot do today (name their family,
hand focus back) get fixed — was rejected. grootmenu is the proof that the
simple model works: its only defects are the poll loop (gap 4), the heir rule
(gap 2) and the one-bit sibling test (gap 1).

## 2. The replacement, in full

Four additive changes. Each follows an established contract, so under D012
each is routine kernel work with ordinary review, not a Fable gate.

### 2a. Owner link (`popup attach`)

- A new call attaches an owned POPUP window (the latched `GUI_WINDOW_POPUP`
  bit, so the window system already knows what it is) to an owner window of
  the same task. Owner must be live; the owner's own chain must not reach
  the popup (cascades attach to their parent level, so chains are legal and
  bounded by the handle table); a second attach is `BAD_ARGS`. Stored as the
  owner's **window id**, never a pointer (window.h's rule for anything held
  across a lock release). The popup is never raised above its band rules; it
  is already PINNED.
- **Focus return.** In `wm_destroy`, if the dying window holds focus and has
  an owner that is live, visible and belongs to the same task, focus goes to
  that owner (`wm_raise` within its band), else the existing rule applies.
  Escape and selection paths therefore land back on the document; an outside
  click already moved focus and triggers nothing. This is exactly D013's
  prohibition on *unconditional* focus restoration, satisfied in the kernel.
- **Destroy refuses while popups are attached** (`BUSY`): the mount-under-mount
  rule, "unmounting one thing must not take out a second thing nobody named".
  The task-exit sweep bypasses it, as it bypasses everything.
- **The focus event names the other party.** Extend `focus` to
  `{gained, sibling, other_handle}` (a `uint16_t`; handles are 1..32 today and
  will never exceed 65535). `sibling` stays for the existing consumers.
  `wm_create` already takes the owner for the birth grab; it takes the handle
  the same way (gui_client chooses the slot before calling it). Static asserts
  on both sides move with the field.
- **Popups leave the Alt+Tab ring** (`wm_recency_ids` skips `POPUP` as it
  skips `DESKTOP`). This is a wart grootmenu has today: its levels appear in
  the switcher strip.
- **The owner keeps its active chrome** while one of its popups holds focus
  (`composite_one`'s `focused` test gains "or the focused window's owner
  chain reaches me"). Cosmetic, one line, and what every desktop shows.

### 2b. Wait on any owned window

- `event_wait_any(out_handle)`: under `kGuiLock`, scan the handle table for
  the caller's windows; if one has a pending event, return its handle (peek
  semantics, the `out == NULL` form of today's wait); otherwise register the
  caller as waiter on **every** owned window and park on the same SIGSLEEP
  backstop. `wm_deliver_event` already wakes a window's waiter, so no new wake
  path exists. Every exit path clears the caller from every slot it took, at
  the top of every pass, per the stale-waiter rule.
- **One waiter per window, enforced**: a slot already naming another thread
  answers `BUSY` rather than silently overwriting it. Tripwire over silence.
- Handle reuse after destroy is the same hazard it is today; the caller polls
  the returned handle and treats `INVALID_HANDLE` as "it died while I slept".
- Second consumers, today: grootmenu's 33 ms poll loop goes away; Appearance
  and Frame Studio sleep on both their windows; gterm could own tabs.

### 2c. Content rect readback

- `window_get_content_rect(handle, rect_t *)`: the content area's screen
  rectangle, computed under `kGuiLock` from the frame and the installed
  insets, for an owned window. `rect_t` already crosses the ring. A menu
  anchors to `content.y + bar_height`; a cascade anchors to the parent
  level's own content rect plus its row offset.
- The "atomic placement" worry does not survive contact: the only way an owner
  moves while a menu is open is a press on the owner (chord or titlebar),
  which gives the owner focus and dismisses the menu first.

### 2d. Popup capacity is its birth size

- Today every window reserves two screen-sized surfaces (DEBTS § canvas
  capacity: 6.3 MB per window at 1024x768, roughly 16 MB at the P5's
  resolution). An eight-level grootmenu cascade already costs that eight
  times over. For a window born `POPUP`, `wm_canvas_capacity_for` answers the
  content size instead of the screen. Nothing else changes: `wm_clamp_frame`
  already clamps a resize to capacity, so a chord-resize of a popup can only
  shrink it, and maximize is already declined for popups. The pitch contract
  in `gui.h` stays true (pitch is still the capacity's width). This pays the
  memory half of the design with no new allocation class, no raster budget
  and no managed-panel lifetime rules.
- This changes a published rule (`OS64_GUI_WINDOW_DIM_MAX`'s capacity comment
  in gui.h, `canvas_cap_w`'s in window.h): both say "capacity is the screen".
  They become "the screen, or the content size for a popup". Same commit.

### 2e. Not proposed, and why

- **Staged/hidden candidate panels and a publication barrier.** A menu level
  is create, draw, publish, in the same millisecond. A font change while a
  menu is open is a close-and-reopen in the toolkit, or a repaint per panel;
  the worst case is one frame in which two panels disagree about the font,
  and no window system I know of promises better across windows. If a gray
  flash between create and first publish is ever *seen* on a screendump, the
  cure is a "born hidden until first publish" flag, which is small and
  general. Measure first.
- **A per-session input queue and INPUT_OVERFLOW.** The per-window ring with
  "focus is never dropped" (Codex #34) is the contract menus need. Routing by
  panel id is what windows already are.
- **Kernel-consumed dismissing clicks.** The press that dismisses a menu lands
  on whatever is under it and is routed as always. The toolkit sees two facts
  from the same press, in some order: focus-lost on the popup naming the
  owner as `other`, and the button-down on the owner. Whichever it sees
  first, the menu session is still open or the toolkit has just closed it
  for that reason, so "swallow this press and its release" is a deterministic
  toolkit rule with no kernel help. The opening press is already safe: the
  implicit grab (`s_pointer_window`) delivers its release to the owner, never
  to the popup.
- **"VT departure" as a cancellation reason.** This contradicts a ruling in
  DECLINED/DEBTS (PR #34, 2026-08-26): a popup survives a VT switch, on
  purpose, and a kernel-to-WM notification for it was judged not worth
  having. Drop it.
- **Root-launcher "enrollment mode".** With popups-are-windows there is no
  mode: grootmenu attaches each level to the one above and the root to
  nothing, and replaces its poll with `event_wait_any`.
- **Owner geometry/minimize/close rules.** Minimize and close chords act on the
  focused window, which is the popup: minimize is declined (`popup_declines`),
  close delivers `WINDOW_CLOSE` to the popup, which the toolkit treats as
  dismiss. Resize of the owner needs a press on the owner, see 2c.

## 3. Two questions the design asked me to decide

**The `task->heapEnd` race (design § Lifetime).** Confirmed, and it is not a
popup question. `syscall_map` and `gui_window_create` both read and advance
`task->heapEnd` with no lock (create reads it *before* `kGuiLock` and nothing
guards `map` at all), and `paging_map_page` silently overwrites a present PTE.
Two threads of one task (`SYSCALL_THREAD` exists) racing a map against a
window create, or two creates, map two extents at one VA; the loser's pages
are unreachable and are freed on destroy through a mapping that now belongs
to the winner. Fix: a CAS loop or a tiny per-task lock on the bump, in both
sites, as its own small PR with its own fixture. It does not block E1; it
predates E1 and I would pay it this week.

**The sibling-thread stale-TLB caveat on destroy.** Unchanged by any of the
above and not a prerequisite. Popups are the windows most often destroyed,
so the exposure is the same shape as today's grootmenu, at the same accepted
size (a burned VA that can never alias a live mapping; exposure ends at the
sibling's next CR3 load). No VA-reservation or shootdown slice is needed for
E1. Book nothing new.

## 4. Things found on the way (outside E1K)

- `GUI_MAX_WINDOWS` is 32, global, and a desktop with two gterms, a clock,
  Scribe with a four-level menu and grootmenu's three levels is at thirteen.
  Raising it to 64 is 256 bytes; do it when the first app refuses a window.
- grootmenu's levels show in the Alt+Tab strip (2a fixes it).
- libui's `WINDOW_FOCUS` handler (`ui.c`) cancels the grab and the focused
  widget on *any* focus loss. Once a popup can take focus from its owner,
  that handler must not cancel a menubar press because its own dropdown was
  born — it reads `other_handle` and leaves the owner's state alone when the
  focus went to one of its own popups. E1B's job; noting it so it is not
  rediscovered.

## 5. Consequences for E1 and the records

- `GUI_POPUPS.md` is superseded in shape, not in intent: keep its gap
  analysis and its evidence table (the diagnostic-client cases are still the
  acceptance list), replace §§ object model, API, placement, input and limits
  with the four changes above. Explicit ABI: three syscalls (`attach`,
  `wait_any`, `content_rect`), one event field, one capacity rule, one
  recency rule. Error values are the existing `OS64_GUI_ERR_*` set plus
  `BUSY`, which this boundary has not needed before and should add as its
  own number rather than borrowing `BAD_ARGS`.
- `E01_MENUS.md` § Event routing: "E1K wakes the owner's existing event wait"
  becomes `event_wait_any`; § Visibility and focus: "native keyboard focus
  stays with the owner" becomes "the popup takes focus; the owner's
  focus-lost names it; the kernel returns focus on dismissal". § Layout:
  the font barrier becomes toolkit-side close/reopen or per-panel repaint.
- D013's text ("do not emulate native ownership with app-specific polling and
  unconditional focus restoration") is satisfied: the polling goes with 2b,
  the restoration is kernel-side and conditional with 2a.
- Each of 2a–2d is independently shippable and independently useful; 2b and
  2d have consumers that could land before a single menu widget exists. I
  would ship 2b first (it retires grootmenu's poll loop the same day), then
  2a+2c together with the diagnostic client, then 2d.

## 6. The other model, stated fairly: keyboard stays at the owner

E1K's draft and § 2 above are two different answers to one question: **while a
menu is open, who holds keyboard focus?** The four gaps close either way; the
choice is about shape, cost and what each model teaches the toolkit.

| | Popups take focus (§ 2) | Owner keeps focus (E1K's grab model) |
| --- | --- | --- |
| Lineage | Win32 owned windows; grootmenu today | X11 `XGrabKeyboard`/`XGrabPointer` with override-redirect windows; GTK and Qt menus |
| Where menu keys arrive | The focused popup's own queue (the deepest cascade) | The owner's queue; the toolkit forwards to the menu controller |
| Who dismisses on an outside click | Focus loss, which the WM already produces for free | The grab: the compositor must know a grab is held and report the outside press to the grab holder instead of routing it |
| Kernel additions | Owner link, `wait_any`, content rect, popup capacity | The same four, **plus** a grab object: begin/end, a holder, routing rules in `route_event_locked` for press/motion/wheel while held, grab death on owner death/task exit, and a rule for which WM chords break a grab |
| Titlebar look while open | Owner paints active through the owner chain (one line) | Owner is still focused; nothing to do |
| Minimize/close chords while open | Act on the popup: minimize declined, close dismisses the menu | Act on the owner; the toolkit must cancel the menu when the owner minimizes (no event exists for that today, so one is added) |
| Dismissing-click consumption | Toolkit rule, deterministic (§ 2e) | Natural: the grab holder sees the outside press and the kernel never routes it; closest to Chris's stated wish |
| Root launcher | No special case | grootmenu's root window must be its own grab holder; a second shape for the same widget |
| Surprise for existing apps | libui's focus-loss handler learns to read `other_handle` (§ 4) | None; owners never lose focus to their own menus |
| What it is the seed of | Dialogs and tooltips (transient-for) | Drag-and-drop (E12 needs a pointer grab across windows anyway) |

What the grab model buys that § 2 does not: the owner never sees a focus
transition at all, so nothing in a toolkit's focus-loss path can misfire; the
dismissing click is consumed by construction; and E12's cross-window drag will
want a pointer grab, so the object would have a second customer later. What it
costs: a new kernel object with a lifetime, one more place the compositor's
routing forks (`route_event_locked` already forks on control capture, WM
buttons, drag, band and the implicit grab), and a toolkit that must forward
keys from the owner's queue rather than read them from the popup's.

My lean stays with § 2, because the grab object is the only genuinely new
kernel lifetime in either plan and E1 does not need it; when E12 does, the
grab can be designed against a real drag, with the owner link and `wait_any`
already in place underneath it. But if the deciding value is "the application
never loses focus to its own menu", the grab model is the honest way to get
it, and the table above is what it costs. Either way E1K should be rewritten
as a choice between these two, not as the only way to close the four gaps.

Disposition is Chris's.
