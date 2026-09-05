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
| `make` | Build kernel + userland + disk images + ISO. Everything that boots on this machine is ready after this |
| `make copy` | Hand the ISO to VirtualBox: copy it to /mnt/c/temp, where the VM boots it from. NOT part of `make` since 2026-08-28 — it was the slowest step of every build and the only thing needing it is a VM that is rarely opened now the P5 boots os64 itself. Fails loudly if /mnt/c/temp isn't there |
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
whole ISO — nothing below a phony prerequisite can ever be up to date. The disk
image is a real file target now, so an unchanged tree rebuilds nothing (~0.1s)
and `make run` just launches QEMU.

Default QEMU config: 8GB RAM, `-smp 8`, serial → `qemu_com1.log`, monitor
on telnet 127.0.0.1:55555, NVMe disk from `disk/os64.img`, plus the data disk
`disk/os64_data.img` (ext2, mounted at `/home`, where `LOGD=` writes).

**WHEN THE LOG ITSELF IS THE SUSPECT, read `/sys/log` before theorizing.** It
reports the sink (userland daemon / kernel-to-serial / retained-in-memory),
whether a serial port exists at all, the active format, and per-core
`used`/`lost`. On 2026-08-18 an hour went into "something must be stuck in the
per-core buffers" when every ring was empty and `printd` was discarding at the
filter — `cat /sys/log` answers that in one line. Second instrument, for a
guest that has already stopped talking: the QEMU monitor. `info registers -a`
showed all eight cores halted in `task_idle_loop` (which killed the
stuck-buffer theory outright), and `x/2xg <symbol>` against addresses from
`nm kernel/bin/os64_kernel` read `kDebugLevel` straight out of the running
machine. Symbols are trustworthy as long as the binary has not been rebuilt
since the guest booted — check `stat` on `kernel/bin/os64_kernel` against the
QEMU process start time before believing an address.

## Headless QEMU (the agent's mode)

A windowed QEMU steals the human's keyboard focus mid-typing. Agent runs
are headless, with serial going somewhere session-private:

The blessed invocation — copy it whole, then adjust paths:

```bash
# Copy the disk images first: the human's own QEMU holds exclusive write
# locks on disk/*.img (even -snapshot can't share them), and an agent boot
# should never write the human's /home image anyway.
cp --sparse=always disk/os64.img disk/os64_data.img "$SCRATCH/"

qemu-system-x86_64 -machine q35 -cdrom os64_kernel.iso -boot d \
  -m 8g -no-reboot -smp 8 \
  -display none -serial file:$SCRATCH/run_com1.log \
  -monitor telnet:127.0.0.1:55556,server,nowait \
  -drive file=$SCRATCH/os64.img,format=raw,if=none,id=nvme1 \
  -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=$SCRATCH/os64_data.img,format=raw,if=none,id=data1 \
  -device nvme,drive=data1,serial=data1-serial
```

Monitor port **55556** — 55555 is the human's, per the ratified split. Keep
the monitor if driving input/screenshots; `-monitor none` otherwise.

**Lift the flags from the `run` target verbatim — never reconstruct them
from memory.** `-machine q35` in particular is load-bearing: os64's PCI
config path is ECAM-only (`kPCIBaseAddress` from MCFG), and QEMU's default
i440FX board has no MCFG — the base stays zero and the first runtime PCI
read page-faults at VA 0x1000 (bus 0/dev 0/func 1 against a zero base).
A hand-typed flag set that "looks right" cost a real debugging detour on
2026-07-11 — **and again on 2026-08-18**, when the flags were rebuilt from
`QEMU_BASE_FLAGS`, which at the time did not carry `-machine q35` (it does
now; every target inherits it). The 8/18 autopsy, for the next fingerprint
match: on i440FX the guest triple-faults **3–65 seconds in, silently** —
the #PF lands while the faulting core's GS base is 0, `exception_dispatch`'s
GS:0 CLS load reads still-mapped page 0 (IVT garbage, `f000ff53f000ff53`),
and the #GP spiral exhausts two stacks into a triple fault. With `LOGD=` on
the cmdline the kernel is holding serial for a daemon that never attaches,
so the serial log shows only the four pre-ring lines and *nothing else* —
a wedge and a working quiet boot look identical from the wire. **A headless
default-entry boot that dies with a four-line serial log and no panic text:
check the machine type FIRST, then `-d cpu_reset` for the triple fault,
then `-d int` for the cascade.**

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
- **Waiting on boot progress: the marker is `"boot complete"`, and on the
  default entry it is the ONLY one you may wait on.** Don't chain sleeps:
  `until grep -q "boot complete" "$LOG"; do sleep 1; done`. `[pool] boot
  complete: N/M paging pages used` comes out of `kernel_park` through
  `serial_print_string` — the direct polled write, the same door panic uses —
  so it is ungated by debug level and survives a logd claim on the port by
  construction, which is why it was written that way. It prints when the kernel
  task parks *because userland is running*. ~32s on the default entry.

  Every other milestone in this file's next section is a `printd`, and with
  `LOGD=` on the cmdline (the default entry has it) those go to logd's FILE and
  NEVER reach the wire — waiting on `All VFS tests passed` or `BUILT-IN TESTS`
  there is waiting for something that will not come. Chris's ruling, 2026-08-28,
  after watching two models burn 90- and 200-second timeouts on exactly that.
  Read the file host-side after the guest exits (`debugfs -R "cat /os64.log"
  "disk/os64_data.img?offset=1048576"`), or boot an entry with no `LOGD=`.

  Under WSL2 the serial drain can crawl (unsolved QEMU/WSL2 artifact) — a quiet
  log is not a dead kernel; give markers tens of seconds before declaring a
  hang.

