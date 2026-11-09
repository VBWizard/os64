# SPYGLASS.md — a VNC viewer a model can drive

*Design, 2026-09-23 night, by Fable. Chris's ask: our own VNC client, AI-centric,
controllable from the command line the way QEMU is, so that even GUI changes
can be tested on the real machine. Name proposed, not ruled: a spyglass is
what you look at something far away with, and what it looks at is
`/dev/glass`.*

## The wall it removes

The P5 lag hunt (REMOTE.md, 2026-09-23) ended at the sentence "QEMU cannot
reproduce it". Every proof in this project runs through the `vm*` tools
against an emulated machine, because those are the only hands a model has.
The real machine has a real NIC, real firmware and real timing, and today a
model can only read its logs. vncd (§ 5) put the P5's screen on a socket;
this puts that socket behind the same tools, so `vmshot` and `vmtype` mean
the same thing whether the machine is emulated or real.

## Shape

One host program, `tools/spyglass.py`, Python, one file. It is three things:

1. **An RFB 3.8 client**, Raw and ZRLE. `tools/vncd_probe.py` is already
   the handshake and Raw; `tools/test_zrle_host.sh` is already a ZRLE
   decoder written from the RFC. It connects to `localhost:<port>`; the
   tunnel is the operator's (`ssh -N -L 5900:localhost:5900`, from WSL2,
   never Windows' ssh.exe, REMOTE.md § Using it). Keeping SSH out of the
   viewer keeps every key decision out of it too.
2. **A VM, as the `vm*` tools understand one.** It creates
   `/tmp/os64-vm-<id>/` and serves QMP over the same `monitor.in` /
   `monitor.out` named pipes `tools/vmmonitor.py` opens, with the same JSON
   framing and `id` tokens. `vmshot`, `vmtype`, `vmcmd` and `vmstop` then
   work unchanged. The one change on the tools' side: `running_pid` today
   accepts only a `qemu-system-x86_64` whose argv names the directory; it
   learns one more spelling, a `spyglass.py` whose argv names it.
3. **A window, optional** (`--window`, Tk canvas). Chris likes to watch. It
   is not the control path and never becomes one: nothing typed into the
   window goes to the machine except the arming gesture below. For his own
   use he has TigerVNC; vncd serves four viewers.

The viewer keeps one incremental update request outstanding at all times,
as every viewer does, so its shadow of the screen is current within one
round trip. A `screendump` is the shadow written out.

## The vocabulary, and why it is QEMU's

| Command | Does |
|---|---|
| `screendump <path>` | Writes the shadow as the PPM QEMU writes, pixel-exact. `vmshot`'s PIL step is unchanged. `--settle <ms>` waits for that long with no update before writing |
| `sendkey <keys>` | QEMU key names with `-` chords (`ctrl-alt-f1`, `shift-a`), press then release, the hold time QEMU uses |
| `input-send-event` | The QMP form: a key DOWN or UP on its own, an absolute pointer, a button. This is what the chord tests need (#123's lesson: a viewer that lifts the last key on every press hides bugs) |
| `mouse_move`, `mouse_button` | The HMP forms, for scripts that already use them |
| `info spyglass` | Connection state, encoding in use, frames and bytes, the age of the last update, and whether the hands are armed |
| `quit` | Ends the session; `vmstop` works |

Anything else is refused with QMP's own error shape, so a script learns it
the way it would from QEMU.

Key names map to X11 keysyms through one table that mirrors vncd's
`keysym_key`, US layout, and a shifted character carries Shift itself
(`shift-1` and `!` are the same report). The two tables are checked against
each other by a host test that drives vncd's own `keysym_key` from C, so
the viewer cannot drift from the server.

## The rule about hands

This is input on the machine Chris sits at. Two rules, and both are the
operator's, never the model's:

- **The viewer runs only because Chris launched it**, on his desk, through
  his tunnel. There is no daemon and nothing starts it.
- **Input is refused until armed.** `--hands` at launch, or `h` typed into
  the window, arms it; the window title says `HANDS ON` while it is, and
  `info spyglass` says so on the port. Unarmed, `screendump` and `info`
  work and every input command is refused with a reason. A model that
  wants the hands asks the person.

Today models are read-only on the P5. This changes that deliberately, on
his say-so, per session, and visibly.

## Proof

1. **Against vncd in QEMU, through the real tunnel**: `vmshot` through
   spyglass equals QEMU's own `screendump` in every pixel, Raw and ZRLE.
   The same assertion vncd_probe makes, now through the viewer.
2. **`vmtype` through spyglass** into a QEMU guest's husk, read back by a
   separate `ssh` exec, as vncd_probe's step 5 does.
3. **A chord**: `input-send-event` holds Ctrl+Alt, presses F1, releases in
   report order, and the text terminal takes the screen; Alt+F8 brings the
   desktop back. `vmshot` shows each.
4. **On the P5**: `vmshot` of the real desktop, the first picture of the
   real machine a model has ever taken.

## Deferred, said out loud

Clipboard (vncd defers it too, DEBTS), the scroll wheel (same), non-US
layouts (os64 has one), and more than one spyglass on one tunnel (start
two tunnels).

## For Chris to rule

- **The name.** Spyglass is my pitch.
- **The VM id for the P5.** `vm_setup` takes digits; a convention like `5`
  reads well: `tools/vmshot /tmp/desk.png 5`.
- **Python.** The RFB client, the ZRLE decoder, PIL and the whole `vm*`
  layer are Python already; C would buy nothing here.
- **Who builds it.** Opus has the vncd side in his hands and the `vm*`
  rework is uncommitted on this tree, so whoever owns that lands first.
  I review either way.
