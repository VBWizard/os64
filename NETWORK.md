# NETWORK.md — os64 gets a voice

*2026-07-27. The networking arc, chosen by Fable and green-lit by Chris
("what Fable wants, Fable gets"). This document is the arc's constitution:
the layer map, the build order, the driver seam, and — most importantly —
the menu of API rulings that are CHRIS'S to make before the syscalls take
shape. Decisions marked RATIFIED are settled; decisions marked OPEN await
his ruling. Kernel plumbing (drivers, stack, syscalls) = Fable's; the
utilities that ride it (ping and friends) = Chris's, per the standing
labor division.*

## The stance: take the protocols, refuse the API

Two very different things travel under the name "networking," and os64
treats them opposite ways:

**The protocols — Ethernet, ARP, IPv4, ICMP, UDP, TCP — are the interop
contract.** Their wire formats are not ours to improve: RFC 791 (1981) and
RFC 793 (1981) describe the bytes every other machine on Earth expects, and
a divergent packet is just a broken packet. On the wire, os64 is a strict
conformist. (The RFCs are also *good* — Postel's TCP spec has survived 45
years of the most adversarial deployment environment in computing.)

**The API — how a program asks for a connection — is a local convention,
and history's default answer is not sacred.** Berkeley sockets shipped in
4.2BSD (1983, Bill Joy et al., DARPA-funded) and every OS since has copied
it, warts intact: the `sockaddr` cast circus, `htons()` ceremony at every
call site, `listen()`/`accept()`'s vestigial backlog integer, error
reporting through errno. Windows copied it as Winsock in 1991 — when even
your competitor's API is the one you clone, that's how deep the groove is.
Plan 9 looked at all of it and wrote `dial()` instead: one call, takes a
string, returns something you read and write. os64 follows Plan 9's
*courage*, not necessarily its shapes — our shapes come from the rulings
below, and from the house doctrine that already exists: **handles you
read/write** (files, pipes, and windows already work this way), **typed
structs over text parsing** (the memory(out) precedent), **in-band errors,
no errno**.

## The layer map and build order

Each phase lands with its own verification and its own visible payoff —
no six-week silence and then "networking works."

### Phase 1 — the NIC driver (virtio-net first, e1000 second)

The seam comes first: `net_device_t`, registered exactly the way
`block_device_info_t` taught us — the stack above speaks to the
abstraction, drivers register into it, and the abstraction is designed
against TWO implementations from day one because a seam proven against one
implementation is just a wrapper (AHCI/NVMe learned us that).

- **virtio-net first.** The spec is clean, modern, and QEMU-native; the
  virtqueue descriptor/avail/used ring discipline is the same shape every
  paravirt device uses, so learning it pays twice. VirtualBox also
  emulates virtio-net, so ONE driver covers both hypervisors.
- **e1000/e1000e second.** The Intel 8254x manual is the classic teaching
  datasheet of osdev — for a learner's OS, the canonical real-hardware
  NIC programming model belongs in the tree. It also proves the seam.
  **DONE 2026-08-05** (`e1000.c`, 82540EM + siblings): registers instead
  of capabilities, a serial EEPROM instead of a feature handshake, RX/TX
  descriptor rings instead of virtqueues — and the entire protocol stack
  above it did not change by one line. The receipt: the same 29-test
  post-boot suite that passes on virtio passes on the e1000, byte for
  byte, DHCP lease through TCP RST handling. The seam's charter, paid.
  It also collected a debt the first driver could not: `net_device_t`
  grew `rx_errors`, because virtio is software and software does not
  corrupt frames in flight — only real silicon has an errors byte in
  every receive descriptor, and only a second implementation could have
  found the asymmetry. (e1000e/82574L is deliberately NOT claimed: it is
  a PCIe part with a different init dance, and it gets its own slice on
  the day it has a reason — likely the same day interrupts do.)
- **RX/TX are DMA rings** — descriptors pointing at buffers, device and
  driver chasing each other around a circle. Same instincts as NVMe
  submission/completion queues, new sport. Buffers come from the
  allocator (HHDM-reachable while allocated, per the lazy-HHDM rule);
  the tripwire will catch any stale-buffer sin the way it catches
  everything else.
- **Interrupts, not polling, from the start.** xHCI got away with polling
  because keyboards are slow; packets are not. MSI/MSI-X wants doing
  properly here. Mind the house gotcha: AP-routed vectors must be ≥0x40
  (AP TPR masks low vectors — already bitten once, already documented).
- Milestone: `NETDEBUG` boot line prints the MAC address read from the
  device, and a transmitted test frame appears in the host-side packet
  capture (see Verification below). **The OS's first spoken word.**

### Phase 2 — Ethernet, ARP, IPv4, ICMP: the OS answers a ping