## The chaos rig (weather in the wire)

slirp is a perfect network: nothing is ever lost, reordered, duplicated or
late, so `/sys/net/tcp`'s retransmit and out-of-order counters read zero on
this harness forever, and the TCP debts they exist to adjudicate (a send
window — and reassembly and the measured RTO, until each was paid) cannot be
measured here. `tools/cable.py` puts
a cable with weather in it between the guest's NIC and slirp, using QEMU's
`filter-redirector` (the COLO building block): every frame goes out to the
cable over a loopback socket and the survivors come back in. No tap, no
netem, no root. The header of `tools/cable.py` is the design; the
`QEMU_CHAOS_FLAGS` comment in the GNUmakefile is the wiring, including the
direction trap (QEMU's `queue=rx` on the slirp netdev is the guest's UP).

```bash
python3 tools/cable.py --seed 1 &            # passthrough until told otherwise
make run-chaos                               # run-net, wired through the cable
tools/cable.py ctl set both loss 0.2         # weather, changed live
tools/cable.py ctl set down reorder 0.3      # hold 30% of inbound frames 50ms
tools/cable.py ctl set down dup 0.2
tools/cable.py ctl link up off               # the FIN-into-the-void test
tools/cable.py ctl stats                     # the ledger: every frame accounted for
```

Start the cable BEFORE the guest and leave it running for the guest's whole
life: weather changes go through `ctl`, never through a restart, because a
cable that goes away under a live QEMU 8.2.2 wedges its redirectors (half the
chardevs never reconnect, the rest fail EPERM) — restart the guest, not the
cable. For an agent run, add `QEMU_CHAOS_FLAGS`'s chardev/object lines to the
blessed invocation above (the pcap path is the only thing to change).
`up`/`down` are always the guest's view. Directions are impaired
independently; parameters are `loss delay jitter reorder reorder_ms dup
blackhole`, on the command line as `--up-loss 0.1` or live through `ctl`.
The seed makes a run reproducible: same seed, same drops.

**The workload has to be big enough to show the weather.** A whois is one
data segment: holding it 50ms reorders nothing, and a duplicated pure ACK
has no counter. Fetch a file of a hundred KB or more from `tools/os64serve.py`
on the host (`os64get 10.0.2.2 mid.bin /home/mid.bin`) and the counters
move. The rig's first day (2026-09-02), same 100KB file, all CRC-verified:

| Weather | Time | What moved |
|---|---|---|
| none | 2s | nothing |
| `loss 0.1` both ways | 12s | `retransmits` |
| `delay 100` `jitter 30` both ways, order kept | 2s | nothing — a download rides the peer's send window; what 200ms round trips cost an os64 *upload* is the table below |
| `reorder 0.3` down | 29s | `out_of_order_dropped 49` on one connection — the price of v1's no-reassembly, measured |
| `reorder 0.3` down, **after reassembly** (2026-09-04) | 1s | `out_of_order_held 34`, `out_of_order_dropped 0`, `retransmits 0`; the trunk kernel the same morning, same seed: 34s and 47 dropped. Both files CRC-verified |
| `dup 0.2` down | — | `duplicates_dropped` |

