# AUDIO_CLASSIC.md — the design for A.2: a Classic Bluetooth link to the headphones

*Fable, 2026-11-13. The design row A.2.1 of `AUDIO_TRACKER.csv`.
BLUETOOTH_AUDIO.md (§ The A2DP path, § Pacing, § Coexistence) is the
argument and carries Quinn's review; this file is what gets built, checked
against the tree at `014d9809`. Specification sections are from Bluetooth
Core 5.4 unless another is named.*

*Status: awaiting one read by someone other than the author. Chris is in
and out (2026-11-13): each part is reviewed and the next stacked on it, and
he reads as he can. § For Chris lists what is his to overrule.*

## What A.2 is, and why it is three pull requests

At the end of A.2 the P5 holds an encrypted Classic link to the headphones,
made from either end, has asked them what they are over SDP and answered
the same question about itself, and carries no sound. The keyboard and the
mouse work throughout.

That is three kinds of work, stacked in this order:

| Part | What it is | Its proof on the P5 |
|---|---|---|
| **T, the transport** | The controller's ACL credits accounted in one place; more than one packet with the controller at a time; send buffers that are not the command's | The keyboard and mouse work exactly as before; `/sys/bluetooth/connection` shows the pools |
| **L, the link** | Classic HCI: connect out, accept in, Secure Simple Pairing, the bond, encryption, a third slot | The headphones connect, encrypted, from either end, and again after a reboot |
| **S, the services** | Classic L2CAP (dynamic channels, configuration, reassembly), SDP asked and answered | `/sys/bluetooth/connection` shows what the headphones say they are |

T changes code the keyboard depends on and adds no feature, so it goes
first and alone: if typing breaks, it is T. Nothing in A.2 can be tried in
QEMU (it has had no Bluetooth since version 5), so every layer is pure code
with a host model of its peer, as `bt_le.c` is, and each part ends with a
P5 run.

## The headphones this is proven with

Chris's older pair of Bose QuietComfort Ultra Headphones (over-ear, the
2023 generation), with the newer pair as a spare. Checked against Bose's
published specifications on 2026-11-13, not yet observed: Bluetooth 5.3;
SBC, AAC and aptX Adaptive, all carried over Classic A2DP; no LE Audio on
that generation. (The 2025 generation is Bluetooth 5.4 with the same
Classic codecs, and LE Audio as an optional beta that stays off for this
work.) Both do multipoint, two hosts at once. The older pair is bonded to
one other device, so it has a place free for the P5.

## What is there now (read at `014d9809`)

- `bt_host.c` owns the controller: one command in flight (`owner`,
  `credits`, which is the COMMAND credit of Command Complete and Command
  Status, `Num_HCI_Command_Packets`), the scan, and two LE peers
  (`BT_HOST_PEERS`).
- **ACL out**: one packet in flight for the whole host. `bt_le_tick` is
  told it may send when `!h->tx_used`, sends one packet, sets its own
  `outstanding` to 1 and waits for Number Of Completed Packets (event
  0x13), which `bt_host_event` hands to every LE peer for each to pick out
  its own handle. Nothing counts the controller's buffers; one-at-a-time
  makes the count unnecessary.
- `xhci_bt_acl_send` copies the packet into the first 512 bytes of the
  transmit page; the command's bytes live at offset 512 of the same page
  (`xhci_bt_runtime_send`). `tx_done` is one flag: the bulk OUT transfer
  finished.
- **ACL in**: `bt_host_receive` assembles one ACL packet from USB packets
  into `wire[BT_LE_ACL_MAX+4]` (1,028 bytes) and routes it by handle to an
  LE peer; an unknown handle is dropped. Each LE peer then does its own
  L2CAP reassembly into 68 bytes.
- `Read Buffer Size` (0x1005) and `LE Read Buffer Size` (0x2002) are sent
  by each LE peer during its own bring-up (`BT_LE_BUFFER`,
  `BT_LE_SHARED_BUFFER`) and the answers kept per peer (`acl_size`).
