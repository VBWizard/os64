# Bluetooth discovery

The first operational Intel AX210 USB adapter (`8087:0032`) stays attached after
firmware bring-up. Discovery uses the existing polled xHCI transport. USB interrupt
wiring remains a separate debt.

## Using discovery

Put devices into discoverable/pairing mode, then run:

```sh
echo scan > /sys/bluetooth/scan
cat /sys/bluetooth/scan
cat /sys/bluetooth/devices
```

`scan` reports initialization, Classic inquiry, LE scanning, completion or failure.
A normal dual-mode scan takes about 21 seconds plus command overhead. Read the
files again to see progress: each open is a consistent snapshot. Discovery does
not pair or connect. Classic devices must answer inquiry, and LE devices must
advertise to appear. This is not a radio spectrum analyzer.

`devices` lists up to 64 distinct transport/address-type/address combinations,
with RSSI when supplied, Classic class-of-device bytes, and advertised names.
Names can arrive in Extended Inquiry Response or LE advertising/scan-response
fields. Names are limited to 63 bytes and displayed as printable ASCII, with
other bytes replaced by `?`. Missing names remain explicitly unnamed; discovery
does not issue Classic Remote Name requests. LE class bytes are zero because LE
advertisements do not supply the Classic class-of-device field. Random LE
addresses may rotate; the listing does not resolve private addresses to identity.

A new scan replaces the previous list. Requests while busy, absent or failed are
refused. The command is bounded to 32 bytes, accepts surrounding whitespace, and
is submitted on close so separate word/newline writes work. An empty write handle
closed without data does nothing. `devices` is read-only. No operational adapter
is reported explicitly. A read racing a USB poll may report `USB busy; retry this
read` rather than waiting under a scheduler-related lock.

Periodic discovery and device-cache policy are recorded in DEBTS.md. No periodic
radio scanning is enabled by this slice.

## Controller and transport

Firmware loading and its boot-time receive recovery precede runtime discovery.
Successful bring-up transfers the device and transport state into controller-owned
storage; failed bring-up disables the slot before releasing DMA allocations, or
retains them if disabling fails. The retained adapter owns its DMA storage for
its remaining boot lifetime, including disconnect and runtime failure.

Each requested scan resets normal HCI state, reads supported features and the
controller address, and configures event masks. Classic inquiry uses GIAC for
8 times 1.28 seconds, with extended/RSSI inquiry mode selected from capabilities.
LE uses active legacy scanning on the primary 1M advertising channels, a 60 ms
interval and 30 ms window, public own address, no accept-list restriction, and
controller duplicate filtering. It runs for ten seconds before Scan Disable.
Extended/coded-PHY advertising is not decoded by this slice.

The runtime state machine submits one EP0 command at a time and waits across
poll passes for both USB completion and the matching HCI reply. Inquiry uses
Command Status followed by Inquiry Complete. Interrupt IN carries operational
HCI events; bulk IN is not decoded as events after the firmware handoff. Receive
rearming is deferred until after the event drain to bound radio work per pass.
The decoder allocates no memory, waits on nothing and sanitizes untrusted names.

Command waits are bounded to two seconds, Classic completion to twelve seconds,
and the overall procedure to forty seconds before cleanup. A protocol failure
attempts HCI Reset once when its command DMA is no longer in flight. A transport
failure or an unsuccessful cleanup cannot prove that radio scanning stopped;
status preserves `discovery may be active: yes` when appropriate. A failed
adapter requires reboot before another scan; DMA storage remains owned.
Timing depends on scheduler polling continuing to run, as recorded by the xHCI
dispatch-frequency debt.

## Validation

`tools/test_bt_scan_host.sh` exercises HCI sequencing, command credits/replies,
USB-completion ordering, deadlines and cleanup, report bounds, fragmentation,
deduplication, name updates and hostile data. `tools/test_bt_loader_host.sh` runs
production xHCI rings and the firmware sequence against a simulated controller,
then checks retained state and runtime discovery without additional allocation.
`tools/test_bt_intel_host.sh` covers firmware wire/container parsing, and
`tools/test_mouse_wheel_host.sh` covers existing HID decoding.

The P5 confirmed cold firmware bring-up at commit `9b21368b`. Runtime discovery
requires its own hardware validation; simulated HCI success does not prove what
will be discoverable around a real desk.
