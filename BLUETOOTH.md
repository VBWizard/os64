# Bluetooth discovery and LE keyboard/mouse input

The first operational Intel AX210 USB adapter (`8087:0032`) stays attached after
firmware bring-up. Discovery uses the existing polled xHCI transport. USB interrupt
wiring remains a separate debt.

For connection diagnostics, add `DEBUG_USB` to the boot command line. This
records discovery outcomes, LE phase transitions and transport failures without
logging each keyboard report. Add `DEBUG_DETAILED` for runtime HCI command
opcodes and interpreted key usages; add `DEBUG_EXTRA_DETAILED` for raw HID
keyboard reports (the boot option also enables detailed logging). The shared
HID report gate applies to USB and Bluetooth keyboards. Routine typing produces
no keyboard-report or key-usage lines with `DEBUG_USB` alone. Boot enumeration
and bounded mouse samples remain at the base USB level.

## Using discovery

For a new pairing, put devices into discoverable/pairing mode. Turn an already
bonded keyboard on normally, without requesting a new pairing. To inspect discovery:

```sh
echo scan > /sys/bluetooth/scan
cat /sys/bluetooth/scan
cat /sys/bluetooth/devices
```

Runtime discovery uses two-second LE scans plus command overhead. Controller
initialization runs an initial scan before connection commands are accepted.
Read the files again for progress; each open is a consistent snapshot. Discovery
does not pair or connect, and a device must advertise to appear. Classic inquiry
is not exposed by the shared runtime controller.

`scan` also reports the latest round's start and finish dates in UTC,
boot-relative milliseconds, and outcome. Manual and automatic rounds both use
the `background LE` scan type. An unfinished round says `active` / `in progress`.
If the wall clock is unavailable, boot-relative timing remains available.
Peer disconnects preserve the results and timing; the next scan replaces them.

`devices` lists up to 64 distinct transport/address-type/address combinations,
with RSSI when supplied, Classic class-of-device bytes, and advertised names.
Names can arrive in Extended Inquiry Response or LE advertising/scan-response
fields. Names are limited to 63 bytes and displayed as printable ASCII, with
other bytes replaced by `?`. Missing names remain explicitly unnamed; discovery
does not issue Classic Remote Name requests. LE class bytes are zero because LE
advertisements do not supply the Classic class-of-device field. Random LE
addresses may rotate; the listing shows air addresses. Bonded reconnect resolves
RPAs using the saved identity key.

A new scan replaces the previous list. Requests while busy, absent or in
`failed (reboot required)` are refused. `stopped (retry available)` retains a
scan error after successful cleanup and permits another scan or connection.
The command is bounded to 32 bytes, accepts surrounding whitespace, and
is submitted on close so separate word/newline writes work. An empty write handle
closed without data does nothing. `devices` is read-only. No operational adapter
is reported explicitly. A read racing a USB poll may report `USB busy; retry this
read` rather than waiting under a scheduler-related lock.

Broader periodic discovery and device-cache policy are recorded in DEBTS.md.
Each saved peer has its own automatic connection policy. An absent peer can
trigger LE discovery while another peer remains connected. Connection setup is
serialized; established input continues during discovery and pairing.

## Keyboard and trackball together

The runtime supports two LE peers, numbered 0 and 1. Commands without a slot
prefix address slot 0, preserving existing keyboard scripts and its bond file
`/home/bluetooth_bond`. Slot 1 uses `/home/bluetooth_bond_1`. Each slot has its own
saved bond, automatic reconnect policy, and input source. Neither slot is tied
to a device class. The connection status file shows both slots.

For an MX Ergo S (MR0113), keep the keyboard in slot 0, select Bluetooth pairing
on the trackball, then scan and use its actual address and address type:

```sh
echo scan > /sys/bluetooth/scan
cat /sys/bluetooth/scan
cat /sys/bluetooth/devices
# Wait for scan completion. Substitute the trackball's scanned type/address:
echo slot 1 bond-justworks random XX:XX:XX:XX:XX:XX > /sys/bluetooth/connection
cat /sys/bluetooth/connection
```

