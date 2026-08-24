// icmp_conn.c — dial an echo conversation; the object under `ping`.
//
// Structurally this is udp_conn's twin (registry list, reply ring,
// park-and-sweep blocking read), so read that file's context map first if
// this one's locking looks terse. The differences are all consequences of
// echo having no ports: the identifier is allocated here instead of by a
// bind table, delivery is routed by icmp.c calling icmp_conn_deliver
// instead of by a port demux, and a "connection" filters on identifier +
// peer address rather than a four-tuple.

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"
#include "serial_logging.h"
#include "memcpy.h"
#include "spinlock.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "smp_core.h"
#include "signals.h"
#include "scheduler.h"
#include "time.h"            // wait() — the ARP-resolution retry nap in write
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/icmp.h"
#include "driver/net/icmp_conn.h"

static icmp_conn_t* kIcmpConnList = NULL;
static spinlock_t s_list_lock;

// Identifiers are ours to hand out. Starting at 0x6F00 ("o") makes os64's
// pings recognizable in someone else's packet capture — a courtesy to the
// next person debugging this network, and a debugging aid for us.
static uint16_t s_next_identifier = 0x6F00;

icmp_conn_t* icmp_conn_dial(net_device_t* dev, uint32_t peer_ip)
{
	icmp_conn_t* c = kmalloc(sizeof(*c));
	if (c == NULL)
		return NULL;
	c->slots = kmalloc(ICMP_CONN_QUEUE_SLOTS * ICMP_CONN_MAX_PAYLOAD);
	if (c->slots == NULL)
	{
		kfree(c);
		return NULL;
	}
	c->dev = dev;
	c->peer_ip = peer_ip;
	c->next_sequence = 1;   // ping counts from 1; seq 0 reads as "unset"

	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	// Walk until we find an identifier nobody is using. The population is
	// tiny (one per open ping), so a linear check is the whole allocator.
	bool taken;
	uint16_t tries = 0;
	do
	{
		c->identifier = s_next_identifier++;
		if (s_next_identifier == 0)
			s_next_identifier = 0x6F00;
		taken = false;
		for (icmp_conn_t* t = kIcmpConnList; t != NULL; t = t->next)
			if (t->identifier == c->identifier)
			{
				taken = true;
				break;
			}
	} while (taken && ++tries < 256);
	if (taken)
	{
		spinlock_release_irqrestore(&s_list_lock, lf);
		kfree(c->slots);
		kfree(c);
		return NULL;
	}
	c->next = kIcmpConnList;
	kIcmpConnList = c;
	spinlock_release_irqrestore(&s_list_lock, lf);

	printd(DEBUG_NET, "icmp: dialed %u.%u.%u.%u (identifier 0x%x)\n",
	       NET_IPV4_OCTETS(peer_ip), c->identifier);
	return c;
}

long icmp_conn_write(icmp_conn_t* c, const void* buf, size_t len)
{
	if (len > ICMP_CONN_MAX_PAYLOAD)
		return -1;   // over MTU: refused, never fragmented (the Phase 2 stance)

	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	uint16_t seq = c->next_sequence++;
	spinlock_release_irqrestore(&c->lock, irqflags);

	// icmp_send_echo carries OUR identifier and sequence; the payload is
	// the caller's business (a timestamp, a pattern, whatever `ping`
	// wants to recognize when it comes home).
	//
	// -2 = "next hop unresolved, ARP query just fired" — and unlike the
	// RX-context responders, we are task context and may simply wait it
	// out (the same loop udp_conn_write has had all along; this one shipped
	// without it, and a long-running ping paid for the asymmetry ONCE PER
	// MINUTE: the ARP cache's 60s lazy TTL expired the gateway entry, the
	// next echo ate the first-packet drop, and write() failed for one
	// beat. 500ms of retries is geological time for an ARP round trip.)
	for (int tries = 0; tries < 50; tries++)
	{
		int32_t rc = icmp_send_echo(c->dev, c->peer_ip, c->identifier, seq,
		                            buf, (uint16_t)len);
		if (rc == 0)
		{
			c->requests_sent++;
			return (long)len;
		}
		if (rc != -2)
			return -1;
		wait(10);
	}
	// Neighbor never answered ARP: nobody home at the next hop. TIMEOUT,
	// not a generic -1 — the caller can tell silence from a bad argument.
	return ICMP_CONN_ERR_TIMEOUT;
}

