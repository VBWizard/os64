# Bluetooth discovery and LE keyboard input

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

## LE boot keyboard session

Put the keyboard's LE ("BT 5.0") slot in pairing mode. Select its current address
and address type from `devices`, then request a connection:

```sh
echo connect random f8:2c:fe:ff:f0:1a > /sys/bluetooth/connection
cat /sys/bluetooth/connection
```

The address above was observed on the P5; it is not a default or an automatic
connection target. `public` and `random` are distinct address types. Read the
connection file again while pairing. When it displays a six-digit passkey, type
that code **on the Bluetooth keyboard**, including leading zeroes, then Enter.
The passkey window lasts up to sixty seconds. `keyboard ready` means link
encryption, HID service discovery, Boot Protocol selection and notification
subscription succeeded. `key reports` counts matching notifications; ordinary
keys then use the existing HID input path, including modifiers and repeat.

```sh
echo disconnect > /sys/bluetooth/connection
```

Disconnect resets the Bluetooth controller and releases held keys. Once status
returns to `idle`, another scan or connection can be requested. Scanning is
refused during an active connection or connection attempt. Connection commands
are bounded to 64 bytes, validated as a whole on close, and never wait for radio
or USB completion in the syscall. Empty writes do nothing. Invalid/busy requests
are refused. Status preserves the seven public Pairing Response capability bytes
and identifies the unsupported field when negotiation is refused; these remain
available after cleanup. One peer is supported. No implicit pairing, automatic reconnection,
or selection by an advertised name occurs.

The `connect` command negotiates **LE legacy Passkey Entry**, with a 16-byte key,
a DisplayOnly host and a KeyboardOnly/KeyboardDisplay peer. It refuses to fall
back to Just Works. For a keyboard that requires pairing without a passkey,
select that association method explicitly:

```sh
echo connect-justworks random f8:2c:fe:ff:f0:1a > /sys/bluetooth/connection
cat /sys/bluetooth/connection
```

`connect-justworks` advertises NoInputNoOutput with no MITM requirement, uses the
Core's all-zero Temporary Key (TK), and does not generate or display a passkey.
The link still requires successful encryption before HID discovery/input. Status
labels the selected mode `legacy Just Works (unauthenticated)`. A peer that
requires authenticated pairing is refused by this mode. The address above is the
unnamed P5 keyboard's LE slot, not the ARTECK's Classic address.

Both modes require a 16-byte key, use the controller's LE Rand and LE Encrypt
commands for fresh nonces and the Core c1/s1 functions, verify the peer's confirm
value, and wait for successful Encryption Change before discovering HID. Neither
mode is LE Secure Connections. Legacy Just Works does not protect the pairing
exchange against passive listeners or active impersonation; legacy passkey
pairing also lacks Secure Connections' resistance to passive capture. Pairing is
session-only: no bond or identity keys are distributed or stored, so a later
connection requires pairing again.

The keyboard must expose HID service 0x1812, Boot Keyboard Input 0x2a22 with
notifications and its CCC descriptor, and Protocol Mode 0x2a4e with Write Without
Response. Generic Report Map interpretation, consumer/media keys, LED output,
mice, Classic HID and audio are not implemented. An unsupported service reports
an explicit error instead of treating an arbitrary eight-byte report as keys.

Connection initiation has a twenty-second wait; HCI commands have two seconds,
ATT requests and ACL completion have five seconds, and setup has an overall
150-second bound. Runtime errors release held keys and attempt a bounded HCI
Reset after outstanding USB transfers finish. Failure status is retained and
requires reboot; it never claims the link is inactive unless a disconnect or
successful reset proves it. USB DMA remains allocated if transport fails.

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
HCI events; bulk IN carries ACL data after the firmware handoff. Receive
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

LE input has separate USB packet, HCI ACL and L2CAP framing. Receive storage is
bounded (1024-byte HCI ACL payload, 64-byte L2CAP payload); ATT uses the default
23-byte MTU. The transmit queue is bounded to eight packets and uses one
controller ACL credit at a time, returned by Number Of Completed Packets. USB
completion independently gates DMA reuse. EP0 commands and bulk OUT have disjoint
regions in the retained transmit page. Protocol decoders allocate no memory or
perform blocking waits. Optional peripheral L2CAP connection-parameter updates
are rejected, retaining the central's chosen interval. Input stops on encryption
loss or disconnect, with an empty report releasing held keys.

## Validation

`tools/test_bt_scan_host.sh` exercises HCI sequencing, command credits/replies,
USB-completion ordering, deadlines and cleanup, report bounds, fragmentation,
deduplication, name updates and hostile data. `tools/test_bt_loader_host.sh` runs
production xHCI rings and the firmware sequence against a simulated controller,
then checks retained state and runtime discovery without additional allocation.
`tools/test_bt_intel_host.sh` covers firmware wire/container parsing, and
`tools/test_mouse_wheel_host.sh` covers existing HID decoding.

`tools/test_bt_le_host.sh` uses an independent OpenSSL-backed controller/peer
simulation and Core c1/s1 example values. It exercises authenticated pairing,
explicit Just Works with the P5 capability bytes, encryption, paginated GATT
discovery, key notification, disconnect, malformed
framing, unsupported keyboards, wrong confirmation, command rejection, credits
and USB ordering. OpenSSL is a host-test dependency, not linked into the kernel.
The production xHCI harness also checks bulk receive delivery/release and
non-overlapping control/ACL DMA storage.

The P5 confirmed cold firmware bring-up at commit `9b21368b` and discovery at
`76c6eedc`: fifteen LE addresses on the first scan, followed by eighteen results
including `BT 3.0 Keyboard?` (Classic) and `BT 5.0 Keyboard` (LE). Both scan status
reads reported complete, no malformed reports, no error and discovery inactive.
The first P5 LE attempt established a connection and received a Pairing Response,
then refused capabilities outside the initial pairing policy and reset cleanly.
That build did not retain the rejected capability bytes; the diagnostic view
preserves them for the next attempt. That attempt returned
`02 03 00 01 10 00 00`: NoInputNoOutput, no OOB data, bonding requested,
16-byte key, and no distributed keys. The passkey-only request was refused
without starting encryption; cleanup reported the link inactive.
The explicit Just Works path is covered by the simulated peer using those
capability bytes. Pairing, encryption and typing still require their own P5
validation; simulated success does not establish keyboard compatibility.

Wire references: [Bluetooth Core Security Manager](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/security-manager-specification.html),
[ATT](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/attribute-protocol--att-.html),
[L2CAP](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/logical-link-control-and-adaptation-protocol-specification.html),
and [Linux v6.12 HCI definitions](https://github.com/torvalds/linux/blob/v6.12/include/net/bluetooth/hci.h).