`mouse ready` means a matching report has decoded. Test motion, left/right/middle
buttons and vertical scrolling; extra buttons and horizontal scrolling are not
mapped. Wait for slot 1's `bond storage: saved`, then test trackball off/on and
host reboot. Keyboard input should continue through trackball pairing and
reconnection. Test keyboard off/on while moving the trackball as well.

The same prefix selects policy and lifecycle commands:

```sh
echo slot 1 inspect > /sys/bluetooth/connection
echo slot 1 disconnect > /sys/bluetooth/connection
echo slot 1 reconnect > /sys/bluetooth/connection
echo slot 1 auto off > /sys/bluetooth/connection
```

Host simulations cover concurrent keyboard input during mouse pairing,
saved-key reconnection, cancellation races, interleaved ACL fragments, shared
command/ACL credits, and independent bond files. The P5's MX Ergo S accepted
legacy Just Works and completed bond-key distribution on 2026-10-09; its bond
was saved in slot 1 while slot 0's keyboard remained connected. HID discovery
then stopped on a peer ATT Read By Group Type request (`0x10`): the handler
mistook that server-side request for a malformed response to its own discovery.
The handler now replies to peer requests independently of the pending client
transaction, with a host regression covering this exchange and keyboard input.
**Trackball input and saved-key reconnection still need hardware validation**
with that fix. LE Secure Connections is not implemented; no implicit fallback
or fresh pairing occurs during saved-key reconnect.

A helper that writes `bond-justworks TYPE ADDRESS` without a slot prefix targets
slot 0. Use `slot 1 bond-justworks TYPE ADDRESS` for the trackball, or
`slot 1 reconnect` once its bond is saved.

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
input notification is decoded. `input reports` counts these notifications; keys
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

Disconnect releases the selected peer's keys or buttons and terminates its link
without resetting the controller. Another peer remains usable. Scanning is
allowed alongside established input, but refused during connection setup.
Connection commands are bounded to 64 bytes, validated as a whole on close, and
never wait for radio or USB completion in the syscall. Empty writes do nothing.
Invalid/busy requests are refused. Status preserves public pairing capabilities
and identifies unsupported fields after cleanup. Automatic connection reuses
explicit bonds; it does not pair implicitly or select by advertised name.

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

Use `bond-justworks` (or `bond` for Passkey Entry) to pair once and enable
saved-bond automatic connection. Substitute the current scanned LE address:

```sh
echo bond-justworks random de:73:10:60:93:6a > /sys/bluetooth/connection
cat /sys/bluetooth/connection
```

The host requests encryption and identity keys. It accepts encryption-only peers
at public/static-random addresses, but bonding over an RPA requires identity
keys. Encrypted distribution must supply LTK, EDIV/Rand, then the negotiated IRK
and identity address in order within ten seconds. Partial or rejected exchanges
wipe pending secrets without replacing the committed bond. Identity keys never
appear in status; `LTK verified` distinguishes a distributed identity from one
confirmed by encrypted reconnection. Legacy pairing confirms use the air address.

The maintenance worker saves the bond to `/home/bluetooth_bond` and restores it
at boot; the worker must be enabled. Wait for **`bond storage: saved`** before rebooting. The 80-byte versioned
record has explicit fields and a CRC; it is not encrypted. As with Os64's SSH
host key, local programs are trusted and can read the file. Status and logs do
not expose keys. Commands are briefly refused while initial storage loading runs.

Saving writes and syncs `/home/bluetooth_bond.new`, closes it, then requests
atomic replacement. This requires filesystem support for atomic replacement
(ext2 supports it; FAT replacement is refused). A write failure retains the RAM
bond and reports `unsaved`; the worker retries after ten seconds, or `save`
requests an earlier attempt. Corrupt or unreadable records are not silently
replaced at startup. Successful saves support normal reboot; sudden power-loss
crash consistency remains subject to the filesystem and hardware, not the CRC.

**Normal operation needs no connection command.** After bonding, or after host
reboot with an enabled saved bond, the manager runs two-second LE discovery rounds.
It matches the saved identity or checks RPAs with controller AES, then reconnects,
restores encryption, and configures HID. An absent peer causes backoff from two
to thirty seconds between rounds; discovery continues so a keyboard turned on
later can connect. A failed runtime scan whose Scan Disable and USB completion prove safe
cleanup uses the same capped backoff, without exhausting retries. Its partial
results do not trigger a connection. The worker normally runs every two seconds
to handle storage; it starts discovery when it publishes a restored bond. USB polling advances
discovery/connection handoffs and retry deadlines without waiting for another
worker visit. This is bounded background discovery, not an instantaneous wake
guarantee.

