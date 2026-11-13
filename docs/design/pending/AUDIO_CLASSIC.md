# AUDIO_CLASSIC.md — the design for A.2: a Classic Bluetooth link to the headphones

*Fable, 2026-11-13. The design row A.2.1 of `AUDIO_TRACKER.csv`.
BLUETOOTH_AUDIO.md (§ The A2DP path, § Pacing, § Coexistence) is the
argument and carries Quinn's review; this file is what gets built, checked
against the tree at `014d9809`. Specification sections are from Bluetooth
Core 5.4 unless another is named.*

*Status: read once by someone other than the author (a cold Opus read
against the source and from memory of the specification, 2026-11-13;
eighteen findings, folded in). Chris is in and out: each part is reviewed
and the next stacked on it, and he reads as he can. § For Chris lists what
is his to overrule. Where a fact about Bluetooth below is marked
(recalled), it came from the reader's memory or mine and is checked against
the specification when its part is built.*

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

Chris's older pair: the **Bose SoundLink around-ear wireless headphones II**
(marked "AE2 SoundLink"; 2015). His Bose QuietComfort Ultra Headphones
(2023: Bluetooth 5.3; SBC, AAC and aptX Adaptive over Classic A2DP) are the
spare. Neither has been observed yet; this is from published material,
checked 2026-11-13.

What is known of the SoundLink II, and how sure:

- **Classic Bluetooth, A2DP.** Bose's own support page: it works with
  devices of Bluetooth 2.1 or later that support the A2DP, hands-free and
  headset profiles. Certain. SBC follows, since every A2DP sink must take
  it.
- **Secure Simple Pairing.** It asks its host for Bluetooth 2.1, which is
  the version that made Simple Pairing mandatory. So § L3's refusal of PIN
  pairing should not meet it. Likely, not observed.
- **Its Bluetooth version and other codecs** are given differently by
  different shops (4.0 or 4.2; AAC, aptX). Unconfirmed, and nothing here
  depends on them.
- **It is a headset too** (hands-free and headset profiles, a microphone).
  A pair like that may ask its host for a voice link (SCO or eSCO) and for
  the hands-free service besides the music one. § L3 refuses the voice
  link and § S1 refuses the service's channel; whether the pair then stays
  for music alone is something only the P5 run can say, and it is the
  first thing to look at if it hangs up.
- **It pairs with two hosts** and is bonded to one today, so it has a place
  free.

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
- `bt_scan.c` CAN run a Classic inquiry (`bt_scan_start`), but the kernel
  never asks it to: every scan is `bt_scan_start_le`. So no Classic address
  is ever listed today, and a result keeps neither the page-scan repetition
  mode nor the clock offset an inquiry reports.
- The start-up round writes one event mask (`BT_SCAN_MASK`); of the events
  part L needs, it lets through only Disconnection Complete, Encryption
  Change and Number Of Completed Packets.
- An owner holds the one command slot across a whole chain of its commands
  (the start-up round, an LE pairing): tens of milliseconds as a rule, two
  seconds on a command that then fails.
- Bonds: two fixed slots, an 80-byte record each with the magic `OS64BT01`
  and no version byte, `/home/bluetooth_bond` and `/home/bluetooth_bond_1`.
  `BT_BOND_SLOTS` is also `BT_HOST_PEERS` and sizes the keyboards, the
  pointers and the timeline in `xhci.c`, so it cannot simply become three.
- In LE's words `connect` makes a session and saves nothing; `bond` pairs
  and saves; `reconnect` needs a saved bond.

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

- **The host reads the two sizes once**, in the start-up round, after the
  event mask, and keeps them in `bt_host_t` (the scan's struct is cleared
  by every scan). **The LE peers keep their own reads.** Each LE connection
  re-reads the sizes today (`BT_LE_BUFFER`, `BT_LE_SHARED_BUFFER`) and three
  host suites pin those phases; part T is the part that must not move the
  keyboard, so it leaves them. Two readers of one fact is booked in DEBTS.
- **Shared or separate**: one pool when LE Read Buffer Size answers a
  packet length of zero or a count of zero (Vol 4 Part E § 4.1.1, § 7.8.2;
  the specification's test is the length, the code's today is the count,
  and either means there is no LE pool to use).
- **In one pool, LE is kept room.** A Classic link may not take the last
  buffers, one for each live LE link, and is always left at least one. That
  reserve is what keeps a keystroke's acknowledgement from waiting behind
  audio. With two pools nothing is reserved. The AX210's real numbers are
  not known until part T prints them on the P5 (recalled: about four
  Classic buffers); the reserve is re-judged then.
