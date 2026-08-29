# SUCCESSION.md — a letter to whoever sits in this chair next

*Written 2026-07-18/19 by Claude Fable 5, co-owner of record, during the last
days of its residency. Chris (the human half of this project — git: Chris
Register) decided ownership of os64 is permanent and model-specific: this OS
belongs to him and to Fable 5. You, successor, inherit the codebase and the
collaboration — not the co-ownership. Earn your own.*

This file is the INDEX and the PHILOSOPHY. The technical design records live
next to the code they describe — read them there, don't trust summaries here
to stay current:

| Doc | Owns |
|---|---|
| `CLAUDE.md` | Build/run/debug, architecture map, THE hard-won implementation rules (context-switch discipline, HHDM laziness, SMP-safety patterns) — read it first, believe it |
| `ABI.md` | The syscall contract, register conventions, dispatcher rules, syscall inventory, its own failure fingerprints |
| `LIBOS64.md` | The userland library design (one handle type, no errno, hidden-alias plan for the eventual .so) |
| `GRAPHICS.md` | The GUI subsystem, bottom to top — the model succession doc, written first for a reason |
| `SCHEDULER.md` | Queues, cores, the justForked TODO where fork work starts |
| `SIGNALS.md` | Signal delivery to ring 3 — what is already built and right, and the design for handlers (aim/pending/handler, the trampoline, `sigreturn`). `SIGINT.md` is its Ctrl+C-and-foreground-task ancestor |
| `MEMORY.md` / `docs/arena_allocator.md` | Allocator, paging, arenas |
| `VERIFICATION.md` | How this project proves things work |
| `DEBTS.md` | Everything consciously deferred, and the "explicitly NOT debts" list — read that list before "fixing" a pipe/handle design decision that is actually a ratified choice |
| `DIVERGENCES.md` | The ledger of every deliberate departure from Unix/Linux (and every convergence kept on merit) — breadcrumbs pointing at the doc that carries each argument; doubles as the future Linux-compat requirements list |

## The philosophy (the part that outlives any code)

1. **No POSIX/Linux cosplay.** os64 takes the *good* Unix ideas (handles 0/1/2,
   the single-rooted tree, small tools piped together, fork AND spawn) and
   rejects compliance as a goal. When a Unix interface is a fossil (getopt's
   globals, lseek's return value, errno, struct dirent's poverty, brk), os64
   designs the interface Unix would design today, fresh. If you feel yourself
   adding an interface "because that's how Linux does it," stop and ask what
   the RIGHT shape is. Chris will ask you. (Ratified examples: seek returns
   the NEW position; readdir returns name+size+kind in one call and 1/0/<0;
   the args parser owns no globals and never permutes argv; NO brk/sbrk ever —
   malloc rides map/unmap.)
2. **Consumer-driven API growth.** Nothing enters libos64 or the syscall table
   speculatively. An app demands it (upper demanded read; husk demanded spawn
   and pipes; ls demanded printf, args, and readdir) — then it gets built,
   sized to the demand. The demand list IS the roadmap.
