# E1B first review follow-up

2026-10-12. PR #259 review of `17fb4eeb`. Reproduce the host runs using the
commands in the [parent evidence](../README.md#host-reproduction); the output
directories for this run are `/tmp/e1b-review-final-fake`,
`/tmp/e1b-review-final-real`, `/tmp/e1b-review-editor` and
`/tmp/e1b-review-mutations`. Leak detection is enabled outside the sandbox.

| Finding | Fix and regression |
| --- | --- |
| Invalidated submenu leaves descendants/help alive | Refresh closes below the first invalid selection, cancels pending opening/activation, and recomputes help. Test disables the sole action in an open child and checks its parent remains, the child disappears and help clears. |
| Alt/F10 remembers a disabled heading | Refresh bar states, select an enabled visible heading including overflow, or decline activation and undo temporary reveal. Tests cover F10/Alt skipping a disabled first heading and all-disabled hidden-bar recovery. |
| Native destruction failure discards live borrowed state | Close/dispose return ERROR with surviving panels, views and hooks retained. Root/child replacement and callbacks stop; font adoption/release refuse with BUSY. Tests inject persistent failures, dispatch/paint after rejected disposal, exercise child-first partial closure and retry successfully. Gallery checks disposal before releasing owner state. |
| Session accepts a foreign/unattached bar | Reject when the supplied bar's UI differs from the owner UI. Test covers unattached and sibling bars without attaching hooks. |
| Owner wheel reaches obscured content | Consume owner wheel during an active menu session. Test confirms no owner delivery while active, then normal delivery after closing. Popup-local scrolling remains in the existing suite. |
| Native recheck: press-only PS/2 navigation remains held | Extended PS/2 navigation emits escape bursts without release events. Decoded arrows/Home/End/Page keys now repeat without the activation guard; Enter/Space, mnemonics and mode keys keep it. A test uses Right in bar mode then Right in a later popup with no intervening Right release. It fails before this fix in [ps2-before.txt](ps2-before.txt). |

Before the fixes, the navigation/ownership/wheel regressions produce seven
behavioral failures in [regressions-before.txt](regressions-before.txt). The
destruction regression uses the new result-returning API and injects errors
through the unchanged native backend seam.

- [Fake-font suite](host-fake.txt): 2,079 checks, zero failures.
- [Real-font suite](host-real.txt): 2,127 checks, zero failures, including
  rejected real-font adoption with a live popup whose destruction failed.
- [Editor suite](editor-real.txt): 1,626 checks, zero failures.
- [Font contracts](font-contracts.txt): lifetime/allocation/layout gates pass.
- [Mutations](mutations.txt): six behavior mutations detected; lifecycle source
  anchors updated for the result-returning close API.
- [Build](build.txt): matching-header full userland strict cross build passes.

[Source hashes](sources.sha256) and [binary hashes](binaries.sha256) identify
the tested review sources/artifacts.

The [review controller](acceptance.py) was rerun with these artifacts in the isolated four-core
QEMU VM and unchanged E1K kernel, using the parent reproduction commands and the
existing private disk copies. Modes `hid`, `ps2-font`, `external` and `quit` all
exit zero. It uses one Right from Actions to narrow overflow and asserts that
the subsequent child screenshot differs from the parent. The original controller
used two Right presses, of which the held-key guard ignored the second, and
did not check that its child capture actually opened. The initial evidence's
child claim was wrong and has been corrected. [HID](controller-hid.txt), [PS/2/fonts](controller-ps2-font.txt) and
[external-focus](controller-external.txt) logs retain the screenshot sequence;
[serial](serial-acceptance.txt) records guest startup and task exits.

- USB HID/PS/2 traces preserve keyboard navigation and close-before-callback:
  [HID](guest-hid.txt), [PS/2](guest-ps2.txt), [overflow](guest-overflow.txt).
  Every gallery exit reports zero font bytes/references.
- [Dismissing click](dismiss-click-final.png) leaves one target call;
  [subsequent click](later-click-final.png) makes two.
- Native real-font change closes menus before adoption, then reopens with the
  new size: [report](font32.txt), [closed](font-closed-final.png),
  [32px View](font32-view-final.png). Color-only adoption remains live:
  [palette report](palette.txt).
- Narrow [overflow](overflow-menu-final.png) retains View, Fixtures and Long
  overflow heading; its [child](overflow-child-final.png) remains usable.
- Popup-local [wheel before](wheel-before-final.png)/[after](wheel-after-final.png),
  [external focus](external-focus-final.png), [popup close](popup-close-final.png)
  and [owner close](owner-close-final.png) remain correct; [trace](guest-external.txt).
- Twelve warm cycles then 24 additional cycles return mapped bytes (1,048,576),
  live bytes (193,216), live blocks (493) and high-water (463,280) to the same
  values, audit OK: [warm](heap-warm.txt), [after](heap-after.txt).

Native destruction refusal and owner-wheel delivery are exercised by the host
backend injection/dispatch tests, not by a native failure induced in QEMU.
[Stale-reference output](stale-refs.txt) was reviewed: both reported documentation
paths still exist and their links remain valid; no new superlative claims.

Initial E1B native evidence is retained in
the parent directory; the P5 report there predates these fixes. No kernel or
gclock debt changes are included.
