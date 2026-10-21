# E5A review R1 — restored size and a smaller display

[Finding](https://github.com/VBWizard/os64/pull/322#discussion_r4332568298).
The native creation guard rejects dimensions above `wm_dim_max()` before
FIT_SCREEN runs. The host stub previously applied fitting without the preliminary guard,
masking the problem.

The corrected stub and added cases against the pre-fix production code
(`12c571b5`) reproduced [16 failed checks](before.txt). With the caller clamping
before creation, [4,621 checks pass](after.txt), using ASan, UBSan and
LeakSanitizer outside the sandbox. The new matrix tests both Open and Save at
2560×1440, 1920×1080, 1024×768 and then 5120×2880, using saved preferences of
2500×1360, 5000×2700 and 800×5000. It verifies successful usable dialogs,
expected fitted dimensions, cancellation without preference writes, restoration
on the larger display, and the screen-query failure fallback. These host
surfaces omit decoration insets; the native check below includes them.

Reproduce with:

```sh
python3 tools/test_file_dialog_host.py --output /tmp/dialog-polish-host --build-only
ASAN_OPTIONS=detect_leaks=1 /tmp/dialog-polish-host/test_file_dialog userland/libfreetype/fixtures
```

The Scribe/filedialogtest/libos64 cross-build passed with warnings as errors.
[Source and binary hashes](sha256.txt) identify the revised payload, refreshed
in `/tmp/file-dialog-polish-wire`. The original E5A hashes and screenshots
remain historical evidence of the preceding implementation.

## Native check

A fresh copy of the scratch VM disks used Q35, 4 CPUs, 4 GiB RAM, the same
kernel/GUI boot ISO as the original E5A check, and 1024×768 resolution. The root
copy received the rebuilt libos64. Before boot, its home copy was seeded with
`size = 5000,2700` in `file-dialog.conf`.

Scribe's [Open](open-clamped.png) and [Save As](save-clamped.png) both opened at
(0,0), with 1024×768 frames and 1022×747 content, fitting the display including
decorations. [Window creation log](native-windows.txt). Both were cancelled;
reading the config afterwards showed [the original 5000,2700 pair](preference-preserved.png).
The VM shut down cleanly. Repository images and P5 files were not changed.

[Stale-reference audit](stale-refs.txt) found no retired names or new
superlative claims in the code change. The original P5 acceptance predates
this correction; targeted P5 retesting and review of R1 remain pending.
