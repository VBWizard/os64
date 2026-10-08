# Bluetooth bonding and reconnection

Os64 should remember an explicitly paired keyboard and automatically reconnect
after either device restarts, without another pairing ceremony or a connection
command. Automatic reconnection is required for completion of this feature.
The implementation includes background connection. This work extends the AX210
LE keyboard path on `codex/bluetooth-ax210-discovery`. The operational record and
commands are in [BLUETOOTH.md](../../../BLUETOOTH.md).

The implementation proceeds in controlled stages: recover a stopped session,
collect and resolve peer identity, persist complete bonds, then automate
reconnection. One selected LE keyboard remains the scope. Classic HID, multiple
simultaneous peers, mice, audio, and LE Secure Connections are separate work.

## Existing behavior and evidence

P5 testing established encrypted keyboard input and explicit same-boot
reconnection using a cached legacy bond. At `a58ad4bf`, the cache contained the
peer address, local controller address, LTK, EDIV, Rand, and authentication flag. It was lost
on reboot, and pairing requested encryption-key distribution but no identity
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
retry. Normal keyboard off/on testing on the P5 remains an acceptance check.

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

Reconnect uses a directly advertised saved identity or resolves RPAs from a
bounded discovery snapshot using HCI LE Encrypt. The implementation checks the
Core D.7 `ah` vector and both matching and nonmatching addresses. It copies up to
64 RPA candidates under serialization, validates the local controller address,
then checks candidates asynchronously. An explicit reconnect without a completed
scan attempts the identity address directly; the background manager obtains a
fresh LE scan before reconnecting. No controller resolving list is used.

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
local and peer identities, authentication mode, LTK, EDIV/Rand, optional
IRK, and an integrity check for accidental corruption. Do not serialize a native
C struct or treat a checksum as protection against malicious replacement.
The format fixes the encryption key size at 16 bytes. Load into temporary
storage, validate the entire record, then publish through the serialized state
boundary. Verify controller binding before supplying any key to HCI. Short, oversized,
unknown-version, corrupt, or invalid-flag records must not become active bonds.

The storage audit found no enforced local file-permission model. The selected
policy follows the SSH host-key model: trusted local programs can read machine keys.
The store is `/home/bluetooth_bond`, an unencrypted 80-byte record with magic and
version `OS64BT01`, fixed fields, reserved zero bytes and CRC32. Keys remain out
of public status, command arguments and logs; this is not protected local storage.

The worker writes and syncs `/home/bluetooth_bond.new`, closes it and requests
`OS64_RENAME_REQUIRE_ATOMIC_REPLACE`. Unsupported replacement is refused instead
of unlinking the previous bond first. A failed write retains the RAM bond and
reports `unsaved`; automatic retries occur after ten seconds and `save` requests
an earlier retry. An unreadable existing record is not treated as absent:
startup uses exclusive creation to initialize a missing record without replacing
an existing one. Temporary records are not promoted on startup.

The supported target is a saved bond surviving a normal host reboot. Sync and
atomic replacement do not establish arbitrary power-cut crash consistency on
unjournaled filesystems. That stronger guarantee requires filesystem/hardware
validation; corruption refuses automatic use of the record.

`forget` invalidates queued writes and removes persistent state as well as RAM
state by publishing a keyless tombstone. A completed old write must not
acknowledge a newer forget as saved. While forgetting is reported as unsaved,
a reboot can still load the prior disk bond. Replacement,
load, and deletion operate on one serialized generation sequence.

Acceptance includes save/reboot/load/reconnect without SMP pairing, write and
read errors, truncation/corruption, replacement failure, forgetting across reboot,
forget racing a save, and proof that status and logs do not disclose keys. Verify
normal reboot behavior on the actual filesystem selected for storage.

## Stage 4 Automatic reconnect

This stage is required, after explicit reconnect passes hardware validation.
Successful explicit bonding enables automatic connection for the selected peer.
On host startup, load the saved bond and begin looking for that peer once the
controller and storage are ready. After keyboard power loss, complete session
cleanup, then resume looking. Turning the keyboard on normally should be enough:
recognize its direct identity or resolve its RPA using the saved IRK, connect,
restore encryption, and configure HID input without a shell command.

The background manager advances discovery and connection policy from USB
polling under the USB serialization lock. The maintenance worker loads and
saves bonds in task context, releasing that lock before disk I/O, and starts
discovery when a restored bond is published. Scan completion and retry deadlines
do not wait for a worker visit. USB polling continues protocol progress and
input delivery. A separate userland daemon is not required. Manual commands
and the manager share the controller owner.
Boot enables the manager after mounts and configuration settle. Disk callbacks
use the kernel-context trampoline with HHDM-backed arguments because the worker
has its own page tables and stack. The maintenance worker must be enabled.

