# Bluetooth bonding and reconnection

Os64 should remember an explicitly paired keyboard and reconnect after either
device restarts, without another pairing ceremony. This work extends the AX210
LE keyboard path on `codex/bluetooth-ax210-discovery`. The operational record and
commands are in [BLUETOOTH.md](../../../BLUETOOTH.md).

The implementation proceeds in controlled stages: recover a stopped session,
collect and resolve peer identity, persist complete bonds, then automate
reconnection. One selected LE keyboard remains the scope. Classic HID, multiple
simultaneous peers, mice, audio, and LE Secure Connections are separate work.

## Existing behavior and evidence

P5 testing established encrypted keyboard input and explicit same-boot
reconnection using a cached legacy bond. The cache contains the peer address,
local controller address, LTK, EDIV, Rand, and authentication flag. It is lost
when Os64 reboots. Pairing requests encryption-key distribution but no identity
key. The five Input Report subscriptions required by the test keyboard must
survive this work.

Unexpected keyboard power loss enters cleanup. At the starting revision
`a58ad4bf`, even successful HCI Reset cleanup leaves the session in a terminal
failed state. Connection and scan requests cannot proceed. This explains a
recovery failure independently of any address change.

The local Linux capture shows encrypted legacy key distribution including an
Identity Resolving Key (IRK) and a static-random identity address. That address
matches the connection address, and the enhanced connection event reports no
peer resolvable private address. The capture does not establish the keyboard's
advertising address after an ordinary power cycle. Raw captures contain secrets;
regression fixtures use synthetic keys and public protocol metadata.

## Address and identity rules

The connection address and the bonded identity are separate fields. A public or
static-random address can identify a peer directly. A resolvable private address
(RPA) can be checked against its saved IRK. An IRK does not identify an arbitrary
new static-random address. Random addresses use their two most significant bits
to distinguish static (`11`), resolvable private (`01`), and non-resolvable private
(`00`) forms. Static addresses may change on device power-up.

These rules follow the Bluetooth Core
[LE address definitions](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/low-energy-controller/link-layer-specification.html)
and [Security Manager protocol](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/security-manager-specification.html).

Do not identify the keyboard by its name, RSSI, or HID descriptor, and do not
send a saved key to a newly observed address solely because those attributes
match. If a power cycle changes the static identity and no resolvable identity
is available, report that explicit pairing is needed. Matching an RPA selects
a candidate; successful encryption with the bond still gates HID input.

## Stage 1 Session recovery

Add a stopped state distinct from terminal failure. Enter it after cleanup has
received a successful HCI Reset completion and the outstanding control and
bulk USB transfers have completed. Keep the failure reason, disconnect reason,
diagnostic counters, and committed bond available for inspection. Wipe temporary
pairing secrets and incomplete bond material, and release held keys.

From stopped, accept `reconnect`, an explicit new `connect` or `bond` request,
`forget`, and discovery. `disconnect` may clear the retained session diagnostics
and return to idle. A new connection clears session state while preserving the
committed bond. No command implicitly starts pairing or downgrades passkey
pairing to Just Works.

Reset changes event masks and controller state. Clear the old scan state when
LE cleanup completes, and route subsequent discovery events to the scan state
machine. A USB failure, unsuccessful reset, or cleanup deadline expiry remains
terminal. An observed disconnect alone is insufficient proof that controller
commands and USB buffers can be reused.

Acceptance: after a simulated connection timeout, held keys release, diagnostics
and bond survive, and direct reconnect reaches encrypted HID readiness without
fresh SMP pairing. Discovery must also work from stopped. Delayed USB completion,
missing or rejected reset replies, and transport errors must block premature
retry. Repeat normal keyboard off/on testing on the P5 before widening the change.

## Stage 2 Bond identity

Extend the bond with an explicit identity-address type and address, an optional
peer IRK, and a separate last connection address. Request responder encryption
and identity distribution for bonding; preserve encryption-only peers when the
negotiated identity-key bit is absent. Negotiate supported key masks explicitly.

Accept key distribution only on the encrypted pairing link. Validate packet
lengths, order, negotiated masks, duplicates, address type, and identity-address
form. Stage the LTK and EDIV/Rand with any negotiated IRK and Identity Address
Information. Publish a bond only after the negotiated set is complete. A missing
identity half must time out without replacing a previous committed bond.

Reconnect first uses the saved identity if directly addressable. For an RPA,
resolve a bounded discovery result against the saved peer IRK before initiating.
Use the controller's AES operation or a reviewed existing AES implementation;
test byte order against published `ah` vectors. If using the controller resolving
list instead, implement and test list setup, address-resolution enablement, event
masks, and identity-versus-air-address reporting as one coherent change. Reset
reinitialization must restore the chosen resolution mechanism before initiation.
Choose one mechanism during implementation; do not mix their address semantics.