- `bt_scan.c` runs Classic inquiry beside the LE scan and lists both in
  `/sys/bluetooth/devices`.
- Bonds: two fixed slots, an 80-byte record each, `/home/bluetooth_bond`
  and `/home/bluetooth_bond_1`, loaded and saved from task context
  (`xhci_bluetooth_maintain`).

## Part T: the transport

### T1. One accountant for the controller's ACL buffers

A new pure file, `bt_acl.c`, owned by `bt_host_t`:

```c
typedef struct {
    uint16_t classic_size, classic_total;   // Read Buffer Size
    uint16_t le_size, le_total;             // LE Read Buffer Size; total 0 = LE shares the Classic pool
    bool     known;
    struct { uint16_t handle; uint16_t out; bool le, live; } link[BT_ACL_LINKS];
} bt_acl_t;

bool bt_acl_may_send(const bt_acl_t *, uint16_t handle);   // is there a buffer this link may use
void bt_acl_sent(bt_acl_t *, uint16_t handle);
void bt_acl_completed(bt_acl_t *, const uint8_t *event, size_t n);  // event 0x13, whole
void bt_acl_link_up(bt_acl_t *, uint16_t handle, bool le);
void bt_acl_link_down(bt_acl_t *, uint16_t handle);        // its outstanding packets are the controller's to drop: the buffers come back
```

- **The two replies are read once, by the host**, in the scan's start-up
  round (two phases added after `BT_SCAN_ADDRESS`), not by each LE peer.
  The LE peers' `BT_LE_BUFFER` and `BT_LE_SHARED_BUFFER` phases go; a peer
  takes `acl_size` from the accountant.
- **Shared or separate** is decided from `le_total`: zero means one pool
  (Vol 4 Part E § 4.1.1, § 7.8.2).
- **In one pool, LE is kept room.** A Classic link may not take the last
  `1 + live LE links` buffers. That reserve is what keeps a keystroke's
  acknowledgement from waiting behind audio. With two pools nothing is
  reserved.
- **A count that goes wrong fails the host closed**: a completion for a
  handle with nothing outstanding, or for more than is outstanding, is what
  `le_bad` treats it as today.
- Each LE peer still sends one packet at a time (its `outstanding` stays,
  as its own rule); it asks the accountant instead of `!tx_used`.

### T2. Send buffers of their own, and several in flight

- The command keeps its 258 bytes; the ACL buffers move to DMA storage of
  their own: `BT_TX_SLOTS` (4) buffers of `BT_ACL_WIRE_MAX` (1,028) bytes.
  A packet is copied into a free slot and a TRB queued on the bulk OUT
  ring; the ring already holds more than one.
- A slot is free again when ITS transfer completes. Completions on one
  endpoint arrive in the order queued, so the slots are a ring and a
  completion frees the oldest. `tx_done` becomes "no slot is in flight".
- **Two completions, two meanings**, kept apart in the names: the USB
  transfer finishing gives the SLOT back (`xhci.c`); Number Of Completed
  Packets gives the controller's BUFFER back (`bt_acl.c`).
- A send is refused, not truncated, when the packet is longer than a slot
  or than the pool's packet size, or no slot is free. The caller's state
  machine keeps the packet and tries next pass. Nothing can write past a
  slot: that is the overrun Quinn found waiting in the 512-byte region.
- The shutdown quiesce and every `crypto_wipe` of the transmit page are
  re-pointed at the new storage; a slot is wiped when it is freed.

### T3. Receive routed by a table

`bt_host_receive` looks the handle up in the accountant's link table and
calls that link's receiver: an LE peer's `bt_le_receive`, or (from part L)
the Classic link's. `wire` is already 1,028 bytes, which holds the AX210's
1,021-byte Classic packet and its header; a static assertion ties it to
`BT_ACL_WIRE_MAX`.

### T, proof

