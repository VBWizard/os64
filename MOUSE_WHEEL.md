# Mouse wheel

The wheel is an input primitive for Yonder and existing scrolling views.
VT1 through VT7 use their existing scrollback buffers. Gterm has no history
buffer; adding one is a separate follow-up after these existing consumers.

## Contract

`OS64_GUI_EVENT_MOUSE_WHEEL` / `INPUT_EVENT_MOUSE_WHEEL` is event **13**.
The event stays 32 bytes. `mouse.x/y` are the pointer position, translated
to content-local coordinates for clients. `dy` is signed notches, positive
toward the user / scroll down; `dx` reserves positive-right tilt and is zero
from both drivers. Buttons are the combined state of pointing devices;
modifiers are sampled once for the packet. Move and button edges precede
the wheel under the input lock. Zero wheel deltas produce no wheel event.

PS/2 negotiates IntelliMouse with rates 200/100/80 and reads the ID before
streaming. ID 0 selects three bytes; ID 3 selects four, with a signed wheel
byte. A failed rate/ID exchange gets a bounded defaults-and-ID recovery.
Defaults does not reset the device's negotiated protocol: the driver must
verify ID 0 or 3 even after recovery. If it cannot, initialization declines
instead of guessing a packet length. ID 4 is unsupported. Header and gap
resynchronization apply at the selected packet length; overflowing motion
is discarded while valid button and wheel state survive.

USB boot-capable mice first enter boot mode. The driver then reads their
HID report descriptor and selects report protocol when it can identify one
complete relative X/Y/wheel report with button 1. Fields are located by
usage and bit offset; report IDs, packed axes up to 16 bits, padding, usage
ranges and global Push/Pop are supported. Other IDs and truncated reports
are ignored without changing buttons. Unsupported or ambiguous descriptors
retain boot mode and its optional signed fourth-byte wheel convention.
If the switch to report protocol fails with an EP0 STALL, the driver clears
the halt and explicitly restores boot mode. Other switch failures, or an
unconfirmed restore, refuse initialization. The existing compatibility path
for devices that reject the initial boot-protocol request is unchanged.

The parser is bounded to 1 KiB of descriptor data, eight nested collections
and global saves, 32 local usages and four candidate mouse report IDs. The
selected report must contain movement, buttons and wheel together. Input
reports must fit within 64 bytes and one endpoint packet. Each interrupt TD
requests at most the endpoint's maximum packet size, so successive full-size
reports cannot be combined. Wheel sign is inverted for os64; negating the
most negative 16-bit value saturates to the positive ABI limit.
The wire conventions and defaults behavior were checked against upstream
[QEMU PS/2](https://github.com/qemu/qemu/blob/master/hw/input/ps2.c) and
[QEMU HID](https://github.com/qemu/qemu/blob/master/hw/input/hid.c).

## Routing and consumers

A GUI wheel goes to the implicit pointer-grab owner, or to the content of
the topmost window under the pointer. It does not raise or focus a window.
Frames and window-management gestures consume it without an action. A
queued GUI wheel is discarded if a text VT takes over before it is drained.

Libui hit-tests the widget under the pointer and bubbles unhandled wheels
to parents, skipping disabled controls. It does not change keyboard focus
or button grabs. Lists and textviews scroll three rows per notch without
moving the selection or caret; scrollbars move one application unit per
notch along their axis. A vertical wheel over a horizontal scrollbar moves
that bar. Viewport owners consume the wheel at their limits. Listboxes'
optional `on_view` callback synchronizes companion bars independently of
selection changes, including in Appearance and Frame Studio.

Hovered integer sliders adjust by their configured step per notch: up/right
increases, down/left decreases. Values clamp to the range and callbacks fire
on changes. Wheel input preserves keyboard focus and is consumed without
adjustment while a pointer grab is active, keeping the drag in control.

On VT1..VT7, the wheel changes the focused VT's existing history offset by
three rows per notch and clamps to available history or the live screen.
This follows the terminal focused at arrival, like the keyboard scrollback
chord. It takes the tty lock and marks the glass dirty, leaving repainting
to `tty_flush_if_dirty`; the compositor lock and renderer lock are not taken
by this input path. PS/2 starts on text-only boots as well as GUI boots.
Shift+PgUp/PgDn retain their existing half-screen steps. Deferred repaints advance the
terminal generation, as explicit repaints already do, so a stationary mouse
overlay is restored even if it painted before the flush or opposite wheel
steps returned to the same offset.

## Follow-up

Gterm scrollback comes after this work: add history storage and a viewport,
then wheel and Shift+PgUp/PgDn, with selection defined across history and
resizing. It is not an existing viewport that can be wired up here.

Five-button PS/2, horizontal device tilt, high-resolution scrolling,
additional USB HID layouts (including split reports and absolute pointers),
and `/dev/glass`/VNC wheel transport are separate extensions. Chris confirmed
working console scrolling and Frame Studio title-font list scrolling on
the P5 after installing the report-protocol build.

## Validation

### Wireless receiver investigation

The P5 trace after the diagnostic reboot identified mouse receiver
`03f0:a407`, interface 1, endpoint 2, maximum packet size 9. It accepted
SET_PROTOCOL boot and emitted length-three mouse reports; no other report
length was logged during the user's movement and wheel tests. Movement and
clicks worked, but VT and Frame Studio wheel scrolling did not. This exposed
the initial implementation's reliance on an optional boot wheel byte.

The descriptor-based implementation adds bounded `DEBUG_USB` traces of the
raw descriptor, selected report ID/length/axis offsets, report lengths and
up to eight decoded nonzero wheel samples. Raw report samples show at most
eight bytes; bytes beyond the reported length are zero padding. Keep
`DEBUG_USB` on the selected Limine entry for the next P5 test and inspect
`/home/os64.log`. QEMU and host results for this change are recorded in
[report-protocol evidence](docs/mouse-wheel-evidence/report-protocol/README.md).
Chris subsequently confirmed working console and Frame Studio title-font
list scrolling on the P5. This is user-reported hardware evidence; the
working boot did not enable DEBUG_USB, so its parsed layout was not logged.

### Feature validation

- Strict full kernel/userland/ISO build.
- `ASAN_OPTIONS=detect_leaks=0 tools/test_mouse_wheel_host.sh`: actual driver
  decoders, negotiation recovery, packet framing and timeout resync, input
  ordering, shared buttons, short USB reports, signed extremes, VT bounds,
  and restoration of text overlays after deferred repaints. Removing the
  repaint notification makes the overlay regression assertion fail.
- `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real`:
  actual widget dispatch, focus/grab preservation, bubbling, hidden/disabled
  targets, clamping, viewport callbacks, and selection preservation, alongside
  the existing real-font regression suite.
- Appearance regression suite: `ASAN_OPTIONS=detect_leaks=0 tools/test_appearance_host.sh`.
- Scribe regression suite: `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py`.
- `git diff --check` and `tools/stale_refs.sh`.

QEMU observations, selected screenshots, and host results are in
[docs/mouse-wheel-evidence/README.md](docs/mouse-wheel-evidence/README.md).
Slider-specific results are in
[slider evidence](docs/mouse-wheel-evidence/sliders/README.md).
These author tests and Chris's reported P5 results are distinct from
independent review.
