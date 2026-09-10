# The network stack, as it stands

*2026-09-07. This is the present-tense inventory: what os64 speaks today,
layer by layer, and what each layer deliberately does not. It is not the
design record — NETWORK.md is the plan and its rulings, TCP_SENDER.md the
sender's contract, DOORBELL.md the bottom half, RTL8125.md one driver,
BROWSER.md the campaign that drives the next slices. Every section names
the file that owns it; when this page and the code disagree, the code is
right and this page is stale. The send window described under TCP is
PR #65; until it lands, the sender is stop-and-wait.*

## The shape

A frame arrives at a NIC. The driver's interrupt handler rings a doorbell
and returns; `knet`, one kernel thread pinned to the BSP, wakes, drains
every registered NIC, and hands each frame up: ethernet demuxes by
ethertype to ARP or IPv4; IPv4 validates and demuxes by protocol to ICMP,
UDP or TCP; each of those finds the conversation the frame belongs to and
delivers into it, waking the thread parked on it. Outbound is the mirror:
a program writes to a handle, the conversation builds a segment, IPv4
decides on-link or gateway, ARP supplies the MAC, ethernet frames it, the
driver's ring takes it.

Three facts shape everything below. **os64 is a host, not a router**: it
never forwards. **It is single-homed**: one machine address, however many
cards. **Nothing textual crosses the syscall boundary**: a program dials a
bang path, the library lowers it to a struct, the kernel owns the wire and
does every byte swap at the packet edge (`net_wire.h` is the whole swap
surface).

| Layer | Owner | In a sentence |
|---|---|---|
| NIC drivers | `virtio_net.c`, `e1000.c`, `r8125.c` behind `net_device.h` | frames in, frames out |
| Bottom half | `knet.c`, `doorbell.c` | one thread drains every NIC, runs the timers |
| Ethernet | `ethernet.c` | DIX framing, ethertype demux, runt padding |
| ARP | `arp.c` | who has this IP; a 16-entry cache; one parked frame per neighbour |
| IPv4 | `ipv4.c` | validate, demux, the one routing question a host has |
| ICMP | `icmp.c`, `icmp_conn.c` | echo, both directions |
| UDP | `udp.c`, `udp_conn.c` | ports, datagrams, connected conversations |
| DHCP | `dhcp.c` | the address, the mask, the router, a name server |
| TCP | `tcp.c` | the stream: active open, a real sender, a held receiver |
| The API | `abi/include/os64/net.h`, `libos64/net.c` | dial, read, write, close |
| The resolver | `libos64/resolve.c` | hosts files, then one DNS question |
| The eyes | `/sys/net/*` | counters and rows for every layer that has them |

## The drivers

`net_device.h` is the seam, designed against two drivers from day one so
it could not become one driver's private wrapper. A device moves frames;
nothing above it may assume a cable, a PHY or a PCI bus, because the
wireless future presents ethernet-shaped frames too. Drivers register
once and live for the life of the system.

- **virtio-net** (QEMU). Polled: the tick rings knet's bell once every
  10 ms and the queue is drained then. The transmit path reaps
  completions before refusing a frame, because a burst of ACKs that
  outran the ring cost a retransmit timeout each until it did. No
  interrupt is wired (DEBTS: MSI-X).
- **e1000** (QEMU, VirtualBox). Legacy INTx through the IOAPIC, adopted
  once the IMCR switch is done; the 82540EM has no MSI of any kind. The
  boot-time probe that finds its GSI is bounded at 200,000 spins per
  candidate, because under TCG each was forty seconds.
- **RTL8125** (the P5). MSI addressed at the BSP. The driver programs the
  PHY's advertisement at boot (10/100/1000, and 2.5G as the PHY's own
  default) and reports both sides of the negotiation on the boot line and
  in `/sys/net`. The register map is confirmed against the vendor's GPL
  driver, each definition naming the vendor's spelling; RTL8125.md is the
  record.

