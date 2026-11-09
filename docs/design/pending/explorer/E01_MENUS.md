# E1 — Shared actions, menus and status

## Status and ownership

- **State:** E1A and E1K complete; E1B implemented and validating for ordinary review.
- **Design owner:** Quinn. E1A owner: Sol (PR #254 merged `c6cc6e3c`, accepted).
  E1B owner: Quinn; implementation branch `codex/explorer-e1b`.
- **Base:** fresh checkout at `b36e9ddc` (PR #257), advanced to current
  `origin/userland` at `f7f2f508` before publication.
- **Prerequisite:** E1K in [GUI_POPUPS.md](GUI_POPUPS.md) and its
  [handoff](E01K_HANDOFF.md) is approved and merged. Chris confirmed E1K
  complete when authorizing E1B; earlier pending banners were stale.
- **Implementation:** [E1B handoff](E01B_HANDOFF.md), with
  [host/guest evidence and P5 walkthrough](evidence/e01b/README.md).
- **Fable scope:** E1K changes compositor ownership/input behavior and requires
  architectural review (D012/D013). E1's actions/widgets/adapters follow ordinary
  code review; new kernel work must not be folded into their PRs.
- **Governing decisions:** D002, D008–D015 in [DECISIONS.md](DECISIONS.md).

This remains the governing contract. The concrete E1A/E1K/E1B APIs and evidence
are in their handoffs; names in the original proposed-operation tables map to
those headers. Consumer semantics and boundaries remain binding.

## Outcome and logical delivery boundaries

Roam, Scribe and the desktop launcher can use the same menu presentation and
interaction. The menu follows the Interface font and theme. Actions share
enabled state, shortcuts, checkmarks and help across different presentations.
Menus and status bars remain independently usable.

| Packet | Coherent outcome | Dependencies | Boundary |
| --- | --- | --- | --- |
| E1A | Action model, key routing and status widget, exercised in a gallery | Existing libui | No kernel, native popups or icon files |
| E1K | Owned popup focus, multi-window wait, content readback and diagnostic | Fable-reviewed standalone design | Focused GUI prerequisite, no menu semantics |
| E1B | Menu bar, popup widget and cascade controller integrated with E1K | E1A, E1K | Toolkit implementation and gallery; optional icon references |
| E1C | grootmenu migration and Scribe action/menu/status adoption | E1B | Two production consumers; retain their existing policies |

These are logical implementation packets, not size targets or promises of one
PR each. E1A is complete independently of E1K. E2 adds real icon assets and
the provider described in [E02_ICONS.md](E02_ICONS.md); E1B's layout and ownership
must already accommodate that provider. E1 is accepted after E1C's evidence.
Do not rename the other roadmap slices to accommodate these suffixes.

Out of E1: file browsing, pickers, drag/drop, transfer work, IPC, desktop icon
placement, application-instance policy and a general docking framework. Scribe
continues to use its existing filename field until E5 supplies the picker.

## Evidence from the code

Paths are relative to the repository root, at the inspected revision above.

| Existing code | Consequence |
| --- | --- |
| Pre-E1C `userland/apps/grootmenu/grootmenu.c` private painter/cascade controller | Private native windows and direct bitmap text; app-level cascade logic. Theme colors are shared, Interface font is not. |
| `userland/libos64/include/os64/menu.h`, `userland/libos64/menu.c` | Launcher grammar and argv conversion, not a widget. Preserve parser behavior and bounds. |
| `userland/libos64/ui.c`, `os64_ui_dispatch` | One UI tree; mouse-down dispatch currently accepts left button only. Add explicit context invocation, not a broad change making existing buttons accept right clicks. |
| `userland/libos64/ui_text.c`, `os64_ui_decode_key` | Arrow/edit keys arrive in escape bursts; real Escape has a distinct physical code. Reuse the decoder and test both PS/2 and HID. |
| `userland/libos64/ui_font.c`, `ui_font_settings.c`, `include/os64/font_adopt.h` | Text/layout preparation can fail; commit cannot allocate or fail. Menu rows need class-specific retained runs and font adoption hooks. |
| `userland/apps/scribe/scribe.c`, `app_shortcut`, `status_show`, `compute_layout` | App commands precede widget dispatch; field modes already own their clipboard actions. Preserve this ownership during migration. |
| `abi/include/os64/gui.h` | Existing create/focus/event APIs do not supply native owner-relative popup groups. E1K specifies the additive APIs and explicit changes to destroy/wait/popup-capacity behavior. |

## Shared actions

Add `os64/ui_action.h` with a registry owned by one application UI scope.
An action has a nonzero `uint32_t` ID, UTF-8 label, help text, optional semantic
icon name, optional mnemonic, shortcut, kind (command/toggle/radio), radio-group
ID and current state (enabled/checked). ID zero means no action. IDs are local
to the registry and independent of labels, widget pointers and menu positions.

The application owns semantic state and command effects. A query callback fills
enabled/checked state for a given context; an invocation callback receives the
action ID and that context. Callbacks run on the UI thread, without toolkit or
kernel locks, and may not start a nested event loop. A registry contains immutable
descriptors; a separate **action view** retains one target context and owns its
state snapshot. Two views of the same action may be enabled differently; a
directory context-menu refresh must not overwrite a filename field's state.
The toolkit never flips a checkmark optimistically: after invocation it queries
the actual application state again, including after a failed operation.

Use these operations, with explicit status returns:

| Proposed operation | Contract |
| --- | --- |
| `os64_ui_actions_create(specs, count, callbacks, user, out)` | Validate and copy descriptors/strings; failure leaves `out` NULL and retains nothing. |
| `os64_ui_action_view_create(actions, context, out)` | Retain the context and registry; allocate bounded state storage and query initial state. Failure retains nothing. |
| `os64_ui_actions_refresh(view)` | Validate context, query and atomically replace this view's snapshot. Query is nonallocating/nonfailing and cannot mutate the registry or pump events. Invalid context disables the view and reports STALE. |
| `os64_ui_action_invoke(view, id)` | Revalidate context and enabled state, invoke once, return performed/refused/error. A disabled shortcut is consumed but has no effect. |
| `os64_ui_action_binding(button, view, id)` | Use the action's caption/state/invocation in an ordinary button; preserve legacy button constructors. Binding holds a tracked view reference until explicitly detached. |
| `os64_ui_action_view_destroy(view)` | Refuse BUSY while bindings/menu sessions retain it, then release context and registry. |
| `os64_ui_actions_destroy(actions)` | Release copied descriptors after bound controls/sessions detach. Return BUSY if still referenced; no implicit dangling bindings. |

Initial bounds: 256 actions, labels 128 bytes, help 256 bytes, icon names
48 bytes, each including NUL. Maximum copied descriptor/string storage is
128 KiB. Refuse over-limit or malformed input as a whole; do not truncate labels
or silently omit commands. Validate unique IDs, legal kinds and radio groups.
Shortcut collisions within the same scope are an error, including disabled
actions. Caps/Num/HID identity bits are not shortcut modifiers. Shortcuts use a
normalized key plus exact Shift/Ctrl/Alt flags. Ctrl-letter ASCII control codes
must normalize to the same letter shown in the shortcut legend.

State storage is at most one entry per registered action per view; enforce a
32-view limit per registry and account it in a separate 64 KiB state budget.
Query initializes every state to disabled/unchecked before the callback, so an
omitted answer cannot inherit a former target's checkmark. Radio groups may have
zero or one checked action per view; refuse an inconsistent refreshed snapshot
and disable that view until a valid refresh. Toggle state is application-owned.

Descriptors are immutable until registry replacement. For dynamic labels, build
a new registry/model beside the old, close its menu session, then swap bindings
as one UI-thread update. State updates do not require rebuilding descriptors.

An invocation context is app-owned, with explicit retain/release/validate
callbacks and a generation. Opening a menu retains an action view for its target. A right-click
menu keeps targeting the item/selection that opened it even if keyboard focus
moves; the app rejects a stale generation before a mutation. No borrowed widget
or filename pointer is hidden in a long-lived toolkit closure. E4 will define
the browser's selection context; E1 proves this with synthetic mutable items.

After selection, the controller closes its popup windows and releases menu
state before invoking application code. Copy the selected ID and retain its
view/context first. Callbacks may request window destruction; actual teardown is
deferred to the end of dispatch. No code dereferences a widget after calling
an application callback that may invalidate its tree.

## Menu model and widgets

Add `os64/ui_menu.h`. A menu model is immutable copied data: action entries,
separators and submenu entries with a label/mnemonic and child model. It owns
references to the action registry and its child models. A submenu is not itself
an invokable action. Reject cycles, invalid action IDs, excess depth and empty
submenus rather than displaying a launcher that appears to have lost entries.
Allow an empty top-level model to return EMPTY without creating a popup.

Model limits: 256 entries in the graph, 16 levels (matching the launcher
grammar), 64 entries per level. Separators are not selectable. Leading,
trailing and repeated separators are normalized in presentation; action order
does not change. A runtime screen or memory limit can still refuse a deep
cascade; report that failure once per explicit opening attempt.

Three toolkit objects share the model:

- `os64_ui_menubar_t`: a retained widget showing top-level menu labels.
- `os64_ui_popup_t`: a retained widget for one panel, including its scroll
  viewport, row runs, check/icon/label/shortcut/arrow columns and hit testing.
- `os64_ui_menu_session_t`: the controller owning the open panel path, popup
  transport, context, navigation/pressed state and transient help token.

The popup is a widget, not a launcher process. It draws only within its supplied
surface and knows no command strings, paths, process-spawn policy or menu.conf.
E1K provides native popup windows beyond the owner window; libui owns their
menu layout and closes them before adopting a changed Interface font.
One session per owner UI;
an explicit new context-menu request closes the old one before opening another.

Proposed entry points: menu-model create/release, menubar initialize/bind,
menu-session open(owner UI, model, context, anchor, invocation kind), event
dispatch, refresh-state, close(reason) and dispose. Opening returns a status
and leaves no windows or retained context on failure. Close is idempotent.
Disposal releases panel runs, models, icon leases and popup resources before
the owner text context is released.

## Event routing and interaction

Application loops use a shared menu pre-dispatch hook, then their existing
application command routing, then ordinary `os64_ui_dispatch`. The default
`os64_ui_run` uses the same hook. Handle close, appearance, resize and doorbells
even while a menu is open: menus do not run a nested blocking loop. Use E1K's
`event_wait_any` and a live handle-to-owner/popup registry in the shared dispatcher.
It dispatches the focused popup's keys to the controller and owner events to the
owner; apps must not each invent a polling loop. Drain round-robin with a total
64-event budget per turn, then service paints/work. Remaining events keep the
next wait ready. One UI thread owns window creation/destruction and dispatch.
Another same-task window's focus is not a menu-family transition. Destruction
marks handle tombstones and forbids creation until each surviving queue has been
drained after the last destruction. Retain tombstones across budget boundaries;
see E1K's lifecycle protocol. Draining only before destruction is insufficient.

| Input / condition | Behavior |
| --- | --- |
| Bare left Alt press/release | Arm on down; reveal/focus menu bar on release only if no other key, pointer gesture or focus transition intervened. Another bare Alt exits menu mode. |
| F10 | Enter menu-bar mode on down, or leave if already active. Suppress repeats until release; Shift+F10 is context invocation. |
| Alt+mnemonic | Open that top-level menu, including when the bar is normally hidden. Match ASCII mnemonic case-insensitively; labels remain UTF-8. |
| Left/Right on bar | Move among enabled top-level menus, wrapping. Down/Enter opens the selected menu. |
| Up/Down in popup | Move among enabled entries, skipping separators and disabled entries; wrap. Home/End select first/last; Page Up/Down move a viewport. |
| Right on submenu | Open child and select its first enabled entry. Left closes the child; at a menu-bar root it moves to the previous top-level menu. |
| Escape | Close one cascade level; at root close menu mode and restore the original eligible widget focus. Temporary bar visibility ends. |
| Enter/Space on command | Activate once on a matched down/up pair; repeats do not repeat commands. On submenu, open it. |
| Unmodified mnemonic inside popup | Unique match activates/opens; duplicates cycle highlighted matches and require Enter. No arbitrary first-match activation. |
| Tab/Shift+Tab | Close menu mode and perform one normal owner focus traversal. Do not insert a Tab into the document. |
| Left click on top label | Open or toggle that menu; movement across other labels while menu mode is active switches them. |
| Hover on popup row | Highlight immediately; a submenu opens on entry, matching current grootmenu behavior. Keep it open while the pointer enters that child panel; an unrelated parent row replaces/closes it. No hover timer is required in E1. |
| Left press/release on entry | Activate only if release matches the pressed enabled entry; leaving it cancels the press. The release that opened a menu never activates an item. |
| Right click / Shift+F10 | Open the registered context menu; keyboard invocation anchors at the focused item's supplied bounds, or the focused widget bounds. |
| Wheel | Scroll the popup under the pointer; do not scroll the obscured directory/document. |
| Outside click / external focus | Toolkit closes the family; consume an owner-content dismissing gesture, let another window receive its normal click once. No unconditional focus syscall. |
| Owner close/geometry invalidation | Close children before owner destruction or relayout. A focused popup's Alt+F4 dismisses it; popup minimize/maximize remain declined. |
| VT switch alone | Preserve the popup, following the existing DEBTS.md ruling; VT return alone neither activates nor dismisses a command. |

Menu mode consumes its key gestures before application shortcuts. Registered
action shortcuts recognized during menu mode close the popup path, revalidate
the action context and invoke that action once against its saved context;
unmatched chords are consumed without reaching the document. While menus are closed, registered
window shortcuts run before ordinary widget dispatch, except editing chords:
Copy/Cut/Paste/Select All resolve through an explicit focused-editor context.
Applications with field modes keep that existing context selection. No global
Ctrl+C that copies a directory path while a filename field owns the selection.
Menu invocation of an editing action uses the saved editor target, not the
menu's highlighted row as the new editing focus.

While menus are closed, unrelated events follow the existing dispatch path.
An explicit optional `on_context` widget callback bubbles from hit child to
parents for right-button down; existing left-click `event` callbacks are not
reused for this. Keyboard context invocation uses the focused widget. Returning
false leaves the event to existing application behavior, including terminal
right-click paste. E1 does not retrofit a context menu onto every text widget.

Track owner-content dismissal across popup focus-lost and owner focus-gained
regardless of which queue drains first. Retain suppression until the matching
button release; distinguish that transition from deliberate popup destruction
returning focus. Cancel armed gestures on input loss and discard unsafe queued
input according to E1K R2. A lost release must not pair with a later click.
These are E1B controller tests; they do not expand E1A's shortcut scope.

Keep one decoder per menu session and reset it on dismissal/external focus loss;
internal cascade focus changes preserve menu navigation state. Consume
escape-burst prefixes; one arrow press moves once, not three times. For bare
Alt use left Alt only: the PS/2 extended right-Alt path does not publish the
same edges, and right Alt is reserved for future AltGr behavior. F10 and other
nonprinting keys require PS/2 and HID mappings; do not infer them from ASCII 0.
Kernel-owned chords retain precedence (Alt+Tab, Alt+F4, VT switching and WM
commands). Losing focus or seeing a changed modifier state disarms pending Alt.

## Visibility and focus

Implement View → Show as toggle actions in the consumer. Their query reads the
actual layout state; their invocation updates it and requests one layout pass.
The widget library does not save application preferences.

In Roam, the planned visibility scope is per window for menu/navigation/address,
sidebar, status and transfer shelf; per pane for tab strip and preview. A menu
operating on per-pane settings captures the active pane context. E4/E10 own the
final persistence format and hidden-filter indication, not E1. E1's gallery
demonstrates two independent panes without claiming to implement the browser.

Turning off the menu bar closes menu mode and reclaims its layout height.
Alt/F10 reveals it temporarily; this does not toggle or save the persistent
setting. Escape restores the hidden layout. Directory context menus include
Show Menu Bar even in the bare layout. During temporary reveal, the application
reflows the content; pointer press state is cancelled and scroll position is
preserved. There is no transparent menu overlay covering file rows.

Before hiding a focused component, cancel its gestures and transfer logical
focus to the surviving directory view (gallery's content control for E1).
Hiding a tab strip, pane or shelf preserves its model; no close or cancel
operation is implied. The owner widget saved for focus restoration must still
be attached, visible, enabled and focusable; otherwise use an application
fallback. Native keyboard focus goes to the popup. The shared pre-dispatch hook
recognizes menu-family transitions before generic libui focus-loss handling,
preserves the saved editor target, and cancels unrelated widget gestures. E1K
returns native focus conditionally when a focused child is destroyed; the toolkit
never calls an unconditional owner-focus operation on dismissal.

## Layout, fonts, icons and appearance

Labels, shortcut legends and status text use `OS64_FONT_ROLE_UI`. Measure
actual text runs; byte length times `theme.font_w` is not an acceptable width.
Checkmarks, radio dots, separator rules and cascade arrows are geometric widget
marks, distinct from application icons. Menus align these columns separately:
check/radio, optional icon, label, shortcut, cascade arrow. A checked action with
an icon must display both. Reserve an icon column per panel when any entry has
an icon reference; absent artwork leaves its slot empty, not a shifted label.

Row height is `max(interface line height, icon slot height) + 2 * row padding`;
separator height and column gaps come from named menu metrics with compiled
defaults. Start with 3 px row padding and 6 px column gaps. E2 supplies an
optional icon provider and stable semantic names; E1 uses NULL provider and
synthetic pixel fixtures. Paint never opens files or decodes images. Missing
icons cannot make a menu unusable or hide its text.

The toolkit reads the current native content rectangle, anchors panels to the
owner or parent row, flips cascades at the screen edge and clamps bounds. A tall
menu uses visible scroll affordances plus wheel/keyboard scrolling; selected entries are
scrolled into view. It may not silently drop entries to fit. Clamp width to the
available screen and truncate displayed labels at text cluster boundaries with
an ellipsis; retain the full label/help in the model. Refuse if even one row and
scroll affordances cannot fit. A narrow menu bar uses a trailing overflow menu,
containing complete remaining top-level menus in order, rather than clipped
unreachable headings. The overflow affordance is keyboard reachable.

Close the popup path before adopting an Interface font change. Cancel pressed
rows and release panel runs while the old text context is valid, then use the
owner's existing prepare/commit/discard transaction. Failed adoption keeps the
owner's old font with menus closed; success commits the prepared owner state.
Do not automatically reopen or steal focus back from Appearance. The next
explicit opening measures and paints the new font. The standalone launcher
exits on a font change and uses the adopted font at its next invocation.

Use the shared text-context borrowing supported by libui, rather than resolving
a separate font set for every panel. Release popup runs before the owner's
context. Color-only changes can repaint live panels. There is no cross-window
publication barrier or claim of all-or-old pixels across several windows.

## Status bar

Add `os64/ui_status.h` and `os64_ui_status_t`. It is an independent retained
widget with three sources: base information, transient action help, persistent
notice. Setters copy bounded UTF-8 strings (512 bytes including NUL per source)
and return status; failed allocation or oversize leaves the old message intact.
Do not borrow worker buffers. Updates happen on the UI thread after a doorbell.

Base information describes the current document/directory. Menu help temporarily
replaces the base slot; a separate persistent slot shows a job summary, warning,
error or active confirmation. When width cannot show both, the persistent slot
wins and help is suppressed. A narrow status bar may ellipsize presentation but
retains complete text. It does not dismiss a notice merely because a menu closes.

Transient help is token-owned: setting help returns a generation token, and
clearing stale help cannot erase a newer producer's message. Application notice
clear likewise names its generation, so an old completion cannot clear a newer
failure. No auto-expiry is needed in E1. Consumers decide when information has
been acknowledged or superseded. Scribe's unsaved-work prompt uses the persistent
slot and remains readable while navigating menus.

Hiding the status widget preserves these sources. E4 must supply another visible
path for errors in a bare browser; hiding status cannot turn failed operations
into apparent success. E1 demonstrates retained state and explicit re-showing,
without adding an Explorer notification subsystem.

## Production integration

### grootmenu

Keep invocation syntax, config ladder, named menus, grammar limits, command
parsing, error reporting and spawn policy. Convert parsed nodes into the shared
model; use registry IDs derived from stable node indices for that load. The
adapter owns command strings and invokes `os64_menu_argv`/spawn only after the
menu closes. Empty child cascades are omitted without suppressing usable
siblings. An empty named menu or unsupported model limit still reports why.
Do not add inline icon syntax to menu.conf in E1; E2 can map known applications
to semantic icons with a generic fallback without changing the grammar.

Replace private painting, row measurement and cascade event handling. Native
standalone hosting uses an unattached root popup and attached child windows;
the launcher remains its own process,
independent of the desktop. It uses the same popup widget/controller as an app's
context menu. On selection or dismissal release text/icon/model/session state
and exit. Launch errors still reach its existing diagnostic sink. Test no GUI,
missing config, empty menu, invalid reference, max depth and quoted arguments.

### Scribe

Use the existing Open, Save, Save As, Find, Help, Quit and clipboard callbacks
behind actions. Add a File/Edit/View/Help menu bar, remove the duplicate
Open/Save/Save As buttons, and adopt the status widget. Keep the filename field,
find field, help view, save failures and unsaved-exit flow intact. Do not add a
second document model or silently change the meaning of repeated Ctrl+Q.

File: Open (Ctrl+O), Save (Ctrl+S), Save As (no new shortcut in E1), separator,
Quit (Ctrl+Q). Edit: Cut/Copy/Paste using the focused editing target, separator,
Find (Ctrl+F). View: Show Menu Bar and Show Status Bar. Help: existing guide
(Ctrl+G). Do not offer commands without an existing implementation. Escape
closes menus before it exits help or clears document selection.

Scribe's content context menu supplies editing commands and Show Menu Bar;
right-click does not move the text caret or replace an existing selection in
E1. Keyboard invocation anchors at the caret if its bounds are available,
otherwise at the document viewport. Path/search field clipboard ownership stays
with the field. No additional Fable review is required for these adaptations.

## Files and build integration

Expected new headers: `userland/libos64/include/os64/ui_action.h`, `ui_menu.h`,
`ui_status.h`, and the small icon-provider types shared with E2. Expected source
modules: `ui_action.c`, `ui_menu.c`, `ui_status.c`, with a separate private popup
adapter if that keeps kernel transport out of widget logic. Extend `ui.h` only
for hooks/state that the core must own; do not put the entire menu API there.

Coordinate edits to `ui.c`, `ui_font.c`, `ui_font_settings.c`, `ui_internal.h`,
`userland/GNUmakefile`, Scribe and grootmenu. Preserve existing widget constructor
semantics and update host harness source lists when new core dependencies appear.
Use the actual shared-library build/export conventions; libos64 has no explicit
exports map at the inspected baseline. Do not introduce a libos64 → libimage
dependency (libimage already depends on libos64).

## Acceptance and evidence

These are the governing gates. Completed E1A/E1K checks remain in their evidence
packets; E1B results and applicability are in its linked evidence packet. E1C
consumer migrations and E1B hardware acceptance are not inferred from host tests.

| Environment | Required evidence |
| --- | --- |
| Host model/controller harness | Duplicate/cyclic/oversize rejection; allocation failure at each owning allocation; state refresh and disabled invocation; duplicate mnemonics; checked+icon columns; invalidated target generations; callback-triggered teardown; status token ordering; one activation for repeated input. |
| Host with real fonts | Real UI face measurement, UTF-8 labels, close-before-font-adoption, failed owner preparation leaves old font and closed menus, screen-edge placement and scroll reachability. Sanitizers including supported leak checking. |
| Existing regressions | `tools/test_ui_text_host.py` and relevant font-contract/Scribe suites, with source-list updates. Existing buttons/textfields still ignore unclaimed right clicks; Tab and clipboard ownership retained. |
| QEMU gallery | PS/2 and emulated HID inputs; native panels beyond owner edges; keyboard-only traversal, cascades, bar overflow, tall menus, tiny screen refusal, context placement and hidden-menu recovery. Toggle every gallery component off and restore it with keyboard and pointer. |
| QEMU integration | Scribe's save/help/unsaved-exit and field editing flows; grootmenu launch syntax/config failures; external focus/owner death dismiss correctly; VT switch alone preserves menus; repeated open/close returns handles and live allocations to baseline; VA high-water follows existing monotonic policy. |
| P5 | Chris confirms Interface font/size adoption in desktop menu and Scribe, right-click positioning, mouse/keyboard cascades, menu hiding/recovery and no stranded popups. Record installed revision separately from host/QEMU results. |

Mutation checks should target hazards: invert enabled checks, accept stale
context, omit close before callback, clear newer status help, skip font abort,
and treat an arrow's ESC byte as Escape. Tests must expose those failures rather
than assert private struct layout. Preserve ASan/UBSan/LeakSanitizer; rerun
outside sandbox when attachment restrictions prevent supported leak checking.

Before each implementation commit, re-read touched comments and sibling sites;
run `tools/stale_refs.sh` when retiring grootmenu helpers or moving API names.
Gallery evidence belongs under a durable repository evidence directory, with
commands, screenshots and exact tested commits, not only `/tmp`.

## Handoff and readiness

- Current output: E1B toolkit and extended gallery; see its handoff and evidence.
- Prerequisite: E1K approved and merged as `b36e9ddc` (PR #257). Fable's
  [second review](GUI_POPUPS_REVIEW.md) closes the design questions; the
  [concrete handoff](E01K_HANDOFF.md) includes queue-loss and lifecycle details.
- Icon appearance: E2 visual choice remains open; NULL-provider behavior lets
  E1 progress without placeholder artwork becoming the shipped icon family.
- E1A and E1K are complete. E1B ordinary review/P5 acceptance precede E1C.
  E1B tests explicitly cover
  dismissal suppression expiry when a focus transition has no content click
  (titlebar, programmatic focus), as well as both cross-queue delivery orders.
