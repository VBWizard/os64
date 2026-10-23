# E5C — Roam file actions

## Status and ownership

Accepted, 2026-10-23. PR #325 merged into userland as `12dc6b64`.
Codex review of final head `20dcf0ec` completed without findings; Chris reports
the latest Open With refinement works beautifully on the P5. No installed
binary hash was captured. Quinn's implementation and refinement records remain
below; [E5D window placement](E05D_WINDOW_PLACEMENT.md) is the next active slice.
No kernel or syscall changes were made by E5C.

## Outcome and contract

Double-click/Enter and Open launch a file's associated application. Open With
selects an executable using a compact shared application picker, initially at
/bin; unknown extensions take that same path. The optional, initially unchecked
“Remember for this file type” checkbox saves the final extension after a
successful launch. Without it, the choice applies to this launch. Copy Full
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
Chris reports that all initial additions work on the P5. His follow-up requests
were association persistence and a compact Open With browser. The refinement
passes host and native checks; final independent review and user-reported P5
acceptance are complete. See [refinement evidence](evidence/e05c/open-with/README.md).

Open With refinement implementation `2800bef2` is pushed to PR #325.

## Handoff

Next: E5D reusable remembered window placement for Roam and Scribe.
Thread lifetime remains a separate slice, including the known dialog close
delay and exited worker retention. File attributes and direct executable
activation are deferred at Chris’s request.

## Open With refinement — 2026-10-21

Application mode shows navigation buttons, location and the directory list,
with the remember checkbox and Open/Cancel controls below it. Navigation errors
and incomplete-listing notices remain visible when needed. Normal
Open/Save dialogs retain their controls. Up and Back provide parent navigation.

Saving uses the selected `roam.conf` association configuration. Bare names
read the current search-path file and publish a merged copy at the top of that
ladder, preserving shipped mappings on the first save. Absolute paths update
that file directly. The source must be readable and valid; a missing, malformed
or unreadable source is not replaced with a guessed empty file. Publication
uses the checked temporary-file/sync/atomic-rename writer. Concurrent complete
saves can still supersede each other; this is not a serialized transaction.

Cancellation, an unchecked box and failed launches do not write associations.
Extensionless files have no remember option. Unrepresentable config keys/values
are refused; if persistence fails after launch, Roam reports that distinction.