Manual controls remain available:

```sh
echo disconnect > /sys/bluetooth/connection
# Suppresses automatic connection for this boot until reconnect or auto on.
echo reconnect > /sys/bluetooth/connection
# Reuses the saved key; after a completed scan it can resolve a new RPA.
echo auto off > /sys/bluetooth/connection
# Persists manual mode; does not tear down an established connection.
echo auto on > /sys/bluetooth/connection
echo save > /sys/bluetooth/connection
```

`forget` is accepted while idle, stopped or failed, and during the manager's own
discovery round. It disables automatic connection and wipes the RAM bond. The
worker atomically replaces the disk record with a keyless tombstone; wait for
`bond storage: saved` before assuming forgetting will survive reboot. It does not
erase the keyboard's own bond. A stale in-flight save cannot acknowledge a newer
forget as saved. A new explicit bond replaces the selected slot's cache.

The manager yields to manual discovery and pairing. Key rejection, incompatible
HID, controller mismatch, and terminal transport errors stop automatic attempts
for user action; peer absence and loss of a working link permit another round.
A changed static-random identity cannot be recognized by name or by an unrelated
IRK. Confirmed per-peer cleanup permits an explicit retry without reboot; transport
failures and unconfirmed cleanup still require reboot. See
[Bluetooth bonding and reconnection](docs/design/pending/BLUETOOTH_BONDING.md).

A manual scan reserves sixty seconds from its start before background discovery
may resume, allowing time to inspect the results and select a connection.

The keyboard must expose HID service 0x1812. A notifying Boot Keyboard Input
0x2a22 with a CCC descriptor and writable Protocol Mode 0x2a4e selects Boot
Protocol. Otherwise the host reads Report Map 0x2a4b, including Read Blob chunks,
and examines notifying Report 0x2a4d characteristics and their Report Reference
0x2908 descriptors. It selects a supported keyboard or relative mouse input report for
decoding, finishes descriptor discovery across the notifying Report candidates,
and enables each candidate's CCC when its Report Reference identifies Input.
This includes input reports whose layouts are not decoded; their notifications
are counted and ignored. Output/Feature references are not subscribed. Each
write must succeed before readiness; status retains per-report subscription
completion. The selected input CCC is read back. Report Protocol is selected
when Protocol Mode is present. Report IDs come from Report Reference, not a
prefix in GATT values.

Report Map support covers absolute keyboard usage bitmaps and one contiguous
key array, including modifiers and padding. The map is bounded to 1024 bytes,
eight notifying Report candidates, and a selected input value of at most twenty
bytes (ATT MTU 23). Keys convert to the shared eight-byte input format; more than
six non-modifier keys produce HID rollover. Multiple keyboard report IDs are not
combined. Mouse maps use the shared relative X/Y/wheel decoder, restoring the Report ID
octet omitted by GATT before decoding. Consumer/media keys, LED output, Classic
HID and audio are not implemented. Unsupported layouts report an error instead of treating arbitrary
bytes as keys. Connection status retains report candidates and, after failure,
the public Report Map bytes for hardware diagnosis. Receive counters distinguish
complete ACL packets, packets after readiness, unmatched connections, and
ignored notifications; notification handles and lengths are retained without
key contents. Status also retains initial connection parameters, L2CAP parameter
update requests, and ready/disconnect timestamps (milliseconds since boot).
Inspection reads use the existing encrypted ATT path; pairing policy is unchanged.

Connection initiation has a twenty-second wait; HCI commands have two seconds,
ATT requests and ACL completion have five seconds, and setup has an overall
150-second bound. Runtime errors release the selected source and perform bounded
peer cleanup after pending commands and USB transfers finish. An established
link uses HCI Disconnect; pending initiation uses LE Create Connection Cancel
and waits for its Connection Complete event. If establishment wins the race,
cleanup disconnects the resulting handle. Confirmed cleanup permits slot reuse
and retains errors and bonds. A transport failure, missing command reply, or
unconfirmed cleanup makes the controller terminal and releases both inputs.
A routine peer disconnect does not reset the controller. DMA stays allocated.