**The send side has its own workload**, because our retransmit timer governs
only the segments os64 sends, and a download's losses are the peer's timer's
problem: `/tests/netsend HOST PORT BYTES` pushes a seeded byte stream up a
connection and prints the milliseconds; `tools/tcpsink.py` on the host drains
it, checks every byte against the same stream, and answers one byte — and
**that byte is where netsend's clock stops**, because a write returns when
its bytes are queued and a close returns with the ring still draining, so
only the far end can say when the bytes arrived. 100KB, seed 1, same cable,
same 21 segments lost each time (2026-09-04 — three kernels in one day):

| Weather (upload) | fixed 1s RTO | Jacobson/Karn RTO (stop-and-wait) | send window + NewReno | What moved |
|---|---|---|---|---|
| none | 0.8s | 0.8s | 0.03s | `srtt_ms 10`, `rto_ms 200` (the floor); the window sends 100KB in one slow-start burst |
| `loss 0.1` up | 11.8s | 3.3s | 0.02s | `retransmits 10`, `fast_retransmits 4` — every hole found by duplicate acks, none by the timer |
| `delay 100` both + `loss 0.1` up | 31.5s | 19.9s | 4.7s | `srtt_ms 210`; ~7 loss events at a 200ms round trip, each a recovery round trip and a halved cwnd — what NewReno costs at 10% loss; SACK is the next number down |

Three of the window's own lessons are in that third column, each read off
the capture. The first cut (Reno, RFC 5681 alone) read **6.7s on the loss
leg — slower than stop-and-wait**: a fast retransmit filled the first hole
and the ack landed on the second, where only two duplicates could ever
follow, so every further hole in the window waited for a backed-off timer
(0.6s, 0.8s, 1.6s, 3.2s). NewReno's partial-ack rule (RFC 6582) took it to
0.09s. The second, on the delay leg: a tail loss cost 3.7s because the
doubled timer waited for a clean sample that a windowed sender under loss
rarely gets; 4.4BSD's rule — progress ends the backoff — took the leg from
9.0s to 5.1s. The third, found answering the first review round: a timeout
resent only the head, and every further hole behind it then waited for its
own timeout (0.5s, then 1.4s, for two holes behind one). 4.4BSD's go-back-N
— `snd_nxt = snd_una`, everything outstanding resent in order under slow
start — took the leg to 4.7s, with no gap over a quarter second anywhere
in its capture.

Delay and reorder are separate knobs ON PURPOSE: the cable keeps frame order
under jitter, as a real pipe does, so a delay leg measures the clock and only
the `reorder` knob measures reassembly. (The first delay leg did not keep
order and read as a 30× slowdown; all of it was reassembly.)

