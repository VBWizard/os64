# Frame Studio: rebase onto vncd and ZRLE

2026-09-23. `codex/frame-studio` now rests on `userland` commit `be2d755`,
including vncd, `/dev/glass`, loopback, SSH forwarding and ZRLE. Frame Studio's
work remains uncommitted and unstaged; the main checkout was left unchanged.

The original 161 changed/untracked files were preserved in a named stash
(`9a0e30f9aede2e4a37500b8d489a445126e31b37`) and a separate backup before
rebasing. Restoring the work produced one conflict, in the libos64 source
list: the resolution retains both the upstream PSF2 reader and the decoration
modules. The compositor, its header, sysfs and GRAPHICS.md merged automatically.
Comparison against the backup confirmed that only those five overlapping files
changed during integration; the design document was then updated to name the
new base.

The merged compositor retains backbuffer publication and glass damage reporting
after composition under the GUI lock, with remote keyboard repeats outside
that lock. Frame Studio's control state and decoration changes still feed the
same damage path.

Validation:

- Strict full builds passed, including kernel, userland, vncd, Frame Studio
  and the bootable ISO. Normal Limine settings were restored and rebuilt.
- Sanitized [decoration/storage/startup](rebase/decoration-host.txt),
  [appearance/widget](rebase/appearance-host.txt),
  [ZRLE](rebase/zrle-host.txt) and [gzip](rebase/gzip-host.txt) host tests passed.
- QEMU at 1920x1080 with 24px interface text loaded and applied the saved
  [Traffic lights composition](rebase/traffic-lights.png).
  The [editor log](rebase/studio-guest.txt) records publication at generation one.
- With that decoration installed, the guest [glass suite](rebase/glass-guest.txt)
  passed capture, banding, malformed-input refusal, painting, reader close,
  remote typing/clicking, held-key repeat and release handling. Its
  [exit value](rebase/glass-exit.txt), decimal 1638223872 (`0x61A55000`), is the
  fixture's declared success sentinel.
- vncd started in the guest and [listened on 127.0.0.1:5900](rebase/tcp-guest.txt).
  This run did not establish an SSH tunnel or connect an external VNC viewer.
- `git diff --check` passed. `stale_refs.sh` reported the existing
  `NO_DECORATIONS` shorthand in GRAPHICS.md and gui.h; the corresponding flag
  remains live. No new superlative claims were reported. The private VM was
  stopped after verification.

Delete selected and independent button-symbol colors remain the next slices.
