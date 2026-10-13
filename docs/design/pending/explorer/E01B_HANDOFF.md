# E1B — shared menu toolkit handoff

2026-10-11–12. Owner: Quinn. Branch `codex/explorer-e1b`, fresh worktree
`.worktrees/explorer-e1b`, based on current `origin/userland` at `b36e9ddc`
(PR #257). Before publication it fast-forwarded to `f7f2f508` (CSS-transform
review fixes only; menu APIs and tested gallery/library code are unchanged). Chris confirms E1A and E1K are complete; the old pending-review
banners preceded E1K's approval and merge.

[PR #259](https://github.com/VBWizard/os64/pull/259) merged into `userland`
as `88f8b672`, confirmed by Chris on 2026-10-12.
Initial tested implementation revision: `7c6de5cf`. PR review fixes add failure
propagation for native destruction and the navigation/dispatch regressions below.

E1B implements the [menu contract](E01_MENUS.md) using the
[E1K APIs](E01K_HANDOFF.md). Host and QEMU evidence is recorded in
[evidence/e01b](evidence/e01b/README.md). Chris reports the P5 walkthrough passed;
the retained trace ends with zero font bytes/references. Review is complete;
E1C retains the grootmenu and Scribe
migrations; E2 retains the asset catalog and artwork. No kernel, browsing,
IPC, libimage dependency or deferred gclock investigation is included.

## Concrete APIs and ownership

- `os64/ui_menu.h`: immutable copied models, application-owned menu bar,
  retained popup widget and menu session. The launcher grammar in `menu.h`
  is unchanged. Invalid IDs, duplicate commands in a panel, malformed labels,
  empty submenus, depth/graph limits and foreign registries are refused.
  Immutable children make cycles unrepresentable through the public API.
  Separators normalize without changing command order. Empty roots return
  `OS64_UI_EMPTY`; no window is created.
- Models retain their registry and child models. Popups retain a model and
  target view, prepare label/shortcut runs, and acquire optional icon leases
  before publication. Paint uses those runs and leases without allocation or
  I/O. A narrow panel shortens labels at cluster boundaries with an ellipsis;
  its model keeps the full label and help.
- `os64/ui_icon.h`: optional application-owned provider with checked semantic
  keys, immutable leases, menu size buckets and clipped straight-alpha drawing.
  Disabled drawing halves alpha without changing cached pixels. Missing or bad
  icons keep the reserved column and command label. The gallery supplies
  synthetic pixels; `--no-icons` demonstrates a NULL provider.
- `os64/ui_dispatcher.h`: one UI/lifecycle thread registers owner, popup and
  application sibling windows. The optional backend is a deterministic test
  seam; NULL uses E1K. Event turns use wait-any, round-robin polling and a
  64-event budget before boundary hooks and paints. Applications retain their
  command policy in an owner callback after menu predispatch and before
  ordinary widget dispatch. `os64_ui_run` uses this same menu turn when a
  session is attached.
- `os64_ui_menu_session_create` attaches one session to an owner UI. An owner
  of zero supports a standalone unowned root, without a menu bar, for E1C's
  launcher adapter; this reserves a maintenance slot in the dispatcher.
  Positive-owner anchors are content-local; standalone anchors are screen-local.
- `os64_ui_menu_bar_view` explicitly selects and retains a bar target. Opening
  copies that view reference into the session; subsequent focus/view changes
  do not retarget it. Action invocation revalidates the saved generation and
  enabled state. It closes the family and clears transient help first, retains
  the invocation view independently, and touches no widget after the callback.
  Application teardown is deferred until dispatch returns.

## Queue and lifecycle rules

Use dispatcher destruction for registered windows. Destroy children first.
Each successful destruction tombstones its handle and resets surviving queue
empty observations. Creation is blocked until each surviving queue has been
polled empty after the last destruction, even across budget boundaries.
Replacement/child opens return `OS64_UI_BUSY` while their retained intent waits
for this drain. This is pending work, not an allocation failure; an actual
opening failure leaves no new windows or retained session target, unless native
destruction itself fails. Close and session disposal return `OS64_UI_ERROR` in
that case: surviving panels, dispatcher registrations, views and UI hooks stay
alive. Replacement and application invocation stop; retry close/disposal before
releasing owner widgets or font storage. Font adoption/release refuses with BUSY
while popup destruction is blocked. Successful disposal detaches hooks and frees
the session. A supplied menu bar must already belong to the session's UI.

Refresh closes descendants below the first invalidated selection and recomputes
transient help. Alt/F10 normalizes the remembered bar selection to an enabled
visible heading (including overflow); an all-disabled bar remains inactive.
Active menus consume owner wheel events so obscured content cannot scroll.
Decoded navigation repeats without a release guard: extended PS/2 keys emit
press-only escape bursts. Command activation, mnemonics and mode keys retain
their repeat guards. The review evidence corrects the initial native overflow
child capture, which showed only the parent despite its original description.

The second review fixes refresh error precedence: a failed close returns ERROR
before a stale/failed action-refresh result, keeping teardown obligations visible.
Scrolling clears a selected row that leaves the viewport and clears its help;
arrow navigation from no selection starts within the visible rows. Parent
selection changes cancel queued children and close descendants before changing
the anchor, including keyboard, pointer and duplicate-mnemonic paths. Hidden
top-level mnemonics open their requested model while selecting the visible
overflow affordance for hover and Left/Right interaction. A standalone session's
zero owner is not a family handle or an owner-content dismissal destination.
See [second review evidence](evidence/e01b/review-r2/README.md).

The third review centralizes pending-descendant cancellation in `close_to`,
including Escape/Left collapse, and refuses panel creation without a live parent.
Menu navigation, matched Enter/Space and enabled popup mnemonics take precedence
over action shortcuts; unmatched registered shortcuts still close/invoke against
the saved target. Failed retained-view refresh closes/collapses a temporary bar.
Page Up/Down computes a target one viewport's model-row count away, then adjusts
once for an enabled landing entry, rather than taking a viewport's number of
enabled-entry steps. See [third review evidence](evidence/e01b/review-r3/README.md).

The fourth review uses a [state/invariant audit](E01B_INVARIANTS.md). Refresh
shares selection's descendant-close transaction: failed removal preserves the
surviving parent's anchor, including partial teardown and top-level replacement.
Bar state publication schedules owner damage for target swaps and changed enabled
state, including failed refresh; unchanged same-view paint remains idle. Context
menus do not highlight a remembered bar heading. External loss resets delivery
pairing even after shortcut closure; family dismissal preserves the matching
release. Covered cancels input pairing and preserves menus. Alt/F10 and popup
opening share retained-view preparation, and event-only failures report through
the configured hook. [Fourth review evidence](evidence/e01b/review-r4/README.md)
records interrupted sequences and production owner pixel/damage checks.

The fifth review makes bar-only selection eligibility part of snapshot
publication, including same-view updates and invalid pending-root cancellation.
Failure cleanup precedes notification and gives refused teardown precedence for
deferred/root opening and preparation. A shared viewport calculation preserves
children, pending intent and help on clamped scrolling; actual movement closes
descendants before changing top. The final viewport packs command/separator
heights instead of allowing a one-row tail. [Fifth review evidence](evidence/e01b/review-r5/README.md)
records the combined-failure and scroll transition regressions.

The sixth review preserves registry identity when rebinding an inactive bar,
matching the existing bar-view binding check. Opening prepares a retained fresh
candidate before eligibility decisions, including overflow and keyboard stepping;
its ownership replaces the saved session view after successful teardown. Disabled
or failed requests do not retarget the old session. Declined same-view requests
share explicit refresh's invalid-descendant reconciliation. [Sixth review evidence](evidence/e01b/review-r6/README.md)
records both snapshot transitions, registry collisions and candidate lifetimes.

Retired focus records are ignored by a pending replacement, including old birth
focus-lost records. Unknown live focus peers dismiss conservatively. A sibling
application window is outside the family. Native conditional focus return is
used; the toolkit does not call an unconditional owner-focus syscall.

Owner-content dismissal consumes its down and matching up in either queue
order. A focus-only intent expires when the queues reach an empty dispatch
boundary; exhausting the budget preserves it into the next turn. A new down
cancels an unmatched old suppression, so a lost release cannot swallow a later
click. Explicit close does not arm dismissal suppression. INPUT_LOST closes
menus and resets presses, held keys, decoder and suppression; E1K discards the
unsafe queued gesture records at loss delivery.

Keep widgets alive until the session is disposed. Close before removing/freeing
widgets. Restoration checks tree membership, visibility, enabled state and
focusability; applications supply a surviving fallback widget. The gallery
uses its content control. Public UI structs grew optional hook fields; rebuild
userland consumers with this libos64 rather than mixing old binaries and new
struct layouts. Before owner destruction, successfully dispose the session,
unbind actions, release menu models/views, then release owner fonts and storage.

## Fonts, appearance and consumers

Panels borrow the owner's text binding rather than resolving another font set.
Labels and shortcut widths use Interface runs; geometric marks remain separate
from icons. Menu row padding/gaps/separators use named compiled menu metrics.
Cascades re-read parent content geometry, flip at the screen edge and clamp
including the popup border. Tall panels expose scroll arrows and support wheel,
Home/End and page navigation. Bar overflow retains complete remaining menus.

The direct font consumer and settings follower close menus while the old text
context is alive, before candidate preparation/adoption. Failed adoption keeps
the owner's old font with menus closed. Color-only session changes repaint
panels. Neither path reopens menus or steals native focus. A VT switch alone
preserves the family.

`on_context` bubbles explicitly with separate `context_user` storage. It does
not repurpose button callbacks, move editor focus/caret/selection or claim an
unhandled right click. Closed-menu clipboard routing remains the application's
existing policy; the gallery registers no global editor clipboard commands.
It includes a real textfield for that regression. E1C must preserve Scribe's
focused field/document contexts and gterm's unclaimed right-click behavior.

The gallery demonstrates independent target views, stale/disabled targets,
radio and duplicate-mnemonic fixtures, a 50-command UTF-8 menu, visibility
state, bar recovery and retained status tokens. Its diagnostic `--trace` records
key source, focus, native surface capacity, live font bytes and action teardown
ordering. Run the installed path `/tests/actiongallery`.

## Next action

Review the fixes on this userland PR. Chris's P5 report and log are recorded
separately from automated evidence; the installed commit/hash was not supplied,
and the report precedes these review fixes. E1C starts with the shared interfaces after
E1B review. The global native window ceiling and existing monotonic task VA
policy remain unchanged.
