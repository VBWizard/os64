// udp.c — ports, the pseudo-header, and a small table of mailboxes.
//
// Everything here is bookkeeping around an 8-byte header; the interesting
// decision is the BIND TABLE — the kernel-internal ancestor of the Phase 3
// handle plumbing. A bound port is a claimed mailbox: inbound datagrams
// demux to exactly one handler or die on a counter. When the syscall era
// arrives (ruling #4: dial a peer, read/write the handle), those handles
// become entries here and the demux below doesn't change shape.

#include <stdint.h>
#include <stdbool.h>
#include "serial_logging.h"
#include "memcpy.h"
#include "spinlock.h"
#include "CONFIG.h"
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/net_checksum.h"
#include "driver/net/ethernet.h"   // ETH_HDR_LEN — the datagram buffer's frame math
#include "driver/net/ipv4.h"
#include "driver/net/udp.h"

udp_stats_t kUdpStats;

typedef struct udp_binding
{
	uint16_t port;              // 0 = slot empty (port 0 is unusable by IANA decree, so 0-as-empty steals nothing)
	udp_rx_handler_t handler;
	void* ctx;
} udp_binding_t;

static udp_binding_t s_bindings[UDP_MAX_BINDINGS];
// Guards the table. RX context walks it while task context binds/unbinds —
// irqsave, held for a scan of eight slots, never across a handler call
// (the handler runs after release; an unbind racing a delivery loses
// gracefully — at worst one already-in-flight datagram reaches a handler
// that was just resigning, which is any mail system's honest semantics).
static spinlock_t s_bind_lock;

// ── The pseudo-header ───────────────────────────────────────────────────────
// 12 bytes the wire never carries: src addr, dst addr, a zero, the
// protocol number, and the UDP length — borrowed from the IP layer and
// summed BEFORE the real datagram. 1980's anti-misdelivery insurance: a
// datagram that a corrupted IP header steered to the wrong host fails its
// checksum THERE, because the intended addresses are baked into the sum.
static uint32_t udp_pseudo_sum(uint32_t src_ip, uint32_t dst_ip, uint16_t udp_len)
{
	uint8_t pseudo[12];
	net_write32(pseudo + 0, src_ip);
	net_write32(pseudo + 4, dst_ip);
	pseudo[8] = 0;
	pseudo[9] = IPV4_PROTO_UDP;
	net_write16(pseudo + 10, udp_len);
	return net_checksum_add(0, pseudo, sizeof(pseudo));
}

// ── Bind table ──────────────────────────────────────────────────────────────
int32_t udp_bind(uint16_t local_port, udp_rx_handler_t handler, void* ctx)
{
	if (local_port == 0 || handler == NULL)
		return -1;

	uint64_t irqflags = spinlock_acquire_irqsave(&s_bind_lock);
	udp_binding_t* slot = NULL;
	for (int i = 0; i < UDP_MAX_BINDINGS; i++)
	{
		if (s_bindings[i].port == local_port)
		{
			// A port is a mailbox; two owners each get half the mail.
			// Refused loudly, per the no-silent-anything doctrine.
			spinlock_release_irqrestore(&s_bind_lock, irqflags);
			printd(DEBUG_NET, "udp: port %u already bound — refused\n", local_port);
			return -2;
		}
		if (s_bindings[i].port == 0 && slot == NULL)
			slot = &s_bindings[i];
	}
	if (slot == NULL)
	{
		spinlock_release_irqrestore(&s_bind_lock, irqflags);
		printd(DEBUG_NET, "udp: bind table full (UDP_MAX_BINDINGS=%u)\n", UDP_MAX_BINDINGS);
		return -3;
	}
	slot->handler = handler;
	slot->ctx = ctx;
	slot->port = local_port;   // port LAST: the RX scan below keys on it,
	                           // so a half-filled slot is never findable
	spinlock_release_irqrestore(&s_bind_lock, irqflags);
	return 0;
}

void udp_unbind(uint16_t local_port)
{
	uint64_t irqflags = spinlock_acquire_irqsave(&s_bind_lock);
	for (int i = 0; i < UDP_MAX_BINDINGS; i++)
		if (s_bindings[i].port == local_port)
		{
			s_bindings[i].port = 0;
			s_bindings[i].handler = NULL;
			s_bindings[i].ctx = NULL;
			break;
		}
	spinlock_release_irqrestore(&s_bind_lock, irqflags);
}

