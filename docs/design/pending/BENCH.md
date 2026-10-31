# BENCH.md — a real machine a model can drive the way it drives QEMU

*Design 2026-10-31 (Fable), from an afternoon's conversation with Chris.
Ruling owner: Chris; the rulings he made that afternoon are marked RULED
below and are not open. The ask, in his words: "give you access to the P5
the way that you have access to QEMU. Not a jenky half hearted hack.
Something that could be reused elsewhere." SPYGLASS.md (2026-09-23, now in docs/design/unimplemented) was the
first answer to the same wall; this supersedes it, because sshd on the
machine makes a VNC client the long way round. The `vm*` tools in `tools/`
are the vocabulary this builds on; VERIFICATION.md § Headless QEMU is how
they are used today.*

## 1. The wall

Every proof in this project runs through five verbs against QEMU: boot it,
type into it, look at its screen, pull a file out, read its last words.
What makes those verbs trustworthy is that all five come from OUTSIDE the
kernel under test. The monitor answers after the guest triple-faults, the
serial file holds the panic, the disk image reads from the host while the
guest is dead.

The P5 has one door, sshd, and that door IS the kernel under test. While
the kernel is well a model can exec and read. When it dies, the model is
blind, deaf and handless, and Chris walks to the desk. The bugs that only
the real machine shows (the NIC, USB, firmware, timing, the GUI at real
pixel rates) are exactly the ones a model cannot chase today.

| Verb | QEMU | P5 today | What closes it (slice) |
|---|---|---|---|
| exec, exit status | sendkey + screendump | sshd | nothing |
| file out | debugfs on the image | `ssh 'cat f'`, byte-exact | nothing |
| screen | monitor `screendump` | vncd through a tunnel | `shot` (1) |
| hands | `sendkey`, `mouse_move` | nothing | `hands` (1) |
| install a build | rebuild the image | os64get | nothing |
| reboot | `system_reset` | `reboot`, in-band only | the watchdog (4) |
| last words after death | serial file, `-d cpu_reset` | the glass, for Chris's eyes | the black box (3) |
| boot a trial safely | a throwaway image | edit limine.conf by hand | the trial protocol (5) |
| power on | launch QEMU | the button | Wake-on-LAN (6) |

## 2. The shape

Three layers. The middle one is the reusable part.

1. **In-band programs on the machine**, reached over ssh exec: `shot` and
   `hands`, over `/dev/glass`, which vncd already consumes. Useful to a
   person at a terminal as much as to a bench.
2. **The bench vocabulary on the host**: the `vm*` verbs, taking a MACHINE
   NAME where they take a monitor port today. A machine is a profile that
   says how to reach it, see it, type into it, reset it, install to it and
   read its death. `qemu` and `p5` are profiles; the 3900X, a VirtualBox,
   a second real box are profiles, not new tools. Every script written
   against QEMU runs against the P5 unchanged in meaning.
3. **Out-of-band kernel instruments** that answer when the kernel cannot:
   a black box the panic path writes to disk, a hardware watchdog that
   resets a hung machine and records what hung, a trial-boot protocol that
   makes an untested kernel safe to boot, and Wake-on-LAN so a powered-off
   machine can be brought back.

Rejected, by name: a second computer as a console server, a USB serial
dongle, a smart plug over HTTP. All workable, all the kind of hack the ask
ruled out, and none of them travel to the next machine.

## 3. The gate (RULED 2026-10-31)

The P5 is the machine Chris sits at. Everything that can CHANGE it from the
wire is behind one switch, and the switch is physical in the only sense
that matters: a hand on the real mouse.

- **A checkbox in Control Center, "Allow remote control".** Chris: "it
  only applies to the current session. When the machine is rebooted, it
  goes back to unchecked." Per session, not per occurrence, because a
  bench may drive the machine all day. "That can't be done remotely ...
  unless the box is already checked" is the whole policy: once armed for
  the day a model can do anything a person at the keyboard could, and a
  reboot takes it all back.
- **The state is the kernel's.** One flag, clear at every boot, published
  read-only at `/sys/gui/hands` (the holder of the glass, the lease, and
  whether the gate is open) so a refused tool can say "not armed, ask
  Chris" instead of failing mysteriously. It is never written to a conf
  file; Control Center's other settings save through the ladder and this
  one must not, or a reboot would come back armed.
