# E1B second review follow-up

2026-10-12. PR #259 review of `1d5e3e5c`. Reproduce host checks with the
[parent commands](../README.md#host-reproduction), using outputs
`/tmp/e1b-r2-final-fake`, `/tmp/e1b-r2-final-real` and `/tmp/e1b-r2-mutations`.
ASan, UBSan and LeakSanitizer run outside the sandbox with leak detection enabled.

| Finding | Fix and evidence |
| --- | --- |
| Refresh masks failed native teardown | Return close's ERROR before action refresh's STALE/other failure. Injection combines stale target with persistent destruction refusal, verifies the registration/session survive, then retries successfully and receives STALE. |
| Scrolling leaves an offscreen selection | Clear a selection that leaves the viewport and its transient help. Wheel and affordance paths cancel pressed activation and queued children. Subsequent arrow/page navigation starts in the visible rows. Tests cover wheel, affordances, retained visible selection and extreme scroll counts; Enter cannot invoke the hidden Record command. |
| Keyboard movement leaves a queued child | Shared selection handling cancels deeper pending models and closes descendants before changing row. Tests queue a child replacement behind tombstones, then move by Up/Down, Home/End, Page Up/Down or duplicate mnemonic before the empty boundary; no stale child is born. Pointer selection shares the same handling. |
| Hidden mnemonic has no visible bar highlight | Keep the requested model in the panel and select the displayed overflow affordance. Tests verify hover preserves that model and Left navigates from the displayed index. |
| Standalone zero is treated as an owner | Require a nonzero owner for family membership and owner-content focus transitions. Root and cascade tests verify dismissal when external focus's other handle is zero. |

The standalone finding's reported nondismissal was not reproduced: the previous
owner-content branch already closed the menu, but incorrectly armed suppression
for a nonexistent owner. Both zero-handle classifications are corrected.

[Before-fix regressions](regressions-before.txt) produce ten failures across the
first four findings (including downstream consequences of hidden activation).
The final [fake-font suite](host-fake.txt) passes 2,526 checks; the
[real-font suite](host-real.txt) passes 2,574. [Six mutation gates](mutations.txt)
and the [full userland cross build](build.txt) pass. Earlier editor/font tests
remain the first-review evidence; this change touches menu/controller code only.

[Source hashes](sources.sha256) and [binary hashes](binaries.sha256) identify the
tested sources/artifacts. The [focused native controller](round2.py) uses the
same private VM and unchanged E1K kernel as the parent reproduction. No new
P5 deployment is claimed; Chris's earlier successful hardware report is retained
separately. Native destruction failures and pending-child queue races use host
injection. No kernel, E1C migration or gclock debt changes are included.

## Native reproduction

Use the parent QEMU launch/image-staging commands and
[first-review acceptance controller](../review-r1/acceptance.py), which checks
actual overflow-child opening. Copy this directory's `round2.py` to
`/tmp/e1b-vm/round2.py`. With USB HID present initially, run modes in this order:

```sh
python3 /tmp/e1b-vm/acceptance.py hid
python3 /tmp/e1b-vm/round2.py r2-hid
python3 /tmp/e1b-vm/acceptance.py ps2-font
python3 /tmp/e1b-vm/round2.py r2-ps2
python3 /tmp/e1b-vm/acceptance.py external
python3 /tmp/e1b-vm/acceptance.py quit
```

The focused controller applies Interface 32px, launches a narrow gallery,
opens hidden Fixtures with Alt+F, enters overflow directly and asserts that
the menu body stays identical. Left then opens Actions. It opens Tall via
Alt+L, moves onto its first row, wheels downward and presses Enter (no command),
then Down/Enter selects and invokes a visible row. It closes the owner, syncs
the trace and restores Interface 16px. Guest reports are
`/home/e1b-r2-hid.txt` and `/home/e1b-r2-ps2.txt`.

An early probe moved through Actions on its way to overflow and correctly
switched headings. That trial was discarded; the retained probe routes through
the titlebar to enter overflow directly. No code change was made for that
controller path.

## Native results

All six controller modes above exit zero on the retained four-core VM.
[HID](controller-hid.txt), [PS/2/fonts](controller-ps2-font.txt) and
[external focus/cleanup](controller-external.txt) retain the broader run;
[serial](serial-acceptance.txt) records guest startup/exits. Focused controller
logs: [HID](controller-focused-hid.txt), [PS/2](controller-focused-ps2.txt).

| Focused check | HID evidence | PS/2 evidence |
| --- | --- | --- |
| Hidden Fixtures highlights overflow | [Mnemonic](r2-hid-hidden-mnemonic.png) | [Mnemonic](r2-ps2-hidden-mnemonic.png) |
| Direct overflow hover preserves Fixtures | [Hover](r2-hid-overflow-hover.png) | [Hover](r2-ps2-overflow-hover.png) |
| Left starts from the displayed affordance and opens Actions | [Left](r2-hid-left-actions.png) | [Left](r2-ps2-left-actions.png) |
| Wheel clears hidden selection; Enter leaves the menu open | [Wheel](r2-hid-wheel-no-selection.png), [Enter](r2-hid-enter-no-invocation.png) | [Wheel](r2-ps2-wheel-no-selection.png), [Enter](r2-ps2-enter-no-invocation.png) |
| Down selects visible Row 03; Enter invokes it | [Selection](r2-hid-visible-selection.png), [invocation](r2-hid-visible-invocation.png) | [Selection](r2-ps2-visible-selection.png), [invocation](r2-ps2-visible-invocation.png) |

The focused [HID trace](focused-hid.txt) and [PS/2 trace](focused-ps2.txt)
each contain four Enter edges before the first callback: the first Enter invokes
nothing, and the second invokes visible command 103 with `menus=0`. Quit is
the only subsequent callback. All gallery traces exit with zero font bytes and
references, including [broader HID](guest-hid.txt), [PS/2](guest-ps2.txt),
[overflow](guest-overflow.txt) and [external-focus](guest-external.txt) runs.
[Overflow child](overflow-child.png) actually opens in the broader PS/2 run.

Twelve warm cycles then 24 more return to matching mapped bytes (1,048,576),
live bytes (193,216), live blocks (493), high-water (463,280) and audit OK:
[warm heap](heap-warm.txt), [after heap](heap-after.txt).
[Stale-reference check](stale-refs.txt) reports no retired prose references or
new superlative comments. Refresh/destruction and queued-child races remain
host-injected evidence; standalone focus is checked through that shared backend,
without adding an E1C launcher adapter to this slice.
