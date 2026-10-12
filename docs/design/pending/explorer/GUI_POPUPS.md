# E1K — Native owned popups

**Status:** architecture revised after [Fable's review](GUI_POPUPS_REVIEW.md),
2026-10-11. Quinn selects focus-taking owned windows. The whole E1K packet is
**Implemented; awaiting ordinary code review** following Fable's second pass and the concrete
[E1K handoff](E01K_HANDOFF.md). See the [evidence](evidence/e01k/README.md); Chris confirmed QEMU and P5 validation after the diagnostic fix. [Disposition](GUI_POPUPS_DISPOSITION.md) records what
was accepted, changed and declined. E1A is merged and Chris has completed its
P5 walkthrough. E1K implementation owner: Quinn.

Chris selected native menus that can extend beyond their application window
(D013), and delegated feedback disposition to Quinn. This is the GUI prerequisite,
separate from the future E6 message-channel design. Inspected source: `userland`
at `6d988565`; relevant GUI sources match the design branch parent `f3f5f50e`.

## Problem and selected boundary

The desktop launcher owns its popup windows and polls them. An ordinary app
also owns document/settings windows and must process their work while a menu
is open. Four existing gaps need closing:

| Gap | Source evidence | Selected direction |
| --- | --- | --- |
| `WINDOW_FOCUS.sibling` names a task, not a popup family | `window.c:focus_window`, `abi/include/os64/gui.h` | Identify the other same-task window in focus events; toolkit tracks its family. |
| Popup creation takes focus; destruction chooses a stacking neighbor; explicit focus refuses popups | `window.c:wm_create/wm_destroy`, `gui_client.c:gui_window_focus` | Same-task owner links and conditional focus return. Resolve safe initial activation below. |
| Public state reports frame coordinates, not current content origin | `window.c:wm_content_rect_on_screen`, `gui_client.c` | Read content rectangle under `kGuiLock`; place/flip/clamp in userland. |
| Wait blocks on one window; launcher polls every 33 ms | `gui_client.c:gui_event_wait`, `grootmenu.c` main loop | Wait for an event on any window owned by the caller's task. |

Source paths above are under `kernel/src/gui/` and
`userland/apps/grootmenu/`, respectively. The diagnostic consumer exercises
native windows without linking menu widgets.

**Choose popups that take keyboard focus.** The focused popup receives keys
through its ordinary event queue. The owner retains its logical editor target
in the toolkit and active titlebar appearance through the owner chain. The
kernel owns window identity, association, focus, ordinary input delivery and
surface lifetime. Userland owns the menu session, placement, hit testing,
actions, scrolling, font adoption and click suppression in its own content.

The alternative, keeping keyboard focus at the owner, requires an explicit
input-grab lifetime and compositor routing. It is not selected for E1. E12 may
design a pointer grab against actual drag/drop needs. There is no new kernel
menu-session object, private input queue, panel publication transaction,
64 MiB session budget or root-launcher enrollment mode.

## Ownership and focus

The boundary adds `event_wait_any(out_handle)` (63) and
`window_get_content_rect(handle, out_rect)` (64). Owned popup creation uses
the existing create call with owner handle in flags bits 48–63. There is no
attach operation. Existing errors apply, plus BUSY (-7) and NOT_FOCUSED (-8). Preserve
`INTERRUPTED` as the existing system-wide sentinel. New output pointers require
the existing user-memory validation/copy discipline; do not dereference them
while holding the GUI lock.

An owned create must request the latched POPUP property and cannot request
START_UNFOCUSED. Its parent must belong to the caller and be focused at birth.
Validate its identity before allocation and again under `kGuiLock` before
`wm_create`; a reused handle cannot substitute a different parent. A failed
focus check creates nothing and returns NOT_FOCUSED. The immutable owner link
stores a window ID, not a pointer or slot. A newborn cannot introduce a cycle.
Cascades name the parent level; a dropdown names the document. An unowned
grootmenu root keeps the existing independent-process and birth-focus behavior.
The toolkit limits menu depth to 16.

On destroying a **focused** owned popup, return focus to its live, visible,
same-task owner, raising within existing bands. If that owner is ineligible,
use the existing eligible-window fallback, excluding the dying window. If the
popup no longer has focus, destruction must not change focus. No later userland
focus syscall attempts to restore it. Close a cascade deepest-first; Escape
closes one level, selection closes the family before invoking a command.

Public destroy returns BUSY while children are attached, before freeing canvases
or altering the window. The task-exit sweep bypasses refusal, removes associations
safely and does not hand focus to another window already being swept. This is
an explicit change to destroy behavior, not merely a new wrapper. Ordinary
unattached windows retain their destruction contract. Owner close requests go
through the app; the toolkit closes children before accepting destruction.

Add a uint16_t `other_handle` to focus events, retaining `gained`, `sibling`
and the 32-byte event size; assert the table bound. Stamp the handle before
birth focus delivery. It identifies the other party at delivery, not a durable
identity. E1B uses the handoff's single-thread lifecycle/quarantine protocol:
after destruction, drain surviving queues before allowing handle reuse through
another create. A 64-event budget defers creation across turns if necessary.

Skip POPUP windows in the Alt+Tab recency list. When focus starts in a popup,
the switcher must treat its eligible ordinary owner as the current application,
so the first Tab selects the next application rather than merely dismissing
the menu onto its owner. Test the standalone launcher separately. Active chrome
follows the focused popup's owner chain. Invalidate the affected ancestor chrome
on create, focus change and destroy; changing `composite_one`'s predicate
alone cannot repaint undamaged pixels.

libui's menu hook recognizes its own popup transitions before generic focus-loss
handling. Preserve the saved editor target and menu interaction, while cancelling
unrelated pressed widgets. Ordinary focus loss still clears ordinary interaction.
Same task is insufficient: another settings/document window is outside the family.

## Wait on any owned window

`event_wait_any` returns 1 plus an owned handle with pending events, without
consuming the event. It returns INVALID_HANDLE if no owned windows remain,
INTERRUPTED for the existing signal/termination conditions, and BUSY on waiter
conflict. Invalid output memory must not leave registrations behind.

Under `kGuiLock`, clear registrations belonging to the calling thread from the
previous pass, check termination, then scan the current owned windows. Inspect
pending state with `wm_has_event`, including coalesced notifications. Check for
conflicting waiters before taking events or installing registrations. Register
on the whole owned set or none; never overwrite another thread's waiter. The
legacy single-window wait must honor the same conflict rule or it can overwrite
an any-window wait. This is a narrow, explicit compatibility change covered by
Fable's proposed one-waiter rule; callers that relied on silent overwrite fail
with BUSY instead.

Use the existing registration/park/backstop mechanism and `wm_deliver_event`
wake path. Clear this thread's slots on success, interruption, conflict and
failure, including when the previously selected window was destroyed. Re-scan
on each wake rather than holding pointers across a park. Windows born during
a wait are discovered on the next pass/backstop; do not promise an immediate
wake for a slot that did not exist at registration. No-owned-window exit avoids
sleeping indefinitely after the last window dies.

A rotating scan cursor avoids starving a higher-numbered handle behind a busy
lower-numbered one. Applications also drain round-robin with a bounded total
budget (64 events per turn), then service paints/work. A returned handle is a
readiness hint, not a reservation; another reader can consume the event or
another thread can destroy/reuse the handle before poll. A GUI toolkit uses
one event-dispatch/lifecycle thread and routes by its current live registry;
multi-threaded clients must serialize lifecycle with polling. The kernel still
validates ownership on each call.

This can ship independently of menu widgets. A launcher-only change may replace
its polling loop once tested; the full widget/font migration remains E1C.
Appearance and Frame Studio are further existing multi-window consumers, not
required migrations in this packet.

## Placement, capacity and appearance

`window_get_content_rect` reads an owned window's current screen-space content
rectangle under `kGuiLock`. It is a snapshot, not a lease on the position. Use
signed, checked arithmetic in userland; anchor dropdowns at the bar, cascades
at the parent row, prefer the requested side, flip then clamp to screen bounds.
Use current content/frame information and the content-size create path rather
than assuming a decorated owner's insets. Recheck placement on opening; cancel
on owner geometry/appearance invalidation instead of building a transaction.

Most interactive owner moves first transfer focus out of the popup. That is
not a universal geometry guarantee: `wm_set_decoration` can update frames, and
popups themselves permit chord movement/resizing. The toolkit closes/rebuilds
affected menus on appearance or size changes, and re-reads parent geometry
before opening a child. A snapshot/create race can briefly show an old anchor;
E1 does not promise atomic geometry across windows. Such stale placement must
not authorize a command against a changed application target.

For windows born POPUP, capacity is their initial content size. Ordinary windows
keep `min(max(screen, content), wm_dim_max())`. Preserve stable canvas addresses
and pitch for a window's lifetime; shrinking then growing a popup within its
birth capacity is legal. A larger menu uses a replacement window. Pass the
same derived popup property/capacity policy to both `wm_create` and the task
canvas allocation in `gui_window_create`; do not size only one buffer.

Audit/create-time clamps, decoration races, default minimums exceeding a small
popup's capacity, `set_min_size`, and both kernel/userland callers. Clamp must
not produce an oversized content rectangle. Update the published capacity and
resize comments in `abi/include/os64/gui.h`, `window.h` and sibling sites in the
same implementation commit. This affects existing popup clients and needs
regression evidence; it does not change ordinary-window resize capacity.

Retain the 32-slot global limit. Sixteen menu levels is a model ceiling, not a
guarantee of available windows. Refuse and explain resource failure without
silently deleting menu commands. Account both published and task canvas stores
and page rounding in evidence; no per-session raster reservation is introduced.
Raise the handle limit only with measured need and total allocation accounting,
not on the assumption that the slot table's extra bytes are its total cost.

For Interface font changes, E1B closes the popup path before adopting the owner's
new font. Cancel pressed rows and release panel runs while the old context is
valid; use the owner's existing prepare/commit/discard contract. Failed adoption
leaves the owner on its old font with the menu closed. **Do not automatically
reopen** on a settings change: focus may have moved to Appearance. The next
explicit opening uses the adopted font. The standalone launcher dismisses/exits
on font change and uses the new font on its next invocation. Color-only changes
may repaint live panels. This removes the need for cross-window atomic publish.

Use ordinary create/draw/publish. Measure visible birth flash in the diagnostic;
add a general born-hidden-until-publish capability only if demonstrated and
separately scoped. There are no staged replacement panels in this design.

## Input and lifetime

Keys go to the focused popup; pointer events go through existing window routing
and the implicit gesture grab. The toolkit routes them to its menu controller
before application commands. Menubar hover arrives on the owner's exposed content
without changing keyboard focus. The opening mouse release still belongs to the
opening window's implicit grab; it cannot count as a new popup press/release pair.

A click in owner content dismisses and consumes that press and its matching
release before ordinary widgets receive them. A click in another window follows
normal routing once and dismisses the family through focus loss. The owner queue
contains focus-gained before its button-down in the normal routing path; use
that ordering in the shared dispatcher. Do not close on a popup's focus-lost-to-
owner and immediately forget the dismissal: keep the controller's transition
state through the owner's corresponding focus event and gesture. Focus return
from deliberate menu teardown must not swallow the next unrelated click.
The handoff specifies input-loss reset and queue quarantine; tests must cover
both drain orders and destruction-generated focus return.

Alt+F4 on the focused popup dismisses it; it does not close the document.
Minimize/maximize remain declined for popups. Owner destruction requests close
children first. A VT switch alone **preserves** the menu, following the ruling
in `DEBTS.md` under “A popup survives a VT switch”. Covered/uncovered is not
proof of external focus. Do not add a VT-to-menu cancellation path.

Application commands execute after menu teardown and action-context revalidation.
Ordinary queued input defines command order; no atomic kernel activation fence
is promised. A later external focus change does not undo an already processed
command. A controller that has observed dismissal must not act on a subsequently
drained stale selection from another panel. Queue loss must not fabricate a
matched gesture. INPUT_LOST resets client interaction and discards pending
key/mouse records before ordinary ring delivery resumes; retained pre-loss
presses must not re-arm a control after reset.

Serialize association/focus/handle mutations under `kGuiLock`. Follow existing
`canvas_release_locked` / `canvas_pages_release` cleanup with lock order
`kGuiLock -> task signalLock`, caller CR3 reload and task-exit sequencing. Do not
free task canvas mappings from compositor focus notifications. No raw window
pointer survives an unlock without a separate existing lifetime guarantee.

Fable confirmed the pre-existing unsynchronized `task->heapEnd` updates in
`gui_window_create` and `syscall_map`. Track a separate focused allocator fix
covering both sites, checked reservation, failure rollback and concurrent
page-table updates; a CAS on the bump alone is not evidence for the whole map
path. It is not an E1A prerequisite or authorization to add allocator work here.
The existing documented sibling-thread stale-TLB limitation remains; avoiding
VA reuse does not mean freed physical pages cannot be recycled. E1 adds no new
cross-thread canvas-sharing promise and no general shootdown subsystem.

## Focused packets and acceptance

| Packet | Scope | Readiness |
| --- | --- | --- |
| E1K-W | Any-window wait, shared waiter conflict rule, wrappers and diagnostic | Implemented; see E01K_HANDOFF.md and the evidence packet. |
| E1K-O | Owner links, focus identity/activation/return, content readback, chrome/switcher and input-loss contract | Implemented and exercised by popuptest; ordinary review pending. |
| E1K-C | Popup birth-size capacity in both stores, comments and resource tests | Implemented; small popup pitch/minimum checks pass in the guest. |

These are scope boundaries, not three mandatory PRs. Keep E1K-O coherent with
its lifetime/input evidence. Chris authorized implementation on 2026-10-11.

| Evidence | Required cases |
| --- | --- |
| Host state/placement | Invalid-owner/cross-task/unfocused-owner rejection, checked edge placement, clipped tall menus, small birth capacity, allocation failure and rollback. |
| Wait/SMP guest | Empty set, busy waiter in any slot, single/any-wait collisions, interruption, destroy/create/reuse while parked, no stale waiter, fair progress under noisy owner traffic. |
| Diagnostic GUI | Panels beyond owner, submenu Escape returns parent, complete dismissal returns owner, outside click reaches other app once, owner dismissing click consumed in both queue-drain orders, same-task settings dismissal, owner chrome damage, Alt+Tab from deep popup. |
| Input fault injection | Drop an up then a later down before an up; evict focus under repeated focus pressure; hold Enter/Alt across popup creation; close while parent events are queued; no false action or stale-handle family match. |
| Focus/lifetime | External focus during slow opening, failed owned create, BUSY owner destroy with no partial cleanup, task exit in any handle order, input arriving during teardown, handle reuse before old focus records drain. |
| Font/appearance | Close on font adoption, failed owner preparation, no automatic reopen/focus theft, next opening uses new metrics, all four screen edges, VT return preserves menu absent another cancellation reason. |
| Resources/regressions | Repeated open/close restores window count and live allocations; VA high-water may grow by existing policy. Ordinary focus/resize, legacy launcher, doorbells, settings windows and supported ASan/UBSan/LSan tests. |

## Review closure

Fable's [second pass](GUI_POPUPS_REVIEW.md#7-second-pass-r1-and-r2-fable-2026-10-11-against-the-revised-gui_popupsmd)
accepts atomic owned create and INPUT_LOST on the existing event queue. Quinn
selects owned create rather than attach. The handoff makes the bounded drain
protocol explicit after destruction, and discards queued gesture records at
loss delivery so stale presses cannot re-arm state. These are implementation
requirements of the reviewed identity/loss guarantees, not a kernel menu object.

E1B/E1C wait for the implementation evidence and stable public wrappers. E6
still needs its own standalone channel design and architectural review.