- **The arming write is refused from any REMOTE lineage.** This is what
  makes the checkbox a gate and not a decoration: an ordinary sysfs write
  could be made over ssh. sshd's children carry a REMOTE mark on the task,
  inherited across spawn the way `PINCPU` inherits, and the write that
  arms the gate is refused from any task carrying it, however many
  programs it spawns and whether or not one of them opens a window.
  telnetd's children carry it too. The only task that can arm is one
  descended from the local desktop.
- **What the gate covers:** opening `/dev/glass` for writing (the hands),
  `shutdown` in both verbs (a reboot throws away more of Chris's state
  than any click; power-off leaves the machine dark), and therefore the
  trial boot. Wake-on-LAN is the power button's pair and needs no gate:
  it only ever turns a dark machine on. **What it does not cover:** reads
  (`shot`, `cat`, `/sys`, `/proc`), and installs through os64get, which
  stay as they are today. The gate is about the running machine's state,
  not its files; a file install changes nothing until a reboot, and the
  reboot is gated.
- **Unchecking the box takes effect at once:** every writing glass viewer
  is closed and the keys and buttons it held are lifted, the same path a
  dropped connection takes.
- **An audit trail.** Every arming, disarming, hands session (open and
  close, with the task) and gated refusal goes to the kernel log, and
  `/sys/gui/hands` names who holds the glass right now. "Did the model do
  that?" is answered by a line, not a guess.

QEMU needs none of this. The rig types and clicks through the monitor,
which is physical input to the guest, so the `qemu` profile keeps
`sendkey` for hands and uses the glass only for `shot`. There is
deliberately NO cmdline token that pre-arms the gate: a token that opens
it by default is exactly what must never ride to the P5 by accident, and
the tokens that would make a QEMU soak convenient are the ones that get
pasted into a boot entry.

## 4. Slice 1: `shot`, `hands`, the gate

Two userland programs and a small kernel change. The dev who works the
slice also makes the Control Center change (RULED).

### `shot`

Open `/dev/glass` for reading, read until nothing is pending (the first
read is the whole screen; it may arrive as bands), assemble the frame,
write it out. **Stdout by default, a file only when asked**, because
sshd's exec channel is byte-exact (3 MiB binary both ways in its own
probe) and the bytes then land on the bench with nothing left on Chris's
disk:

```
ssh p5 shot > desk.ppm          # the bench's shape
shot -o /home/shots/desk.ppm    # a person's shape, from a terminal on the machine
```

The format is the PPM QEMU's `screendump` writes, so `vmshot`'s PIL step
is unchanged. A 1920x1080 frame is about 6 MB; `shot | gzip` is free and
a PNG encoder is slice 6. Two facts about the glass worth knowing: it
serves the DESKTOP's pixels even while a text VT holds the screen, and
says so in the record's flags, so `shot` on a GUI boot showing VT1 shows
the desktop behind it and reports the flag; and a boot with no desktop
refuses the open. Seeing a text VT is slice 6, not something `shot` fakes.

### `hands`

A command language on stdin, one action per line, one result line per
action on stdout, so a bench pipes a whole sequence through one ssh exec
and reads what happened:

```
key ctrl-alt-f1      -> ok
type ls /home        -> ok
move 640 400         -> pointer 640 400
click left           -> ok
down shift           -> ok        held until `up shift`, or until hands exits
up shift             -> ok
```

