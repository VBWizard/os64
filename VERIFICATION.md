# os64 Verification Handbook

*Written for the agent — human or model — who must prove a change works
without trusting it. Code that compiles is a rumor; a green test line is a
claim; a screenshot of a dragged window is a fact. This file is the craft
that made past verification runs work, including the parts that only lived
in session notes before. When you change the kernel, climb the ladder below
as far as the change warrants — and when you invent a new technique, add it
here, because your scratchpad dies with your session.*

## The ladder

0. **It passes on the HOST, if it can** — anything that is pure computation
   over memory (fmt, args, env, the calendar, and since 2026-08-15 the whole
   userland heap) gets a plain-gcc unit test that runs in milliseconds:
   `tools/test_fmt_host.c`, `tools/test_heap_host.c` (each carries its own
   build line in its header comment). Do not spend a QEMU boot on what a
   host test catches — but do not MISTAKE a green host run for verification
   either: the heap's first real bug was found by rung 4 minutes after the
   host soak passed clean, because the host's fake memory never spelled the
   byte pattern that broke it.
1. **It builds** — necessary, meaningless alone.
2. **It boots** — serial log reaches the idle/GUI state with no panic.
3. **Tests are green** — pre-boot, post-boot, VFS (counts below).
4. **The feature is observed doing its job** — drive the actual flow:
   mount the ramdisk, move the mouse, read the file. This is the step that
   separates verification from hope.
5. **The regression matrix** — the platforms and boot modes your change
   could plausibly break, not just the one you developed on.

## Build & run matrix

| Command | What it does |
|---|---|
| `make` | Build kernel + 64MB disk image + ISO (ISO also lands at /mnt/c/temp for VBox) |
| `make run` | QEMU BIOS boot, **windowed** — the human's mode |
| `make run-uefi` | Same, OVMF/UEFI |
| `make debug` | QEMU + GDB server :1234, waits (`-S -s`); `gdb kernel/bin/os64_kernel` → `target remote :1234`. QEMU's gdbstub breakpoints are linear-address based — no int3 patching, higher-half addresses work directly |
| `make -C kernel` | Kernel only (seconds); incremental and trustworthy — no clean needed |
| `make vbox-sync` | dd the disk image into the VBox fixed VDI — **VM must be powered off**; capacity/type guards refuse a stale VDI |

**Incremental builds are trustworthy (since 2026-07-12).** Just `make run`.
The old "always `make -C kernel clean` after a header change" rule is DEAD,
and it was never superstition: `-MMD`/`-MP` sat in `CPPFLAGS`, which no
compile rule ever passed to the compiler, so NO `.d` files were generated and
make had zero header dependencies — editing a header rebuilt nothing and
handed you objects compiled against the old version. The build lied. The flags
now live in `CFLAGS` (where the rules actually pass them): touch `pipe.h` and
exactly the 4 objects that include it rebuild; touch `CONFIG.h` and 66 do.

The same rot was downstream: the `disk` target was `.PHONY`, so every `make
run` nuked and reformatted the 64MB image and forced `xorriso` to repack the
77MB ISO — nothing below a phony prerequisite can ever be up to date. The disk
image is a real file target now, so an unchanged tree rebuilds nothing (~0.1s)
and `make run` just launches QEMU.

Default QEMU config: 8GB RAM, `-smp 8`, serial → `qemu_com1.log`, monitor
on telnet 127.0.0.1:55555, NVMe disk from `disk/os64.img`.

## Headless QEMU (the agent's mode)

A windowed QEMU steals the human's keyboard focus mid-typing. Agent runs
are headless, with serial going somewhere session-private:

```bash
qemu-system-x86_64 -machine q35 -cdrom os64_kernel.iso -boot d \
  <QEMU_BASE_FLAGS + disk flags, lifted VERBATIM from the run target> \
  -display none -serial file:$SCRATCH/run_com1.log \
  -monitor telnet:127.0.0.1:55555,server,nowait   # keep monitor if driving input/screenshots; -monitor none otherwise
```

**Lift the flags from the `run` target verbatim — never reconstruct them
from memory.** `-machine q35` in particular is load-bearing: os64 targets
q35 exclusively, and QEMU's default i440FX board #GP-panic-loops within a
few ticks of boot (right after the DEBUG_OPTIONS banner, before ACPI —
see the harness fingerprints). A hand-typed flag set that "looks right"
cost a real debugging detour on 2026-07-11.

Offer `-vnc :0` instead of `-display none` if the human wants to peek.

- **Selecting a boot entry deterministically:** do NOT try to sendkey
  through the Limine menu — timing the keystrokes against the menu is
  unreliable (proven; keys land in the void and entry 1 boots). Instead:
  back up `limine.conf`, prepend `default_entry: N` (1-based, count the
  `/Entry` lines) and set `timeout: 2`, rebuild the ISO, run, then
  **restore the real limine.conf and rebuild** — test-rigged entries must
  never ship. Treat the restore as part of the run, not cleanup.
