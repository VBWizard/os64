# E5A — Shared dialog layout and remembered size

Implemented by Quinn on `codex/file-dialog-polish`, based on `5fb5f7a6`.
Implementation `5efff386` is published as [draft PR #322](https://github.com/VBWizard/os64/pull/322).
PR #319 merged as `ddaad166`; Chris reported a clean review and successful P5
use, including resizing. No installed binary hash was captured.

The footer uses a filename row and a controls row, wrapping the action buttons
when necessary. Message space appears only for a prompt or error. Open and Save
share a content-size preference across applications. `file-dialog.conf` stores
`size = width,height` in pixels through the configured config search ladder;
there is no hardcoded home path. Each open reads the preference afresh.
A completed resize publishes the pair atomically, including before Cancel.
Closing an unchanged older dialog does not replace a newer preference.
Maximize/restore and automatic screen/font clamping do not save a preference.
Missing or malformed preferences use 880 × 700; failed persistence leaves the
previous config intact and file selection available.

Parent-owned stacking needs a separate window-manager follow-up. File
associations and Yonder integration remain separate work.

[Evidence](evidence/e05a/README.md): 4,223 shared-dialog and 2,584 Scribe
checks passed with ASan, UBSan and LeakSanitizer enabled. Cross-build passed.
Native QEMU verified resize/Cancel/restoration across Scribe and filedialogtest,
wrapped controls, replacement message expansion, and preservation of the
newer preference when an older dialog closes. Chris reported successful P5
acceptance at 2560 × 1440 with an Appearance Workshop appearance and a Frame
Studio composition: both dialogs looked correct, Open and Save As shared the
resized dimensions across reopening and a reboot, and navigation and New
Folder worked. This is user-reported evidence; no installed binary hash or
hardware screenshot was captured. Independent review and merge remain pending.

Chris also noted the Pin titlebar button on the dialogs. They inherit the
selected frame's actions as ordinary decorated windows. Pin currently raises
a window into the global pinned band; parent-relative stacking remains separate
work. Proposed follow-up: omit independent Pin for parent-owned file dialogs
when adding their ownership/stacking policy, consistently in frame painting,
hit testing and the keyboard action. This proposal is not implemented in E5A.

## Delivery and P5 walkthrough

Bundle: `/tmp/file-dialog-polish-wire`, with `SHA256SUMS`. Install `scribe`
to `/bin`, `libos64.so` to `/lib`, and `filedialogtest` to `/tests` through the
usual os64get routes. Rebuild/deploy these consumers together: the app-owned
dialog structure changed. Close applications using the previous library
before replacing it. Chris has tested the follow-up on P5 as recorded above;
the exact installed payload was not independently identified.

1. Open Scribe's picker and check the compact footer. Resize narrow enough to
   wrap the action buttons; the checkbox and New Folder should remain usable.
2. Resize Open, Cancel, then open Save As: it should retain the chosen size.
3. Launch `/tests/filedialogtest open /home`: it should use that size too.
   Resize it, cancel, then reopen Scribe's picker to see the new size.
4. With two dialogs open, resize the newer one and cancel both. Closing the
   older unchanged dialog should not restore its previous size.
5. Trigger replacement consent: its message row appears; Keep File removes it.

`file-dialog.conf` is resolved through `/etc/os64.conf`'s configured `conf`
ladder and writes to its top directory. Moving that ladder to `/home/yogi`
moves the preference destination without a source change. Invalid sizes fall
back to the default; dimensions are decimal integers from 1 through 32767,
separated by a comma. The caller clamps to the current display before native
creation, which then fits the frame decorations; composition
raises the minimum for the current font. A screen too small for those controls
can refuse creation. The stored preference is unchanged by these adjustments.


## Review R1 — saved size after a display change

The [review finding](https://github.com/VBWizard/os64/pull/322#discussion_r4332568298)
identified a creation-order problem: the kernel rejects dimensions above
`wm_dim_max()` before applying FIT_SCREEN. The host stub omitted that preliminary
limit and fitted the request directly, so the original screen-clamp checks
did not expose the failure.

Open now queries the screen and bounds each requested dimension before window
creation. If that query fails, the documented `OS64_GUI_WINDOW_DIM_MAX` provides
a safe bound for the native dimension guard; FIT_SCREEN still accounts for
decorations. These adjustments do not write back to the shared preference.
The original larger size can be restored when returning to a larger display.

The corrected stub enforces the native guard first. Regression cases exercise
Open and Save at 2560×1440, 1920×1080, 1024×768 and 5120×2880, with 2500×1360,
5000×2700 and 800×5000 preferences, plus a failed screen query. The expanded
suite reproduced 16 failures before the code fix and passes 4,621 checks after
it with ASan/UBSan/LeakSanitizer enabled. Cross-build passed. Native QEMU at
1024×768 opened both dialogs from a 5000×2700 preference and retained that
preference after cancellation. [Review evidence](evidence/e05a/review-r1/README.md).
Chris subsequently verified R1 on P5: Save As was sized almost to the full
2560×1440 display, then was fully visible after rebooting at 1024×768. After
rebooting back to 2560×1440, it restored the original large size. Chris approved
the fix. No installed binary hash was captured; independent review and merge
remain separate. His later close-delay report is tracked in
[E5B](E05B_CLOSE_LATENCY.md).
