# os64 Verification Handbook

*Written for the agent — human or model — who must prove a change works
without trusting it. Code that compiles is a rumor; a green test line is a
claim; a screenshot of a dragged window is a fact. This file is the craft
that made past verification runs work, including the parts that only lived
in session notes before. When you change the kernel, climb the ladder below
as far as the change warrants — and when you invent a new technique, add it
here, because your scratchpad dies with your session.*

## The ladder

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
| `make` | Build kernel + 512MB disk image + ISO (ISO also lands at /mnt/c/temp for VBox) |
| `make run` | QEMU BIOS boot, **windowed** — the human's mode |
| `make run-uefi` | Same, OVMF/UEFI |
| `make debug` | QEMU + GDB server :1234, waits (`-S -s`); `gdb kernel/bin/os64_kernel` → `target remote :1234`. QEMU's gdbstub breakpoints are linear-address based — no int3 patching, higher-half addresses work directly |
| `make -C kernel` | Kernel only (seconds) — but see the clean-build rule |
| `make vbox-sync` | dd the disk image into the VBox fixed VDI — **VM must be powered off**; capacity/type guards refuse a stale VDI |

**Clean-build rule:** the kernel makefile's `-MMD` dependency tracking is
broken (no .d files are generated). After ANY header change, `make -C
kernel clean` first, or you will debug a ghost built from stale objects.
When in doubt, clean — the build is seconds.

Default QEMU config: 8GB RAM, `-smp 4`, serial → `qemu_com1.log`, monitor
on telnet 127.0.0.1:55555, NVMe disk from `disk/os64.img`.

## Headless QEMU (the agent's mode)

A windowed QEMU steals the human's keyboard focus mid-typing. Agent runs
are headless, with serial going somewhere session-private:

```bash
qemu-system-x86_64 <base flags> \
  -display none -serial file:$SCRATCH/run_com1.log \
  -monitor telnet:127.0.0.1:55555,server,nowait   # keep monitor if driving input/screenshots; -monitor none otherwise
```

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
- **Boot lands in the wrong Limine entry:** default_entry edit didn't make
  it into the ISO (forgot the rebuild) or the menu timeout raced you.
- **Serial log stays empty for ages under WSL2:** the slow-walk, not a
  hang — watch for growth, not instant content.
- **No serial output ever + no QEMU error:** wrong `-serial` path, or the
  kernel died pre-serial-init, or a silent `cli; hlt` (see MEMORY.md's OOM
  limitation).
- **Ghost bugs that survive your fix:** stale objects — the clean-build
  rule.
