# REMOTE.md — the machine from across the room

*Design record for using os64's desktop from another computer: VNC over an
SSH tunnel. Written 2026-09-22 by Opus, for Chris's approval before any code.*

## The shape

```
Windows                                   os64 (P5)
───────                                   ─────────
VNC viewer ──► localhost:5900             vncd ◄── 127.0.0.1:5900 (loopback ONLY)
                  │                         ▲  │
               ssh -L                       │  └── /dev/glass: read = the screen,
                  │                         │                  write = keyboard + pointer
                  └──── TCP 22 ────► sshd ──┘  (direct-tcpip channel, dials loopback)
```

There are three rules, and the rest of the design follows from them:

1. **SSH provides the crypto and the authentication.** vncd uses RFB security
   type None. That is safe only because vncd listens on loopback and nowhere
   else, and anything that reaches loopback from off the machine has already
   authenticated to sshd with an enrolled ECDSA key. If vncd cannot get a
   loopback-only listener it refuses to start. There is no "LAN mode" to
   forget to turn off.
2. **The screen is read from RAM, never from video memory.** The
   compositor's backbuffer is the canonical screen image (GRAPHICS.md
   invariant 1). It is still composited while a text VT holds the glass, and
   it is cacheable. The uncached hardware framebuffer is never read, by vncd
   or anyone else.
3. **Remote input is a keyboard the system already knows.** It is not a third
   keyboard dialect. A remote keystroke arrives as a HID boot-keyboard report
   and goes through the same interpreter the P5's USB keyboard uses. So
   chords, typematic repeat, and the modifier and release edges behave
   exactly as they do locally, and `every scancode check needs BOTH dialects`
   does not become "all THREE dialects".

The protocol is RFB (RFC 6143). It was designed at the Olivetti Research
Laboratory in Cambridge in the late 1990s (AT&T Labs Cambridge after 1999)
so that a desktop could follow its user to any screen. The server pushes
nothing: the client asks for an update, and the server answers with whatever
changed since the last one. That pull model is why a slow link degrades
gracefully. Damage piles up in one place and goes out as fewer, larger
updates, never as a backlog.