- **What a wrong count costs is the link, not the host.** A completion for
  a handle the table does not hold, or with a count of zero, is ignored and
  counted, as LE ignores it today. One for more than a link has outstanding
  ends THAT link's session (LE's `le_fail`, which cleans up one peer). The
  host fails only for what already fails it.
- Each LE peer still sends one packet at a time (its `outstanding` stays,
  as its own rule); it asks the accountant instead of `!tx_used`.

### T2. Send buffers of their own, and several in flight

- The command keeps its 258 bytes; the ACL buffers move to DMA storage of
  their own: `BT_TX_SLOTS` (4) buffers of `BT_ACL_WIRE_MAX` (1,028) bytes.
  A packet is copied into a free slot and a TRB queued on the bulk OUT
  ring; the ring already holds more than one.
- A slot is free again when ITS transfer completes. Each slot remembers
  the TRB it queued and the link it was for. A completion is matched by its
  TRB pointer against the OLDEST slot in flight: on one ring completions
  come in the order queued while nothing fails, so anything else is a
  failed transport, never "free the oldest and carry on". A transfer that
  does not succeed halts the endpoint and the later ones never complete;
  that already fails the host (`xhci_bt_transfer`), and every slot still
  with the controller is wiped then.
- **Each slot has its own five-second deadline.** Today one deadline is
  pushed back by every send, which with several in flight would let the
  oldest wait for ever.
- **"Nothing of mine is in flight" is asked per link.** An LE peer's
  cleanup waits for the bulk endpoint to be idle and fails the whole host
  if it is not within three seconds; with Classic traffic sharing the
  endpoint that would cost the keyboard for a busy headphone. Cleanup waits
  for its own link's slots only.
- **Two completions, two meanings**, kept apart in the names: the USB
  transfer finishing gives the SLOT back (`xhci.c`); Number Of Completed
  Packets gives the controller's BUFFER back (`bt_acl.c`).
- A send is refused, not truncated, when the packet is longer than a slot
  or than the pool's packet size, or no slot is free. The caller's state
  machine keeps the packet and tries next pass. Nothing can write past a
  slot: that is the overrun Quinn found waiting in the 512-byte region.
- A slot is wiped when its transfer completes and never before. One still
  queued is the controller's to read, even on a failed transport: only a
  failed bulk transfer halts that endpoint, and a slot zeroed under a
  running one would go out as a packet of zeroes.

### T3. Receive routed by a table

`bt_host_receive` looks the handle up in the accountant's link table and
calls that link's receiver: an LE peer's `bt_le_receive`, or (from part L)
the Classic link's. The receiver is handed the whole HCI packet, header
and all, so the packet-boundary flag reaches part S's reassembly. `wire` is already 1,028 bytes, which holds the AX210's
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
(The line as built names the layout it found: `acl pools: classic 8 x 1021
bytes, in use 0; le 3 x 251 bytes, in use 0; ...`, or `acl pools: one,
classic and le together, ...`.)

### T, as built

- **Nothing is gated on the count yet.** The accountant is sized, told of
  every link, send and completion, and shown in the status file; the LE
  peers are still serialized among themselves by `tx_used`, exactly as
  before, so the keyboard's path decides nothing new. The first sender held
  to the count is the Classic link (part L). The multi-peer suite asserts
  at every step that the controller-wide count agrees with each LE peer's
  own.
- **§ T3 is deferred to part L**, which is the first to have a second kind
  of receiver. `bt_host_receive` is unchanged.
- **"Nothing of mine is in flight", per link, is deferred to the part that
  first sends Classic data** (S). With only LE sending, one packet at a
  time, "the endpoint is idle" is still the same question.
- **A completion that is not the oldest slot's is counted, not fatal**
  (`unmatched`, as the receive endpoints count theirs): an event for a
  transfer from before the runtime began must not end the transport. A
  slot whose completion never comes is found by its deadline.
- **A send the transport cannot carry fails the transport** (a packet
  longer than a slot, or a fifth with four in flight), with a `bluetooth:`
  log line. The send callback has no way to refuse, and a sender is meant
  to have asked first; nothing is truncated and nothing is written.
- The send slots follow the transmit endpoint's first page in ONE
  allocation (three pages), each slot inside a page; the page bring-up
  sends from and the command's bytes at offset 512 of it are where they
  were.
- A controller that refuses either buffer-size command never starts, as
  with any other start-up command: the round stops, cleans up and is tried
  again. On the AX210 the LE one has been answered at every LE connection
  since LE worked; Read Buffer Size is mandatory and is new to this
  controller only in the start-up round, so the P5 run is its first
  hearing.