Key names are QEMU's `sendkey` names, mapped to HID usages through one
table that mirrors vncd's, US layout, a shifted character carrying Shift
itself, so a `vmtype` script needs no translation. A key is a report with
the usage held followed by a report without it; `type` presses and
releases each character in turn. The pointer is absolute in screen pixels,
so after a `move` hands knows the position exactly and reports it. Closing
the glass lifts every held key and button (the kernel's guarantee), so a
dropped ssh never leaves Ctrl down. While it holds the glass, hands draws
a small mark on the screen the way vncd draws its banner, so Chris can
see the difference between "allowed" and "someone is driving right now".
Unarmed, every input line is refused with the reason and the exit status
says so.

### The kernel half

The REMOTE mark on the task and its inheritance; the `/sys/gui/hands`
file; the arming write and its lineage refusal; the check at glass write
open and in `shutdown`; the close-on-disarm; the log lines. Small, but it
is a ring-boundary rule, so it gets the review that implies.

## 5. Slice 2: the bench

The `vm*` tools learn a machine name. `vmshot p5 desk.png`, `vmtype p5 ls
/home`, `vmget p5 /home/out.txt`, `vmtest p5 'cmd'`, `vmcmd p5 reboot`
mean what they mean against QEMU. A profile is a small file (where it
lives is the builder's call; it carries no secret, the key file does):

```
machine = p5
reach   = ssh yogi@192.168.137.246 identity ~/.ssh/os64_models
see     = shot                 # qemu: monitor screendump
type    = hands                # qemu: monitor sendkey
reset   = reboot               # qemu: monitor system_reset
install = os64get              # qemu: rebuild the image
record  = /home/os64.log       # qemu: the serial file
blackbox= /sys/blackbox        # slice 3
wake    = 00:11:22:33:44:55    # slice 6
```

- **Boot complete on a real machine** is ssh answering plus a read of
  `/sys/cpu/count`, whose uptime says this is a NEW boot and not the old
  one still dying. The bench polls that, never sleeps blind, the same rule
  the QEMU rig lives by.
- **The lease.** Two agents on one real machine would undo each other's
  work. Before any controlling verb the bench takes a lease: a small file
  on the machine naming the holder, when it was taken and when it lapses,
  refreshed by every verb, shown beside the holder in `/sys/gui/hands`. A
  second agent's `vmtype p5` refuses with "leased to Opus since 14:02";
  read-only verbs still work. It is an honor lock between cooperating
  models, and Chris breaks it with one `rm`. Until it exists the rule is
  one agent on the P5 at a time, written where the models read.
- **`vmboot` on a real machine** is `reset` followed by the boot-complete
  poll; `vmstop` is `shutdown` (gated) or nothing at all.

No kernel work. This is the slice that makes every script in the project
run on real hardware, and the one the next machine inherits for free.

## 6. Slice 3: the black box

A panic on the P5 writes its full report, registers, forensics and call
chain, to a glass that stays lit until the power button, and nowhere else.
The log rings hold the last minutes of the kernel's own story and die
unflushed with it. The black box gives the panic path somewhere to write
that survives.

- **Where:** a reserved region on disk with its own os64-authored GPT type
  GUID, so the build stamps it on the QEMU images and `p5-refresh.sh`
  carves it beside root, the lifeboat and /home. Found at boot by the same
  partition walk that finds the others; absent, the black box is off and
  says so once.
- **What:** the panic report as the serial would have received it, then
  the TAIL of every core's log ring, which is the real prize on a machine
  with no serial: the last few hundred lines before death. A header names
  the boot (its start time and command line) and the sequence number, so
  a stale record is never mistaken for a fresh one.
- **How:** polled NVMe, the way panic already writes serial by polling,
  from the panicking core with every other core stopped. No locks, no
  allocation, no cache: the block path is NOT the vfs, it is the driver's
  own submit-and-spin, the same discipline `logd_emergency_flush` uses for
  the wire. AHCI is not covered until its DEBTS row is paid.
- **Who reads it:** the next boot finds a record whose sequence it has not
  seen, publishes it as `/sys/blackbox`, and prints one line to the log
  saying a record from the previous boot is waiting. The bench reads it
  first after any reboot; a person reads it with `cat`. It is cleared by
  the next panic, never by reading.

Lineage: the flight recorder, and Linux's pstore (2010), which keeps the
oops in whatever persistent store the platform offers. The panic path and
a polled write under a dead machine are kernel concurrency and lifetime
work, so this is a Fable-review slice.

## 7. Slice 4: the hardware watchdog

A silent hang on the P5 ends with a finger on the button. A watchdog is a
countdown the kernel must keep resetting; if it reaches zero, the chip
resets the machine without asking anybody.

- **Hardware:** QEMU's q35 carries the ICH9 TCO watchdog, so the whole
  path tests in the rig; the P5's FCH carries AMD's, and `/sys/acpi` is
  where to look for how the firmware describes it (a WDAT table, or the
  FCH's own registers). Which exact register set the P5 exposes is the
  first thing the slice establishes, on the machine, before a line of
  driver.
- **Two timeouts, one instrument.** TCO-style watchdogs fire twice: the
  first timeout raises an NMI, the second resets. On the first, every
  core takes the NMI and snapshots its registers and call chain the way
  `nmi_probe.c` already does (that code exists for exactly this question,
  "what is a core that stopped answering doing?", and its safety model,
  snapshot on the sick core and interpret on a healthy one, is the one
  the handler keeps), and the snapshot goes into the black box. On the
  second, the machine resets. The next boot's `/sys/blackbox` says which
  core was spinning where. QEMU's monitor `nmi` tests the handler alone.
- **Petting:** from the tick, so a machine whose tick has stopped is by
  definition hung. The long legitimate stalls (the 20 s ramdisk module
  load at boot) happen before the watchdog is armed; it is armed at boot
  complete, when userland is running, and the timeout is seconds, not
  milliseconds.
- **Policy lives in `/etc/os64.conf`**, which travels to the P5 where a
  cmdline token does not: `watchdog = off | <seconds>`, and `panic = halt |
  reset <seconds>`. `halt` stays the default so the screen is Chris's to
  read; `reset` is what a bench sets so a dead trial comes back on its own.
  The black box is written BEFORE either, so nothing is lost to the reset.

Chris has known what a watchdog is for decades and never built one; this
is the one. Small, hardware-facing, a cold read before it lands.

## 8. Slice 5: the trial boot

Today an untested kernel reaches the P5 through os64get and the next
reboot runs it, with Chris at the Limine menu as the only way back. The
trial protocol makes that safe to do from the bench.

- **Two entries, both always present** in limine.conf: the known-good
  kernel and the trial kernel, at two paths. The bench installs under the
  trial name, flips the file's DEFAULT to the trial entry with a short
  menu timeout, and reboots (gated). At boot complete a `promote` step
  flips the default back to known-good, copies the trial over it if the
  bench says the trial passed, and records the result in the log.
- **The bench only ever switches the default between two entries that
  exist.** It never writes an entry. limine.conf is the one file that can
  make the machine unbootable, which is why it does not travel with
  os64get, and this protocol keeps that power out of reach.
- **The residue, said out loud:** Limine has no boot counter. A trial
  kernel that dies before it can write the FAT partition boots again on
  the next reset and dies again, until a person picks the known-good or
  lifeboat entry at the menu. That is no worse than today, and the bugs
  the real machine is for (NIC, USB, timing, GUI) live past root mount.
  A kernel that dies before root mount is caught in QEMU first.

## 9. Slice 6: the small ones

- **Wake-on-LAN.** The magic packet is AMD and Hewlett-Packard's Magic
  Packet Technology (1995): six bytes of 0xFF followed by the card's own
  MAC sixteen times, anywhere in a frame. A sleeping NIC scans every
  frame for that and ignores all else; an optional six-byte SecureOn
  password is the only variant and os64 does not use it. The driver's
  half is small: at `shutdown`, program the RTL8125's wake configuration
  to accept magic packets and leave power-management events enabled
  instead of killing the card. The host's half is the `wakeonlan`
  package or ten lines of Python. The BIOS must allow wake from the LAN.
  Ungated, because it only turns a dark machine on. This is what makes an
  overnight soak possible: run, power off, wake, repeat.
- **The glass sees text.** Serve a text VT's cells rendered to pixels
  through the same records, so `shot` works on a text boot and a
  text-mode bench exists. The flag already says a VT holds the screen;
  this changes what the pixels are while it does.
- **PNG out of `shot`**, an encoder in libimage, which shrinks a frame on
  the wire by about ten.

## 10. Verification

- Slice 1: `shot` through ssh into QEMU equals QEMU's own `screendump`
  pixel for pixel (vncd_probe's assertion, through the new door);
  `hands` types into a QEMU guest's husk and a separate exec reads the
  result back; a chord (Ctrl+Alt+F1, then Alt+F8) takes and returns the
  screen; the gate refuses an `echo` over ssh, accepts Control Center,
  and unchecking the box closes a live hands session with Shift held and
  Shift comes up. On the P5: the first picture of the real desktop a
  model has taken, and the gate exercised by Chris's own hand.
- Slice 2: every existing `vm*` script, unchanged, against `qemu`; then
  `vmtest p5` end to end; two agents and the lease.
- Slice 3: `TESTPANIC` on QEMU, reboot, read `/sys/blackbox`; the same on
  the P5.
- Slice 4: a deliberate hang (`nosyscall` pinned on the BSP with the tick
  stopped, or QEMU's `stop`-free equivalent) resets the machine and the
  black box names the core; `nmi` from the monitor snapshots without
  resetting.
- Slice 5: a trial that passes is promoted; a trial that panics after
  root mount comes back on known-good with the black box filled.

## 11. Open

- The lease file's home and lapse time.
- Where profiles live (`tools/bench/<name>.conf` is the obvious place).
- Which watchdog the P5's firmware actually exposes: the slice's first
  finding, read from `/sys/acpi` before design is final.
- The name. "Bench" is where a machine sits while it is under
  instruments; it is a pitch, not a ruling.