// RX context (inside the NIC poll): enqueue only, never wake directly —
// the sweep under the scheduler lock does that. Same rule, same reason,
// as udp_conn_rx.
void icmp_conn_deliver(uint32_t src_ip, uint16_t identifier,
                       const void* payload, uint16_t length)
{
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	icmp_conn_t* c = NULL;
	for (icmp_conn_t* t = kIcmpConnList; t != NULL; t = t->next)
		if (t->identifier == identifier && t->peer_ip == src_ip)
		{
			c = t;
			break;
		}
	spinlock_release_irqrestore(&s_list_lock, lf);
	if (c == NULL)
		return;   // nobody's conversation (or a stale reply after close)

	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	if (c->count >= ICMP_CONN_QUEUE_SLOTS)
	{
		c->dropped_full++;   // asking faster than reading: the newest waits for nobody
		spinlock_release_irqrestore(&c->lock, irqflags);
		return;
	}
	uint16_t slot = (uint16_t)((c->head + c->count) % ICMP_CONN_QUEUE_SLOTS);
	uint16_t n = length > ICMP_CONN_MAX_PAYLOAD ? ICMP_CONN_MAX_PAYLOAD : length;
	memcpy(c->slots[slot], (void*)payload, n);
	c->lens[slot] = n;
	c->count++;
	c->replies_delivered++;
	spinlock_release_irqrestore(&c->lock, irqflags);
}

long icmp_conn_read(icmp_conn_t* c, void* buf, size_t len, uint64_t deadline)
{
	core_local_storage_t* cls = get_core_local_storage();
	thread_t* self = cls->currentThread;

	for (;;)
	{
		if (sigset_any(self->signals.sigind, SIGNALS_TERMINATING))
			return ICMP_CONN_ERR_INTERRUPTED;

		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->count > 0)
		{
			uint16_t slot = c->head;
			uint16_t have = c->lens[slot];
			uint16_t n = (len < have) ? (uint16_t)len : have;
			memcpy(buf, c->slots[slot], n);
			c->head = (uint16_t)((c->head + 1) % ICMP_CONN_QUEUE_SLOTS);
			c->count--;
			spinlock_release_irqrestore(&c->lock, irqflags);
			return (long)n;
		}
		// Deadline check comes AFTER the data check, under the same lock:
		// a reply that arrived at the buzzer is still a reply (data
		// outranks the clock), and an expired waiter must deregister
		// itself so the sweep never wakes a reader that already gave up.
		if (deadline != 0 && kTicksSinceStart >= deadline)
		{
			if (c->waiter == self)
				c->waiter = NULL;
			spinlock_release_irqrestore(&c->lock, irqflags);
			return ICMP_CONN_ERR_TIMEOUT;
		}
		c->waiter = self;
		spinlock_release_irqrestore(&c->lock, irqflags);
		// A ping that gets no answer must not park forever: park until the
		// backstop OR the caller's deadline, whichever lands first. Timeout
		// POLICY still belongs to `ping` (how long is too long, what to
		// print) — what the kernel now provides is the mechanism, because
		// a policy nobody can implement is just a comment. (It was: this
		// paragraph used to end "the utility decides what silence MEANS"
		// while offering the utility no way to act on the decision.)
		uint64_t wake = kTicksSinceStart + TICKS_PER_SECOND;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		sigaction(SIGSLEEP, NULL, wake, self);
	}
}

void icmp_conn_wake_if_ready(void)
{
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	for (icmp_conn_t* c = kIcmpConnList; c != NULL; c = c->next)
	{
		thread_t* w = NULL;
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		// THE ISLEEP CHECK IS PART OF THE CONDITION, not just the wake.
		// A registered waiter has a window between "c->waiter = self" and
		// actually reaching ISLEEP; if the sweep clears the registration
		// during that window, scheduler_wake_isleep_thread_locked
		// silently declines (it only moves parked threads) and NOBODY
		// wakes the sleeper — it eats the full 1-second backstop instead.
		// Measured on the first ICMP boot: every echo reported an RTT of
		// exactly 100 ticks, which is the backstop, not the network.
		// Leaving a not-yet-parked waiter REGISTERED costs one more
		// sweep (~1 tick) and is what makes the reply latency the wire's
		// instead of the timer's.
		if (c->waiter != NULL && c->count > 0 &&
		    c->waiter->threadState == THREAD_STATE_ISLEEP)
		{
			w = c->waiter;
			c->waiter = NULL;
		}
		spinlock_release_irqrestore(&c->lock, irqflags);
		if (w != NULL)
			scheduler_wake_isleep_thread_locked(w);
	}
	spinlock_release_irqrestore(&s_list_lock, lf);
}

void icmp_conn_close(icmp_conn_t* c)
{
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	icmp_conn_t** pp = &kIcmpConnList;
	while (*pp != NULL && *pp != c)
		pp = &(*pp)->next;
	if (*pp == c)
		*pp = c->next;
	spinlock_release_irqrestore(&s_list_lock, lf);

	// Off the list first: after this, icmp_conn_deliver can never find
	// this conn again, so the frees race nothing.
	kfree(c->slots);
	kfree(c);
}
