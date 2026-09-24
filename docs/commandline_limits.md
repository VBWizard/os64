# Command-line and spawn limits

The limits cover different data: a shell command is source text, a spawn
argument is one already-parsed string, and a path is a filesystem name.

| Boundary | Limit, including NUL unless noted |
|---|---|
| husk interactive, script, rc and `-c` input | 4096 bytes |
| SSH exec request | 4095 command bytes, no NUL on the wire |
| husk expanded command | 4608 bytes |
| One spawn argument | 128 KiB (`OS64_SPAWN_ARG_MAX`) |
| Whole child argv block, pointers included | 1 MiB (`TASK_ARGV_MAX_BYTES`) |
| Argument count | 512 |
| Path | 256 bytes (`OS64_PATH_MAX`) |
| Environment block | 64 KiB |

`userland/apps/husk/limits.h` supplies the shell input size to husk and sshd.
A compile-time assertion requires a shell command to fit one spawn argument:
SSH exec launches `/bin/husk -c <command>`.

## Spawn ownership and failure

The kernel measures the caller's argv before allocating scratch storage,
including the pointer slots and string terminators. Scratch size follows the
actual strings, so short commands do not pay for maximum-sized arguments.
The copy pass checks its bounds and copied terminators again because another
thread can change the caller's memory between passes. The snapshot need not
be atomic, but it cannot overrun its allocation or publish unterminated strings.

`OS64_SPAWN_TOO_LONG` (-3) reports an oversized argument, too many arguments,
or an oversized aggregate. Unreadable input and input that changes beyond
the measured storage are rejected as bad user data (-2). husk distinguishes
"argument list too long" from an executable that could not start.
Interpreter rewrites are measured again before environment inheritance or task
construction. Their additional strings and pointer slots count toward the same
limits, and an oversized rewrite returns -3 through the syscall unchanged.

`/proc/<pid>/cmdline` appends arguments in bounded chunks without placing a
whole argument on the kernel stack. Paths retain their independent 256-byte
limit. `echo -e` sizes decoded storage by the input argument, so longer
arguments are not truncated at the former shell limit. The path copier accepts
a terminator in the final byte: a 255-character path fits the 256-byte buffer.

The existing `os64_proc_info_t.command` field remains a 256-byte display summary
so older callers retain their allocation size and array stride. The separate
`os64_proc_command()` API returns allocated full command text, growing storage
as needed up to the 1 MiB argv window. It fails without returning a prefix on
read, allocation, close, or size errors. `ps -f` uses this API and writes command
text without printf's 1 KiB formatting limit, retrying partial writes. A failed
command read is marked `[command unavailable]`. `top` and `lsof` retain their
name-based displays.

## Shell storage

History is a 16 KiB byte ring of NUL-terminated records. It evicts complete
oldest records, suppresses consecutive duplicates, and holds four maximum
length lines or many short lines. A recalled record is copied into a line
buffer because it may straddle the ring boundary.

Command expansion uses libos64 reset-only arenas, one per substitution depth.
Expanded text, its syntax/data mask, captured output, substitution input and
assignment values belong to that depth's command lifetime. Starting another
line at that depth resets its arena. Nested substitutions use a different
arena, and their result remains valid until the parent consumes it. Retained
chunks are reused; each arena has a 256 KiB allocation budget. Allocation
failure refuses the affected command with a diagnostic. Persistent history
and collected control-flow blocks have separate storage lifetimes.

The original September 14 plan proposed a local mark/release arena. The
shared arena library now supplies reset/destroy semantics, so this feature
uses separate ownership scopes instead of adding rewind.

## Remaining independent limits

This change does not increase the eight command segments per line, four
pipeline stages, substitution depth three, or block depth eight. The line
editor remains byte-oriented. A line taller than its terminal may scroll
its beginning out of reach of the cursor model; the edit buffer stays
correct, but that picture limitation remains tracked in `DEBTS.md`.

## Verification

- `python3 tools/test_spawn_argv_host.py`: measurement/copy boundaries,
  inaccessible pages, mutations between passes, rewritten argv preflight,
  and path terminators at the final buffer byte or beside an inaccessible page.
- `tools/test_proc_command_host.sh`: full command reads through 1 MiB, short
  reads/writes, growth and I/O failures, and 128 KiB `ps -f` output under ASan/UBSan.
- `tools/test_husk_storage_host.sh`: history wrap/eviction, maximum records,
  nested arena ownership, reuse, and allocation failure under ASan/UBSan.
- `tools/test_echo_long_host.sh`: 128 KiB escaped arguments, short writes,
  allocation failure, and write failure under ASan/UBSan.
- `tools/test_ansi_host.sh`: editor and terminal model regression suite.
- `python3 tools/test_sshd_host.py`: OpenSSH integration under ASan/UBSan,
  including accepted 1000/4095-byte commands and refused 4096-byte requests.
- `/tests/argsize` through `testrun`: long argv/cmdline data, boundary-length
  arguments, 512 arguments, excess length/count/aggregate, bad pointers,
  repeated oversized shebang refusals, fitting scripts, full command reads,
  and 255-character path creation/removal.
- `tools/sshd_probe.py`: real guest SSH streams/status/rekey/PTY checks plus
  accepted long exec requests and rejection beyond the shell boundary.

The 4096-byte input and 16 KiB history ring follow Chris's September 15
choices. Validation on 2026-09-24: the full strict kernel/userland/image build and
listed host suites passed. On an isolated QEMU guest, `testrun argsize`
reported 1 passed, 0 failed; the SSH fixture reported 2393 checks, 0 failures.
The SSH probe passed 1000/4095-byte exec, refusal at 4096, nested long command
capture, escaped echo, long loop words, script and `-c` overflow rejection,
1000-byte interactive input/history recall, binary streams/rekeys, PTY resize,
and recovery after a dropped connection. Logs are in
`/tmp/os64-spawn-*.log` and `/tmp/os64-spawn-argsize-guest.txt` on the build host.
After Chris installed the updated components on the P5, hardware SSH checks
also passed: 1000- and 4095-byte commands returned their expected output
byte-for-byte, a 4096-byte request was rejected, and a fresh short SSH command
succeeded afterward. The hardware check covers the command-length boundary;
the larger spawn and shell regression suite above ran in QEMU. Its transcript
is `/tmp/os64-p5-long-command-retry.txt` on the build host.

## PR #132 review-fix validation (2026-09-24)

The strict kernel/userland/image build, both focused ASan/UBSan host suites,
whitespace check, and stale-reference audit passed. On the rebuilt eight-core
QEMU guest, the expanded `testrun argsize` suite reported 1 passed, 0 failed.
The guest SSH fixture reported 2393 checks, 0 failures; the integration probe
passed long exec limits, interactive recall, nested expansion, rekeys, streams,
and PTY resize. A live `ps -ef` capture preserved a command longer than 3 KiB,
including its final marker. These review fixes were tested on QEMU; the P5
results above describe the earlier revision.
