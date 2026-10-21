# E5C — Roam file actions

## Status and ownership

Reviewing, 2026-10-21. Quinn owns `codex/roam-file-actions` in
`.worktrees/roam-file-actions`, based on `328cba6d` (PR #322, awaiting review).
Implementation `9f31fb0f` is pushed in [draft PR #325](https://github.com/VBWizard/os64/pull/325). Scribe's shared dialogs are merged through #319. This slice
stacks on the dialog polish; the worker-lifetime design remains deferred.
D012 architectural review does not apply: this is userland work, without kernel
or syscall changes. Ordinary review and P5 acceptance remain required.

## Outcome and contract

Double-click/Enter and Open launch a file's associated application. Open With
selects an executable using the shared Open dialog, initially at /bin; unknown
extensions take that same path. The choice applies to this launch. Copy Full
Path copies one row's absolute path, or the current directory from background.
Open Terminal Here opens the selected folder, or the current directory.

A shared libos64 launch helper reads extension-to-executable mappings. Values
are absolute executable paths, not shell commands; spaces are literal. The
file is passed as one argv argument. Keys are final extensions including the
dot, case insensitive; last duplicate wins. Dotfiles without another dot and
extensionless files have no association. Empty values disable a mapping.
Malformed, oversized or unreadable configuration produces an error before
spawning. The config dialect reserves # for comments and trims whitespace.

`roam.conf` selects `associations`, either an absolute path or a bare config
name resolved through the system config search path. Default:
`file-associations.conf`. No fixed home path. Settings are reread for each Open.
Shipped text mappings target /bin/scribe. Open With bypasses mappings.

Roam snapshots an owned path after checking the directory generation and row
identity. Dialog creation/retirement happens between dispatcher turns. Parent
controls are disabled while the picker owns that path; cancellation never
launches. Failed destruction retains dialog state for retry.

Gterm gains `--directory PATH` before its optional program argv, so terminal
launches do not mutate Roam's process cwd or interpolate a shell command.
Roam reaps finished children on the UI thread. With live launched children it
uses bounded polling (sleep on idle turns); without them it uses the existing
blocking GUI wait. Os64 has no SIGCHLD or timed any-window wait. No extra
worker thread is introduced for child collection.

Shared interfaces/files: new os64/launch.h and launch.c, libos64 build list;
Roam consumer; gterm startup arguments; two shipped configs and image recipes.

## Acceptance

[Host and QEMU evidence](evidence/e05c/README.md): ASan/UBSan/LSan passed;
cross-build passed; native file launch, chooser cancellation/selection, clipboard,
terminal cwd, child-process collection and live custom configuration passed.
Independent review and P5 appearance/hardware acceptance remain pending.

## Handoff

Next: independent review and P5 acceptance of PR #325. Retarget to userland
after #322 merges; merge remains pending. Thread lifetime remains a separate
slice, including the known dialog close delay and exited worker retention.
