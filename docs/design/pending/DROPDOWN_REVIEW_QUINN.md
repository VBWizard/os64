# Dropdown design review by Quinn

Quinn, 2026-10-26, against `fe4746b7` in the dropdown worktree.
Review of [DROPDOWN.md](DROPDOWN.md), informed by
[Fable's review](DROPDOWN_REVIEW.md) and the menu, dispatcher, widget and
yonder form paths. This is a design and source review; no runtime tests
were run for it.

## Verdict

**D is the right architecture. Resolve the four findings below before
building the cut.** The shared popup controller preserves the family,
dismissal and quarantine rules, and viewport-sized row storage fits a
data list. These findings tighten lifetime and behavior contracts without
reopening the architecture decision.

## 1. P1 A refusing widget destructor cannot prevent yonder from freeing it

Design location: DROPDOWN.md, contract 1, lines 137–146.

The proposed destructor follows `os64_ui_menubar_destroy`: return early
if closing the popup fails. But yonder's `widget_destroy` calls the
widget's `void` class destructor and then unconditionally releases its
text runs, releases the DOM node and frees the `FormWidget`.

If native popup destruction fails during DOM replacement or navigation,
returning from the dropdown destructor leaves a live popup whose row and
pick callbacks borrow a freed `FormWidget`. The refusal must reach the
code deciding whether to free or replace the widget and its backing data.

Use a fallible teardown or close preflight that the form rebuild and
navigation paths honor. On failure, preserve the widget, callback data
and other borrowed state until close succeeds. Copying the menubar's
early return alone does not establish that lifetime contract.

Evidence: `userland/apps/yonder/yonder.c:7070` (`widget_destroy`),
`userland/libos64/ui_menu_widget.c:389` (`os64_ui_menubar_destroy`), and
`userland/libos64/ui_menu.c:82` (`os64_ui_menu_close`).

**Acceptance checks:** inject native destroy refusal during DOM replacement
and navigation; verify the callback data remains alive, teardown can be
retried, and no stale callback reaches freed storage.

## 2. P2 Menu mnemonics do not provide highlight-only type-ahead unchanged

Design location: DROPDOWN.md, lines 125–127 and the mnemonic seam entry.

The existing mnemonic scan cycles multiple matches, but `key_event`
immediately calls `activate` when there is exactly one match. Giving a
list row its label's initial as a mnemonic would make typing C commit and
close when Canada is the only C option, while merely highlighting when
two options start with C. That conflicts with the design's explicit
commit contract.

Share the matching and cycling scan, but give lists selection-only
behavior even for a unique match. Keep menu command activation unchanged.

Evidence: `userland/libos64/ui_menu.c:638`, the `count == 1` branch in
`key_event`.

**Acceptance checks:** unique and repeated initials move the highlight
without invoking `pick`; Escape then closes with the original value
intact. Enter, Space and a deliberate row click remain commit actions.

## 3. P2 The content seam must include requests deferred before a panel exists

Design location: DROPDOWN.md, lines 187–210, especially the claim that the
seam is in popup content and the boundary hook stays unchanged.

The pending request is itself model-dependent. `request_panel` retains a
menu model when quarantine prevents creation; the boundary hook later
opens that model and releases it. A list has no model, and there is no
`os64_ui_popup_t` yet in which to store its content kind.

Extend the pending request representation to carry either content kind.
Copy the list descriptor and define the lifetime of its borrowed callback
data from acceptance of the request through cancellation or successful
close, including the interval before native creation. Anchor invalidation
must cancel a pending list as well as close a visible one.

This preserves the quarantine policy while extending the state it carries.
It is required for ordinary menu-to-list replacement, which destroys the
old popup and therefore enters quarantine before opening the new one.

Evidence: `userland/libos64/ui_menu.c:189` (`request_panel`), `:221`
(`boundary`), and `s->pending` in `ui_menu_internal.h`.

**Acceptance checks:** replace a menu with a list during quarantine and
verify it opens after the drain; remove the dropdown before the drain
finishes and verify no popup appears and no row callback runs afterward.

## 4. P2 Committing the existing value must not fire change

Design location: DROPDOWN.md, contract 2, lines 147–156.

The proposed path runs `list_changed`, `os64_page_set_chosen` and `IN_LIST`
once per commit. Opening the dropdown and pressing Enter without changing
the highlighted row would therefore emit `input` and `change` despite no
value change. The existing listbox explicitly suppresses its change
callback when the selected index is unchanged.

The session may report `pick` for every explicit commit. The dropdown
must compare the committed value with its previous value before notifying
yonder, so closing on the existing value does not manufacture a change.

Evidence: `userland/libos64/ui_list.c:185` (`choose`),
`userland/apps/yonder/yonder.c:6822` (`list_changed`) and `:9641`
(`IN_LIST` delivery).

**Acceptance checks:** Enter or a click on the existing value closes
without `input` or `change`; moving away and back before committing does
the same; committing a different value emits each event once.