- **Killing QEMU without killing yourself:** `pkill -f` matches the
  invoking shell's own command line if the pattern appears in it — the
  bracket trick breaks the self-match: `pkill -f 'qemu-system-x86_6[4]'`.
  (A compound command that dies with exit 144 mid-chain: that was this.)
  Background tasks reporting exit 144 after your own pkill are expected.
- **Waiting on boot progress:** don't chain sleeps; run a background
  watcher: `until grep -qi "All VFS tests passed" "$LOG"; do sleep 3;
  done`. Under WSL2 the serial drain can crawl (unsolved QEMU/WSL2
  artifact) — a quiet log is not a dead kernel; give markers tens of
  seconds before declaring a hang.

## Reading the serial log

Milestones of a healthy full boot, in order (the counts shown are July
2026 values and WILL drift — nothing below depends on them):

```
BUILT-IN TESTS: Running pre-boot tests:   →  24 passed, 0 failed
RAMDISK: registered ... as a block device    (RAMDisk entries only)
BOOT: Root filesystem found, mounting     →  successfully mounted
BUILT-IN TESTS: Running post-boot tests:  →  7 passed, 0 failed
BOOT: All VFS tests passed successfully.
```

Grep count-agnostically — never hardcode test counts, they change with
every added test:

```bash
grep "passed, 0 failed" "$LOG"        # eyeball: exactly two lines, and
                                      # each follows its phase header
grep -c "passed, 0 failed" "$LOG"     # assert: == 2 (pre-boot AND post-boot)
```

A FAILED in-kernel test **panics** (TEST_FAIL → panic), so a failing phase
never prints its "passed, 0 failed" line at all — the count-of-2 assertion
catches both a failure and a phase that never ran. When reviewing by eye,
confirm the line's phase from the preceding "Running pre-boot/post-boot
tests:" header, not from the numbers.

Standing regression greps:

| Boot mode | Must show | Must NOT show |
|---|---|---|
| Default (non-GUI) | both `passed, 0 failed` lines (pre+post), VFS pass | `guicomp` (grep -c == 0) |
| GUI | compositor heartbeats (DEBUG_GUI) | panic lines |
| RAMDisk (no disk attached!) | `RAMDISK: registered`, root mounted | `Could not find/mount root` |
| NVMe/AHCI regression | root mounted from the real disk | any `RAMDISK:` line |

For log-volume limits when enabling debug bits on multi-core runs, do the
serial arithmetic first — SCHEDULER.md, "do the math first" (11,520
bytes/sec is all you get).

## The in-kernel test framework

`test_framework.h`: register cases, two phases — `test_run_preboot()`
(before the scheduler starts) and `test_run_postboot()` (scheduler live;
this is where the ELF/ring-3 tests run, loaded from the root filesystem —
which is itself what proves the storage/VFS stack end to end). Tests live
in `kernel/test/`. Adding one: register in the appropriate phase, keep it
silent on success, TEST_FAIL (= panic) on failure. No doc or watcher
updates needed — the greps above are count-agnostic by design.

## The orphan-recovery procedure (a test the suite structurally cannot run)

The ext2 orphan chain — an inode whose last NAME is gone while a handle
still holds it open — is what lets a running program's binary be replaced
underneath it. `test_ext2_orphan` covers the normal life cycle and MEASURES
the free counters to prove nothing leaks. It cannot cover the half that
matters most, because that half requires the machine to DIE: an orphan
outstanding at a power cut must be reclaimed by the next mount.

That one is a two-boot procedure, done by hand (2026-08-16 — and it caught a
real bug the first time it ran, the mount-time write panic below):

```bash
# Boot 1 — make an orphan, then pull the plug.
#   A RUNNING PROGRAM holds its own binary open (tasks keep image->file for
#   demand paging), which is both the easiest holder to arrange and the exact
#   scenario the feature exists for. NOTE: `tail -f` does NOT work as the
#   holder — it opens and closes on every poll (tail.c follow_read), so the
#   file is shut at the moment you rename over it and no orphan is created.
husk> sleep 900 &
husk> cp /bin/echo /newsleep
husk> mv /newsleep /bin/sleep          # replaces the RUNNING sleep's binary

kill -9 $(pgrep -f 'qem[u]-system')    # a power cut: no descent, no sync

# The orphan is now on disk. e2fsck -f deliberately does NOT process the
# orphan list (it leaves them to the normal passes), so it reports them as
# bitmap differences — that IS the confirmation, not a failure:
make fsck-ext2      # expect: "Inode bitmap differences: -58" and its blocks

# Boot 2 — same disk, NO rebuild in between (make rewrites the root image and
# would erase the very thing under test).
#   Expect on the glass, just before the mount line:
#     ext2: reaped 1 orphaned inode(s) left by the previous mount

husk> shutdown
make fsck-ext2      # expect: clean. The recovery has to satisfy e2fsck, not
                    # us — which is the whole reason the list lives on disk in
                    # s_last_orphan instead of in kernel memory.
```