## Controller and transport

Firmware loading and its boot-time receive recovery precede runtime discovery.
Successful bring-up transfers the device and transport state into controller-owned
storage; failed bring-up disables the slot before releasing DMA allocations, or
retains them if disabling fails. The retained adapter owns its DMA storage for
its remaining boot lifetime, including disconnect and runtime failure.

Controller initialization resets HCI state, reads features and address, and
sets event masks covering discovery, connection, encryption and ACL completions.
Subsequent scans preserve that configuration and established links. LE uses
active legacy scanning on the primary 1M advertising channels, a 60 ms interval
and 30 ms window, public own address, and duplicate filtering. Runtime scans
last two seconds. Extended/coded-PHY advertising is not decoded.

The runtime state machine submits one EP0 command at a time and waits across
poll passes for both USB completion and the matching HCI reply. Inquiry uses
Command Status followed by Inquiry Complete. Interrupt IN carries operational
HCI events; bulk IN carries ACL data after the firmware handoff. Receive
rearming is deferred until after the event drain to bound radio work per pass.
The decoder allocates no memory, waits on nothing and sanitizes untrusted names.

Command wait deadlines are two seconds and scan setup has a forty-second bound.
Completed HCI replies and USB transfers are processed before command timeout;
overall setup limits still apply. Runtime scan cleanup disables scanning. An
unconfirmed command cannot be replaced with another command using the same USB
storage. Confirmed initial Reset cleanup retries initialization after two seconds;
unconfirmed cleanup or transport failure requires reboot. Timing depends on
scheduler polling, as recorded by the xHCI dispatch-frequency debt.

The host routes commands to one owner, ACL packets by connection handle, and
completed-packet credits to the relevant peer. Each peer has its own ATT/SMP and
L2CAP state. It spends at most one controller ACL credit across both peers;
a confirmed disconnect also returns that handle's unacknowledged credits, as
specified by [Core Vol 4 Part E, section 4.3](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html).

LE input has separate USB packet, HCI ACL and L2CAP framing. Receive storage is
bounded (1024-byte HCI ACL payload, 64-byte L2CAP payload); ATT uses the default
23-byte MTU. Each peer's transmit queue is bounded to eight packets and shares one
controller ACL credit at a time, returned by Number Of Completed Packets. USB
completion independently gates DMA reuse. EP0 commands and bulk OUT have disjoint
regions in the retained transmit page. Protocol decoders allocate no memory or
perform blocking waits. Optional peripheral L2CAP connection-parameter updates
are rejected, retaining the central's chosen interval. Input stops on encryption
loss or disconnect, with an empty report releasing that source's held keys or buttons.

## Validation

`tools/test_bt_multi_host.sh` drives the production shared controller with an
independent SMP/GATT peer model. It covers a live keyboard during mouse bonding,
saved-key reconnect, both directions of peer loss, cancellation success/races,
rejected initiation, duplicate commands, USB command ownership, shared ACL credits,
interleaved L2CAP/USB fragmentation, malformed mouse reports and scan cleanup.
The xHCI harness also checks mouse injection and HID-to-desktop wheel direction.
The bond-store suite checks that slot 1 writes and failed replacements preserve
slot 0. These suites pass ASan/UBSan with LeakSanitizer enabled outside the sandbox;
the sandbox denies LSan thread attachment with EPERM and TracerPid zero.


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
secret cleanup. Power-loss fixtures verify direct cached-key reconnect after
confirmed cleanup, held-key release, diagnostic retention, and refusal to retry
after missing/rejected reset completion or USB failure. The production xHCI
harness checks that peer cleanup preserves discovery state and routes subsequent scan
events correctly. A fixture with the P5's public HID attributes checks subscription
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