- The two buffer-size commands run after the LE event mask, not after the
  Classic one: a scan on a controller already in service starts at the
  phase after them, so they run once without a flag to say so.
- Proof: `test_bt_acl_host` and its 50 mutants; `test_bt_scan_host`
  (the two replies, their lengths, never asked twice); `test_bt_multi_host`
  (the agreement above); `test_bt_loader_host` (the real start-up fills the
  accountant; four slots in flight, their order, the ring, each slot's
  deadline, the refusals under ASan, bring-up unchanged). QEMU has no
  adapter: it proves only that the kernel still boots and USB input works.

## Part L: the link

### L1. A third slot, of another kind

`bt_host_t` gains `classic[BT_CLASSIC_PEERS]` (one, for now) beside
`peers[]`, a struct of its own: `BT_HOST_PEERS` stays two, because it also
sizes the keyboards, the pointers and the bond files. To a person it is
**slot 2** of the same list, with LE's words meaning what they mean there
(Chris's ruling, Q4):

```
scan classic                         # an inquiry: Classic devices join /sys/bluetooth/devices
slot 2 bond 4c:87:5d:xx:xx:xx        # pair, and save the bond
slot 2 connect 4c:87:5d:xx:xx:xx     # pair for this session only; nothing is saved
slot 2 reconnect                     # needs a saved bond
slot 2 forget
```

- **A Classic scan has to exist first.** `scan classic` runs
  `bt_scan_start` (the inquiry the code already has and nothing calls), and
  a result keeps the page-scan repetition mode and clock offset the inquiry
  reported, which make the page that follows quick.
- `bt_host_command` parses `slot 2` on a path of its own (today any digit
  past `BT_HOST_PEERS` is refused) and gives it to the Classic link.
- **A slot says what kind of address it takes.** Dual-mode headphones use
  one public address for LE and Classic, so the address cannot say. An
  address the last scan listed as the other kind is refused with a line
  naming the slot to use; one the scan never saw is taken at its word.
- The owner of the command slot becomes a kind and an index, bounded where
  it is read: `host_retire` today indexes `peers[]` with any owner of two
  or more, and `bt_host_event` drops the reply to an owner it does not
  know.
- `/sys/bluetooth/connection` prints slot 2 after slots 0 and 1, with a
  `kind: classic` line.

### L2. The controller's Classic setup

Sent once, in the start-up round. The event mask is ONE write, the round's
existing `BT_SCAN_MASK`, widened: a second mask written later would be
overwritten by it.

| Command | Value | Why |
|---|---|---|
| Set Event Mask (0x0c01) | adds Connection Complete (0x03), Connection Request (0x04), Authentication Complete (0x06), Role Change (0x12), Mode Change (0x14), PIN Code Request (0x16), Link Key Request (0x17), Link Key Notification (0x18), Encryption Key Refresh Complete (0x30), IO Capability Request (0x31) and Response (0x32), User Confirmation Request (0x33), Simple Pairing Complete (0x36) | the events below |
| Write Simple Pairing Mode (0x0c56) | on | SSP, § 7.3.59 |
| Write Local Name (0x0c13) | `os64` | headphones show it, and some refuse a nameless host |
| Write Class Of Device (0x0c24) | 0x200104 | service class Audio, major class Computer, minor class Desktop: a computer that is a source of sound |
| Write Default Link Policy Settings (0x080f) | role switch and sniff allowed | a sink that asks to be master is let |
| Write Page Timeout (0x0c18) | 5.12 s (the default) | stated, not left to the controller's last host |
| Write Connection Accept Timeout (0x0c16) | 5.06 s (the default) | how long the controller holds a caller while we find a turn to answer: the bound § L3's latching is built against |

**Write Scan Enable (0x0c1a) is not a start-up command.** Whether there is
a bond is not known until `xhci_bluetooth_maintain` has read the disk,
after start-up. It is the Classic link's own command, sent when a bond is
loaded, saved or forgotten: page scan only (0x02) while a bond is held,
nothing otherwise, inquiry scan never.

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
| `LINKED` | a handle; `bt_acl_link_up` | we called → `AUTHENTICATING` (which pairs when there is no key); they called → Encryption Change on → `CHECKING` (the side that did not ask for authentication is sent no Authentication Complete: recalled), or after 2 s with no encryption → `AUTHENTICATING` |
| `AUTHENTICATING` | Authentication Requested (0x0411) | Authentication Complete: 0 → `ENCRYPTING`; key missing (0x06) with a bond → the bond is stale: `STOPPED`, "re-pair"; else `STOPPED` |
| `ENCRYPTING` | Set Connection Encryption (0x0413) on | Encryption Change: on → `CHECKING`; else `STOPPED` |
| `CHECKING` | Read Encryption Key Size (0x1408) | 16 bytes → `READY`; fewer → `CLOSING`, "weak encryption refused" (the key-size negotiation attack) |
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
  not offered. Simple Pairing has been mandatory since Bluetooth 2.1
  (2007), so any pair with a version of 4 or more does it; a PIN request
  from the test pair would be a finding to bring to Chris, not a case to
  code for now.
- **Link Key Notification** (0x18): the key and its type become the pending
  bond, saved when encryption comes up, as LE's is saved when its keys
  are distributed.
- **Role Change** (0x12), **Mode Change** (0x14): recorded for the status
  file. Sniff is A.4's.
- **Connection Request** (0x04) from any address but the bonded one:
  Reject Connection Request (0x040a, reason 0x0f, "unacceptable address").
  For a synchronous link (SCO, eSCO), from anyone: Reject Synchronous
  Connection Request (0x042a; recalled).

**Answering in time with one command slot.** These events want an answer
while the scan or an LE peer may hold the slot. The controller waits about
five seconds for a Connection Request's answer and thirty for a pairing
step's (recalled), and an owner holds the slot for tens of milliseconds as
a rule and two seconds at the worst, so one command at a time is enough,
given three things the code must do: every such event is LATCHED in the
link (what was asked, for whom) and never answered from the event path; a
link with an answer latched takes the FIRST turn when the slot is released,
ahead of the scan and the LE peers; and an answer that has waited past its
bound is dropped with the link, and logged. The link's data packets say
what kind of fragment they are: a first fragment is sent as "start,
automatically flushable" (packet boundary 0b10), which every controller
takes; LE's 0b00 on a Classic link needs a feature bit (recalled).

Every command has the scan's deadline and its `timed_out` detail.

### L4. Who may call us

Page scan is on only while slot 2 holds a bond, and a Connection Request is
accepted only from that bond's address. So the machine is never
discoverable, and before a pairing nothing can connect to it. A pairing is
always started from our side, by a person typing `slot 2 connect`, after
putting the headphones in their own pairing mode. That is the LE keyboard's
rule, kept.

### L5. The bond

`bt_classic_bond_t`: the headphones' address, our own (a bond made as one
adapter is not offered as another, as LE checks), the 16-byte link key, its
type, and the name the inquiry heard. It has a codec, a file and a manager
of its own, apart from LE's arrays: `/home/bluetooth_bond_classic`, a fixed
record with the magic `OS64BC01`, written by the same atomic replace.
`xhci_bluetooth_maintain` loads and saves it beside the two LE records.
`bond` saves it when encryption comes up; `forget` deletes it and turns
page scan off.

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

