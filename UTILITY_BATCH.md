# Userland utility batch

Branch: `codex/userland-utils`, based on `userland` at `ac8a8952`.

## killall

`killall NAME...` sends SIGKILL to live user tasks whose `/proc/<pid>/status`
name equals an operand, case-sensitively. Names are executable basenames,
without paths or command arguments. `--substring` selects literal substring
matching; it does not interpret glob patterns or regular expressions.

Examples:

```
killall clock
killall --substring clock
killall -s INT clock
killall -9 clock calculator
```

Signals and the default follow os64 `kill`: `2`, `INT`, `SIGINT`, `9`, `KILL`,
and `SIGKILL` (signal names ignore ASCII case). `-2` and `-9` are accepted;
`-l` lists supported signals. `--` ends option parsing. Empty name operands
are rejected before any signals are sent.

The caller, kernel tasks, and zombie/none-state tasks are excluded. Overlapping
or duplicate operands send one signal per selected task. Exit status is 0 if
each operand had a successful signal and no signaling or directory error
occurred, 1 for operational failure or an operand with no successful signal,
and 2 for invalid usage. Success reports command delivery, not confirmed exit.

Enumeration walks `/proc` by PID without a fixed task-count limit. It reads
status names directly because the shared display reader truncates at 63 bytes.
Incomplete or unreadable status reports are skipped, including tasks that
exit during enumeration. Names and task lifetime can change between reading
status and writing ctl; this is not an atomic selection-and-signal operation.

## Validation

- Strict cross-build: `make -C userland "$PWD/userland/bin/killall"`.
- Host behavior and fault injection, with ASan/UBSan:
  `tools/test_killall_host.sh`.
- Guest exercise: run `tools/test_killall_guest.husk` with `husk` in a
  disposable QEMU instance. It records task listings and exit codes in
  `/home/killall-*.txt`.

Validated on 2026-09-24: strict cross-build, ASan/UBSan host tests,
`git diff --check`, and `tools/stale_refs.sh` passed. The generated ELF was
installed into a private copy of the existing guest image and compared
byte-for-byte before boot. No kernel or main-checkout image rebuild was used.

QEMU evidence under `/tmp/os64-killall-results`: two live `ka-nap` tasks and
one `ka-nap-extra` became zero and one after exact matching, then zero and
zero after substring matching. A fresh `ka-nap` exited after SIGINT. Exited
children remained visible as zombies, so the assertions counted live states.
Recorded exit codes: exact 0, absent 1, substring 0, interrupt 0, self-only 1,
unsupported signal 2. The disposable VM was stopped after collection.

The killall utility is published independently; additional utilities can follow separately.

## P5 hardware validation (2026-09-24)

Installed through SSH to `192.168.137.246` using `os64get yogi killall`.
The installed size (20,712 bytes) and transfer CRC32 (`8f0a2279`) matched the
worktree binary. `killall clock` returned 0 and changed all three live clock
tasks to zombies, while the existing `gclock` remained live.

Controlled checks passed: exact matching terminated two `ka-p5-nap` tasks
while preserving `ka-p5-nap-extra`; substring matching then terminated the
latter. SIGINT terminated a fresh task. Absent/self-only selections returned
1 and unsupported SIGTERM returned 2. Two fresh Scribe processes were live
before `killall scribe`, which returned 0; both became zombies afterward.
These GUI checks establish process termination, not visual window inspection.

Temporary task binaries and their directory were removed. SSH, the compositor,
and the existing gclock remained live; no reboot was needed. Transcripts are
in `/tmp/os64-killall-p5-*.txt` on the build host. Test orchestration needed
short SSH requests and husk's `&;` separators; inherited background output
handles kept one SSH request open until the client timeout. The final tests
completed normally. Directory cleanup used `rm -r` because the P5 has no
standalone `rmdir` utility.

Chris also tested killall on the P5 and confirmed that `--substring clock`
terminated his gclock (user-reported hardware validation).
