# E1B fifth review: publication and viewport transitions

2026-10-12. PR #259 reviewed `36e93f67`. These findings extend the
[controller audit](../../../E01B_INVARIANTS.md) with selection eligibility after
publication and viewport effects, rather than treating repaint as complete
refresh handling. The public API and native E1K kernel are unchanged.

| Finding | Invariant and shared fix | Regression |
| --- | --- | --- |
| Deferred opening reports the opening error before a failed close | I2/I6/I8: attempt cleanup before reporting, and give a failed close precedence. Immediate root opening, deferred opening, bar model preparation and view preparation use shared failure cleanup; `request_panel` returns its result for its caller's policy. | Deferred child failure (allocation/native limit) × parent destruction (success/refused). The error hook observes completed cleanup or retained parent ownership and receives one final result. The overflow preparation sibling combines allocation failure with refused root teardown. |
| Bar-only refresh leaves a disabled selected heading | I5/I9: bar-only mode needs an eligible displayed selection after publication. Entry, explicit refresh and application same-view/target publication share normalization. No enabled heading closes mode and collapses temporary reveal. An invalid pending bar root is cancelled before its heading changes; live panels keep their anchors until teardown. | Alt/F10 × hidden/visible; disable the selected heading, use Down to open its enabled successor; all-disabled hidden recovery; same-view publication; pending root behind 150 owner events; popup refresh still schedules owner damage. |
| Clamped wheel input closes children | I3/I10: teardown belongs to an actual viewport change. Controller and widget share a nonmutating requested-top calculation. No-op wheel/affordance events preserve child path, pending intent, selection and help token. | Short parent; zero delta; top/bottom clamps × wheel/affordance; pending child behind queue drains. Actual movement still closes children, while refused removal leaves the old viewport/anchor intact until retry. |
| Downward scrolling leaves a one-row tail | I9/I10: the final viewport contains the largest fitting suffix of model rows. Compute its first row by actual command/separator heights, and bound both scrolling and selection's viewport to it. | Mixed-height rows at 150/183/220px, extreme signed counts, repeated small steps, reverse clamp and End selection. The final row is visible and no preceding row could fit in the remaining viewport space. |

The current regressions can be run against the reviewed menu sources with
[baseline.py](baseline.py). It retrieves immutable `36e93f67` versions of
`ui_menu.c` and `ui_menu_widget.c` into a scratch directory and compiles the
current harness against them. The private layouts and other linked modules are
compatible with that revision. [Before-fix results](regressions-before.txt)
record the source hashes and 78 behavioral assertion failures; fixture faults
are rejected. This total includes repeated between-event checks, rather than
representing 78 different findings.

```sh
ASAN_OPTIONS=detect_leaks=1 python3 docs/design/pending/explorer/evidence/e01b/review-r5/baseline.py --output /tmp/e1b-r5-baseline
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --output /tmp/e1b-r5-fake
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --menu --real --output /tmp/e1b-r5-real
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_menu_mutations.py --output /tmp/e1b-r5-mutations
make -C userland -j8
```

Set `E1B_R5_ONLY=1` for the focused follow-up, or use the complete suite as above.
[Fake](host-fake.txt) and [real-font](host-real.txt) suites pass
125,601 / 125,649 assertions with ASan, UBSan
and LeakSanitizer outside the sandbox, with leak detection enabled. The structural
observer also checks visible selected rows and eligible bar-only selection
between events. Repeated invariant assertions are not counts of unique traces.
The complete suite retains prior allocation/font/target/callback/input tests.

The strict [full userland build](build.txt) passes. [Source](sources.sha256),
[guest binary](binaries.sha256) and [host binary](host-binaries.sha256) manifests
identify tested artifacts. Earlier [editor/Scribe/font checks](../review-r4/README.md)
remain separate evidence; this revision changes menu code and its harness.
Destruction refusal, allocation failures, stale snapshots and precise backlogs
are host-injected. No new P5 result, E1C migration, icon assets, kernel change
or gclock debt work is claimed.

[Fourteen mutation gates](mutations.txt) detect this round's hazards plus the
previous ten. Each injected binary must report a behavioral assertion failure;
a crash without a failing assertion is not counted as detection.

## Native checks and focused P5 repeat

Use the [parent private-disk setup](../README.md#qemu-reproduction) and
[first-review controller](../review-r1/acceptance.py) with the matching binary
manifest. Run `hid`, this directory's `scroll-controller.py hid`, `ps2-font`,
`scroll-controller.py ps2`, `external`, then `quit` against
`/tmp/e1b-vm/qmp.sock`. Python/Pillow are required. The focused controller opens
Fixtures and its first child, moves over the non-scrollable parent and compares
before/after-wheel pixels. It then scrolls Tall to the bottom and selects a
visible command with Down/Enter.

All controllers exit zero: [HID/pointer](controller-hid.txt),
[PS/2/font/overflow](controller-ps2-font.txt),
[HID scrolling](controller-scroll-hid.txt),
[PS/2 scrolling](controller-scroll-ps2.txt),
[external focus/close](controller-external.txt).
[Serial](serial-acceptance.txt) identifies the run.

[Archived artifact checks](native-checks.txt) pass; reproduce them with
`python3 docs/design/pending/explorer/evidence/e01b/review-r5/check-guest.py`.
The six gallery traces show callbacks with menus closed, zero font/target
ownership at exit and explicitly sized popup surfaces. Both scroll traces
invoke visible command 118, then Quit. Before/after parent pixels match for
[HID](r5-hid-short-parent-after-wheel.png) and
[PS/2](r5-ps2-short-parent-after-wheel.png); the child remains live.
[HID tail](r5-hid-packed-tail.png) and [PS/2 tail](r5-ps2-packed-tail.png)
show rows 18–49 fitting at the bitmap Interface size, instead of one command
above a blank panel. [Keyboard selection](r5-ps2-tail-selection.png) stays
within that viewport.

The native [32px font report](font32.txt) and [View](font32-view-final.png),
[overflow child](overflow-child-final.png), [dismissal](dismiss-click-final.png)
and [later click](later-click-final.png) retain the preceding acceptance behavior.
Twelve warm openings followed by 24 more return mapped bytes (1,048,576), live
bytes (193,248), live blocks (493), high-water (463,312) and audit OK to matching
values: [warm](heap-warm.txt), [after](heap-after.txt). Bar-only snapshot changes
and combined failures are deterministic host checks, not induced native races.

For a focused P5 repeat on the updated build, open Fixtures → Duplicate mnemonics
and wheel over the short Fixtures parent; the child should remain open. Then
open Long overflow heading and wheel repeatedly to the bottom. The last command
should share the viewport with the preceding commands that fit; it should not
sit alone above a blank panel. Down/Enter should activate a visible command.
The earlier P5 report remains evidence for its original unspecified revision.