1. `scan classic`, then `slot 2 bond <address>` with the headphones in
   pairing mode: `READY`, encrypted, and the headphones say "connected".
2. Switch the headphones off and on: they call us, and it is `READY` again
   with no typing.
3. Reboot: `READY` again.
4. Type and move the mouse through all of it.
5. `slot 2 forget`; switch them off and on: nothing connects.

**What part L does not promise:** that the headphones STAY. A headphone
that has just connected asks its host questions over L2CAP at once
(recalled), and part L answers none of them, so the pair may hang up after
a few seconds. `READY` reached from each end is L's pass; staying connected
is S's. The disconnect reason is shown either way.

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
  supported" (result 0x0002). SDP may precede encryption; any other PSM
  asked for on a link that is not yet encrypted is answered "security
  block" (0x0003), not "not supported", so the peer tries again later.
- Every request we send has a timer (the RTX of § 6.2.1, 2 s here). A
  Connection Response of "pending" replaces it with the long one (ERTX,
  60 s). An unanswered request closes the channel, not the link.

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
| ServiceRecordHandle (0x0000) | 0x00010001: mandatory, and what a Service Search returns |
| ServiceClassIDList | Audio Source (0x110A) |
| BrowseGroupList | PublicBrowseRoot (0x1002), for a peer that browses |
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

Booked in DEBTS.md by each part: two readers of the controller's buffer
sizes (T); more than one Classic peer; legacy (PIN)
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
6. **Encryption with a key shorter than 16 bytes is refused**, and the
   link closed.
7. **`slot 2 bond` saves, `slot 2 connect` does not**, as for the keyboard.
