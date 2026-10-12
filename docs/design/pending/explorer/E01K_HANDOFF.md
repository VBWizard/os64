# E1K implementation handoff

2026-10-11. Owner: Quinn. Chris authorized implementation after Fable's second
review. Base: `2bc9f2f4`; branch `codex/explorer-e1k`. This is the focused GUI
foundation for E1B, not menu widgets or application IPC. Governing design:
[GUI_POPUPS.md](GUI_POPUPS.md); review: [second pass](GUI_POPUPS_REVIEW.md).

## Concrete ABI

- Syscall 63: `gui_event_wait_any(int64_t *out_handle)`. Nonconsuming readiness;
  1 on success, INVALID_HANDLE for empty set, INTERRUPTED for signal exit,
  BUSY on another thread's waiter. Validate output before parking; copy the
  result from a kernel local after unregistering. Per-thread round-robin cursor.
- Syscall 64: `gui_window_get_content_rect(handle, rect *)`. Owner-checked,
  current content rectangle on screen; 0 or existing GUI error. Snapshot only.
- Errors: BUSY = -7; NOT_FOCUSED = -8. Existing numbers remain unchanged.
- Existing create: bits 48–63 carry a uint16_t owner handle, zero means unowned.
  Named inline owned-popup wrapper validates the full handle before packing;
  requests NO_DECORATIONS | PINNED, content sizing and CREATE_POPUP_CAPACITY
  (bit 34). The capacity hint requires borderless + pinned; omit it for
  resizable applications, including gclock. START_UNFOCUSED with
  an owner is BAD_ARGS. Other invalid flags retain the existing legacy policy.
- Focus payload: uint8_t gained, uint8_t sibling, uint16_t other_handle; same
  32-byte event and existing offsets. Other handle is zero across tasks.
- INPUT_LOST = 16, no payload. Assert both ABI definitions. It resets gestures;
  it is not a command, focus transfer or replacement for a state query.

## Wait and window lifetime

Use existing `kGuiLock` and SIGSLEEP backstop. Clear this thread's registrations
on every pass and exit. Check conflicts across the whole owned set before
registering any window; the legacy wait honors the same BUSY rule. Rotate ready
selection per thread. Newly created windows join the next pass/backstop. Do not
hold window pointers while parked or copy user output under the GUI lock.

Owned create captures the parent's window ID before allocation and validates
that same ID and current focus inside the birth critical section. Refusal runs
the existing pivot cleanup and preserves the specific error. Kernel owner links
are immutable IDs, with allocation refused at ID exhaustion. Stamp the client
handle before focus events. Destroy refuses BUSY before mutation when a direct
child exists. Sweep marks the task's windows retiring before deleting any, so
focus cannot return to another doomed window. Preserve canvas cleanup locking.

Focused child destruction conditionally returns to an eligible parent; external
focus is left alone. Skip popups in Alt+Tab and resolve a focused popup to its
ordinary ancestor for the first-step comparison. Damage ancestor chrome when
focus changes. Root launcher remains unowned and uses existing birth policy.

## Event loss and toolkit lifecycle

A reset before replaying an old queued down is insufficient. On any ring drop
or eviction, latch loss. Deliver INPUT_LOST after the existing appearance and
eligible doorbell slots. The first loss precedes ring events; repeated loss
yields to a retained control between notifications. On each pending loss remove
key/mouse gesture records under the same lock; retain window/control records
and coalesced state. Future input follows the reset. Window wait readiness
includes the loss latch. libui cancels pressed/focused interaction and its key
decoder; the action shortcut router resets held keys. Menu controllers dismiss
on loss. Direct consumers must reset their custom gestures too: gterm selection,
Workshop's overlapping trees, Frame Studio preview capture, and Yonder's page
presses, question bar and key decoders. grootmenu dismisses on loss.

Reusable focus handles are delivery-time facts. E1B owns window lifecycle and
dispatch on one UI thread. Mark destroyed handles as tombstones; forbid window
creation until every surviving owned queue has been drained to empty at least
once after the last destruction. Destruction itself generates focus events,
so merely draining before destruction does not suffice. Queues must be serviced
in bounded round-robin turns; if the budget expires, defer creation and retain
tombstones into the next turn. No kernel producer can reference a destroyed
window after its locked removal. Unknown handles dismiss conservatively.
Application-created sibling windows must participate in this protocol.

The menu dismissal machine retains OPEN / CLOSED_BY_ME / DISMISSED across queue
ordering. Explicit close sets CLOSED_BY_ME before native destruction. Focus
return caused by that close never arms a swallow. A transition to owner content
while OPEN dismisses and consumes one matching down/up pair; an external window
receives its own click normally. Loss cancels suppression and closes the menu;
truncating pending gesture records prevents the dismissing down from reaching
ordinary widgets after suppression is reset. Focus changes alone are not proof
that a content click will follow (titlebar/WM focus also exists); E1B must expire
an unpaired suppression at a defined dispatch boundary rather than eat a later
unrelated gesture. Cover this in the controller acceptance before E1B is Ready.

## Capacity and evidence

CREATE_POPUP_CAPACITY explicitly selects birth content dimensions in both
surface stores and the task allocation. The popup wrapper sets it for owned
and unowned menus; legacy borderless/pinned windows retain ordinary capacity. Normalize popup minimums to
fit that capacity. Resizes never remap the canvas; growth beyond birth capacity
requires a replacement. Test narrow/small popup floors and clamped creation.

Required host evidence: real queue loss/drop/eviction behavior, no rearmed
pre-loss down, preserved control/appearance/doorbell behavior, UI press reset
and shortcut reset, supported ASan/UBSan/LSan. Required guest evidence: no-window
wait, readiness/peek/fairness, wait conflicts/interrupt/teardown, owner errors,
focus events/return/BUSY destroy, task-exit cleanup, small popup surfaces and
content readback. Use SMP and a diagnostic with panels beyond owner edges;
exercise Alt+Tab, external clicks and repeated resource release. Existing GUI,
font and action tests must retain their contracts.

## Implementation checkpoint

The implementation is available for ordinary review in
`.worktrees/explorer-e1k` on `codex/explorer-e1k`, based on `2bc9f2f4`.
[Evidence](evidence/e01k/README.md) records the cross builds, supported sanitizers,
baseline SMP guest run, existing focus/doorbell regressions and visual checks.
The [readiness regression fix](evidence/e01k/wake-regression/README.md) adds
deterministic unrelated-event coverage; ten full QEMU runs passed 153 checks each.
Chris subsequently confirmed validation in both QEMU and on the P5. This is
user-reported acceptance of the corrected diagnostic; no additional run counts,
logs or installed-binary hashes were supplied. First-review fixes add explicit
popup capacity, overflow fairness and custom-consumer resets. Their host tests
and two 159-check QEMU runs pass; same-process gclock font growth and manual
resize were checked visually. Re-review and fresh P5 validation are pending.
Implementation commit `3fee1034` is pushed;
[PR #257](https://github.com/VBWizard/os64/pull/257) targets `userland`.

E1B starts after the public contract, guest evidence and ordinary review are settled.
Keep any uncovered existing VA allocator race separate from this packet.
