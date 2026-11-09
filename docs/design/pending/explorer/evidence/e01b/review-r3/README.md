# E1B third review follow-up

2026-10-12. PR #259 review of `b4d5b897`. Use the
[parent host commands](../README.md#host-reproduction), with output directories
`/tmp/e1b-r3-fake`, `/tmp/e1b-r3-real` and `/tmp/e1b-r3-mutations`.
ASan, UBSan and LeakSanitizer run outside the sandbox with leak detection enabled.

| Finding | Fix and regression |
| --- | --- |
| Registered shortcuts shadow menu gestures | Decode menu keys and enabled popup mnemonics before unmodified action shortcut lookup. Modified registered chords and unmatched unmodified shortcuts still close/invoke against the saved view. A custom registry shadows Escape, Enter, Tab, arrows, Home/End, Page, Space and a popup mnemonic; tests verify menu behavior and the selected command win, while an ordinary unmatched shortcut invokes its registered command. |
| Failed hidden-bar refresh leaves a temporary row | Assign a session view after successful retention, and route refresh failure through close/collapse before reporting it. A stale target with cached enabled state reproduces the hidden-bar reveal leak; the fixed test verifies hidden/temporary/session/window state is restored. |
| Escape/Left orphan a queued child and crash | Centralize pending cancellation in the shared close path. Panel creation also rejects missing parents/depth gaps. Tests open three levels, queue a grandchild replacement behind tombstones and 150 surviving owner events, then collapse its parent with Escape or Left before queues drain. The root survives, no replacement is born, and all resources release. |
| Page advances by selectable-entry count | Compute one target using the viewport's model-row count, clamp at the ends, then search for an enabled landing entry in the requested direction (with reverse fallback). A sparse 61-row model with disabled entries and separators pages from command 1 to 11, and from 61 to 51, rather than skipping several enabled commands. |

The [before-fix suite](regressions-before.txt) reproduces the four findings and
their downstream consequences, ending in UBSan's missing-parent dereference.
The [isolated collapse](collapse-before.txt) reproduces that fault independently;
the [fixed isolated run](collapse-after.txt) passes 1,258 checks with leak detection.
To reproduce isolation using an already-built host binary:

```sh
E1B_R3_COLLAPSE_ONLY=1 ASAN_OPTIONS=detect_leaks=1 \
  /tmp/e1b-r3-fake/test_ui_action userland/libfreetype/fixtures
```

The complete [fake-font suite](host-fake.txt) passes 3,673 checks, and the
[real-font suite](host-real.txt) passes 3,721. [Six mutation gates](mutations.txt)
and the [strict full userland build](build.txt) pass. Earlier editor/font suites
remain separate evidence; this revision changes menu controller code only.

[Source hashes](sources.sha256) and [binary hashes](binaries.sha256) identify
the tested sources/artifacts. Native regression uses the parent QEMU setup and
[first-review controller](../review-r1/acceptance.py), unchanged E1K kernel and
private disks. Shortcut-conflict, stale-target and queued-collapse races are
host-injected, including real font runs. The earlier P5 report is unchanged;
no additional P5 deployment, E1C migration, kernel or gclock debt work is claimed.

## Native regression results

Rerun the parent QEMU setup with this revision's matching gallery/library, using
the first-review controller's `hid`, `ps2-font`, `external`, then `quit` modes.
All four modes exit zero. [HID](controller-hid.txt),
[PS/2/fonts](controller-ps2-font.txt), [external focus/cleanup](controller-external.txt)
and [serial](serial-acceptance.txt) logs retain the run.

- Tall-menu [End](tall-end-final.png) and [Page Up](tall-page-final.png)
  remain usable with disabled UTF-8 rows.
- [Dismissing click](dismiss-click-final.png) leaves one Record call;
  [subsequent click](later-click-final.png) makes two.
- Native [font report](font32.txt) and [32px View](font32-view-final.png)
  retain real Interface adoption; the [overflow child](overflow-child-final.png)
  actually opens. [External focus](external-focus-final.png) dismisses, and
  [owner close](owner-close-final.png) leaves no gallery popup.
- [HID](guest-hid.txt), [PS/2](guest-ps2.txt), [overflow](guest-overflow.txt)
  and [external](guest-external.txt) traces invoke with `menus=0` and exit with
  zero font bytes/references.
- Twelve warm cycles followed by 24 more return mapped bytes (1,048,576), live
  bytes (193,216), live blocks (493), high-water (463,280) and audit OK to matching
  values: [warm](heap-warm.txt), [after](heap-after.txt).

[Stale-reference check](stale-refs.txt) reports no retired prose references or
new superlative comments. Raw serial evidence retains the padded OEM identifier.
