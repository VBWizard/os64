# PR #322 second review — 2026-10-21

Findings: [startup font fallback](https://github.com/VBWizard/os64/pull/322#discussion_r4338277462)
and [manual resize from maximized](https://github.com/VBWizard/os64/pull/322#discussion_r4338277471).
Both reproduced against `328cba6d`. The test uses real font, UI, directory and
browser code with native-window/syscall stubs; the minimum-size stub refuses
requests above screen capacity, matching the ordinary window reservation.

```
python3 tools/test_file_dialog_host.py --build-only --output /tmp/dialog-review-r2
ASAN_OPTIONS=detect_leaks=1 /tmp/dialog-review-r2/test_file_dialog \
  userland/libfreetype/fixtures /tmp/dialog-review-r2/after.ppm
```

The same new regression scenarios yielded [five failures before](host-before.txt)
and [4,693 checks with zero failures after](host-after.txt). ASan, UBSan and
LeakSanitizer remained enabled; runs used the normal outside-sandbox approval
path because this environment denies LSan thread attachment. Existing tests
still cover valid 32-pixel fonts growing a tiny saved window, normal restore
without stale preference writes, display down/up and configuration write failure.

Cross-build passed for libos64, Scribe, Roam and /tests/filedialogtest, using
`CCACHE_DIR=/tmp/dialog-review-ccache make -C userland` with the absolute binary
targets. The updated shared header is included in these consumer builds.

Native QEMU: Q35, 4 CPUs, 4 GiB, 640×480. Scratch disks only, booting the existing
kernel from the dialog-polish ISO with ext2 root. Installed rebuilt libos64 and
consumers; /home/fonts.conf selected /etc/fonts/DejaVuSans.ttf at 96 pixels for
UI, with builtin terminal/document roles. Open and Save both remained usable
with fallback fonts; Cancel left `size = 2500,1360` untouched. The kernel log
reported font layout refusal with current fonts retained.

For the resize test, started Open at `500,350`, maximized with Ctrl+Alt+M,
then completed Ctrl+Alt+right-drag to a smaller frame. After Cancel, the config
contained `538,379`; Save reopened with that content size. Screenshots show the
starting frame, maximization, saved dimensions and reopened Save dialog.

P5 acceptance and independent re-review are pending. The existing close delay
and exited-worker lifetime work remain separate. No kernel changes.