**Phase 1 is the desktop (VT8). Phase 2 is the text VTs**, and the door below
is shaped so that phase 2 changes where pixels come from, not the ABI (see
§3's phase-2 note).

## The slices

Each slice gets its own worktree and branch, stacked on the previous one. A
review finding fixed low in the stack is merged forward.

| # | Slice | Ring | Review |
|---|---|---|---|
| 0 | This document | — | Chris |
| 1 | TCP loopback and per-address announce | kernel | Codex (net concurrency) |
| 2 | sshd: multiple channels and `direct-tcpip` | ring 3, security | Codex (auth/transport boundary) |
| 3 | `/dev/glass`, read side: the screen and its damage | kernel | Codex (ring boundary, lifetime) |
| 4 | `/dev/glass`, write side: HID keyboard and absolute pointer | kernel | Codex (ring boundary) |
| 5 | `vncd`: RFB 3.8, Raw encoding, input | ring 3 | in-house |
| 6 | ZRLE: sync flush in libgzip, and the encoder in vncd | ring 3 | in-house |

After slice 5 the machine is usable remotely. Slice 6 makes it pleasant.

---

## 1. Loopback — a NIC that is a queue

**The debt this pays.** SERVERS.md § Verification: *"the stack has no loopback
(a dial to our own address goes to the wire and is not looped back), so the
machine cannot accept its own connection."* The in-OS listener fixture has
been waiting on this, and so has sshd forwarding, because its target is a
local port.

**How it works.** `lo` is a `net_device_t` registered through the same seam
the e1000, virtio and RTL8125 use:

- Its `transmit` verb copies the frame onto a bounded in-memory queue and
  rings the doorbell. It never delivers inline. Delivering from inside
  transmit would re-enter `tcp_input` while the sender may hold
  `kTcpListLock`, and that path ends in a self-deadlock.
- Its `drain` verb (knet, the BSP daemon, DOORBELL.md) pops the queued
  frames and hands them to the receive path exactly as a NIC's RX ring
  does. knet already drains every registered device and already runs the
  TCP timers, so loopback needs no thread and no special case in TCP.
- IPv4 output routes `127.0.0.0/8` to `lo`. That network has been reserved
  for this since RFC 990 (1986) and RFC 1122 made it a MUST. No ARP is
  involved: the frame carries a fixed pseudo-Ethernet header so the receive
  path is the one every NIC already uses.
- MTU starts at Ethernet's 1500, so MSS and window arithmetic are exactly
  what the stack already runs. It is raised only if a measurement says
  loopback is the bottleneck. It will not be, because SSH's software AES
  will be.
- A full queue drops the frame and counts it, like a NIC with a full ring.
  TCP retransmits. The queue holds 1024 frames, enough for a whole 1 MiB
  window in flight, and `/sys/net/knet` prints its counters on an `lo:`
  line (knet drains it). `lo` is not in `kNetDevices[]`: that table's order
  is load-bearing (device 0 is the NIC the stack dials and DHCP runs on),
  and a boot with no card must still answer `NO_NIC` to a LAN dial. So knet
  drains `lo` by name, and knet now exists whenever networking does, card or
  no card.
- **Each TCP conversation knows its own address** (`local_ip`): zero means
  the machine's address, read live as before, and a loopback conversation
  carries its 127/8 address. Demux matches it, so the four-tuple includes
  our address.
- **A real card never delivers 127/8.** `ipv4_input` drops a 127/8 source
  or destination arriving on anything but `lo`, counted as
  `martians_dropped` in `/sys/net/ip`. Without that rule a forged LAN packet
  could reach a loopback-only door, or be demuxed into a live loopback
  conversation.

**Per-address announce.** `announce` requires `ip == 0` today ("every address
this machine has"), with per-address announce booked "for a second NIC".
Loopback is that second interface. `announce(tcp!127.0.0.1!5900)` matches
only segments addressed to 127.0.0.1, so a SYN from the LAN to port 5900
finds no listener and gets the RST any closed port gets. A port has ONE
listener whatever its address, so a loopback listener and a wildcard one on
the same port are `PORT_TAKEN`, not ranked. BSD's coexistence rule has no
consumer, and the port bookkeeping it would touch has been through many
review rounds. Any address other than `0` or `127.x.y.z` still returns
`BAD_DEST` until a machine has two real NICs.

**Out of scope for this slice** (one DEBTS row, § Networking): UDP and ICMP
to 127/8 are refused at dial as `BAD_DEST`, so `ping localhost` says no
rather than working; a dial to the machine's own LAN address still goes to
the wire; and the one-listener-per-port rule above. Nothing here needs any
of them.

**A bug loopback found on the way:** a dial waited for `ESTABLISHED` and
nothing else. A peer that answers and hangs up at once moves the
connection on to `CLOSE_WAIT` before the dialer's first 10 ms nap ends. On
loopback that happens every time, and a LAN server that speaks first and
closes can do it too. The dialer then waited out its 10-second timeout and
reported `TIMEOUT` for a call that had connected. `CLOSE_WAIT` now counts
as connected.

**Proof:** `/tests/looptest` in the ring-3 suite, the booked in-OS listener
fixture. It checks the refusals, announces on loopback, dials itself, moves
3 MiB each way with every byte checked, reaches the door as `localhost`,
shows that a wildcard door answers on loopback, and checks that the
client's close is the server's EOF. `looptest hold <address>` keeps a door
open so the host can knock through QEMU's port forward: `hold any` answers
the host's call, and `hold 127.0.0.1` answers nothing. The kernel test
`net_martian_dropped` hands `ipv4_input` a forged 127/8 packet from a card,
which the host cannot send through QEMU's user network.

## 2. sshd — channels, plural

The engine had one channel, in the most literal sense: `peer_channel`,
`peer_window`, `receive_window`, `sent_close` and the rest were scalars, and
any channel number other than 0 was a DISCONNECT. `ssh -N -L
5900:localhost:5900` opens no session channel at all, and a plain `ssh -L`
opens a session AND a forward. SSHD.md § Local forwarding is the record of
what was built. In brief:

- **The session stays local channel 0, with its reviewed fields untouched;
  forwards are a table beside it**, local channels 1–7. Every forward
  message looks its slot up by our id, and a free slot's id is a protocol
  violation (DISCONNECT, as before). An eighth open gets OPEN_FAILURE
  `RESOURCE_SHORTAGE`. The engine stays pure: it raises `FORWARD_OPEN`,
  `_DATA`, `_EOF` and `_CLOSE` events naming the slot, and the daemon does
  the I/O. An open is completed before the next packet is read, which is
  resize's contract.
- **`direct-tcpip`** (RFC 4254 §7.2) resolves the requested host with
  `os64_resolve`, the same resolver every dialer uses, and accepts only an
  address in `127.0.0.0/8`. The check is on the ADDRESS, never the spelling,
  so `localhost` works because `/etc/hosts` says `127.0.0.1 localhost` (slice
  1 added that line). A resolve blocks only the session asking, because each
  connection is its own process. Anything else gets OPEN_FAILURE
  `ADMINISTRATIVELY_PROHIBITED` with the text "forwarding reaches this
  machine's loopback only". An authorized key holder can already dial
  anywhere from a shell, so this is not a security boundary against them. It
  keeps the daemon from becoming a pivot that nobody asked for, and it is the
  smallest thing that serves the consumer. A refused dial gets
  `CONNECT_FAILED` with `os64_dial_reason`'s text.
- **`sshd.conf` gets `forward = loopback`** (the default) or `forward = none`.
  An unknown value refuses startup, following round 9's port rule: a file the
  operator wrote is never half-read. The listener hands each session the
  policy on its command line.
- **The relay lives in the existing event loop.** No new threads are needed,
  because network handles take `os64_read_for`/`os64_write_for` with zero
  patience:
  - Client to local: CHANNEL_DATA goes into the forward's queue (its 256 KiB
    window), drained with non-blocking writes. WINDOW_ADJUST is sent as
    bytes actually leave, the credit discipline stdin uses.
  - Local to client: one 32 KiB staging buffer per forward, read only when it
    is empty, so TCP's own window is the backpressure. The buffers are
    allocated when a forward opens: seven reserved up front did not fit
    sshd's link slot.
  - Forwards share one output queue, served round robin from wherever the
    queue last ran out, which keeps busy forwards within one staging buffer
    of each other. A forward held back by its own window is not that point:
    it would pin the rotation and hand every refill to the forward after it. The streams' "after the first sender" rule is exact for
    two and drifts for three or more; the harness shows both.
  - Local EOF becomes CHANNEL_EOF then CLOSE. Client CLOSE closes the local
    connection once the bytes the client sent before it are delivered. **A
    client EOF closes it whole** once the client's bytes are
    delivered, because ring 3 has no half-close (DEBTS). A viewer closes
    fully and loses nothing.
  - A closed session does not end the connection while forwards are live.
- **The loop naps only on a pass that moved nothing.** It used to end every
  pass with `os64_sleep(1)`, one tick, and to read at most 8 KiB. That is fine
  for a shell and a ceiling of about 800 KB/s for a framebuffer.
- **Proof:** `sshtest` (2330 checks, host and guest) gained a forward matrix.
  The daemon-loop harness gained `forward`, `forwardfair` and `linger`. The
  host adapter mirrors the daemon, and real OpenSSH `ssh -N -L` moved 3 MiB
  each way, again across client and server rekeys, along with seven
  concurrent forwards, the eighth refused, both refusals, and a session
  sharing the connection. In QEMU, host `ssh -N -L` through the port forward
  to `looptest echo 127.0.0.1` echoed 3 MiB each way with every byte
  matching, and `tools/sshd_probe.py` stayed green.
- **Paid:** the DEBTS row "SSH v1 intentionally lacks … forwarding" now names
  only remote forwarding and destinations beyond loopback. Concurrent
  *session* channels stay unsupported, because nothing needs them.

## 3. `/dev/glass`, read side — what the screen shows

**One handle is one viewer.** `open("/dev/glass", "r")` returns a
`HANDLE_GLASS` with its own record of what changed: a rectangle that touches
one already pending grows it, up to 16 separate ones, and past that they fold
into their union. It starts as the whole screen, so the first read is a full
frame. Two vncd sessions are two independent handles. A boot with no desktop
refuses the open.

**Why a handle alias and not a devfs file.** A glass read BLOCKS until the
screen changes, and a devfs file read runs in the borrowed kernel context
where sleeping corrupts the kernel. That is the reason `/dev/tty` is a
handle alias (devfs.h, THE ALIAS), so devfs's alias hook grew an object
out-parameter and a second customer. `/sys/gui` counts the open viewers
(`glass_viewers`), since a remote viewer is otherwise invisible from the
machine it watches.

**A read yields one damaged rectangle, with its pixels.** This is the shape
`announce` uses: *accept is a read*, and here *a frame is a read*.

```c
typedef struct {
    uint16_t x, y, w, h;        // the rect; w*h XRGB8888 pixels follow, row-packed
    uint16_t screen_w, screen_h;
    uint32_t flags;             // OS64_GLASS_TEXT_VT: a text VT holds the glass (phase 1)
} os64_glass_rect_t;            // packed, size and offsets static-asserted
```

- A read blocks until the accumulator is non-empty. `os64_read_for`'s
  patience works (`OS64_ERR_TIMEOUT` if nothing changed), and a caught signal
  ends the park, as it does everywhere (`signal_park_must_end`).
- A rect too big for the caller's buffer is returned as a band of whole rows
  from its top, and the remainder stays pending. A buffer that cannot hold
  the header plus one row is refused and never half-filled. The kernel side
  of a read is the read syscall's 1 MiB per-thread scratch, so a band is at
  most that: 255 rows of a 1024-pixel screen.
- **Where damage comes from:** the compositor's frame loop, right after
  `composite_locked` and still under `kGuiLock`, adds that frame's damage
  list to every open glass handle.
- **Why reading without the lock is still correct.** The reader takes its
  rect under `kGuiLock` and copies the pixels after releasing it, so a slow
  copy to user memory never stalls a frame, and a page fault in the copy
  never happens under a spinlock. A composite that overlaps the copy may
  tear what is sent, but that composite finished after our take. It
  therefore added its damage after our take, and the next read re-sends
  those pixels. The screen is *eventually exact*, which is what every
  shared-memory VNC server settles for, and the pull model makes "eventually"
  one request later.
- **The cursor is in the pixels.** The compositor draws it into the
  backbuffer last. Phase 1 lives with that: the viewer shows the real pointer
  one round trip behind the local one. The fix is RFB's Cursor
  pseudo-encoding, which needs a cursor-free image. It is booked below.

**Phase 1 and the text VTs.** The backbuffer is still composited when VT1–7
hold the glass, but input follows the glass, so showing the desktop would
send typing somewhere the viewer cannot see. While a text VT is focused,
every rect carries `OS64_GLASS_TEXT_VT`. vncd shows the desktop dimmed, with
a notice that a text terminal has the screen and Alt+F8 brings the desktop
back. Alt+F8 works remotely because §4 routes keys through the chord logic.

**Phase 2 (not built now, but the ABI is shaped for it).** `BasicRenderer`
keeps a RAM shadow of the console's pixels. Phase 2 makes the read side
serve *whichever RAM image is on the glass*: the backbuffer for VT8, the
renderer's shadow for VT1–7, with the renderer feeding damage the way the
compositor does. The flag stops being set, and vncd's dimmed notice goes
away. No record field changes.

**Proof:** `/tests/glasstest` checks that the first reads compose the whole
screen in whole-row bands, that a ten-row buffer gets ten rows, that a
buffer too small for one row is refused without losing the change, that a
window painted a known colour reaches the viewer pixel for pixel, that a
still screen times out, and that closing a viewer under a parked reader ends
the wait instead of freeing memory under it. It passes on a GUI boot, and
the ring-3 suite reports it as SKIP on a text boot (no desktop). By hand,
`glasstest watch` across Alt+F8 and Ctrl+Alt+F1 printed the whole frame
flagged `TEXT_VT`, the whole frame unflagged, one 52x48 rectangle for a
mouse move, and the flagged frame again.

## 4. `/dev/glass`, write side — hands

A viewer opened with mode `"u"` may also write, one input record per write
(`os64/glass.h` has the structs, packed and size-asserted). A viewer opened
`"r"` only watches, and a write to it is refused.

```c
OS64_GLASS_KEYBOARD: uint8_t kind; uint8_t report[8];                 // 9 bytes: a HID boot keyboard report
OS64_GLASS_POINTER:  uint8_t kind, buttons; uint16_t x, y;            // 6 bytes: ABSOLUTE; bits L/R/M
```

- **The keyboard is a virtual HID keyboard, one per viewer.** The report
  decoder, the usage tables, the chords and the typematic engine moved out of
  `xhci.c` into `hid_keyboard.c` (`hid_keyboard_t` carries the state). That
  was a behaviour-neutral commit of its own, proven on a QEMU USB keyboard
  (shifted text, history, Alt+F2/Alt+F1). xHCI owns one instance and each
  writable viewer owns one. So the chords are the ones a local keyboard
  gets, Alt+F8 included, and Ctrl+Alt+Del prints its polite note as it
  does locally. The modifiers a mouse packet carries are the machine's:
  Ctrl, Shift and Alt are held while any keyboard holds them (X's core
  keyboard), so a viewer holding Ctrl for a Ctrl+drag is not undone by a
  local key or another viewer's typing. A key event still carries its own
  keyboard's modifiers.
- **Typematic runs in the compositor's frame loop**, outside `kGuiLock`.
  Glass exists only where a desktop does, and that loop's nap is bounded by
  its core's scheduler timer. xHCI's poll could not do it, because it returns
  early on a machine with no xHCI, which is QEMU's default. Views with a held
  key are taken under `kGuiLock` with a reference each, then ticked after
  it drops. Delivery never happens under `kGuiLock`, because keystrokes reach
  the tty and the renderer, whose locks may not nest under it. Each view has
  its own input lock instead. A frame serves at most eight views with a
  held key and the start rotates, so a ninth repeats more slowly rather than
  never.
- **The pointer is absolute.** `input_inject_pointer(src, x, y, buttons)`
  sits beside `input_inject_mouse`. Both now end in one `pointer_locked`, so
  the clamp, the MOVE with its implied delta, and the button-edge diffing
  are the same code, and grabs, drags and WM chords see what a real mouse
  would produce. Buttons are held per source (`input_pointer_source_t`: each
  mouse driver and each view owns one), and a button is down while any
  source holds it, so a local mouse nudged during a remote drag does not end
  it, nor the reverse. There is no wheel, because nothing in os64 has one
  yet (DEBTS).
- **Closing the handle lifts every finger.** Close delivers an empty keyboard
  report and lifts the view's own buttons, under the view's input lock so
  that no write can land after it. A dropped connection does not leave Ctrl
  held or a drag grabbed, unless a GUI event queue (the input queue or the
  window's) is full at that moment and the releases are dropped with it
  (DEBTS § Remote access).
- **Validation is whole-record, at the boundary.** An unknown kind, a record
  whose length is not its kind's, a keyboard report listing one usage twice,
  a button bit above middle, or a position off the screen (refused, not
  moved) is refused.
- **A key a chord took stays the chord's until it is let go** (X's passive
  grab). A press the keyboard interpreter takes for itself (a terminal
  switch, Ctrl+Alt+Del, Caps Lock's toggle) is never delivered, nor is its
  release, nor any typematic repeat that no longer matches the chord: after
  Alt+F8 the desktop holds the screen, where bare Alt+F8 is no chord, and a
  held F8 would otherwise reach the focused window as F8 presses and then
  an F8-up. Repeats that still match keep acting (a held Alt+Right keeps
  walking the terminals). The PS/2 driver follows the same rule for its
  hardware repeats.
- **Proof:** `glasstest`'s hands half, on a GUI boot, from VT1:
  - a `"r"` viewer cannot type, and malformed records are refused;
  - its own glass keyboard types Alt+F8, and `/sys/gui` says the desktop
    holds the screen;
  - a window it creates receives `a` with its release, a shifted `A`, a left
    click at its centre, repeats of a key held for 900 ms, and the release
    of a key held by a second viewer that closed without letting go;
  - with two viewers, a second pointer moving with no buttons does not lift
    the first one's held button, and a second keyboard typing does not drop
    a Ctrl the first one holds (the old kernel fails both);
  - a Caps Lock held past the repeat delay toggles once; an Alt+Tab let go
    of in one report leaks no Tab; an Alt+F8 held past the repeat delay
    sends the window neither repeats nor a release; a report listing a key
    twice is refused;
  - Ctrl+Alt+F1 gives the screen back to the terminal.

  A QEMU mouse still moves the pointer through the shared path.

**Permission.** os64 has no users. Any process that can open `/dev/glass`
can see the screen, and with `"u"` type into it. That is consistent with a
machine where any process can already read `/proc/<pid>/mem`, and it is
written down in DIVERGENCES rather than left to be discovered.

## 5. vncd — RFB 3.8

- **Listens on `tcp!127.0.0.1!5900`** (port from `vncd.conf` on the ladder,
  the sshd rule for unreadable files). It **refuses to start** on any
  non-loopback address. One process per connection, the sshd and telnetd
  pattern, capped at 4.
- **The handshake:** `RFB 003.008`. Security types are `[None]`, then
  SecurityResult OK. ClientInit's shared flag is ignored, because every
  viewer is shared. ServerInit carries the screen size, native 32bpp
  depth-24 little-endian true colour (R<<16 G<<8 B), and the name
  `os64 on <HOSTNAME>`.
- **Client messages:**
  - SetPixelFormat: any true-colour format is translated per pixel with
    shift and max arithmetic. A colour-map format ends the session with a
    logged reason, because RFB 3.8 gives the server no way to refuse it.
  - SetEncodings: Raw always, and ZRLE after slice 6.
  - FramebufferUpdateRequest: incremental requests are served from
    `/dev/glass`, and a non-incremental one re-reads the named region.
  - KeyEvent: X11 keysym to HID usage, US layout (os64's only one). The
    modifier keysyms set the report's modifier byte. A repeat of a key
    already down is dropped, because the kernel's typematic repeats it, as it
    would a USB key. An unmapped keysym is logged once and ignored.
  - PointerEvent: absolute coordinates. Buttons 1–3 map to L/M/R, and 4–7
    are dropped.
  - ClientCutText is ignored until the clipboard slice.
- **Threads, the telnetd shape:** an inbound thread parses the socket and
  writes input records. The outbound thread waits on the glass while a
  request is outstanding, encodes, and is the ONLY writer of the socket. A
  TCP write copies what fits and resumes, so two writers could interleave
  inside a message (SERVERS.md § 3). Nothing the client sends needs a reply
  from the inbound side.
- **Launch:** `vncd &` at a prompt, or a `VNCD` command-line token that
  mirrors `SSHD`. **A token does not travel to the P5**: the P5's boot entry
  has to be edited there.
- **Proof:** host `tools/vncd_probe.py`, a small pure-Python RFB client. It
  runs the handshake, requests a full frame and compares it with QEMU's
  `screendump`, requests incremental updates across a window drag, requests
  a 16bpp format, and types into gterm and reads it back through the screen.
  The run goes through the real path: host `ssh -L` → QEMU hostfwd → sshd →
  loopback → vncd. Then a real viewer: TigerVNC.

## 6. ZRLE — the pleasant one

ZRLE (RFC 6143 §7.7.6) is 64×64 tiles, each raw, solid, packed-palette or
RLE, all through ONE zlib stream for the life of the connection, with a sync
flush after each rectangle. The desktop is mostly flat colour and text, so
most tiles are solid or small-palette, and RLE plus DEFLATE does very well
on them.

- **libgzip gains `os64_deflate_flush`**: finish the current block, then emit
  the empty stored block (`00 00 FF FF`), byte-aligned, with the history
  window kept. That is zlib's `Z_SYNC_FLUSH`, and it is the one verb missing.
  vncd writes the two-byte zlib header itself. The stream never ends, so no
  Adler-32 trailer is ever sent.
- The encoder is fixed-Huffman, as it is today. Dynamic Huffman is a later
  improvement *only if* a measurement says the link is the bottleneck.
- **Proof:** host tests where Python's `zlib.decompressobj` inflates a
  sync-flushed stream at every flush point, plus a host ZRLE decode of vncd's
  tiles compared with the source pixels. TigerVNC is the live client.

## Using it (the Windows side)

```sh
# once: the key must be ECDSA P-256 and enrolled in /home/authorized_keys (SSHD.md)
ssh -N -L 5900:localhost:5900 -i ~/.ssh/id_ecdsa os64@<p5>
# then point the viewer at localhost:5900
```

Windows has shipped an OpenSSH client since Windows 10 1809. TigerVNC's
viewer is a single `.exe`.

## Booked, not built (DEBTS rows arrive with the slices)

- **Phase 2: the text VTs** — §3's note.
- **Clipboard both ways**: ClientCutText and ServerCutText to and from
  `/sys/clipboard`. It is small and needs a change notice from the
  clipboard.
- **Cursor pseudo-encoding**: a cursor-free read, so the viewer draws the
  pointer locally with zero lag.
- **CopyRect**: the compositor knows when a window moved, and a viewer could
  copy those pixels instead of receiving them again.
- **Wheel**: first the local mouse drivers, then VNC buttons 4 and 5.
- **Dialing the machine's own LAN address** through loopback.
- **A multi-handle wait** (the "future poll/event story", NETWORK.md). vncd's
  two threads and sshd's polled loop are the workarounds, and they are the
  demand record for it.

## Rulings (Chris, 2026-09-22)

1. **`/dev/glass` and `vncd`.** "Not the most obvious name in the world, but
   it is right here, right now."
2. **Forwarding reaches loopback only** (`forward = loopback | none`), with
   `localhost` working as a name. It does, through `/etc/hosts` (§2).
3. **VNC security None.** sshd handles authentication.
4. **Overnight stacking:** each slice is verified in QEMU, committed on its own
   branch, pushed, and opened as a PR against the previous slice's branch.
   Anything that cannot be done the standard way is deferred, not invented
   around.
5. **The `codex/frame-studio` overlap** in `compositor.c`: either may land
   first, and the second one merges.

**Taking the glass back without the P5's keyboard** was asked for at the same
reading. §4 already gives it: remote keys pass through the same chord logic
as a local USB keyboard, so **Alt+F8** from the viewer brings the desktop
back from any text VT, and Alt+Left/Right walk the terminals. No vncd command
is needed. One viewer-side caveat: TigerVNC uses F8 as its own menu key by
default, so if the viewer takes the chord for itself, change the viewer's
menu key. It is not an os64 problem.