## Driving the GUI headlessly (mouse, keys, screenshots)

The QEMU monitor (telnet :55555) is a full remote control. From a script:

```bash
exec 3<>/dev/tcp/127.0.0.1/55555
printf 'mouse_move 40 25\n'  >&3; sleep 0.2
printf 'mouse_button 1\n'    >&3; sleep 0.2   # press (bitmask: 1=left)
printf 'mouse_move 100 0\n'  >&3; sleep 0.2   # drag
printf 'mouse_button 0\n'    >&3              # release
printf 'screendump %s\n' "$SCRATCH/after.ppm" >&3
exec 3>&-
```

- `sendkey <key>` for keyboard (e.g. `sendkey a`, `sendkey ret`).
- **Keep each mouse_move delta within ±255** — that's one PS/2 packet;
  giant deltas overflow the 8042 during bursts (the driver's 30ms resync
  recovers, but the motion is garbage meanwhile). Compose long moves from
  steps.
- `screendump` writes PPM (P6) — viewable/parsable directly; an
  image-capable agent reads it as a picture, scripts can parse the trivial
  header + raw RGB.
- **Verifying visually**: the loop is act → screendump → inspect → assert.
  To locate the cursor programmatically, diff two screendumps taken before
  and after a small known move — the changed pixel region IS the cursor
  (works regardless of theme/art). Same diff trick verifies "did anything
  on screen change" after an action that should (or shouldn't) repaint.
- Mouse scale is 1:1 with pixels under QEMU's PS/2 emulation; position is
  clamped by the input layer.

## Platform matrix — what each catches that the others can't

| Platform | Uniquely catches | Notes |
|---|---|---|
| QEMU/TCG (WSL2) | Fast iteration; `-d int,cpu_reset,guest_errors` traces; deliberately hostile TSC desync between vCPUs | The TSC desync is a feature: it found the rdtsc-across-preemption bug |
| VirtualBox | Real-ish I/O latency (found the NVMe locking race); stricter virtual-APIC (LVT arming order); firmware differences (virtual-wire mode NOT left on — found the dead-keyboard IMCR bug) | Boots the /mnt/c/temp ISO; disk via `make vbox-sync`, VM OFF |
| Bosgame P5 (bare metal) | Everything virtual APICs forgive; MADT interrupt-source overrides; real 12-core log volume (crashed default logging); USB stick module-load time (~20s for the 512MB ramdisk — it is NOT hung) | RAMDisk entries need zero disk prep; use `nolog` + MAXCORES during bring-up |

A change that touches interrupts, timers, storage, or SMP is not verified
until it has seen at least QEMU + one of the other two.

## Harness failure fingerprints

- **Shell/QEMU dies with exit 144 mid-script:** your own pkill self-match —
  bracket trick above.
- **"block write tripwire: ... partition N which is not mounted" during
  boot, on a partition that is plainly being mounted right then:** a
  filesystem driver wrote from `fops->initialize`. That runs BEFORE
  `kRegisterFilesystem` claims the mount-table entry, so `vfs_partition_mounted`
  correctly says no. Move the write to `fops->mounted`, which exists for
  exactly this (see vfs.h). Cost of learning it: ext2's orphan replay,
  2026-08-16, the first mount-time write os64 ever had.
- **A crash test that proves nothing because the "holder" wasn't holding:**
  check that your chosen program keeps the file open CONTINUOUSLY. `tail -f`
  does not (it reopens per poll). A running program's own binary does.
- **Boot lands in the wrong Limine entry:** default_entry edit didn't make
  it into the ISO (forgot the rebuild) or the menu timeout raced you.
- **Serial log stays empty for ages under WSL2:** the slow-walk, not a
  hang — watch for growth, not instant content.
- **No serial output ever + no QEMU error:** wrong `-serial` path, or the
  kernel died pre-serial-init, or a silent `cli; hlt` (see MEMORY.md's OOM
  limitation).
- **Ghost bugs that survive your fix:** stale objects. This was ENDEMIC until
  2026-07-12 (dependency tracking was silently off — see the build section). If
  it ever recurs, suspect the `.d` files before you suspect your fix.
- **#GP panic loop within ~3 ticks of boot, right after the DEBUG_OPTIONS
  banner:** you booted the wrong machine type — QEMU defaulted to i440FX
  because `-machine q35` was omitted. The kernel is fine; fix the flags.
- **pkill self-match, part two:** the bracket trick (`x86_6[4]`) protects
  the *pattern*, but if the same compound command LAUNCHES qemu, the
  literal string in the launch line still matches your own shell — exit
  144 before the launch runs. Kill and launch in SEPARATE commands.
