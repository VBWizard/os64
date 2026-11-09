# Routine log routing — 2026-09-25

Frame Studio sends lifecycle and successful operation diagnostics to
`os64_debug_log`. Existing `os64_complain` errors remain on stderr and in the
log. The startup-save result uses `os64_complain` on failure and the system
log alone on success. Editor status text remains in the window.

Appearance Workshop already uses `os64_debug_log` for its layout diagnostic
and shared font-settings diagnostics; its errors use `os64_complain`.
No Workshop logging change was needed.

Validation: strict full build, `git diff --check` and `tools/stale_refs.sh`
passed. In a private QEMU GUI boot, Frame Studio launched, applied its draft,
and closed with zero bytes in redirected stdout. Its ready, Apply OK and
closed records appeared as `[user] framestudio:` in the kernel log. Workshop
also launched and closed with zero stdout bytes; its interface-row diagnostic
appeared as `[user] appearance:` in the kernel log. This boot used the serial
log sink; logd file routing was established by source inspection, not a
second guest run. Error paths were inspected, not fault-injected.

P5 follow-up: Chris confirmed that reopening and using Frame Studio no
longer writes routine messages to the console. The installed executable,
worktree build, and a GET from the running update server matched byte for
byte (124,784 bytes, CRC32 `51b72415`). Installed libos64 also matched the
worktree build (712,328 bytes, CRC32 `2ec5632f`). These deployment checks and
user feedback are distinct from independent review.
