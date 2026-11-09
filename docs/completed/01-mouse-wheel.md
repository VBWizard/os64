# 01 — The mouse wheel

## Completed — 2026-09-25

Delivered in [PR #134](https://github.com/VBWizard/os64/pull/134).
The current contract is [MOUSE_WHEEL.md](../../MOUSE_WHEEL.md), with
[validation evidence](../mouse-wheel-evidence/README.md).

The packet below is the original planning handout, preserved as history.
Its scope changed during implementation: VT1 through VT7 gained wheel
scrollback, USB gained descriptor-driven report protocol for the wireless
receiver, and libui sliders gained wheel adjustment. Gterm has no scrollback
buffer; that work remains a separate follow-up recorded in
[DEBTS.md](../../DEBTS.md).

## Original packet

*GRAPHICS.md's known-limitation #6, promoted to a packet because a browser
you scroll with a bar is not a daily driver. yonder is the customer that
asked; gterm's scrollback, Scribe, the listbox and every future page view
are the customers that were waiting.*

## Where it stands

- **PS/2** (`kernel/src/driver/system/mouse.c`): `mouse_init` sends Set
  Defaults (0xF6) and Enable Streaming (0xF4) and assembles 3-byte packets
  in the IRQ12 handler, with a resync rule for a lost byte (bit 3 of byte 0
  is always set). No IntelliMouse handshake, so the mouse never enters
  wheel mode and the fourth byte never exists.
- **USB HID** (`kernel/src/driver/system/usb/xhci.c`): boot-protocol
  mouse; `hid_process_mouse_report` reads bytes 0..2 of an 8-byte report
  buffer (`HID_REPORT_BYTES`). The HID boot mouse report is defined as
  three bytes, but nearly every mouse made since the IntelliMouse appends
  the wheel as a signed byte 3 even in boot protocol, and every host reads
  it (Linux's `usbmouse.c` does `data[3]`). The byte is already in our
  buffer; nobody looks at it.
- **The event** (`kernel/include/gui/input.h`, `abi/include/os64/gui.h`):
  `INPUT_EVENT_*` and `OS64_GUI_EVENT_*` are numbered identically and
  static-asserted against each other. `input_inject_mouse(src, dx, dy,
  buttons)` in `gui/input.c` turns a packet into MOVE plus one DOWN/UP per
  changed button. The compositor delivers mouse events to the window UNDER
  THE POINTER (`deliver_mouse_to_window`), which is the routing a wheel
  wants: you scroll what you are pointing at, not what has focus.
- **libui** dispatches `OS64_GUI_EVENT_MOUSE_*` to widgets; the scrollbar,
  listbox and textview each own a viewport (`os64/ui.h`).

## Scope and ownership

Own `mouse.c`, the mouse half of `xhci.c`, `gui/input.c`/`input.h`, the
ABI event vocabulary in `os64/gui.h`, the compositor's routing of the new
event, and libui's consumption of it (`ui.c`'s dispatch, scrollbar,
listbox, textview; gterm's scrollback is the first application consumer).
The kernel half runs in interrupt context and touches the ABI, so it is
Fable-tier for review whoever writes it. Do not touch vt_select's text-mode
mouse handling except to ignore the new event there.

## Deliverable

**One new event, `OS64_GUI_EVENT_MOUSE_WHEEL` (13) / `INPUT_EVENT_MOUSE_WHEEL`,
static-asserted like the rest.** It reuses the `mouse` member of the event
union unchanged, so the ABI struct does not grow: `x`/`y` are the pointer
position (content-local by the time an app sees it, exactly as for a click,
and what the compositor routes on), `dy` is the wheel delta in NOTCHES —
positive when the wheel rolls TOWARD the user, which is "scroll down" in
every browser and every toolkit since Windows 95 defined it — `dx` is the
horizontal tilt (0 from both drivers today; the field exists so a tilt
wheel is a driver change and not an ABI change), `buttons` and `modifiers`
as on any mouse event. The unit is notches and not pixels or rows: how far
a notch scrolls is the application's business (three rows is the
convention the IntelliMouse shipped with in 1996; a page view will choose
something in pixels).

**PS/2:** after Set Defaults, run the IntelliMouse sequence — Set Sample
Rate (0xF3) 200, then 100, then 80 — and Get Device ID (0xF2). An ID of 3
means the mouse switched to the 4-byte packet: byte 3 is the signed wheel
delta. An ID of 0 means a plain mouse; keep the 3-byte packet. Packet
assembly and the resync rule grow to the packet length the ID decided;
`s_packet[]` sizing and the index reset both follow it. The handshake runs
inside the existing interrupts-off window, and each step's ACK is checked
the way `mouse_send` already does — a mouse that stalls a step is a plain
mouse, never a hang. QEMU's PS/2 mouse answers the sequence, so this is
testable in the harness. **The 5-button variant (ID 4, a second sequence
200/200/80, wheel in the low nibble of byte 3) is BOOKED, not built:** it
is a different packet layout and nothing here has five buttons.

**USB:** read the TRB's transferred length and, when at least four bytes
arrived, take byte 3 as the wheel delta. A three-byte report is a mouse
with no wheel, not an error. Report protocol (the descriptor-driven layout,
the only way to get a wheel from a mouse that really does send three bytes
in boot mode) is BOOKED; the P5's wireless mouse decides whether it is
needed, and Chris tests that.

**input.c:** `input_inject_mouse` grows a wheel argument (or a sibling
`input_inject_wheel`) and emits one WHEEL event per packet that carries a
non-zero delta, AFTER the move it came with, at the position the move
landed on. `input_release_pointer` has nothing to release: a wheel holds
no state.

**Compositor:** route WHEEL like a button event — to the window under the
pointer — including through the implicit grab rules already in
`deliver_mouse_to_window`, and through decorations only if a decoration
scrolls (none does; a frame swallows it). Text VTs ignore it.

**libui:** `os64_ui` dispatches WHEEL to the widget under the pointer
(hit-test, not focus). A widget that owns a viewport scrolls it: scrollbar
(notch × one line's worth, moving `pos` and firing `on_scroll`), listbox
(three rows), textview (three rows). gterm scrolls its scrollback the same
way a Shift+PgUp does. A widget that does not handle it lets it fall to
its parent, so a wheel over a button inside a scrolling panel still
scrolls the panel.

## Required evidence

- Host: none of this is pure computation except the packet decoder;
  worth a tiny host test of the 4-byte assembly + resync if it is factored
  as a function, and not worth contorting the ISR to get one.
- QEMU, PS/2: the boot log (under `DEBUG_GUI`) says which ID the mouse
  answered; drive the wheel through QMP `input-send-event` (`btn` with
  `wheel-up`/`wheel-down`) over the monitor pipe the harness already
  exposes, and screendump gterm's scrollback moving and a listbox in
  Appearance Workshop scrolling. Also QEMU with `-device qemu-xhci -device
  usb-mouse`, which routes input to USB, for the byte-3 path.
- The delta's sign, checked by eye on the screendump: wheel toward the
  user moves the content UP (you are scrolling down). Getting this
  backwards is the classic and it survives every unit test.
- P5: Chris, with the wireless mouse over the dongle — the boot-protocol
  byte-3 assumption is the thing only real hardware can confirm.
- The `_Static_assert` pair for the new number, on both sides.

## Booked out of this packet, by name

| What | Why | Trigger |
|---|---|---|
| 5-button IntelliMouse Explorer packet (ID 4) | different layout, no consumer for buttons 4/5 | back/forward on the mouse, when yonder wants them |
| HID report protocol for the mouse | boot protocol + byte 3 covers nearly every mouse; report descriptors are a parser | a real mouse whose boot report has no wheel |
| Horizontal tilt | `dx` is carried; no driver fills it | a tilt wheel in the house |
| Wheel in the text VTs | scrollback there is keyboard-driven and nobody asked | somebody asks |
| Smooth/high-resolution scrolling | notches are what both wire protocols carry | a device that reports finer than a notch |