Retain the actual pairing addresses for legacy `c1`; replacing them with the
distributed identity would change the confirm calculation. Bind restored keys
to the local controller identity. A different controller cannot silently inherit
the bond. Peer key rejection must retain an actionable error, without silently
pairing again or deleting the stored bond.

Acceptance covers identity distribution with and without an IRK, malformed or
partial distribution, RPA matches and nonmatches, identity changes, controller
mismatch, and encrypted reconnect with synthetic keys. Capture a before/after
power-cycle scan on the P5 to establish which address path the keyboard uses.

## Stage 3 Persistent storage

Persistence runs in task context, outside the serialized USB input poller and
outside its lock. The polling path publishes a bounded bond snapshot plus a
generation number; a storage worker copies it under the lock, releases the lock,
then performs disk I/O. Completion acknowledges that generation so an older
write cannot mark a replacement or forgotten bond as saved.

Use an explicitly encoded, versioned record with fixed bounds, lengths, flags,
local and peer identities, key size, authentication mode, LTK, EDIV/Rand, optional
IRK, and an integrity check for accidental corruption. Do not serialize a native
C struct or treat a checksum as protection against malicious replacement.
Load into temporary storage, validate the entire record and controller binding,
then publish it through the same serialized state boundary. Short, oversized,
unknown-version, corrupt, or invalid-flag records must not become active bonds.

Before choosing the file path and transfer API, audit the branch's actual access
controls and filesystem durability guarantees. Keys must not be exposed through
the public status files, shell command arguments, logs, or ordinary diagnostic
exports. A filename, mode field, or hidden directory is not evidence that Os64
enforces access restrictions. Resolve any missing enforcement in the storage
slice; do not call an unrestricted key file protected storage.

Write a replacement record and use the filesystem's supported commit mechanism
to preserve the previous valid record on failure. Verify its power-loss behavior;
a successful rename alone does not prove durability. If the filesystem cannot
provide the required guarantee, use a validated generation-based record scheme
or explicitly narrow the supported reboot guarantee. Retain the usable RAM bond
on write failure and report `unsaved`, allowing an explicit retry.

`forget` invalidates queued writes and removes persistent state as well as RAM
state. A completed old write must not resurrect a forgotten bond. Report deletion
failure and do not claim durable forgetting until it succeeds. Replacement,
load, and deletion operate on one serialized generation sequence.

Acceptance includes save/reboot/load/reconnect without SMP pairing, write and
read errors, truncation/corruption, replacement failure, forgetting across reboot,
forget racing a save, and proof that status and logs do not disclose keys. Verify
permissions and crash behavior on the actual filesystem selected for storage.

## Stage 4 Automatic reconnect

After explicit reconnect passes hardware validation, add bounded retries with
backoff for the selected saved peer. Suspend retries while manual discovery or
pairing owns the controller. Explicit disconnect suppresses automatic reconnect
until the user reconnects or enables it again. Forget cancels retries. Rejected
keys and unrecognized identities require user action rather than an endless loop.

Expose connection state, saved or unsaved bond state, identity-resolution result,
and the reason for the next required action through `/sys/bluetooth/`. The normal
experience is pair once, then turn the keyboard on and type. Diagnostics remain
available when that cannot succeed.

## Implementation status

- Stage 1: implemented; LE and production xHCI host suites pass with ASan/UBSan,
  and the kernel builds. P5 power-cycle validation is pending.
- Stages 2 and 3: designed; implementation follows the P5 recovery check.
- Stage 4: deferred until explicit persisted-bond reconnect is reliable.

Peripheral connection-parameter updates remain a separate compatibility task;
the working keyboard retries requests that Os64 rejects. Do not combine that
policy change with recovery when testing the reported power-off regression.

## P5 recovery check

Boot the recovery build and establish a working `bond-justworks` session using
the keyboard's current scanned LE address. Record `/sys/bluetooth/connection`
and that address. Power off the keyboard without issuing `disconnect`, then
wait for connection status to reach `stopped (retry available)`; the supervision
timeout must elapse before the host knows the peer has gone away.

Power the keyboard on normally, without requesting a new pairing, and write
`reconnect` to `/sys/bluetooth/connection`. Check encrypted readiness and actual
typing. If initiation times out, wait for stopped and request a scan. Record the
new scan entry and connection status so an address change can be distinguished
from a failed cleanup. Discovery should now be possible without reboot.

This check covers session recovery with the RAM bond. Saving across host reboot
and resolving a changed RPA belong to the following stages.