Identity fixtures check the published `ah` vector, negotiated/partial identity
keys, matching and nonmatching RPAs, air-address pairing confirms, controller
binding, and encryption rejection after an RPA match. Manager tests restore an
encoded bond, remain live through repeated empty scans, and automatically
reconnect after power loss with a new RPA. They cover persistent disable,
disconnect suppression and forgetting. Link-failure fixtures check both
Encryption Change/Disconnection Complete event orders and recovery through the
manager for timeout (08) and establishment failure (3e); missing-key,
authentication and MIC failures remain blocked even when followed by either
disconnect reason. `tools/test_bt_bond_store_host.sh` runs
the production storage code with short I/O, corruption, sync/close/rename errors,
and filesystem read-only demotion. The xHCI harness verifies disk callbacks run
outside the poll lock and an in-flight save cannot acknowledge a newer forget.
It also checks immediate discovery after bond restoration, connection handoff
from USB polling, and absent-peer backoff without a worker visit.
Storage tests also check kernel-context marshalling, allocation failure, and
wiping the marshalled key material before freeing it.

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
follow-up work.

Chris subsequently confirmed that explicit `reconnect` succeeded after a few
seconds. A read-only SSH check found `keyboard ready` for random
f0:5c:3e:03:71:d3, encryption active, and the bond marked cached=yes,
requested=no, reused=yes, complete=yes, received_parts=0. Six keyboard reports
had decoded with no malformed packets or error; all five input subscriptions
and keyboard CCC/Protocol Mode readback succeeded. This verifies cached-key
reconnection and input without a fresh pairing exchange in the same boot.
That test predates disk persistence, identity resolution and automatic connection.
Later P5 checks for those additions are recorded below; hardware privacy-address
rotation remains unverified.

At the end of testing, Chris reported that powering the keyboard off without
first issuing `disconnect` prevented both `reconnect` and a subsequent
`disconnect`/`reconnect` sequence from working. His working address differed
from the earlier example in the instructions. No before/after address scan or
connection-status snapshot was captured for this power-off case, so an address
change is a candidate explanation, not a confirmed cause. At `a58ad4bf` the driver
also retained unexpected-disconnect failures until reboot. Host-tested recovery
now permits retry after confirmed reset cleanup; P5 power-cycle validation must
distinguish peer address changes from session recovery, alongside saving
bonds across host reboot. Explicit same-boot disconnect/reconnect success does
not establish recovery from unexpected keyboard power loss.

### Saved-bond reconnect timeout investigation (2026-10-08)

After Chris observed `bond storage: saved` and powered the keyboard off/on,
the captured status showed a saved-key reconnect to random c8:cb:24:65:d1:95.
The connection reached encryption setup, then reported Encryption Change
status 08 and Disconnection Complete reason 08. Cleanup completed, but automatic
connection reported `user action required`. No new key distribution occurred.
The subsequent manual scan did not contain that saved static identity; this
snapshot alone does not establish what address the keyboard was advertising.

The retry policy incorrectly blocked a supervision timeout during saved-key
encryption setup. It now retries after successful reset cleanup, including when
the timeout is delivered as Disconnection Complete before encryption completes.
The first failure retains its retry classification when a trailing disconnect
arrives. Authentication/key/MIC errors still require user action.
[Core Vol 1 Part F section 2.8](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/architecture,-mixing,-and-conventions/controller-error-codes.html)
defines status 08 as a connection timeout; it does not establish key rejection.

A later read-only SSH snapshot, after a user-confirmed host reboot, showed
`keyboard ready`, the same saved identity, `reused=yes`, `LTK verified: yes`,
all five input subscriptions and 30 key reports. No Bluetooth commands or
updated kernel were sent through SSH during this investigation. Chris also
reported automatic reconnection after a manual scan. The post-reboot snapshot
confirms persisted-key reuse on that identity; it does not establish whether
the scan was necessary. The cause of the earlier radio timeout remains unknown.
The fix passes the LE and production xHCI sanitizer suites and kernel build.
Subsequent successful P5 power-cycle tests are recorded under recovery scenarios
below; they do not establish the cause of this earlier radio timeout.

### Automatic boot connection and latency (2026-10-08)

Cold-boot firmware loading also contributes to time before desktop readiness.
The retained P5 log from the successful 2026-10-07 19:16 bring-up at `9b21368b`
records upload start at tick 499, payload sent at 610, operational image 03
confirmed at 615, and DDC configuration applied at 617. At 100 ticks/second,
that is 1.11 seconds for the payload and about **1.18 seconds** through
configuration, including endpoint-82 recovery. The log's timestamps are at
10 ms granularity; the final console announcement immediately follows DDC in
the loader. This is one measured cold load of 737,744 bytes, not a measurement
from the final PR kernel. The synchronous load runs before the scheduler and
delays later boot work. Warm boots that find operational firmware skip it;
the keyboard reconnect timings below measure a separate stage.

