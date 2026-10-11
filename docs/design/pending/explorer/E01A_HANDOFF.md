# E1A — Implementation handoff: actions and status

**State:** Reviewing, 2026-10-11. Implementation owner: Sol.
Branch: `codex/explorer-e01a`; worktree: `.worktrees/explorer-e01a`; runtime base:
`6d988565`. Implementation commit: `bd86ce4f`; branch pushed to origin.
[PR #254](https://github.com/VBWizard/os64/pull/254) targets `userland`. Chris confirmed a successful OS checkout.
Ordinary review and merge remain pending. Quinn owns the design.
Host and scoped QEMU acceptance are recorded in [the evidence packet](evidence/e01a/README.md).
Fable's E1K review changes no E1A action/status contract. See the
[review disposition](GUI_POPUPS_DISPOSITION.md#effect-on-sols-active-e1a).
This packet uses normal review; it does not need Fable architectural review.

Read [E01_MENUS.md](E01_MENUS.md) for the governing interaction/ownership contract.
E1A is independently buildable while [E1K](GUI_POPUPS.md) resolves its focused
review follow-up. Native popup integration is explicitly not part of this packet.

## Outcome

Provide immutable action descriptions, separate per-target state views, bindings
to existing buttons, shared shortcut matching and an independently usable status
widget. A gallery demonstrates the same action through a button and shortcut,
different enabled state for two target contexts, view toggles and persistent
notice priority. The gallery is a real libui application, not painted mockups.

Start from current `userland` in a fresh implementation worktree. The design
was inspected at `6d988565`; compare changed UI/font/build files before coding.
Do not merge the design worktree's old runtime base over newer userland work.

## Public data contract

Put these types and functions in focused headers, consistent with existing
`os64_` naming. C spelling may be refined by the implementer; these fields and
ownership rules are the contract. No kernel ABI changes are involved.

| Type | Fields / rules |
| --- | --- |
| Action spec | `uint32_t id`; command/toggle/radio kind; `uint32_t radio_group`; borrowed input strings for label/help/optional icon key, copied by create; optional ASCII mnemonic; normalized shortcut. |
| Shortcut | Named key enum for supported nonprinting keys, or ASCII letter/digit/punctuation; exact Shift/Ctrl/Alt mask. NONE means no shortcut. Reject Ctrl+Alt registrations reserved for WM and Alt+Tab/Alt+F4/Alt+Left/Alt+Right. |
| State | Boolean enabled and checked. Command kind cannot be checked; radio-group coherence is validated per view. |
| Context | App cookie plus `uint64_t generation`; retain/release/validate callbacks. Retain returns success or failure; release/validate cannot allocate or pump events. A static application context may use explicit no-op retain/release. |
| Action callbacks | Query `(app_cookie, context, id, state_out)` with no failure/allocation; invoke `(app_cookie, context, id)` returns performed/refused/error. App cookie outlives registry. |
| Registry | Opaque, owns copied specs/strings and callback table; counted references from views and menu models; destroy returns BUSY while referenced. |
| Action view | Opaque, owns registry reference, retained context and bounded state snapshot. No process-global current context. |
| Button binding | Embeds/attaches the association of one ordinary button, view and ID. Track attachment as a view reference; detach before either button storage or view is destroyed. |

Provide the operations listed in E01's action table plus explicit button unbind,
view retain/release for menu controllers, and snapshot lookup by action ID.
Use a common status enum with OK, INVALID, DUPLICATE, LIMIT, NO_MEMORY, STALE,
BUSY and REFUSED. Invocation errors additionally carry an app-provided numeric
result; the toolkit does not manufacture a user-facing diagnostic for an
application operation it does not understand. A status is not an errno alias.

Registry create validates the whole set before publication. View create must
undo context retention if allocation or validation fails. View refresh builds
its candidate states in preallocated storage and commits as one update. If the
context is invalid or radio state inconsistent, make that view noninvokable and
return STALE/INVALID; do not show old enabled state as current. Snapshot queries
are read-only and return no pointers to internal mutable buffers.

Bind/unbind operate on the UI thread outside dispatch of that same button.
Binding preserves button press/release/focus semantics and supplies its caption,
enabled state and callback. Unbind cancels outstanding presses before detaching;
the caller restores/replaces any desired legacy caption/callback explicitly.
Do not modify global widget class objects or borrow a caption from freed registry
storage. Invocation revalidates current state, including after a held press.

Callbacks may request deferred application teardown. Bindings retain views
until unbound; invocation additionally retains its view across the callback and
semantic refresh. The bound click handler performs no button access after the
callback. After dispatch returns, unbind buttons while their storage and UI are
alive, destroy views and registry, release widget/font resources, then free the
tree storage. Test this lifetime rather than assuming callbacks are harmless.

## Keyboard and visibility scope

Add a shared shortcut matcher/router, not a rewrite of the terminal decoder.
Normalize Ctrl letter/punctuation control bytes and ignore Caps/Num/HID tag for modifier
comparison. Use physical dialect only for nonprinting keys. Recognize F10 but
do not implement menu reveal until E1B. Arrow escape bursts must not match
ordinary printable shortcut letters; feed their full gesture through the
existing decoder or reject burst events in the shortcut matcher.

This packet routes only explicit application shortcuts bound to a supplied
action view. It does not seize textfield clipboard chords globally. The gallery
selects its target view explicitly; Scribe's focus/editing adapter arrives in
E1C. Track consumed key identity until release so one held toggle/command fires
once, and reset on focus loss. Do not deduplicate navigation repeats in the
existing widget decoder.

Use toggle actions for gallery panel/status visibility. Query the actual hidden
state after invocation, reflow the root and move focus away before hiding a
focused subtree. Persistent visibility storage belongs to the application and
is outside this packet. Showing two targets in the gallery does not implement
Roam's split panes or tabs.

## Status widget API

Initialize the widget, set base information, set/clear transient help with an
ownership token, and set/clear a persistent notice with its generation. A set
copies text and prepares its UI-font run before replacing current text. On
failure the previous string/run remains usable. Source strings may be released
as soon as the setter returns. Limit each string to 512 bytes including NUL;
reject embedded invalid input according to the existing UI text policy rather
than adding a separate UTF-8 decoder.

Use a 64-bit per-widget generation counter, reserving zero for no message;
refuse exhaustion rather than accepting an old token. Stale clear is a no-op
with STALE result. Hidden widgets retain all messages but receive font preparation
as required by the existing tree contract. Destroy releases active/staged runs
and strings. `prepare/commit/discard/destroy/metrics` must honor libui's font
transaction contract. Narrow layout gives persistent notices priority; menu
help will use the transient slot in E1B.

## Implementation sequence and acceptance

1. Add `ui_action.h`/`ui_action.c`, including view ownership and bounded validation.
   Build a host harness exercising allocation failure, duplicate registration,
   stale target contexts and independent snapshots. Do not write unused menu
   transport stubs to make this compile.
2. Add button binding and shortcut routing; preserve unbound button behavior.
   Test disabled-after-press, held keys, PS/2/HID nonprinting identities, escape
   burst rejection, callback-requested close and detach before view destruction.
3. Add `ui_status.h`/`ui_status.c` with font adoption and token-owned messages.
   Test interleaved help producers, persistent error priority, failed text/font
   preparation, narrow layout and repeated hide/show.
4. Add the gallery application and host harness build wiring. Reuse current
   theme/font settings and verify real fonts as well as the fake backend.
5. Run the existing UI and relevant font/Scribe host checks; run the gallery
   in QEMU, record screenshots and resource baselines. Keep leak checks enabled
   and use the approved outside-sandbox route when necessary.

Required gallery observations: both presentations invoke the same callback
once; the two target contexts have independent state; a stale target disables
its action; hidden focused controls relinquish focus; new transient help cannot
be cleared by an older token; persistent notice remains legible while help
changes; larger Interface font reflows without losing messages or actions.

Actual commands should follow existing harness shapes:
`python3 tools/test_ui_text_host.py`, its `--real` variant where supported,
`python3 tools/test_scribe_host.py`, plus the new focused action/status harness.
These are required future checks, not results from the design session.

## Stop and escalate

Stop this packet and update the design if implementation requires a kernel
change, a new global input policy, a circular library dependency or alteration
of Scribe's document/clipboard semantics. Do not use E1A to implement native
popups while their focused design questions remain open. Routine C naming,
module splitting and test fixture choices belong to the implementer.

At completion, update the E1 board/handoff with commits, actual tests and review
state. A successful E1A does not mark E1 complete or claim native menus exist.

## Implementation result

The public API is in `os64/ui_action.h`, `ui_result.h` and `ui_status.h`;
`ui_action.c` and `ui_status.c` implement it. Shared text validation uses the
existing bounded Western UI profile through private `ui_message.h`. The
`actiongallery` application selects explicit target views; its application
controls use a separately retained static context. No global context or native
menu transport was added. Existing button classes and constructors are unchanged.

Action descriptor input strings are copied. Spec lookups borrow immutable
strings for the registry lifetime; state snapshots copy values. Registry/view
retain-release APIs support future menu ownership. Bindings detach explicitly,
clear their caption/callback and cancel outstanding presses. Invocation queries
semantic state after callbacks without touching buttons; the application
refreshes presentations after dispatch if teardown was not requested.

Navigation escape bursts remain with the widget decoder. Named navigation
shortcuts require a zero-ASCII source with release edges; printable keypad input
matches by ASCII. This is the handoff's permitted burst-rejection choice, not a
kernel input change. F10 identities are recognized without menu reveal.

The status widget copies and prepares replacement messages before publishing,
reserves zero tokens, refuses counter exhaustion and prepares hidden messages
through the font transaction. Notice priority is measured from the retained run.
The gallery uses actual hidden state and transfers focus before hiding a subtree.

Validation: 231 fake-font and 278 real-font focused checks; 148/1626 existing UI
checks; 2470 Scribe checks; font contracts and strict cross build passed. Host
sanitizers retained ASan/UBSan and leak detection outside the sandbox after an
EPERM diagnosis. QEMU covered PS/2 and USB-at-boot, button/shortcut invocation,
independent/stale targets, tokens, visibility, 32px live Interface reflow, stable
warmed heap usage and exit code zero. See the linked packet for exact commands,
logs, screenshots, binary/source hashes and limits. Chris confirmed the gallery
worked perfectly in his OS checkout; hardware and installed revision were not
reported, so this is recorded separately from the automated QEMU results.

Next action: ordinary code review of `codex/explorer-e01a`, then address findings
and integrate after review. Chris explicitly requested commit and push, which
supersedes the QEMU guide's earlier no-commit instruction. The implementation
commit is `bd86ce4f`, introducing `ui_action.c`, `ui_status.c` and this record;
source/binary hashes retain the exact tested state. E1 remains open for E1K/E1B/E1C. The design snapshot was reconciled at `d33e651e`; the active
design branch owns subsequent native review changes.

## First review findings

Both P2 findings against `e0800cb8` are addressed by the review revision:
Ctrl punctuation controls 27–31 match their registered ASCII keys while keeping
physical Escape/editing and burst guards; status secondary text appears only
when its complete measured run fits beside the persistent notice. Messages and
tokens remain retained while suppressed.

[Review evidence](evidence/e01a/review-r1/README.md) records before-fix failures,
387 fake-font and 436 real-font passing checks with ASan/UBSan/leak detection,
strict cross build and QEMU notice priority/help restoration at 32px. Original
acceptance results remain historical evidence for `bd86ce4f`; review source and
binary hashes are separate. Next action: ordinary re-review on PR #254.

## Hidden status row review finding

The gallery font planner now excludes the status row when `status.w.hidden`
is true, while preserving hidden message preparation and ownership tokens.
[Second-review evidence](evidence/e01a/review-r2/README.md) records two
before-fix failures and 428 fake-font / 475 real-font passing checks with
ASan/UBSan/LeakSanitizer enabled. Tests exercise exact-fit adoption, one-pixel
short refusal, visibility restoration and both panel states through the
production font consumer, including real 32px Interface reflow. The strict
cross build passed; ordinary re-review remains pending on PR #254.

## Binding teardown contract review finding

The public header's teardown order was wrong: binding detachment requires live
button and UI storage. The header and this handoff now specify unbind before
view/registry destruction and widget storage disposal, after dispatch returns.
[Third-review evidence](evidence/e01a/review-r3/README.md) records the lifetime
inspection and rerun of the existing 428/475 fake/real sanitizer checks with
leak detection enabled. Runtime behavior is unchanged; re-review is pending.
