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

## LE keyboard session

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
The passkey window lasts up to sixty seconds. `HID configured; awaiting input`
means link encryption, HID discovery, keyboard report selection, subscription
and settings readback succeeded. Status becomes `keyboard ready` after a matching
input notification is decoded. `key reports` counts these notifications; keys
use the existing HID input path, including modifiers and repeat. Host setup
status does not establish the keyboard's own pairing UI state or a saved bond.

```sh
echo disconnect > /sys/bluetooth/connection
```

After subscription, CCC must read back as 0001; Protocol Mode is checked against
the selected mode when the characteristic advertises Read. The readback values
and successful inspection count are retained in status. A mismatch, rejected
read or timeout fails setup with the existing bounded cleanup.

To repeat these reads on an established encrypted session:

```sh
echo inspect > /sys/bluetooth/connection
cat /sys/bluetooth/connection
```

`inspect` is asynchronous, refuses overlapping inspections, and leaves input
and repeat active while it reads. Each inspection has a ten-second overall bound
and the usual five-second ATT response deadline. It preserves the original ready
timestamp and records the last successful inspection time. No periodic reads
are enabled. This checks a live request/reply path without rewriting settings.

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
pairing also lacks Secure Connections' resistance to passive capture. The `connect` commands are
session-only: no bond or identity keys are distributed or stored, so a later
connection requires pairing again. This session-only path is experimental:
[HOGP 1.0 sections 6.1–6.2](https://www.bluetooth.org/docman/handlers/downloaddoc.ashx?doc_id=245141)
require bonding for both the HID Device and Host. The session-only commands
therefore omit a keyboard compatibility requirement, not merely reconnect.

Use `bond-justworks` (or `bond` for Passkey Entry) to request bonding and the
peripheral's 16-byte long-term encryption key:

```sh
echo bond-justworks random de:73:10:60:93:6a > /sys/bluetooth/connection
cat /sys/bluetooth/connection
```

The host waits for Encryption Information and Central Identification in order,
after encryption, before HID discovery. Both halves must arrive within ten
seconds. The completed LTK/EDIV/Rand record is bound to the local controller and
peer addresses and retained in kernel memory for this boot. It is not printed
in status. Partial or rejected exchanges wipe pending secrets and do not replace
a previously committed cache. Status distinguishes requested, complete, cached
and reused bonds.

After an explicit `disconnect` has returned to `idle`, `reconnect` uses that
cached key without repeating pairing or silently falling back to a weaker mode.
`forget` erases the local cache while idle or failed; it does not erase the
keyboard's own record. A new explicit bond request can replace the one-record
cache after successful key distribution.

```sh
echo disconnect > /sys/bluetooth/connection
# Wait for idle before reconnecting.
echo reconnect > /sys/bluetooth/connection
```

**The cache is lost on reboot.** After reboot, put the keyboard back into pairing
mode and bond again. Persistent storage, identity-key resolution and automatic
reconnect are tracked in DEBTS.md. Bond commands accept public or static random
addresses; rotating private addresses require identity support. A failed session
still requires reboot before another connection attempt.

The keyboard must expose HID service 0x1812. A notifying Boot Keyboard Input
0x2a22 with a CCC descriptor and writable Protocol Mode 0x2a4e selects Boot
Protocol. Otherwise the host reads Report Map 0x2a4b, including Read Blob chunks,
and examines notifying Report 0x2a4d characteristics and their Report Reference
0x2908 descriptors. It selects the first supported keyboard input report for
decoding, finishes descriptor discovery across the notifying Report candidates,
and enables each candidate's CCC when its Report Reference identifies Input.
This includes input reports whose layouts are not decoded; their notifications
are counted and ignored. Output/Feature references are not subscribed. Each
write must succeed before readiness; status retains per-report subscription
completion. The selected keyboard CCC is read back. Report Protocol is selected
when Protocol Mode is present. Report IDs come from Report Reference, not a
prefix in GATT values.

Report Map support covers absolute keyboard usage bitmaps and one contiguous
key array, including modifiers and padding. The map is bounded to 1024 bytes,
eight notifying Report candidates, and a selected input value of at most twenty
bytes (ATT MTU 23). Keys convert to the shared eight-byte input format; more than
six non-modifier keys produce HID rollover. Multiple keyboard report IDs are not
combined. Consumer/media keys, LED output, mice, Classic HID and audio are not
implemented. Unsupported layouts report an error instead of treating arbitrary
bytes as keys. Connection status retains report candidates and, after failure,
the public Report Map bytes for hardware diagnosis. Receive counters distinguish
complete ACL packets, packets after readiness, unmatched connections, and
ignored notifications; notification handles and lengths are retained without
key contents. Status also retains initial connection parameters, L2CAP parameter
update requests, and ready/disconnect timestamps (milliseconds since boot).
Inspection reads use the existing encrypted ATT path; pairing policy is unchanged.

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
and USB ordering. Report-only fixtures cover a separate consumer report,
optional Protocol Mode, multi-chunk and exact-boundary map reads, malformed maps
and references, and key release on a mismatched notification. Readback tests
cover incorrect CCC/Protocol Mode values, malformed replies, optional reads,
input during inspection, and inspection timeout cleanup after the original
setup deadline. Bond tests cover encrypted key ordering, missing/partial keys,
cache preservation, cached-key reconnection, identity checks, forgetting and
secret cleanup. A fixture with the P5's public HID attributes checks subscription
to five input reports, including reports after the selected keyboard and a
Report ID absent from the map, filtering of unrelated notifications, exclusion
of Output references, and failure on a rejected secondary CCC write. Its keys
and pairing secrets are synthetic.
`tools/test_hid_keyboard_map_host.sh` checks arrays, bitmaps, unaligned padding,
Report IDs, rollover and mutated descriptor bounds under ASan/UBSan. These are
synthetic fixtures; the map suite also includes the P5 keyboard's captured
166-byte public descriptor, tested with synthetic key values.
OpenSSL is a host-test dependency, not linked into the kernel.
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
At `ced6b35b`, the P5's explicit Just Works attempt reached HID service discovery
after successful encryption, finding service 0015–0034 with no Boot Keyboard
Input. The boot-only guard refused it and reset the link. Its random LE address
changed between attempts, so scan again before connecting.

At `603829b0`, the P5 reached `keyboard ready`, `encrypted: yes`, with no error
or malformed packets. The unnamed keyboard supplied a 166-byte Report Map at
0030 and five notifying Report candidates. Report Reference 001b identified
keyboard input report ID 1 at 0019; subscription to CCC 001a succeeded.
This confirms real-hardware pairing, encryption, Report Map parsing and keyboard
subscription. Prompt typing of abcdefg, Shift+A and Backspace produced no visible input. The later status
retained zero key reports and zero malformed packets, with disconnect reason
0x08 (Connection Timeout). DEBUG_USB logged setup at 20:52:51 and disconnect at
20:53:55, about 64 seconds later. This does not establish whether the cause was
radio loss, device behavior or a missing host operation. The captured map selects
an eight-byte keyboard report and correctly decodes synthetic A, Shift+A,
Backspace and release in the host test; actual input delivery remains unproven.


At `c4d835b9`, another prompt typing attempt produced no input. Receive metadata
stayed at 517 bytes / 21 complete ACL packets, all before readiness: zero packets
after readiness, unmatched connections, notifications, indications or L2CAP
parameter-update requests. The initial connection interval was 36 (45 ms),
latency zero, supervision timeout 400 (4 seconds), and Protocol Mode handle
0017. Readiness at 109840 ms was followed by disconnect reason 08 at 170640 ms
(60.8 seconds later). This places the missing input before keyboard decoding;
it does not distinguish device silence from a stalled receive transport. Confirm
the generic advertised device's identity and verify subscription/receive liveness
before attributing this to the keyboard's HID layout.

A controlled power-off scan after reboot omitted both keyboard entries. After
power-on and BT 5.0 pairing mode, the next scan found Classic 02:11:23:34:59:f9
and LE random de:73:10:60:93:6a again. Both scans completed without error or
malformed reports. This supports the physical keyboard's association with the
LE address used above. Notification-setting readback and live inspection are
the next hardware checks. Chris also reported that rapid pairing-mode blinking
continued during the typing attempts. With no delivered notifications, that
makes incomplete device-side pairing/bonding a candidate cause; the LED's
meaning and the causal link are not established by the host status.

At `5bdf8c98`, the keyboard read back CCC=0001 and Protocol Mode=1. After prompt
typing produced no visible input, explicit live inspection succeeded again:
538 bytes / 23 ACL packets became 559 / 25, with two packets after readiness,
zero notifications, and zero malformed packets. Readiness was 61210 ms and the
second successful inspection was 96670 ms. Thus the request/reply receive path
remained live with both settings correct. Bonding/key distribution is the next
hardware test; simulated bonding does not establish P5 input compatibility.

At `330514cc`, `bond-justworks` with random de:73:10:60:93:6a received
Pairing Response `02 03 00 01 10 00 01`, completed encryption and both responder
key-distribution messages, and cached the bond. HID configuration completed at
209780 ms with CCC=0001 and Protocol Mode=1. Chris reported continued rapid
blinking and no visible input while typing. Live inspection at 250160 ms read
both settings successfully again: 582 bytes / 25 ACL packets became 603 / 27,
with two packets after readiness and no notifications or malformed packets.
Bonding alone therefore did not resolve the missing input in this attempt.

Explicit disconnect returned the driver to idle with the bond retained. An
immediate `reconnect` then timed out waiting for connection establishment
(opcode 200d), before encryption or any ACL traffic. Cleanup confirmed the link
inactive and left the driver failed, requiring reboot. This did not test whether
the keyboard accepts the cached key; its advertising state during reconnection
was not observed. That attempt established neither real key delivery nor
cached-key reconnection.

Chris's subsequent Linux btsnoop capture contains successful encrypted legacy
pairing and eight-byte keyboard notifications at value 0019. The public HID
attributes and 166-byte Report Map match the Os64 observations. Linux enables
the five Input Report CCCs 001a, 0021, 0025, 0029 and 002d (Report IDs 1, 3, 4,
5 and 18), after also subscribing to Battery Level. The final HID CCC reply is
at 15.767 seconds; a peripheral connection-parameter request follows at that
timestamp, then the first keyboard notification at 15.769 seconds. The connection
update completes later, at 16.141 seconds. Linux also distributes more SMP keys
than Os64. The trace does not isolate which setup differences affect the device.

The controlled change at `ce322505` completes Input Report subscriptions while
retaining Os64's pairing and connection-parameter policies. The regression
fixture fails against `330514cc` because it stops after the keyboard CCC. The
raw Linux capture contains secrets and is not a repository fixture.

On the P5 at `ce322505`, Chris confirmed visible keyboard input. A fresh scan
found random f5:fd:1c:1f:b0:4e; `bond-justworks` completed key distribution and
encryption, subscribed all five Input Report CCCs, and read back keyboard
CCC=0001 and Protocol Mode=1. Status reached `keyboard ready` with 15 decoded
reports, then 59, zero malformed packets and no error. The selected report
remained ID 1, eight bytes at value 0019. Chris also confirmed correct Shift+A
and Backspace behavior, and that rapid pairing-mode blinking stopped before
its normal timeout. This establishes real keyboard input
through Os64's encrypted LE Report Protocol path with the expanded subscription
sequence. It does not isolate which secondary CCC the peripheral requires.

The keyboard made seven connection-parameter requests (last request: interval
26/26, latency 32, supervision timeout 300); the existing host policy rejected
them and input nevertheless arrived. Accepting valid peripheral updates remains
follow-up work. Cached-key reconnection and persistence across reboot are not
established by this input test.

Wire references: [Bluetooth Core Security Manager](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/security-manager-specification.html),
[ATT](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/attribute-protocol--att-.html),
[L2CAP](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/logical-link-control-and-adaptation-protocol-specification.html),
and [Linux v6.12 HCI definitions](https://github.com/torvalds/linux/blob/v6.12/include/net/bluetooth/hci.h).