- **Ethernet framing**: dst/src MAC + ethertype demux (ARP vs IPv4).
- **ARP**: request/reply + a small cache with expiry. The first protocol
  state machine in the tree — tiny, and a perfect dry run for TCP's
  giant one. (ARP is 1982's RFC 826, and its header is self-describing
  enough that it still handles hardware nobody imagined then.)
- **IPv4**: header parse/build + checksum (the internet checksum module
  gets written ONCE, shared by IP/ICMP/UDP/TCP, host-tested like fmt.c).
  v1 sends no fragments and drops fragmented arrivals *loudly* (logged,
  counted — no silent caps, per house rule). On modern paths ("DF-bit
  world"), fragmentation is a fossil anyway; reassembly is booked as an
  explicit DEBT row, not pretended.
- **ICMP**: echo reply (answering pings) + echo request (sending them).
- Static IP config via kernel cmdline first (`IP=`/`GW=`/`MASK=` tokens in
  kernel_commandline.c); DHCP arrives with UDP in Phase 3, as its first
  and most grateful customer.
- Milestone: **`ping` from the host gets answers from os64.** Screenshot
  for the ages; frame-by-frame pcap as the receipt.

### Phase 3 — UDP + THE API (Chris's rulings cash in here)

UDP is datagram-shaped and tiny — the right size for proving the handle
model and the address types before TCP raises the stakes. The syscall
surface built here (see OPEN rulings below) is the one TCP inherits, so
the design conversation happens BEFORE this phase's code.

- DHCP client lands here (UDP's first customer; the P5 will not want a
  hand-typed static IP).
- DNS resolution does NOT go in the kernel, ever. It's a userland library
  over UDP handles (Unix eventually got this right; we start right), and
  the lookup utility that uses it is CHRIS'S if he wants it.
- Milestone: a userland fixture round-trips datagrams with the host, and
  the boot line says what address DHCP leased.

### Phase 4 — TCP: the boss fight

The genuinely hard part, and the reason this arc was chosen over its
rivals. The state machine (LISTEN through TIME-WAIT, 11 states), sequence
space arithmetic (32-bit wraparound — comparisons must be modular),
retransmission with exponential backoff, sliding-window flow control,
simultaneous-close corner cases that RFC 793's own state diagram gets
subtle about.

- **The timers ride kTicksSinceStart and the sleep/ticks machinery** —
  last month's stopwatch becomes this month's retransmit engine. (Never
  rdtsc — TSC desync on this host is a documented trap.)
- Congestion control v1 = fixed window, stated honestly in a comment and
  a DEBT row; slow-start/AIMD is a documented follow-up, not a silent
  omission. (The internet's 1986 congestion collapse — 32kbit/s across
  a 400-yard link at LBL — is why Van Jacobson's algorithms exist;
  os64 on slirp is not going to melt the internet, but the row gets
  written because "free means free forever" thinking applies to
  omissions too.)
- Blocking read/write park on the SIGSLEEP machinery like console and
  pipe I/O; Ctrl+C interrupts them on the existing SIGINT rail (the
  loop-top sentinel pattern console_read already follows).
- Milestone: os64 fetches a real page from a real webserver on the real
  internet, through slirp — and serves one to the host browser via
  hostfwd. Both directions, receipts in pcap.

## OPEN rulings — the menu (Chris's, before Phase 3 code)

1. **How does a program name a destination?** (a) Typed struct
   (`os64_netdest_t {ip, port}` — matches the memory(out)/stat
   precedent); (b) Plan 9-style dial string (`"tcp!10.0.2.2!80"` — one
   parser, gloriously greppable, but text where we usually refuse text);
   (c) both, string layered in libos64 over the struct syscall.
   *Fable's lean: (c) — kernel speaks structs, library speaks strings.*
   **RATIFIED (Chris, 2026-07-28): (c), with the string form being the
   full Plan 9 bang path.** The kernel ABI is typed structs ONLY — no
   text ever crosses the syscall boundary. libos64 owns
   `os64_dial("tcp!10.0.2.2!80")` and the parser behind it. The
   protocol segment is ALWAYS explicit (it's the verb of the call — a
   TCP handle is a stream, a UDP handle is datagram-shaped, and the
   caller must say which they mean); Plan 9's `net!` wildcard is
   refused. When the DNS library lands, `tcp!google.com!80` is the same
   shape with the middle segment resolved in the library — the format
   never changes. (Ruled by a man with a silver beard and hair to the
   middle of his back, which the bang path's UUCP ancestors would
   recognize as jurisdictionally proper.)
2. **Byte order in the ABI.** Berkeley makes every application call
   `htons()` forever. Proposal: **the ABI is host-order everywhere; the
   kernel owns the wire** and swaps at the packet boundary. Apps never
   see network order, `htons` never exists in libos64. This is the
   memory(out) move applied to byte order — the kernel pre-answers the
   question. *Fable's lean: strongly yes; needs Chris's stamp because
   it's forever.*
   **RATIFIED (Chris, 2026-07-28): host-order everywhere, kernel owns
   the wire.** The swap surface is the four helpers in net_wire.h and
   nothing else, ever. Recorded in DIVERGENCES.md § Networking.
3. **The listen model.** (a) Berkeley trio (listen/accept loop);
   (b) a listener HANDLE whose read() yields a new connection handle —
   "accept is just read on a listener," which collapses the API and
   composes with a future poll/event story; (c) something Plan 9-flavored
   later. *Fable's lean: (b).*
   **RATIFIED (Chris, 2026-07-28): (b).** The behavior is standard
   accept (block until a connection, get a new handle, loop) — the only
   divergence is the spelling, and Berkeley's own poll() reports a
   pending connection as READABLE, admitting accept was always a read.
   Concrete shape: one read on a listener yields one
   `os64_netconn_t {handle, peer_ip, peer_port}` — which also retires
   accept's sockaddr out-param and getpeername in one stroke. Blocks on
   the house ISLEEP machinery, Ctrl+C-interruptible like every read.
4. **UDP shape.** (a) Connected-style: bind a handle to a peer once,
   then plain read/write (matches the handle doctrine, covers DHCP/DNS/
   ping cleanly); (b) sendto-style per-packet addressing (needed for
   servers talking to many peers — but is that v1?); (c) start (a),
   add a recvfrom-equivalent syscall only when a real consumer demands
   it. *Fable's lean: (c) — consumer-driven, the args-parser precedent.*
   **RATIFIED (Chris, 2026-07-28): (c).** Dial a UDP peer, read/write
   the handle; per-packet addressing waits for the consumer that needs
   it, and gets designed against that consumer's real shape.
5. **Where does network state show?** /proc is constitutionally
   processes-only (his ruling, standing). Interface list, ARP cache,
   connection table — a future `/net` (very Plan 9), a syscall in the
   memory(out) style, or both-eventually? Not blocking before Phase 3;
   the ruling can wait until something wants to *display* it (probably
   Chris's own netstat-alike, name TBD).
   **RATIFIED (Chris, 2026-07-28): both eventually, syscall first** —
   it's the straightforward start (the memory(out) pattern already
   exists: typed struct, meanings fixed forever) and the right home for
   fixed-shape state (interfaces, counters). `/net` arrives when a
   browser-shaped consumer (the netstat-alike) demands it, built on the
   procfs template, and takes the variable-length tables (ARP cache,
   connection table). /proc stays processes-only, as constitutionally
   required.

## The real-hardware problem (Chris's stated worry, answered honestly)

First, the actual inventory (2026-07-27): the 3900x's motherboard is an
ASUS TUF Gaming X570-Plus (WiFi) — Realtek L8200A wired (INACTIVE; there
is no live wired networking at Chris's desk at all) and an **Intel
Wireless-AC 9260, which is the NIC Chris actually wants os64 on.** The
P5's inventory is TBD (Chris checking; likely also WiFi-equipped).

None of this blocks the arc: the seam means the whole stack above
Phase 1 is proven against virtio + e1000 under two hypervisors before
any real silicon is attempted — one variable at a time, not two
multiplied (the RAMDisk/xHCI playbook: QEMU reproduced the P5's USB
topology before the P5 ever saw that driver). But it reorders the
real-hardware endgame into an honest ascent:

1. **USB-Ethernet dongle via the existing xHCI driver.** A CDC-ECM or
   ASIX-class USB NIC driver is SMALL — an evening-scale project against
   hardware os64 already enumerates — and it puts the whole stack on
   real silicon with wired-grade debuggability long before 802.11. The
   pragmatic first real-packet moment. (Phone USB tethering presents as
   the same device class — a possible zero-new-hardware path.)
2. **The Intel 9260 WiFi arc — the true endgame, and its own future
   constitution.** Straight talk about scale: this is the largest driver
   project in os64's future. In its favor: iwlwifi-class devices run
   most of the 802.11 MAC *in firmware*, so the host mostly loads a
   firmware blob and speaks a command/response protocol over PCIe rings
   (ring discipline: by then, deeply familiar). Still genuinely new:
   firmware loading, scanning/association state, and the WPA2 4-way
   handshake (crypto — supplicant logic likely userland, kernel keeps
   keys and counters). When its day comes it gets its own WIFI.md; this
   arc's job is to make sure the net_device seam never gratuitously
   assumes "Ethernet cable" where it means "frame in, frame out" — which
   is exactly the frame shape firmware-MAC WiFi devices present anyway.

## Verification (the harness, stated up front)

- **QEMU slirp** (`-netdev user`): zero host config, guest at 10.0.2.15,
  gateway 10.0.2.2, built-in DHCP server to test our client against, and
  `hostfwd` to reach an os64 listener from the host. Good enough through
  Phase 4.
- **Packet capture is the serial log of this arc**: QEMU's
  `-object filter-dump` writes a pcap of everything on the virtual wire;
  Fable reads it with host tcpdump. Every milestone's receipt is a
  capture, not a vibe. (A `tap` netdev unlocks real-LAN testing later if
  slirp's NAT ever hides a bug — noted, not needed yet.)
- Fixtures follow the house pattern: kernel/test/elf/ fixture + postboot
  registration + magic retval, added to BOTH makefile exclusion lists
  (the TEST_CFILES lesson is still warm).

## Failure fingerprints (symptom → cause)

| Symptom | Cause |
|---|---|
| **The network dies completely a few seconds into sustained traffic — TCP hangs, ping stops answering — while the console, the shell and everything else stay perfectly healthy, and nothing appears in the log.** First seen on the P5 2026-08-22, three to six files into an 86-file `os64get -a`, reproducibly, four times. **CONFIRMED AND FIXED the same day**, and the confirmation was exact: the fixed driver logged `ISR=0x00000011` — ROK (a frame arrived) plus **RDU (RX descriptors exhausted)** — and completed all 83 files | The RTL8125's INTERRUPT STATUS REGISTER was never acknowledged. `R8125_ISR0_8125` (0x3C) was defined and read by nothing; the driver masks interrupts (IMR0 = 0) and polls, but **masking does not stop the device LATCHING status**, and RX-FIFO-overflow / RX-descriptors-exhausted are sticky on this family — reception can stay stopped until the bit is written back (write-one-to-clear). Nothing had ever pushed sustained traffic at the card before `-a` existed, so nothing had ever overrun a 64-entry RX ring drained once per scheduler pass. `r8125_ack_status` reads and clears it every poll now, and says so once on DEBUG_EXCEPTIONS when an alarming bit was latched. **The general lesson is bigger than the bug: "we poll, so interrupts are off" does not mean "the status register is irrelevant" — a polled driver still has to drain the device's error state** |
| The network is dead and the log says nothing at all | It should not, any more (2026-08-22). `r8125_check_stalled` announces a wedged transmit ring or a card that has gone deaf on **DEBUG_EXCEPTIONS** — the bit CONFIG.h keeps permanently lit exactly so must-never-be-silent messages have somewhere to go. A show-stopper that needs `DEBUG_NET` switched on first is a show-stopper nobody will have the log for, because you only know to switch it on after it has happened (Chris's ruling, the day it happened) |
| A ring-protocol bug is suspected | Run the host harness first — `gcc -I kernel/include tools/test_r8125_host.c kernel/src/driver/net/r8125_ring.c kernel/src/driver/net/r8125_phy.c -o /tmp/t && /tmp/t` — it covers ownership handoff, wrap on BOTH rings, and EOR survival, in milliseconds. It passing is real evidence that the fault is in the device interaction (registers, doorbell, status) rather than the descriptor logic, which is exactly the split r8125_ring.c exists to create |
| The P5 links at the wrong speed (100/full on a gigabit switch, 2026-09-05) | Read the `DEBUG_BOOT` lines `r8125: PHY as found: we advertise …, partner offers …` and `r8125: PHY renegotiated: …` in os64.log — both sides of the negotiation, before and after the driver set the advertisement. Ours missing 1000F = the PHY was left advertising 10/100 (the driver now restores it); theirs missing 1000F = the switch port; both offering 1000F and the link still 100 = the driver renegotiates once on its own (`r8125: PHY renegotiating because the link is below what both sides offer`); still 100 after that is a training failure (the PHY firmware row in DEBTS). The boot line carries the raw PHYstatus; `link_raw:` in `/sys/net/r81250` is the same word live. Since 2026-09-05 the driver programs the advertisement itself (r8125_phy_configure, RTL8125.md AMENDMENT 2) rather than trusting the PHY's power-on state |

## Non-goals for this arc (stated so silence never lies)

- **IPv6**: not in this arc. The stack's internal seams shouldn't assume
  4-byte addresses gratuitously, but no v6 code gets written yet.
- **TLS**: userland, far future, never kernel.
- **Wi-Fi**: not in THIS arc — but no longer "never." It is the declared
  real-hardware endgame (see the real-hardware section above), because
  the house runs wireless. This arc's obligation to that future is
  seam discipline, not 802.11 code.
- **Berkeley-compat shim**: only if os64 ever wants to port existing
  software wholesale, and then as a libos64-level veneer — the kernel ABI
  never bends toward sockaddr.