Boot tokens: `NONET` (no networking at all), `NOR8125`, `NETPOLL` (every
NIC drained at the tick only, the flashlight for an interrupt suspect),
`IP=` and `GW=` (static address and gateway, which also switches DHCP
off), `DEBUG_NET` (the stack's own commentary in the log).

## The bottom half

Since PR #67 the stack runs in a thread. `knet` parks on one doorbell; a
NIC's ISR rings it in microseconds with no lock; the tick rings the same
bell once per tick so a NIC with no interrupt keeps its cadence and the
protocol timers run when the wire is quiet. A reader parked on a
connection wakes when its bytes land, not at the next tick, which is what
took the P5 from 33 to 180 Mbit/s on the same wire. `/sys/net/knet`
counts the wakes, the drain rounds and the longest single wake.
DOORBELL.md carries the argument and the numbers.

## Ethernet

The 14-byte DIX header, unchanged since 1982. Ethertype is the demux key
and the whole reason the layer exists as code; 0x0800 goes to IPv4,
0x0806 to ARP, anything else lands on a counter. Frames shorter than the
64-byte minimum are padded on the way out, because receivers may still
discard runts forty years after the physics that needed them died. Every
arrival either demuxes or lands on a named number.

## ARP

RFC 826, the cache and the question. Sixteen entries, oldest evicted when
full, a 60-second lifetime that lookups do not refresh. The waiting room
holds ONE frame per unresolved neighbour and a later frame replaces it:
this is the fix for first-packet loss, deliberately not a queue, on the
argument that a sender with two frames outstanding is a protocol with its
own retransmission (4.4BSD's `la_hold` made the same choice). TCP knows
this and holds its output after a parked segment; see the ARP hold under
TCP. A resolved MAC releases the parked frame before anything else may
send to it.

## IPv4

RFC 791 as a host reads it. Arrivals are validated (version, header
length, total length against the frame, header checksum) and demuxed by
protocol. Departures answer the one routing question a host has, on my
link or via the gateway, and carry DF with TTL 64. **Fragments are
neither sent nor reassembled**: an arrival with MF or an offset is
dropped, counted and logged (DEBTS). Header options are not parsed. There
is no forwarding, and there is one address (`/sys/net/ip`).

The submission contract for the layers above: `ipv4_send_from_ex` reports
SENT, PARKED (held for ARP, or dropped when no slot is free), DROPPED (the
driver refused: a full ring or a failure) or INVALID (could not be built,
today an MTU violation). TCP treats PARKED and DROPPED as its own loss to
recover; INVALID ends the connection.

## ICMP

The echo pair, both directions: os64 answers pings and sends them. An
ICMP handle (`icmp!host`, no port) is a conversation whose identifier the
kernel assigns and filters on, the way it assigns a port, so one program
can never read another's replies; the sequence number is the kernel's
too, and a program that must match a reply puts its marker in the
payload, which is what every ping has done since 1983. **No error
messages are emitted** (unreachable, time exceeded) and **none are acted
on**: a TCP connection learns nothing from an ICMP unreachable and waits
out its own timer (DEBTS).

## UDP

RFC 768: ports, a checksum computed on every departure and verified on
every arrival that carries one. Inside the kernel a bind table lets
in-kernel consumers claim a port (DHCP first). For programs, a UDP handle
(`udp!host!port`) is a connected conversation: an ephemeral local port, a
filter on the dialed peer so a stranger's datagram never reaches the
handle, and a ring of eight whole datagrams — a slot holds anything an
MTU-1500 link can carry — where a ninth arrival drops on a counter,
because the bound is the flow control and dropping is what UDP promises.
One write is one datagram, one read returns one datagram, short if the
buffer is smaller. Well-known ports the kernel itself speaks: 67/68 for
DHCP. A UDP reader still wakes at the tick rather than on arrival
(DEBTS); TCP's readers do not.

## DHCP

RFC 2131 over the 1985 BOOTP packet. Four states: SELECTING, REQUESTING,
BOUND, GAVE_UP, with DISCOVER and REQUEST resent on a timer. It runs by
default when a NIC exists and no `IP=` was given; until BOUND the static
10.0.2.x convention stays live, so a dead server degrades to the
hypervisor's NAT and says so in the log. From the ACK it takes the
address, the mask, the router, and the first name server (option 6),
published in `/sys/net/dhcp` with the lease's counters. **The lease is
recorded, not renewed**: no T1/T2 timers, no RELEASE, no DECLINE (DEBTS;
slirp and VirtualBox leases are effectively eternal, a real LAN's are
not).

## TCP

RFC 793's eleven states, of which os64 uses the active-open nine: there
is **no listener** (DEBTS; `telnetd` is named as its first customer, and
the syscall table reserves the shape). The connection is a handle
(`tcp!host!port`); the handshake sends the MSS option, and a peer that
advertises one below 48 has it ignored and the 536 default kept, because
a tiny MSS is a division by zero waiting in the congestion arithmetic;
the initial sequence number is a fresh draw from the entropy pool
(RANDOM.md), and the ephemeral port sequence starts at a random offset.

**Receiving.** A 64 KB ring is the advertised window, which is the most a
16-bit field can say. A segment inside the window but ahead of sequence
is HELD in the ring at its own offset and absorbed when the gap closes,
up to sixteen held ranges; the sender is told only where we are, never
what we hold (no SACK). A window that closed is reopened with an explicit
update once the reader has drained half the ring. Silly-window avoidance
on the receive side rounds a useless window down to zero.

**Sending** (PR #65; TCP_SENDER.md is the contract). A 64 KB send ring:
`write` queues and returns, blocking only when the ring is full, and
wakes on the ACK that makes room. As many segments as the peer's window
and the congestion window allow are in flight; `snd_max` is what has been
submitted and is TCP's responsibility, `snd_nxt` the output cursor.
Congestion control is RFC 5681 with an initial window of ten segments
(RFC 6928, argued on merit), slow start, congestion avoidance, fast
retransmit on three duplicates, fast recovery with NewReno's partial-ACK
rule (RFC 6582) and limited transmit (RFC 3042). The retransmit timer is
measured (Jacobson/Karels, Karn's rule for samples), 200 ms to 8 s,
doubled per timeout, six timeouts before giving up, and a timeout resends
everything outstanding from the head under slow start (4.4BSD's
go-back-N). Sender-side silly-window avoidance. A peer's zero window is
probed on the persist timer at 1/2/4/8-second intervals for as long as
the handle is owned; a detached connection that makes no ACK progress
for 30 seconds is given up. A local drop by the driver is committed like
packet loss and recovered by the timer; a segment parked for ARP sets a
hold, and output waits until that segment is acknowledged or resent
rather than replace it in the waiting room. `close` queues a FIN behind
the ring and returns at once; the poll finishes the closing dance and the
30-second TIME_WAIT in the background. A dead connection lingers in
`/sys/net/tcp`'s morgue for 15 seconds so the aftermath can be read.

**Not offered, each booked in DEBTS**: a listener, Nagle (a keystroke is
its own segment, which is what a terminal wants), window scaling, SACK,
keep-alive, a write-side half-close, reaction to ICMP errors, and an RST
on close with unread data (a FIN goes, and the peer stalls until its own
timeout).

## The API

One verb opens everything: `os64_dial("proto!host!port")` returns a
handle or one of twelve named refusals, because os64 has no errno and the
return value is the reason. `tcp!` for a stream, `udp!` for datagrams,
`icmp!host` for echo. Host is a dotted quad or a name; port is a number
(no service names). The handle then obeys the house read/write/close
contract, and `os64_read_for` puts a deadline on a read. The refusal
vocabulary is one table in libos64 (`os64_dial_reason`), so "connection
refused" and "timed out" read the same on every glass — they send a
person to different machines. There is no sockets API and never will be
at the kernel boundary; a Berkeley veneer, if one is ever wanted, is
libos64's business.

## The resolver

Ring 3, per process, in libos64. Two answers in order: the hosts files
(`addr name [alias...]`, read up the config ladder and MERGED so a
`/home/hosts` line sits over the system's), then one DNS question to one
server, A records only, five seconds across two tries. The server is
`nameserver =` in `net.conf` on the ladder, else the one DHCP was given.
The query id is two bytes from `/dev/random`, since a guessable id is the
Kaminsky poisoning surface. No cache, no search list, no TCP fallback, no
IPv6 (DEBTS names the cache as the first to arrive, the day one process
resolves twice).

## The eyes

- `/sys/net/ip` — the machine's address, mask and gateway.
- `/sys/net/dhcp` — state, lease, name server, the conversation's counters.
- `/sys/net/tcp` — machine-wide counters (segments, retransmits, fast
  retransmits, window probes, local drops, RTT samples, held and dropped
  out-of-order segments, resets), then one row per connection: state,
  MSS, window, buffer fill, bytes each way, retransmits, RTO and SRTT,
  flight, send queue, cwnd, flags (`det`, `zwin`, `rst`). netstat's data.
- `/sys/net/knet` — the drainer's wakes and drain rounds.
- `/sys/net/<card>` — one NIC: model, MAC, MTU, link speed and duplex, the
  raw PHY status word, traffic counters.
- `DEBUG_NET` in the log: `TCPTX`/`TCPRX`/`TCPWIN` per segment, every
  timeout, fast retransmit and zero-window probe, DHCP's conversation,
  the drivers' own commentary.

## What speaks it

Programs in `/bin`: `ping`, `whois`, `os64get` (HTTP/1.0 and 1.1, chunked
bodies, redirects, gzip, native HTTPS through libtls, and an optional
terminating proxy through `tools/tlsproxy.py`),
`gopher` (the arrow-key client), `ntp`. Fixtures in `/tests`: `netsend`
(an upload with the sink's verdict as its clock), `netclose`, `dialtest`,
`fetchtest`, `rngprobe`.

Host side, in `tools/`: `cable.py` (the chaos rig: loss, delay, reorder,
duplication, link cuts, plugged in through QEMU's filter-redirector),
`tcpsink.py`, `os64serve.py`, `httptestd.py`, `gophertestd.py`,
`tlsproxy.py`, and the differential harnesses `test_tcp_host`,
`test_http_host`, `test_gopher_host`, `test_tcp_close.py`.
VERIFICATION.md carries the rig recipes and the measured tables.

## Not in the stack, on purpose

IPv6 (no v6 code; the seams do not assume four-byte addresses
gratuitously). TLS (ring 3, borrowed, never kernel; BROWSER.md). Wi-Fi
(the declared real-hardware endgame; this arc's obligation is seam
discipline). A sockets API. Forwarding. Fragments. Jumbo frames. Hot
unplug. Everything else that is absent and known is a row in DEBTS.md with
its gate beside it.
