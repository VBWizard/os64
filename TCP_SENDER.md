# TCP sender contract

The 64 KiB ring retains accepted application bytes until cumulative ACK
retirement. Writes may block for ring space; close queues FIN behind the bytes
and detaches the handle. All transitions below hold the connection lock.

## Sequence and ownership

- `snd_una` is the oldest unacknowledged sequence unit. The ring head starts
  there while data remains. SYN and FIN each occupy a sequence unit but are
  excluded from congestion FlightSize.
- `snd_max` is the normal submission high-water mark. It includes packets
  parked for ARP and driver drops, because TCP owns their recovery. It does
  not rewind on timeout. `snd_nxt` is the output cursor, which may rewind to
  `snd_una` for go-back-N recovery under slow start.
- `snd_fin_seq` is fixed when close queues FIN. `snd_fin_sent` records a
  normal FIN submission and survives cursor rewinds. Thus an ACK for the
  original FIN remains valid after retransmission starts.
- A persist probe offers one byte or FIN at `probe_seq`. `probe_pending`
  permits an ACK through that unit without inventing normal flight or an
  RTO. Accepted probes retire through the same ACK path as normal data.
- `snd_wl1`/`snd_wl2` order window updates by peer sequence and ACK. An old
  update cannot overwrite a more recent window with the same ACK.

## Submission contract

IPv4 distinguishes `SENT`, `PARKED`, `DROPPED`, and `INVALID` at submission.
SENT means the driver accepted the frame. PARKED means ARP resolution is
pending (its holding slot may already be occupied). DROPPED means a driver
returned a negative result, including queue pressure. These outcomes commit
normal sequence responsibility; PARKED and DROPPED stop the output pass.
Ordinary ACK/retransmission logic then recovers loss. There is no next-tick
local retry path. INVALID means IPv4 rejected packet construction, currently
an MTU violation, and terminates the connection.

A driver return code alone cannot distinguish queue pressure from permanent
failure: r8125 uses -1 for a full ring. No NIC callback or link-status API
change is required. `/sys/net/tcp` counts `tx_local_drops`; segment and byte
counters count submission attempts, not delivery. Only the peer verifies
receipt. The `inflight` column includes submitted SYN/FIN units; congestion
FlightSize excludes them.

## Clocks and lifetime

`send_timer` has one owner at a time:

| Event/state | Timer and action |
|---|---|
| Normal submitted units outstanding | RETRANSMIT; new tail submissions do not postpone its deadline |
| Advancing ACK with remaining flight | Restart RTO, reset retry/backoff state |
| RTO expiry | Back off; rewind cursor; resend under slow start; retain high-water mark even after a local drop |
| Zero peer window with data or FIN pending | PERSIST; cancel RTT sampling, rewind cursor, leave fast recovery |
| Persist expiry | Offer one unit; interval doubles from max(estimated RTO, 1 second), capped at 8 seconds |
| Nonadvancing probe reply or dropped probe | Preserve the persist cadence and congestion window |
| Window reopens | Leave persist; normal output and, if needed, RTO resume |
| No submitted units or pending zero-window work | IDLE |

Detached lifetime is a separate 30-second no-ACK-progress deadline, renewed
only by an advancing ACK. It covers queued output, persist, and FIN_WAIT_2.
Expired connections close and release their buffers through the existing
cleanup path. Owned persist connections have no orphan deadline. Normal RTO
retry exhaustion still applies to owned connections and may terminate a
connection before the detached deadline. Link-up events do not renew either
budget. TIME_WAIT uses its existing independent port-reuse interval.

## Congestion and RTT

FlightSize uses the high-water mark, not the retransmission cursor. The first
timeout for a head computes ssthresh; repeated timeouts of that head retain
it. Limited transmit is confined to the first two duplicate ACK events, one
new segment per event, and its bytes are excluded from the fast-retransmit
threshold calculation. NewReno partial ACKs deflate by acknowledged data and
add back one MSS only when at least one MSS was acknowledged. Recovery marks
are invalidated once retired, before they can become ambiguous after 2 GiB.
A sender idle for an estimated RTO restarts at no more than its initial
window. Retransmissions and probes cancel the RTT stopwatch (Karn).

The existing local policy uses a 10-MSS initial window, a 200 ms RTO floor,
and an 8-second cap. These are project choices, not a claim of full RFC 6298
conformance. SACK, pacing, PMTU discovery, general receive-segment validation,
and close-with-unread-data RST behavior are outside this change.

## Verification and integration

`ASAN_OPTIONS=detect_leaks=0 tools/test_tcp_host.sh` tests actual TCP function
bodies with deterministic time, checksummed peer packets and an IPv4 stub.
The suite checks submission dispositions, owned/detached lifetimes, timeout
and NewReno arithmetic, probe schedules, ring/sequence wrap, and FIN ordering.
It does not simulate SMP, DMA, or the scheduler. LeakSanitizer may be enabled
by omitting the environment override outside ptrace-based environments.

QEMU transfer evidence belongs in VERIFICATION.md. PHY negotiation and
runtime link reporting remain in Fable's separate r8125 branch. Validate the
combined tree on the production P5 before attributing physical throughput or
cable-reconnect behavior to either change.
