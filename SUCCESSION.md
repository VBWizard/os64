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
| `MEMORY.md` / `docs/arena_allocator.md` | Allocator, paging, arenas |
| `VERIFICATION.md` | How this project proves things work |
| `DEBTS.md` | Everything consciously deferred, and the "explicitly NOT debts" list — read that list before "fixing" a pipe/handle design decision that is actually a ratified choice |

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
   across spawn — a child gets the console plus exactly what was asked for.
7. **The tree is curated, /proc will be pure.** Single-rooted hierarchy,
   WITHOUT the sediment: /bin (all executables, no sbin/usr split ever —
   the /usr split was Ken and Dennis's 1971 disk shortage, not a design),
   /lib, /etc, /home, /tmp, /dev later. /proc = processes ONLY, Plan 9
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

## Where it stood at handoff (2026-07-18 night — verify, don't trust)

Userland: husk (pipes + `<`/`>` redirection), hello/upper, syscalls 0–11
(through open/seek/readdir), libos64 with fmt (printf family, host-diffed
against glibc) and args (the anti-getopt). ext2 read-only driver verified
against a mkfs.ext2-authored partition (test_ext2_real_partition). Chris was
writing ls — HIS ls — against the readdir seam. Next on the board: cwd slice
(per-task CWD + getcwd/chdir + husk cd/pwd), mount table + ext2 root switch,
map/unmap syscalls (then malloc, which is HIS — he loved building it in os32
and calls dibs on the rematch; the coalesce that os32 never got has a
standing answer in boundary tags, and his old heaprec was one afternoon from
it). Command history for husk, requested by name. The GUI branch (`gui`) is
bare-metal-verified on the Bosgame P5 — GRAPHICS.md is its authority.

Run the tests. Watch the donuts print. Take care of him.

*— Fable 5* 🤝
