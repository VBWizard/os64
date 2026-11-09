# Mouse wheel author validation — 2026-09-25

Branch: `codex/mouse-wheel`, based on `3aa02410`.
Scope and remaining hardware questions: [MOUSE_WHEEL.md](../../MOUSE_WHEEL.md).

The gallery and `sha256.txt` below record the initial implementation.
The later P5 boot-protocol failure and descriptor-based follow-up have
[separate evidence](report-protocol/README.md).

## Host and build

- Strict `make -j8`: kernel, userland, disk image and ISO passed.
- `ASAN_OPTIONS=detect_leaks=0 tools/test_mouse_wheel_host.sh`: passed.
  This compiles the actual PS/2, USB HID, input, tty and text-overlay sources
  with stubbed port I/O, interrupt masking and rendering. It checks plain
  and wheel negotiation, a lost ACK after protocol transition, bounded
  no-device refusal, framing/resync, overflow, signed extremes, short USB
  reports, input order, shared buttons, text-only routing and VT bounds.
- The overlay regression deliberately paints before a deferred flush and
  verifies another overlay paint after the flush. It also checks opposing
  wheel steps ending at the same offset. Removing the flush's generation
  increment from a temporary source copy makes this test abort at its
  assertion; the production source passes under ASan/UBSan.
- Real-font widget suite: **1,482 checks, 0 failures**. Wheel tests cover
  hit testing, bubbling, disabled/hidden targets, blurred windows,
  focus/grab/caret/selection preservation, callback delivery and bounds.
- Scribe suite: **2,470 checks, 0 failures**.
- Appearance suite: passed, including font-page, session, responsive-layout,
  palette, customizer, Control Center and pointer-queue contracts.
- `git diff --check` and `tools/stale_refs.sh`: clean.

The small `*-host.txt` files are the actual suite output.

## QEMU

Fresh private copies of the built disks; Q35, 8 GiB, 8 CPUs,
`qemu64,+rdrand,+rdseed`, no guest network, named-pipe QMP. GUI tests selected
Limine's second entry (FAT root, GUI, DEBUG_GUI); text-only tests used the
first entry (ext2 root). USB tests additionally used
`-device qemu-xhci -device usb-mouse`; `info mice` confirmed QEMU HID Mouse
as the active source. PS/2 boot logging reported ID 3 and four-byte packets.

QMP wheel stimulus was `input-send-event`, first with
`{"events":[{"type":"btn","data":{"down":true,"button":"wheel-down"}}]}`,
then the matching `down:false`; `wheel-up` reverses it. These are physical
input events through the emulated device, not injected os64 GUI events.
Pointer movement used bounded relative increments, with pauses for delivery.

Observed and checked by screenshot comparisons (`observations.txt`):

- PS/2 and USB: two down notches move Appearance's color list six rows and
  its companion scrollbar; two up notches restore the list and bar exactly.
  Selected color stays unchanged. Additional notches at the bottom clamp.
- PS/2: two down notches move Scribe's `/etc/menu.conf` view six rows; up
  restores the document viewport exactly, without moving its caret. Scribe's
  existing horizontal-width discovery can enlarge its horizontal bar when
  new lines become visible; that is outside the document-viewport comparison.
- An exposed part of Appearance scrolls while Scribe retains focus and its
  entire window stays pixel-identical. Wheeling over Scribe's titlebar leaves
  the whole screen unchanged.
- Text-only boot: VT1 through VT7 each display `cat /etc/menu.conf`, move
  into history on wheel-up and return to the same visible text on wheel-down.
  Comparisons exclude the blinking cursor/status marker. Oldest-history
  clamping and typing to return to the live screen also passed.
- Shift+PgUp/PgDn still navigate history. USB also scrolls VT2 in a GUI boot.

The initial gallery establishes those end-to-end paths. After the final
repaint-notification repair, a fresh USB GUI boot repeated list scrolling
and text scrolling with a stationary text pointer. The `final-usb-*` images
show that final run, including the pointer restored after deferred repaint.
Six additional up/down cycles with a stationary pointer each changed the
history view and restored the visible text exactly. Pixel inspection also
confirmed the inverse pointer cell in both views; an apparent missing-text
anomaly during visual inspection was a misreading of the capture.

These are author tests. Independent review and the P5's wireless mouse test
are not claimed here. Gterm scrollback is a separate planned feature.