// ── Receive ─────────────────────────────────────────────────────────────────
void udp_input(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
               const void* pkt, uint16_t length)
{
	const uint8_t* p = (const uint8_t*)pkt;

	if (length < UDP_HDR_LEN)
	{
		kUdpStats.rx_truncated++;
		return;
	}

	// The length field is the datagram's own claim about itself; trust it
	// only downward (IP already trimmed ethernet padding, but a corrupt
	// field could still overclaim into bytes that never arrived).
	uint16_t claimed = net_read16(p + 4);
	if (claimed < UDP_HDR_LEN || claimed > length)
	{
		kUdpStats.rx_truncated++;
		return;
	}

	// Checksum: 0 on the wire means the sender skipped it (IPv4-legal,
	// a fossil of 1980s CPU budgets). When present, verify over pseudo-
	// header + datagram — the sum-to-zero self-property holds here too.
	uint16_t wire_ck = net_read16(p + 6);
	if (wire_ck != 0)
	{
		uint32_t sum = udp_pseudo_sum(src_ip, dst_ip, claimed);
		sum = net_checksum_add(sum, p, claimed);
		if (net_checksum_fold(sum) != 0)
		{
			kUdpStats.rx_bad_checksum++;
			printd(DEBUG_NET, "udp: bad checksum from %u.%u.%u.%u — dropped\n",
			       NET_IPV4_OCTETS(src_ip));
			return;
		}
	}

	uint16_t dst_port = net_read16(p + 2);
	uint16_t src_port = net_read16(p + 0);

	// Find the mailbox. Copy the binding out under the lock, call the
	// handler after release — a handler that sends (DHCP does) re-enters
	// this module via udp_send/udp_bind paths, and holding a lock across
	// a callback is how the virtio poll nearly ate itself (that lesson
	// is one file over and two days old).
	udp_rx_handler_t handler = NULL;
	void* ctx = NULL;
	uint64_t irqflags = spinlock_acquire_irqsave(&s_bind_lock);
	for (int i = 0; i < UDP_MAX_BINDINGS; i++)
		if (s_bindings[i].port == dst_port)
		{
			handler = s_bindings[i].handler;
			ctx = s_bindings[i].ctx;
			break;
		}
	spinlock_release_irqrestore(&s_bind_lock, irqflags);

	if (handler == NULL)
	{
		// Nobody home at that port. Real stacks answer this with an ICMP
		// port-unreachable (how traceroute's final hop and `nc -u` probes
		// work); ours is counted-and-silent until something needs the
		// courtesy — a DEBT row, not an oversight.
		kUdpStats.rx_no_binding++;
		printd(DEBUG_NET | DEBUG_DETAILED, "udp: no binding for port %u (from %u.%u.%u.%u:%u)\n",
		       dst_port, NET_IPV4_OCTETS(src_ip), src_port);
		return;
	}

	kUdpStats.rx_delivered++;
	handler(dev, src_ip, src_port, p + UDP_HDR_LEN,
	        (uint16_t)(claimed - UDP_HDR_LEN), ctx);
}

// ── Transmit ────────────────────────────────────────────────────────────────
int32_t udp_send_from(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const void* payload, uint16_t length)
{
	if ((uint32_t)length + UDP_HDR_LEN + IPV4_HDR_MIN > dev->mtu)
	{
		kUdpStats.tx_errors++;
		return -1;
	}

	uint8_t dgram[NET_FRAME_MAX - ETH_HDR_LEN - IPV4_HDR_MIN];
	uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + length);
	net_write16(dgram + 0, src_port);
	net_write16(dgram + 2, dst_port);
	net_write16(dgram + 4, udp_len);
	net_write16(dgram + 6, 0);                    // zero while computing...
	memcpy(dgram + UDP_HDR_LEN, payload, length);

	uint32_t sum = udp_pseudo_sum(src_ip, dst_ip, udp_len);
	sum = net_checksum_add(sum, dgram, udp_len);
	uint16_t ck = net_checksum_fold(sum);
	// The one wire wrinkle: computed 0x0000 transmits as 0xFFFF (same
	// value in ones'-complement), because a literal 0 on the wire means
	// "no checksum" — and we always checksum.
	net_write16(dgram + 6, ck == 0 ? 0xFFFF : ck);

	int32_t rc = ipv4_send_from(dev, src_ip, dst_ip, IPV4_PROTO_UDP, dgram, udp_len);
	if (rc == 0)
		kUdpStats.tx_sent++;
	else
		kUdpStats.tx_errors++;
	return rc;
}

int32_t udp_send(net_device_t* dev, uint32_t dst_ip, uint16_t src_port,
                 uint16_t dst_port, const void* payload, uint16_t length)
{
	return udp_send_from(dev, kNetIPv4Address, dst_ip, src_port, dst_port,
	                     payload, length);
}