Chris confirmed that boot connection worked without manual scan or reconnect
after the timeout fix, reporting about twelve seconds from desktop readiness to
keyboard input. A read-only status snapshot showed encrypted saved-bond reuse,
verified LTK, all five input subscriptions, ten key reports and HID readiness at
21,070 ms after kernel startup. This establishes unattended boot connection on
the tested saved static identity; ordinary off/on recovery, privacy-address
rotation and persistent disable/forget still need their separate checks.

The manager now starts discovery when storage loading finishes and advances
scan completion and retry deadlines from USB polling. These handoffs no longer
wait for the maintenance worker's two-second sleeps. The scan duration and
absent-peer backoff are unchanged. Host tests cover the scheduling change.
After Chris installed the new kernel and rebooted, a read-only snapshot recorded
HID readiness at 17,210 ms, 3,860 ms earlier than the previous boot. Encryption,
saved-key reuse and all five subscriptions succeeded. There is no matching
desktop-ready timestamp for either boot, so this compares time since kernel
startup rather than time after the desktop appeared.

### Establishment failure delivered as disconnect (2026-10-08)

On a later boot with channel #2 selected, untouched status showed a saved-key
attempt to c8:cb:24:65:d1:95, followed by Disconnection Complete reason 3e at
112,270 ms. Cleanup succeeded, but the manager reported `user action required`.
There was no encrypted session or HID discovery. The code already retried 3e
in LE Connection Complete, but excluded it in Disconnection Complete. That
classification gap predates the worker-handoff optimization.

[Core Vol 1 Part F section 2.59](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/architecture,-mixing,-and-conventions/controller-error-codes.html)
defines 3e as failure to establish a connection or synchronize. The retry
classification for 08/3e is now shared across connection, encryption and
disconnect events. Saved-key retries still require successful cleanup; key,
authentication and MIC errors remain blocked, including when a later disconnect
reports 08 or 3e. The expanded host fixture fails before the fix and passes
afterward. The LE and production xHCI sanitizer suites and kernel build pass.

After the initial capture, one explicit saved-key reconnect was issued over SSH
on the running pre-fix kernel. Background discovery resumed but initially found
no matching identity. Chris confirmed he had switched to dongle channel #0 in
the meantime. After he switched back to #2, the manager connected on the same
identity, verified the saved LTK, configured all five input subscriptions and
received four key reports. No pairing or bond replacement was performed. This
separates the channel-related absence from the earlier blocked-retry defect;
the original radio failure's cause remains unproven. The corrected automatic
3e recovery path still needs P5 validation on the new build.

### Packet-integrity failure during saved-key HID setup (2026-10-08)

On the build containing the 3e retry correction but preceding scan timestamps,
Chris reported another boot failure with channel #2 selected and slow blue
blinking on keypress. Untouched status showed saved-key reuse and successful
encryption (`complete=yes`, `LTK verified: yes`), followed by Disconnection
Complete reason 3d at 21,720 ms. HID service 0015-0034 and its 166-byte Report Map
had been read, but descriptor/reference discovery was incomplete, no Input CCC
was marked subscribed, and readiness had not been reached. The host received
62 ACL packets and 41 notifications, all ignored before readiness; it recorded
zero malformed or unmatched ACL packets. The last of two parameter requests
asked for interval 24/24, latency 32 and timeout 300. These observations do not
identify the cause of the disconnect.

[Core Vol 1 Part F section 2.58](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/architecture,-mixing,-and-conventions/controller-error-codes.html)
defines 3d as termination due to a Message Integrity Check failure on a received
packet. The manager intentionally blocks automatic retries for this error.
The 08/3e retry fixes do not cover it. The timing optimization changes when
discovery/connection handoffs run, not their wire formats; this observation
does not rule out a timing-dependent issue, nor justify attributing the MIC
failure to it. No retry policy or connection parameters were changed.

After preserving the failure, one explicit saved-key reconnect was issued over
SSH. The same identity reached encrypted readiness at 429,880 ms, verified the
saved LTK, and delivered four key reports without new pairing. This establishes
recovery on the existing bond, not a fix for the intermittent boot failure.
Logging status showed an active userland sink and no lost kernel log entries.
The cause of 3d remains unresolved and is part of hardware acceptance work.