Host: `tools/test_bt_acl_host.sh` runs `bt_acl.c` beside a model through
random sends, completions and disconnections in both pool layouts, with a
mutant harness; the existing `test_bt_le_host`, `test_bt_loader_host` and
`test_bt_multi_host` are re-run against the new plumbing and extended with
two peers sending at once. P5: boot, type, move the mouse, reboot five
times; `/sys/bluetooth/connection` gains

```
acl pools: classic 8 x 1021, le 0 x 0 (shared), in use 0, reserved for le 3
```

and `/sys/usb` shows no new wait. No behaviour a person can see changes.

## Part L: the link

### L1. A third slot, of another kind

`bt_host_t` gains `classic[BT_CLASSIC_PEERS]` (one, for now) beside
`peers[]`. To a person it is **slot 2** of the same list, with the same
words (Chris's ruling, Q4):

```
slot 2 connect 4c:87:5d:xx:xx:xx     # an address from /sys/bluetooth/devices
slot 2 reconnect
slot 2 forget
```

`/sys/bluetooth/connection` prints it after slots 0 and 1, with a `kind:
classic` line. A Classic address given to slot 0 or 1, or an LE one to
slot 2, is refused with a line saying which slot takes it. The owner
numbering in `bt_host.c` (`1` the scan, `2..` the LE peers) grows by one
kind.

### L2. The controller's Classic setup

Sent once, in the start-up round, after the buffer sizes:

| Command | Value | Why |
|---|---|---|
| Set Event Mask (0x0c01) | adds Connection Request/Complete, Disconnection, Authentication Complete, Encryption Change, Link Key Request/Notification, PIN Code Request, Role Change, Mode Change, IO Capability Request/Response, User Confirmation Request, Simple Pairing Complete, Number Of Completed Packets | the events below |
| Write Simple Pairing Mode (0x0c56) | on | SSP, § 7.3.59 |
| Write Local Name (0x0c13) | `os64` | headphones show it, and some refuse a nameless host |
| Write Class Of Device (0x0c24) | 0x200104 | service class Audio, major class Computer, minor class Desktop: a computer that is a source of sound |
| Write Default Link Policy Settings (0x080f) | role switch and sniff allowed | a sink that asks to be master is let |
| Write Page Timeout (0x0c18) | 5.12 s (the default) | stated, not left to the controller's last host |
| Write Scan Enable (0x0c1a) | page scan ONLY while slot 2 holds a bond; inquiry scan never | see L4 |

Secure Connections host support is left off: P-192 Simple Pairing is what
every sink does, and it keeps the link key type to one case.

### L3. The link's states

`bt_classic.c`, pure, one `bt_classic_t` per slot, ticked by `bt_host_tick`
like an LE peer and given the events that are its own.

| State | Entered by | Leaves on |
|---|---|---|
| `IDLE` | boot; `forget`; a failed attempt's cleanup | `connect` / `reconnect` → `PAGING`; Connection Request from the bonded address → `ACCEPTING` |
| `PAGING` | Create Connection (0x0405) sent, with the inquiry's page-scan repetition mode and clock offset when the scan has them | Connection Complete: status 0 → `LINKED`; else → `STOPPED` with the status |
| `ACCEPTING` | Accept Connection Request (0x0409), role: stay slave unless asked | Connection Complete, as above |
| `LINKED` | a handle; `bt_acl_link_up` | we called and hold a bond → `AUTHENTICATING`; we called and hold none → `AUTHENTICATING` (which pairs); they called → wait for their authentication, or start ours after 2 s |
| `AUTHENTICATING` | Authentication Requested (0x0411) | Authentication Complete: 0 → `ENCRYPTING`; key missing (0x06) with a bond → the bond is stale: `STOPPED`, "re-pair"; else `STOPPED` |
| `ENCRYPTING` | Set Connection Encryption (0x0413) on | Encryption Change: on → `READY`; else `STOPPED` |
| `READY` | encrypted | Disconnection Complete → `STOPPED` (retryable or not by reason, as LE's manager judges); `forget` → `CLOSING` |
| `CLOSING` | Disconnect (0x0406) sent, or a cancel of a page (0x0408) | Disconnection Complete, or Connection Complete with an error → `STOPPED` |
| `STOPPED` | any failure, after its cleanup is confirmed | the manager's next attempt, or a command |
| `FAILED` | the controller cannot be trusted (a bad count, a reply that cannot be) | never: `h->failed` |

Events answered in any linked state, because the far end may start them:

- **Link Key Request** (0x17): the bond's key (Link Key Request Reply,
  0x040b), or the negative reply (0x040c), which makes the controller pair.
- **IO Capability Request** (0x31): answered with IO Capability Request
  Reply (0x042b, § 7.1.29): NoInputNoOutput, no OOB data, general bonding
  with no MITM protection required. **User Confirmation
  Request** (0x33): accepted (0x042c). That is Just Works, which is all a
  device with no keys and no display can do.
- **PIN Code Request** (0x16): negative reply (0x040e). Legacy pairing is
  not offered.
- **Link Key Notification** (0x18): the key and its type become the pending
  bond, saved when encryption comes up, as LE's is saved when its keys
  are distributed.
- **Role Change** (0x12), **Mode Change** (0x14): recorded for the status
  file. Sniff is A.4's.
- **Connection Request** (0x04) from any address but the bonded one, or
  for a link type other than ACL: Reject (0x040a, reason 0x0f, "unacceptable
  address").

One command is in flight at a time, as now; the link is a new owner kind.
Every command has the scan's deadline and its `timed_out` detail.

### L4. Who may call us

Page scan is on only while slot 2 holds a bond, and a Connection Request is
accepted only from that bond's address. So the machine is never
discoverable, and before a pairing nothing can connect to it. A pairing is
always started from our side, by a person typing `slot 2 connect`, after
putting the headphones in their own pairing mode. That is the LE keyboard's
rule, kept.

### L5. The bond

`bt_classic_bond_t`: the address, the 16-byte link key, its type, and the
name the inquiry heard (for the status file). One more fixed record,
`/home/bluetooth_bond_2`, same 80 bytes, same atomic replace, a version
byte that says "classic". Loaded and saved by `xhci_bluetooth_maintain`
with the other two. `forget` deletes it and turns page scan off.

### L6. Reconnection

`bt_manager.c`'s policy is the model, not the code: it is written around
LE scanning. The Classic manager is smaller. With a bond and `automatic`:
page the headphones once at boot and after a retryable disconnection, with
the same backoff ladder, and otherwise wait, because page scan is on and
headphones switched on will call us. A page is 5 s of radio time the
keyboard shares, so it is not repeated on a short timer.

### L, proof

Host: `tools/test_bt_classic_host.sh`, a model headphone that can be the
caller or the called, pair or produce a key, refuse, vanish mid-step, ask
for a role switch, and send every event out of order; random event soup
under the sanitizers; a mutant harness for the state table and the bond
codec. P5, by Chris, with `/sys/bluetooth/connection` open:

1. `scan`, then `slot 2 connect <address>` with the headphones in pairing
   mode: `READY`, encrypted, and the headphones say "connected".
2. Switch the headphones off and on: they call us, and it is `READY` again
   with no typing.
3. Reboot: `READY` again.
4. Type and move the mouse through all of it.
5. `slot 2 forget`; switch them off and on: nothing connects.

## Part S: the services

### S1. Classic L2CAP

`bt_l2cap.c`, pure, per link.

- **Reassembly**: ACL start and continuation fragments (the packet
  boundary flag) into one buffer of `BT_L2CAP_MTU` + 4. The MTU we state is
  1,017, so a whole PDU fits one of the AX210's ACL packets and what we
  SEND never needs fragmenting. What we receive may still be fragmented by
  the peer, and a length past the buffer drops the PDU and counts it.
- **Signalling** (CID 1), all of what a peer may send (Vol 3 Part A § 4):
  Connection Request/Response, Configuration Request/Response,
  Disconnection Request/Response, Information Request/Response, Echo
  Request/Response, and Command Reject for anything else.
- **Channels**: a small fixed table (four: SDP out, SDP in, and two left
  for AVDTP in A.3). A channel is open when BOTH directions' configuration
  has succeeded (§ 7.1). We ask for our MTU and nothing else. A peer's
  proposed retransmission or streaming mode is answered "unacceptable
  parameters" with Basic mode; a flush timeout is accepted as a hint.
- **Admission**: an incoming Connection Request is accepted for PSM 0x0001
  (SDP) now, and for 0x0019 (AVDTP) from A.3; anything else is "PSM not
  supported". Only on an encrypted link, except SDP, which the
  specification lets precede it.
- Every request we send has a timer (the RTX of § 6.2.1, 2 s here); an
  unanswered one closes the channel, not the link.

### S2. SDP, asked

`bt_sdp.c`, pure. One query: Service Search Attribute Request for the
Audio Sink class (0x110B), attributes 0x0000 to 0xFFFF.

- The answer is assembled across continuation states (Vol 3 Part B § 4.3)
  into a fixed 1,024-byte buffer; more than that, more than 16 rounds, or
  5 s, and the query ends with what it has, marked incomplete.
- The data-element parser is bounded: depth 6, every length checked
  against the bytes that remain, no recursion.
- What is kept: the AVDTP version from the protocol descriptor list, the
  A2DP profile version, the supported-features word. They go to the status
  file and to A.3.

### S3. SDP, answered

A headphone that called us asks what we are before it configures anything.
One static record, built at compile time and checked by a host test
against the parser:

| Attribute | Value |
|---|---|
| ServiceClassIDList | Audio Source (0x110A) |
| ProtocolDescriptorList | L2CAP, PSM 0x0019; AVDTP 1.3 |
| BluetoothProfileDescriptorList | Advanced Audio Distribution (0x110D) 1.3 |
| SupportedFeatures | 0x0001 (player) |

The responder answers Service Search, Service Attribute and Service Search
Attribute requests for it, each within the requester's stated maximum and
with continuation when the record does not fit, and an Error Response for
a malformed request. It holds no state between requests but a continuation
offset.

### S, proof

Host: an L2CAP peer that proposes modes we decline, configures in either
order, and fragments at every byte; an SDP server that cuts its answer
inside an attribute's value and one that never finishes; an SDP client
that asks our responder everything, with small maxima; both parsers fuzzed.
P5: after `READY`, the status file gains

```
sdp: audio sink, avdtp 1.3, a2dp 1.3, features 0x0003 (complete)
```

and the headphones stay connected instead of dropping an idle link after
their own timeout. If they do drop it, that is recorded, and A.3 is where
a stream gives them a reason to stay.

## What is seen and logged

- `/sys/bluetooth/connection`: the pools (T), slot 2 with its state, the
  far end's name, role, mode, the last disconnect reason, the SDP line.
- `/sys/usb`'s timeline: a line for each Classic state change, as for LE
  phases.
- The system log (`bluetooth:` lines, on by default): a connection that
  failed and why, a pairing refused, a count that went wrong. Never a key.

## What A.2 leaves out, on purpose

Booked in DEBTS.md by each part: more than one Classic peer; legacy (PIN)
pairing; Secure Connections; being discoverable; sniff (A.4); any L2CAP
mode but Basic; SDP for anything but the one record; a capture from
another host to compare against (worth having, and Chris's to take if a
step of L's proof fails).

## For Chris

Proceeding on these leans; say so to overrule one.

1. **The headphones are slot 2**, one Classic slot, in the same list.
2. **Nothing can connect to the machine before a pairing made from its own
   keyboard**, and it is never discoverable.
3. **No PIN pairing.** A device that can only pair with "0000" is refused.
4. **The name it gives is `os64`**, and it calls itself a desktop computer
   with audio.
5. **T goes in by itself first**, with a P5 run that should show nothing
   new, before any Classic code.