Use two-second LE scans with retry delays doubling from two to thirty seconds
to bound the discovery duty cycle. An absent
keyboard must not permanently exhaust retries: continue occasional discovery so
a keyboard turned on later can connect. Suspend retries while manual discovery
or pairing owns the controller. Explicit disconnect suppresses automatic
reconnect for the current boot until the user reconnects or enables it again;
provide a persistent disable setting for users who want manual connection.
Forget cancels retries and removes the bond. Rejected keys and unrecognized
identities require user action rather than repeated pairing attempts.
Controller-reported connection timeout (08) or establishment failure (3e)
during saved-key setup is retryable after successful cleanup, whether reported
by a connection, encryption or disconnect event. A trailing disconnect preserves the first
failure's classification, so it neither blocks a transient retry nor turns a
key rejection into an automatic retry. The P5 timeout evidence and host
regression coverage are recorded in `BLUETOOTH.md`.

Acceptance requires typing without a `reconnect` command after keyboard off/on,
after host reboot with the keyboard already on, and when the keyboard is turned
on long after host startup. Cover recognized RPAs, absence/backoff, explicit
disconnect and disable behavior, and forget racing discovery. A changed static
identity that cannot be resolved must retain the Stage 2 refusal behavior.

Expose connection state, saved or unsaved bond state, identity-resolution result,
and the reason for the next required action through `/sys/bluetooth/`. The normal
experience is pair once, then turn the keyboard on and type. Diagnostics remain
available when that cannot succeed.
`/sys/bluetooth/scan` includes the latest round's start/finish UTC dates and
boot-relative milliseconds, scan type, and active/complete/failed outcome.
That timing survives controller cleanup clearing the scan results, so an idle
scan state does not erase evidence of the last discovery round. Filesystem
timestamps and per-device last-seen tracking are outside this status field.

## Implementation status

All four stages are implemented with host regression coverage. P5 status
confirms identity-key receipt and saved-key reuse after reboot; Chris confirmed
unattended boot connection without manual scan/reconnect, keyboard off/on,
channel switching and return, delayed power-on after thirty seconds of OS
uptime, sleep/wake, and explicit disconnect/reconnect. Chris also confirmed
that saved `auto off` prevented boot connection; a subsequent `auto on` trace
reached encrypted readiness automatically after a scan missed the peer and
the next found it. Persistent forget and privacy-address rotation remain
unverified on hardware. The USB-poll handoff optimization
has host coverage; successive P5 snapshots measured readiness at 21,070 ms
before it and 17,210 ms afterward. A subsequent 3e disconnect exposed a separate
retry-classification gap, now covered by host tests; corrected automatic 3e
recovery still needs hardware validation. See `BLUETOOTH.md` for the evidence.
An additional boot test on that correction reached encrypted HID discovery,
then disconnected with MIC failure 3d. One explicit retry succeeded using the
existing bond. Its cause remains unresolved; it must not be counted as fixed
by the transient 08/3e retry policy. Hardware acceptance remains incomplete.

Peripheral connection-parameter updates remain a separate compatibility task;
the working keyboard retries requests that Os64 rejects. That policy is unchanged.

## P5 acceptance checks

1. Boot the new build, wait for bond storage to finish loading, and establish a
   working `bond-justworks` session using the keyboard's current scanned address.
   Check encrypted readiness, actual typing, identity-key presence, automatic
   connection enabled, and `bond storage: saved`.
2. Power off the keyboard without issuing `disconnect`. Turn it on normally and
   wait for automatic discovery and encrypted reconnection. Type again without
   issuing `reconnect`. Record status and scan entries if it cannot reconnect.
3. Reboot Os64 after storage reports saved. Test both a keyboard already on at
   boot and one turned on later. Neither should require a new pairing.
4. Verify `disconnect` suppresses automatic connection for this boot, `reconnect`
   resumes explicit use, and saved `auto off` survives reboot. `auto on` enables
   the saved peer again. If a prior protocol failure stopped automatic attempts,
   use explicit `reconnect` to retry after addressing the reported error.
5. Disconnect, issue `forget`, wait for storage saved, and reboot. The keyboard
   must not reconnect with the forgotten bond. Pair explicitly to restore it.

If the keyboard changes its static identity rather than using an RPA resolvable
with its IRK, preserve the scan evidence. This implementation deliberately cannot
infer continuity from an advertised name.