### Recovery scenarios and persistent automatic policy (2026-10-08)

Chris reported successful keyboard off/on, switching away from Bluetooth
channel #2 and back, power-on after the OS had been up for thirty seconds, and
waking the keyboard with a keypress. Each took approximately nine seconds to
connect. The wake-up key itself was not printed; input arriving before HID
readiness is not replayed. Explicit disconnect/reconnect also passed repeated
testing. Five successive boots after correcting the diagnostic boot flag to
`DEBUG_USB` each connected in about nine seconds after system readiness.

Chris then confirmed that saved `auto off` survived reboot and prevented
automatic connection. After issuing `auto on` through his SSH wrapper, he
reported an apparent continued disconnection. Read-only inspection found
`keyboard ready`, encrypted saved-bond reuse, verified LTK, all five input
subscriptions and 98 key reports. No reconnect or scan command was issued
during this inspection. The enabled USB trace shows the first scan completing
at 46,930 ms, followed by `bonded peer not found in scan`; its cleanup completed
at 47,080 ms. The next scan ran from 48,930 to 51,040 ms, found the saved static
identity and led to HID readiness at 58,230 ms. These times are since kernel
startup, not since the `auto on` command (whose receipt time is not logged).
The observed policy resumed discovery and recovered from the missed scan
without manual intervention. No key rejection or MIC failure was reported in
this sequence.
Chris subsequently confirmed that the husk window was not foreground; missing
visible text was a focus issue. Persistent disable and re-enable passed.

### Forget/reboot and re-pairing (2026-10-08)

During the final forget/reboot check, status showed `cached=no`, no identity
key, `bond storage: saved`, automatic connection disabled and an idle session.
The apparent failure was restoring the pairing afterward: the local helper
`/home/yogi/btconnect` used DOS-style `%1` instead of husk's `$1`, so the bond
command contained a literal invalid address and never started a session. The
helper was corrected to `echo bond-justworks random "$1" > /sys/bluetooth/connection`.
Its first real attempt timed out because the keyboard did not answer. After
Chris renewed pairing mode, a fresh scan confirmed ca:dc:f1:6a:af:47 and the
corrected helper completed pairing on that address.

Status showed four received key-distribution parts, a present identity key,
saved bond storage, automatic connection enabled, encryption active and all
five Input Report subscriptions configured. HID readiness was reached at
413,940 ms; Chris subsequently confirmed successful typing in the foreground
husk window. This validates the forgotten state after reboot and restoration
of a new persisted bond with working input. No driver change was needed.
The new pairing's static identity differs from the previous
c8:cb:24:65:d1:95; that is not evidence of an RPA rotation or an ordinary bonded
power-cycle identity change.

### PR #235 round-one recovery corrections (2026-10-08)

Fable identified that scan/LE command deadlines were tested before completions
already drained by a late poll, and that successful scan cleanup still left a
terminal failure. Host regressions reproduce the old command-timeout and manager
behavior. Commands and cleanup now accept completed transfers observed late;
scan cleanup enters the restartable stopped state, and the manager backs off
without a failure-count ceiling, as Chris selected. Partial failed-scan results
are not used for automatic connection, and last-scan timestamps record the failed
round as finished. Actual transport, cleanup and security failures retain their
existing restrictions.

The scan, LE/manager, production xHCI loader/runtime, and shared mouse/HID host
scripts pass under ASan/UBSan; the kernel builds, and the retirement grep and
whitespace check pass. Tests cover missing USB/HCI completions, rejected Reset,
forty consecutive stopped scans followed by saved-key connection, manual commands,
policy disable/re-enable, and scan-history completion. These review corrections
have not yet been boot-tested on the P5 and do not resolve the recorded MIC failure.

Wire references: [Bluetooth Core Security Manager](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/security-manager-specification.html),
[ATT](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/attribute-protocol--att-.html),
[L2CAP](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/logical-link-control-and-adaptation-protocol-specification.html),
and [Linux v6.12 HCI definitions](https://github.com/torvalds/linux/blob/v6.12/include/net/bluetooth/hci.h).