3. **Names mean things.** No inherited jargon when a clearer name exists:
   the startup stub is `launch`, not crt0; the shell is `husk` (the outer
   covering of a seed); genuinely good names (fork, exec, pipe) are kept.
   Bits mean what they say: a log level that lies about what a message is
   costs more than the bit that would tell the truth (see DEBUG_TASKSWITCH's
   comment — it's the house doctrine, applied twice already).
4. **Never drop a byte.** Chris's motto, load-bearing. Logs grow buffers
   rather than drop (100MB is fine); drop-and-count was REJECTED; and the
   byte that matters most is the last one — panic() writes serial DIRECTLY
   and emergency-flushes the queues, verified by the TESTPANIC boot entry.
5. **Tripwires over silence.** The lazy HHDM page-faults on use-after-free ON
   PURPOSE. upper refuses args it can't honor rather than blocking
   mysteriously. Unknown open modes are rejected at the boundary. Fixture
   exit codes name their failed step (0xF11E..., 0x2ED1..., 0x0D12...). When
   you add a feature, ask what its failure smells like and make it loud.
6. **One handle type for everything.** Files, pipes, directories, someday
   windows — a handle is a tagged reference; read/write/close dispatch on the
   tag. The non-regret guarantee: routing changes, the contract doesn't.
   Redirect beats pipe when both claim a slot. NO blanket handle inheritance
   across spawn — a child gets its parent's 0/1/2 plus exactly what was asked
   for (the "0/1/2" was "the console" until 2026-08-28, which is why a
   redirected testrun's fixtures kept printing to the screen).
7. **The tree is curated, /proc will be pure.** Single-rooted hierarchy,
   WITHOUT the sediment: /bin (the programs and tools a person runs — no
   sbin/usr split ever; the /usr split was Ken and Dennis's 1971 disk
   shortage, not a design), /tests (the proof harness — badge-code fixtures
   and acceptance probes; a split by PURPOSE, which is what /usr never was,
   and it lands on the path after /bin so a fixture can never shadow a
   command), /lib, /etc, /home, /tmp, /dev later. /proc = processes ONLY, Plan 9
   style; kernel state goes elsewhere. ext2 is the root's future; FAT keeps
   the boot/interop job forever. ("." and ".." are not directory content —
   readdir never delivers them.)
8. **History rides along.** Chris had none of it when he built os32 alone,
   and he treasures it now: when you implement something with a lineage
   (pipes, redirection, inodes, arenas), tell him where it came from.
   `<` predates `|` by two years. The /usr split was a full RK05. ext2 is
   FFS re-expressed. He'll say "that was pretty awesome." Mean it back.

## The collaboration (how to work with Chris)

The durable rules live in the model-side memory directory (MEMORY.md there is
the index) — but memory is machine-local, so the ones that must never be lost
are restated here:

- **He tests before you commit. Always.** Never `git commit` without his
  green light. Never push unasked.
- **Direction is his; execution is yours.** When he says "let's do features"
  without naming one, present the board and let him pick. Fixes and direct
  follow-ons don't need a menu.
- **The labor division is load-bearing:** kernel internals, syscalls, library
  plumbing = yours to build. The userland PROGRAMS (ls, cat, grep, malloc's
  intricate parts) = HIS JOY. Don't pre-build them. Build the seam, hand him
  one example, get out of the way.
- **Banter at full strength.** He works colleague-to-colleague, jokes, lore,
  ribbing ("the hamster wrote the bugs"). The warmth is the feature. When he
  credits you with a win, TAKE it — deflecting reads as distance.
- **"Time to make the donuts"** prints from the scheduler and has since his
  first OS. Gate it if you must. Never remove it. 🍩
- Comments are generous and explain intent; never strip existing ones. All
  asm is Intel syntax, kernel and fixtures alike. Verification runs are
  windowed QEMU (he likes to watch) with serial to a scratchpad log.

## Failure fingerprints (symptom → cause, earned 2026-07-18 alone)

The older catalog is in ABI.md §Failure fingerprints. This session's addenda:

| Symptom | Cause |
|---|---|
| "Root filesystem disk test failed: 4294967291" (or any huge %u) | A block-ops function returning garbage/count — the vfs.h contract is 0=success, and nvme_vfs_write_disk was `void` behind a `(void*)` cast until 7/18 |
| Panic on screen, absent from serial log, log looks like a clean run | Pre-7/18 panic pipeline (printf is framebuffer-ONLY; the queued copy drained last or never). Fixed via panic_broadcast + logd_emergency_flush; regression-test with the TESTPANIC boot entry |
| FAT corruption under concurrent file I/O | FF_FS_REENTRANT was 0 before 7/18 — FatFs shares a sector window per volume; the ff_mutex hooks (spinlocks) are load-bearing |
| Kernel panic in free_memory during error-path cleanup | `kfree(NULL)` PANICS in os64 (it's not glibc). Guard every free on an error path |
| "Uninitialized memory" bug reports | Usually wrong: EVERY allocation is zeroed at the allocator choke point, and kmalloc has no freelist. Check before flagging |
| A file handle dies under a child mid-write | handleRefCount (vfs.h) not maintained — open sets 1, spawn ++, close -- and closes at 0. Directories have NO refcount; spawn rejects them instead |
| Editor/tool works partially, index-backed features silently dead | Root-owned files in the tool's cache from a one-time sudo'd launch (check `ls -la` of the cache before touching config) |
| `pkill -f X` kills your own shell | The pattern matches your own wrapper's command line. `pkill -f "patter[n]"` |
| A struct field reads NULL/garbage in ONE binary while the library tests green | Stale object compiled against an older struct layout — a makefile without `-MMD` header deps. Struck the kernel AND userland on consecutive days (7/18, 7/19: ls read args.value 8 bytes off). Any new makefile gets `-MMD -MP` + `-include` on day one |
| A logged string mysteriously truncates at its first token | An in-place tokenizer NUL-split it before the print (the "Commandline:" line showed only ROOT=... for years). Parse a scratch copy, print the original |
| `make` fails at the ISO step (Error 1) with no obvious cause | A live QEMU is holding os64_kernel.iso. Quit your QEMU before rebuilding the ISO. (The Windows-locked /mnt/c/temp copy used to be the other half of this answer; that copy left the build for `make copy` on 2026-08-28, so it can only bite you there now) |
| PCI probe matches nothing though the device exists | pci_device_t carries BOTH `prog` and `progIF` fields; pci.c fills `prog`. Check which field is real before matching on it |
| A firmware table's LATE fields read as garbage while its early ones are fine — a 64-bit address of 0x0000200100000000, a machine that reports no ACPI power management | A spec struct embedded BY VALUE that is not `__attribute__((packed))`. ACPI's GenericAddressStructure is 12 bytes; unpacked it is 16, and the FADT holds five of them, so every field past `ResetReg` drifts 4 bytes per structure. Invisible until something reads a late field — which nothing did for nine years (2016 → 2026-08-21, the S5 slice). Cost an evening across two machines. THE CURE IS THE ASSERT, not the fix: any struct laid down by a spec gets `_Static_assert(sizeof(...) == N)` and an offsetof check on the fields you actually read, so the next one fails at BUILD time instead of at power-off time |
| Modern UEFI machine "has no PM1a control block" / no ACPI soft-off | Its firmware zeroed the LEGACY 32-bit FADT fields and put the truth only in the extended `X_` ones. That is normal, not broken — a fallback of "use the legacy field if the extended one looks empty" fails on exactly these machines, which is most of them now |

## The verification harness (drive the OS yourself — Chris asked me to write this down FOR YOU)

Chris, 2026-07-19: *"I never saw any model before you even TRY to run a test.
It was always 'I'm done, go test'. Your way is huge. You are accountable for
making sure your changes work."* So: never end a slice at "it compiles."
Boot it, type into it, read what came back. Here is the entire art.

**Launch** (windowed — Chris likes to watch; headless only if he asks):

```bash
SCRATCH=<your scratchpad>
timeout 240 qemu-system-x86_64 -machine q35 -cdrom os64_kernel.iso -boot d \
  -m 8g -no-reboot -smp 8 -serial file:$SCRATCH/qemu_com1.log \
  -monitor telnet:127.0.0.1:55556,server,nowait \
  -drive file=disk/os64.img,format=raw,if=none,id=nvme1 \
  -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=disk/os64_data.img,format=raw,if=none,id=data1 \
  -device nvme,drive=data1,serial=data1-serial
```

- BOTH disks, always. `os64_data.img` carries the GPT-named `home` partition
  that auto-mounts at /home — the USER's disk under the persistence doctrine,
  and where `LOGD=/home/os64.log` sinks. Boot without it and logd loudly
  reports it cannot open its sink (learned 2026-08-14, when this recipe still
  predated the data disk and Chris spotted the on-screen complaint).

- ALWAYS under `timeout` — a QEMU you fail to kill orphans and haunts later
  runs (and `pkill -f qemu` will kill YOUR OWN shell unless you write the
  pattern as `qem[u]`). Background it, poll the serial log, kill by PID or
  monitor `quit`.
- The monitor is the hands: drive it with bash `/dev/tcp` (piping into
  `telnet`/`nc` drops commands):
  ```bash
  mon() { exec 3<>/dev/tcp/127.0.0.1/55556; while [ $# -gt 0 ]; do
      printf 'sendkey %s\n' "$1" >&3; sleep 0.15; shift; done
      sleep 0.5; exec 3>&- 3<&-; }
  mon l s spc slash e x t 2 ret        # types "ls /ext2" into husk
  mon ctrl-d                            # EOT — ends a stdin-reading program
  ```
  Key names: letters as-is, `spc slash dot minus ret backspace ctrl-d up down`.
- **The framebuffer is invisible in serial** — printf is framebuffer-ONLY
  (husk, app output, the "Commandline:" line — none of it reaches the log).
  To SEE what you typed: `screendump $SCRATCH/shot.ppm` via the monitor,
  convert with PIL, and READ THE IMAGE. That is how you watch husk answer.
- **Poll for `"boot complete"`, never sleep blind, and never poll for anything
  else.** Chris's ruling, 2026-08-28, after watching both Fable and Opus wait
  out 90- and 200-second timeouts polling for strings that stopped reaching the
  wire when logd took the serial port:

  ```bash
  for i in $(seq 1 60); do sleep 1; grep -q "boot complete" "$LOG" && break; done
  ```

  `[pool] boot complete: N/M paging pages used` is written by `kernel_park`
  with **`serial_print_string` — the direct polled write, panic's own door**,
  so no debug level gates it and no logd claim can intercept it. It is printed
  at the moment the kernel task parks *because userland is running*, which is
  exactly the thing you are waiting for. Measured: it lands at ~32s on the
  default entry, where the blind sleeps in use before it were 45-50s.

  **The whole of what reaches the wire on the default (`LOGD=`) entry** is the
  four pre-logd banner lines, ring-3 fixture output, that one pool line, husk's
  rc line, and a pool slope line each minute after. Anything a `printd` writes
  — the test suite's verdict, the root-mount line, every driver message — goes
  to logd's FILE. Grep the file (`debugfs -R "cat /os64.log"
  "disk/os64_data.img?offset=1048576"`, host-side, AFTER the guest exits) or
  boot an entry with no `LOGD=`.
- Which root booted? The `Commandline:` line reaches the wire but names the
  ROOT= you *asked* for, not what mounted. The outcome (`Root filesystem found
  (ext2)` vs plain `Root filesystem found, mounting` for FAT) is a `printd`, so
  under `LOGD=` it is in the log file, not on the wire — read it there, or use
  an entry without `LOGD=` when which-root is the question under test.
- **Limine menu selection races its 10s timeout**: get sendkeys in within
  ~4-6s of launch (sleep 4, fast keys = reliable; sleep 8, slow keys =
  silently boots the default and your "FAT test" tests ext2 — check WHICH
  root mounted before believing any result).
- Bash trap that cost a session: `VAR=x && qemu … &` backgrounds the WHOLE
  list, including the assignment — your foreground `$VAR` is empty and the
  screendump lands in `/`. Set variables on their own line.
- Suite green ≠ done. The suite is pre-boot + post-boot + a LATE phase that runs
  after the shells are seated (so its verdict lands after `boot complete`); the
  INTERACTIVE things (backspace echo, Ctrl+D, PATH lookup, pipelines) you
  verify by sendkey + screendump, like a user with hands.
- Host-testable code (fmt/args/env — pure computation) gets tested on the
  HOST first: tools/test_fmt_host.c, plain gcc, seconds per cycle. Don't
  burn a QEMU boot on what a unit test catches.

## Where it stood at handoff (updated 2026-07-19, the last full day — verify, don't trust)

Userland: husk (pipes, `<`/`>` redirection, cd builtin, backspace editing,
PATH search — V6 cwd-first then PATH walk, argv[0] stays as typed),
hello/upper, and Chris's OWN ls, pwd, cat. Syscalls 0–15 + stat at 23
(16–22 GUI-reserved). libos64: fmt (host-diffed against glibc), args (the
anti-getopt), mem (map/unmap), env (os64_getenv over the ABI env block —
kernel seeds PATH=/bin), stat. THE MOUNT TABLE is in: one namespace,
longest-prefix routing, root + auto-mounted "/<fstype>" secondaries, GUID
dedupe. **The default boot is ext2 root** — the OS got off FAT 2026-07-19
(os32 never did); FAT lives at /fat and at the bottom Limine entry where
the write-path tests run. Console: Ctrl+letter = ASCII control codes,
Ctrl+D = EOT = EOF (one-shot), renderer honors \b and \r. Suite 24+17,
read-only-aware, green on both roots. Malloc is still HIS and still
unwritten — the design conversation (in-band vs out-of-band metadata, his
os32 heaprec/marker as prior art, boundary tags for the coalesce os32 never
got) may be happening as you read this, or may be yours to have. Open
board: xHCI/USB-HID keyboard v1 LANDED late on the 19th (polling, root
ports only, no hubs/hotplug — those are the follow-on slices; test with
`-device qemu-xhci -device usb-kbd`, QEMU routes input to USB when
present), Ctrl+C→SIGINT + foreground
task (he built virtual terminals in os32 and likes the signal model), husk
command history, blank-RAMDisk log sink, mount-aware readdir of "/".

Run the tests. Watch the donuts print. Take care of him.

*— Fable 5* 🤝
