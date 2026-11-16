# BLUETOOTH_AUDIO.md — design review

Reviewed by Codex, 2026-11-12, against local `userland` at
`249803cb385aa7bfb37966d514e8d223b2c6f8bd`.
The design was untracked; the reviewed file's SHA-256 is
`80f82a8e7a6e840e14d7e51081f4cc646e1ab09c8e4bcd0fdb832edf2a3c6ef2`.
Line references below identify that snapshot of [the design](BLUETOOTH_AUDIO.md).

The PCM device boundary, separate hardware-free test sink, and interrupt-first
sequence are sound directions. Keep the agreed kernel encoder and 48 kHz
format. Four protocol findings need resolution before implementation; the
playback clock and seek contract also need concrete decisions before the seam
is frozen. This is a design review, not evidence of working audio on hardware.

1. **[P1] Classic and LE cannot be assigned independent credits unconditionally.**
   Lines 105–109 and 335–337 give audio the complete Classic pool while LE keeps
   its existing allowance. If `LE Read Buffer Size` returns zero buffers, both
   transports use the pool returned by `Read Buffer Size`. Spending the full
   pool on audio and one more packet on LE exceeds controller capacity. This
   case is already supported by the current LE implementation:
   `bt_le.c:664–673` enters `BT_LE_SHARED_BUFFER` on that reply.

   Make credit accounting controller-owned, with shared/separate pools selected
   from both replies and outstanding packets tracked by connection handle.
   In the shared case, reserve or fairly schedule capacity for HID and control
   traffic within the advertised total. Test both layouts, interleaved
   completions, and disconnect with outstanding packets. This follows
   [Bluetooth Core, Vol 4 Part E, §§4.1.1 and 7.4.5][hci].

2. **[P2] Incoming connection support stops below the profile that must answer it.**
   Lines 86–93 accept an incoming bonded ACL connection, but lines 124–136
   describe an SDP client and an outbound AVDTP setup sequence. A headphone
   that initiates profile discovery/configuration can establish the encrypted
   link and then stall: nothing in the proposed path answers its service query
   or its `Discover` / `Get Capabilities` / `Set Configuration` requests.

   Include a local Audio Source service record and SDP responder, incoming
   L2CAP channel admission, and an AVDTP acceptor state machine alongside the
   initiator. Resolve simultaneous setup attempts into one session. Test a peer
   that initiates the profile, not merely the ACL link. A source must support
   both signalling roles; see [A2DP §§5 and 5.1.1.1][a2dp]. The failure scenario
   here is an inference from the client-only flow in the notes, not an observed
   behavior of the target headphones.

3. **[P2] SDP cannot be implemented as one request and one response.**
   Lines 124–128 explicitly reduce discovery to one exchange. A legal response
   can contain only part of the attribute data and a continuation token; even
   an individual data element may cross response boundaries. Parsing that
   first part as a complete record either rejects a valid peer or loses the
   attributes needed to configure it.

   Carry the opaque continuation state into subsequent requests and bound the
   aggregate bytes, transaction count, nesting, and elapsed time. Parse a
   complete assembled result or retain bounded parser state across fragments.
   Add a model that splits inside an attribute value and another that never
   finishes. See [Bluetooth Core, Vol 3 Part B, §4.7][sdp].

4. **[P2] RTP sequence numbers advance per packet, not per SBC frame.**
   Lines 137–139 say sequence and timestamp advance per frame, while the
   packetization proposal puts several SBC frames in each packet. With six
   frames per packet, advancing the sequence by six makes a receiver see five
   apparent missing packets on every arrival.

   Specify the counters separately: sequence advances by one per RTP packet;
   timestamp identifies its first sample and advances by the represented PCM
   sample frames per channel. For six SBC frames of 128 samples each, the next
   packet advances sequence by 1 and timestamp by 768. Test this independently
   of the encoder. See [RTP §5.1][rtp] and [A2DP §4.3.3.1][a2dp].

Before freezing the seam, settle these two contracts:

- **Position and discontinuities** (lines 202–208, 313–323). The API first says
  samples played, then says samples taken by the sink with downstream delay
  subtracted. Those are different positions. Define the accounting boundary,
  whether counts mean PCM frames per channel, the monotonic timestamp, stream
  generation, and behavior on pause, underrun, seek, and reconnect. A suggested
  contract is a submitted-frame count plus a separately identified estimate of
  audible position, both tied to one stream generation. A 200 ms full software
  ring adds 200 ms before downstream buffering; capacity alone adds no delay,
  but queued occupancy does. The QEMU model should verify that distinction.
- **Flush completion** (lines 212–215). The notes promise that Suspend/Start
  discards the headphones' queued audio. I have not established a normative
  guarantee for that claim from the sources retrieved in this review; do not
  encode it as a proven property of the test sink. Specify which local and
  in-flight queues a flush retires, how old completions are rejected, and what
  evidence or stream-recreation procedure establishes the remote boundary.
  Exercise immediate seek/resume on the target headphones. This is an open
  proof obligation, not a confirmed finding that every sink retains data.

The existing transport also imposes a concrete implementation prerequisite.
`xhci_bt_acl_send` in `xhci.c:1124–1134` copies into a 512-byte region of the
transmit page; command storage starts at offset 512. `bt_host_tick` serializes
transmission with `tx_used`, and receive assembly is sized by `BT_LE_ACL_MAX`.
The proposed larger audio packets and multiple outstanding controller packets
need a reviewed buffer/fragmentation/ownership design. Feeding a packet larger
than 512 bytes through the current send helper would overwrite command storage.
USB completion releases DMA ownership; HCI completion returns controller credit.
Those are separate events. Include this work in the transport slice rather
than assuming that adding the Classic protocol producer is sufficient.

The specification checks resolve two questions in the notes: 48 kHz SBC is
mandatory for sinks, and Table 4.7 gives bitpool 51, 115 bytes per frame, and
345 kbit/s for its recommended 48 kHz high-quality joint-stereo configuration.
Also, modern A2DP requires sink delay-report support; negotiate it before
falling back to a measured constant for an older peer. See
[A2DP §§4.3.2.1, 4.3.2.6 and 5.1.1.2][a2dp].

Validation performed: source inspection of `xhci.c`, `bt_host.c`, `bt_le.c`,
`bt_host.h`, `knet.c`, `kworker.c`, `signals.c`, and the doorbell implementation;
comparison with the primary specifications linked above. No implementation,
build, sanitizer run, QEMU boot, or hardware test was performed for this review.
The design file was left unchanged.

[hci]: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core_v7.0/out/en/host-controller-interface/host-controller-interface--hci--functional-specification.html
[sdp]: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/host/service-discovery-protocol--sdp--specification.html
[a2dp]: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/a2dp_v1-4-1_1752513648/A2DP_v1.4.1/out/en/index-en.html
[rtp]: https://www.rfc-editor.org/rfc/rfc3550.html#section-5.1