The pcap dump sits AFTER the cable in both directions: the capture is what
got through, the ledger is what did not, and `orphaned` (a frame the cable
could not hand back because QEMU's inbound socket was gone) is never zero
silently.

## Reading the serial log

**WHERE these lines live depends on the boot entry.** Every one of them is a
`printd`, so with `LOGD=` on the cmdline they are in logd's FILE and not on the
serial wire — this section is about reading the LOG, whichever sink holds it,
and the only thing you may *wait* on at the wire is `"boot complete"` (see the
headless section above). Boot an entry without `LOGD=` and they land on serial
as they always did.

Milestones of a healthy full boot, in order (the counts shown are July
2026 values and WILL drift — nothing below depends on them):

```
BUILT-IN TESTS: Running pre-boot tests:   →  24 passed, 0 failed
RAMDISK: registered ... as a block device    (RAMDisk entries only)
BOOT: Root filesystem found, mounting     →  successfully mounted
BUILT-IN TESTS: Running post-boot tests:  →  7 passed, 0 failed
BOOT: All VFS tests passed successfully.
BUILT-IN TESTS: Running late tests:       →  2 passed, 0 failed
```

Grep count-agnostically — never hardcode test counts, they change with
every added test:

```bash
grep "passed, 0 failed" "$LOG"        # eyeball: exactly three lines, and
                                      # each follows its phase header
grep -c "passed, 0 failed" "$LOG"     # assert: == 3 (pre-boot, post-boot, late)
```

A FAILED in-kernel test **panics** (TEST_FAIL → panic), so a failing phase
never prints its "passed, 0 failed" line at all — the count assertion
catches both a failure and a phase that never ran. When reviewing by eye,
confirm the line's phase from the preceding "Running … tests:" header, not
from the numbers.

**THE LATE LINE ARRIVES LAST, AND LATER THAN YOU EXPECT** (2026-08-29). It
is written by a kernel thread that starts after the shells are seated, so it
lands *after* `boot complete` and after husk's first prompt — a harness that
stops reading at `boot complete` will see two lines and conclude a phase
never ran. Wait for the late line itself when you want all three. The phase
takes ~11s on a quiet machine and can take longer, because
`task_teardown_leak` re-measures when something else on the machine moved
memory under it.

Standing regression greps:

| Boot mode | Must show | Must NOT show |
|---|---|---|
| Default (non-GUI) | all three `passed, 0 failed` lines (pre, post, late), VFS pass | `guicomp` (grep -c == 0) |
| GUI | compositor heartbeats (DEBUG_GUI) | panic lines |
| RAMDisk (no disk attached!) | `RAMDISK: registered`, root mounted | `Could not find/mount root` |
| NVMe/AHCI regression | root mounted from the real disk | any `RAMDISK:` line |

For log-volume limits when enabling debug bits on multi-core runs, do the
serial arithmetic first — SCHEDULER.md, "do the math first" (11,520
bytes/sec is all you get).

## The in-kernel test framework

`test_framework.h`: register cases, three phases — `test_run_preboot()`
(before the scheduler starts), `test_run_postboot()` (scheduler live, shell
NOT yet seated; this is where the ELF/ring-3 tests run, loaded from the root
filesystem — which is itself what proves the storage/VFS stack end to end),
and the LATE phase (`late_tests_thread`, a kernel thread started once the
shells are up). Tests live in `kernel/test/`. Adding one: register in the
appropriate phase, keep it silent on success, TEST_FAIL (= panic) on
failure. No doc or watcher updates needed — the greps above are
count-agnostic by design.

**Choosing the phase** is a question about the machine, not about the test's
subject: PREBOOT for kernel invariants, POSTBOOT for anything needing a
quiet machine or a verdict before a person can write to the disk (every
`TEST_POLICY_RO` write gauntlet, by definition), LATE for slow tests that
survive a live system. Slow is the reason to go late; tolerant is the
permission. A late test asserting on a global that userland also moves —
the free-page count is the one that bit — must gate on quiescence and be
willing to SKIP, or it belongs in POSTBOOT.

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

### Modifier drags need QMP, not the monitor (2026-08-19)

**HMP `sendkey` cannot express a held key.** It presses and releases as one
unit, so "hold Ctrl+Alt, then drag the mouse" — the whole Ctrl+Alt window
gesture — is not testable with it. `sendkey ctrl-alt <hold_ms>` looks like the
answer and is a trap: measured against a raw-scancode log in the guest, plain
`sendkey ctrl-alt` delivered `1d 38 b8 9d` (both keys, correctly), while
`sendkey ctrl-alt 8000` delivered `1d` and **nothing else** — the Alt make code
never reached the guest at all. An hour went into the kernel looking for a bug
that was in the harness, which is the reason this section exists.

The answer is QMP's `input-send-event`, where every edge is its own event.
Add `-qmp tcp:127.0.0.1:55557,server,nowait` to the QEMU line and speak JSON:

```python
q.cmd("qmp_capabilities")
q.key("ctrl", True); q.key("alt", True)      # held, for real
q.btn("right", True)
q.drag(90, 70)                                # steps of <=120px per packet
q.shot("/path/band.ppm")                      # QMP screendump, same as HMP
q.btn("right", False); q.key("alt", False); q.key("ctrl", False)
```

`tools/guidrive.py` is that client (~90 lines: key/tap/btn/move/drag/shot over
a QMP socket) plus a `main` that execs a driver script with `q` in scope, so a
gesture test is a dozen readable lines. Boot with the monitor for the Limine
menu and the final `quit`, and hand the running guest to the driver.

**The lesson generalizes past QEMU:** when an input test fails, prove what the
GUEST received before theorizing about what it did with it. One `printd` of the
raw scancode in `keyboard_handle_scancode` settled a question three layers of
plausible reasoning had not.

## Platform matrix — what each catches that the others can't

| Platform | Uniquely catches | Notes |
|---|---|---|
| QEMU/TCG (WSL2) | Fast iteration; `-d int,cpu_reset,guest_errors` traces; deliberately hostile TSC desync between vCPUs | The TSC desync is a feature: it found the rdtsc-across-preemption bug |
| VirtualBox | Real-ish I/O latency (found the NVMe locking race); stricter virtual-APIC (LVT arming order); firmware differences (virtual-wire mode NOT left on — found the dead-keyboard IMCR bug) | Boots the /mnt/c/temp ISO, put there by `make copy`; disk via `make vbox-sync`, VM OFF |
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
