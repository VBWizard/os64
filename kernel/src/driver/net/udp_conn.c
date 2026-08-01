// udp_conn.c — the conversation object: dial, park, deliver, hang up.
//
// The blocking read follows pipe.c's canonical recipe to the letter:
//   1. take the lock, test the condition
//   2. can't proceed? register as waiter, DROP the lock, park (SIGSLEEP
//      with a backstop tick)
//   3. on wake, LOOP AND RE-TEST — a wake is a hint, never a promise.
// The wake side is level-triggered (udp_conn_wake_if_ready re-evaluates
// "ring non-empty", never a remembered edge) which is what makes the whole
// arrangement lost-wakeup-free — same argument, word for word, as pipes.
//
// CONTEXT MAP (who runs where — the part worth being pedantic about):
//   udp_conn_rx        RX context (inside the processSignals NIC poll, or a
//                      test injecting at the seam): enqueue only, never wake
//                      directly — a test-context injection isn't under the
//                      scheduler lock, and queue surgery without that lock
//                      is the exact bug class 9badced buried.
//   udp_conn_wake_if_ready  processSignals, UNDER the scheduler lock, after
//                      the polls: the only place waiters get woken.
//   read/write/dial/close   task context (syscall handlers, kernel tests).

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"          // kTicksSinceStart — the park backstop clock
#include "serial_logging.h"
#include "memcpy.h"
#include "spinlock.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "smp_core.h"        // get_core_local_storage — who is reading
#include "signals.h"         // sigaction(SIGSLEEP) — how a reader parks
#include "scheduler.h"       // scheduler_change_thread_queue — how it wakes
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/udp.h"
#include "driver/net/udp_conn.h"
#include "time.h"            // wait() — the ARP-retry nap in udp_conn_write

// One second, same value and same meaning as PIPE_BACKSTOP_TICKS: a
// liveness backstop against a missed wake, NOT a poll interval — the sweep
// normally wakes a reader the same scheduler pass its datagram arrives.
#define UDP_CONN_BACKSTOP_TICKS (TICKS_PER_SECOND)

// The live-conversation list, walked by the wake sweep. Guarded by its own
// lock (dial/close mutate from task context while the sweep walks under
// the scheduler lock — irqsave on both sides keeps them honest).
static udp_conn_t* kUdpConnList = NULL;
static spinlock_t s_list_lock;

// Ephemeral ports: IANA's dynamic range, 49152-65535 (RFC 6335 — the range
// exists so well-known services and short-lived clients never fight over a
// number). A free-running cursor; udp_bind's double-bind refusal is the
// collision check, so this loop just walks until a free one answers.
static uint16_t s_next_ephemeral = 49152;

// ── RX: the udp.c bind-table callback ───────────────────────────────────────
static void udp_conn_rx(net_device_t* dev, uint32_t src_ip, uint16_t src_port,
                        const void* data, uint16_t length, void* ctx)
{
	(void)dev;
	udp_conn_t* c = (udp_conn_t*)ctx;

	// The connected-UDP filter (ruling #4): this handle hears its dialed
	// peer and nobody else. Anything else on our ephemeral port is either
	// a scanner or an accident — counted, dropped, never delivered.
	if (src_ip != c->peer_ip || src_port != c->peer_port)
	{
		c->rx_dropped_stranger++;
		return;
	}

	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	if (c->count >= UDP_CONN_QUEUE_SLOTS)
	{
		// Ring full: the newest arrival drops (the bound IS the flow
		// control; see the header). Dropping OLD instead would hand a
		// request/response protocol stale answers — worse than none.
		c->rx_dropped_full++;
		spinlock_release_irqrestore(&c->lock, irqflags);
		return;
	}
	uint16_t slot = (uint16_t)((c->head + c->count) % UDP_CONN_QUEUE_SLOTS);
	uint16_t n = length > UDP_CONN_MAX_DGRAM ? UDP_CONN_MAX_DGRAM : length;
	memcpy(c->slots[slot], (void*)data, n);
	c->lens[slot] = n;
	c->count++;
	spinlock_release_irqrestore(&c->lock, irqflags);
	// No wake here — see the context map up top. The sweep runs later in
	// the SAME processSignals pass for real NIC traffic, so the latency
	// cost of this discipline is zero.
}

// ── The wake sweep ──────────────────────────────────────────────────────────
void udp_conn_wake_if_ready(void)
{
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	for (udp_conn_t* c = kUdpConnList; c != NULL; c = c->next)
	{
		thread_t* w = NULL;
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->waiter != NULL && c->count > 0)
		{
			w = c->waiter;
			c->waiter = NULL;
		}
		spinlock_release_irqrestore(&c->lock, irqflags);

		// The scheduler's own wake primitive does the check-clear-relink
		// behind its queue lock discipline (the 9badced rule: every relink
		// pays the toll). The _locked variant because this sweep runs from
		// processSignals, which already holds kSchedulerSwitchTasksLock —
		// same reasoning, same variant, as pipe_wake_if_ready's calls.
		// (A still-RUNNING waiter is left registered — the primitive's
		// ISLEEP check — and the condition stays true for the next sweep.)
		if (w != NULL)
		{
			scheduler_wake_isleep_thread_locked(w);
			printd(DEBUG_NET | DEBUG_DETAILED, "udp_conn: woke reader on port %u\n", c->local_port);
		}
	}
	spinlock_release_irqrestore(&s_list_lock, lf);
}

// ── Dial ────────────────────────────────────────────────────────────────────
udp_conn_t* udp_conn_dial(net_device_t* dev, uint32_t peer_ip, uint16_t peer_port)
{
	udp_conn_t* c = kmalloc(sizeof(*c));
	if (c == NULL)
		return NULL;
	c->slots = kmalloc(UDP_CONN_QUEUE_SLOTS * UDP_CONN_MAX_DGRAM);
	if (c->slots == NULL)
	{
		kfree(c);
		return NULL;
	}
	// (kmalloc zeroes at the choke point — ring indices, counters, waiter,
	// and lock all start correctly at rest, by house doctrine not luck.)
	c->dev = dev;
	c->peer_ip = peer_ip;
	c->peer_port = peer_port;

	// Claim an ephemeral port. udp_bind refuses doubles, so "walk until a
	// bind sticks" is the whole allocator; 200 tries covers pathological
	// clustering (the realistic conflict population is single digits).
	bool bound = false;
	for (int tries = 0; tries < 200 && !bound; tries++)
	{
		uint16_t port = s_next_ephemeral++;
		if (s_next_ephemeral == 0)          // wrapped past 65535
			s_next_ephemeral = 49152;       // stay inside the dynamic range
		if (port < 49152)
			continue;
		if (udp_bind(port, udp_conn_rx, c) == 0)
		{
			c->local_port = port;
			bound = true;
		}
	}
	if (!bound)
	{
		kfree(c->slots);
		kfree(c);
		return NULL;
	}

	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	c->next = kUdpConnList;
	kUdpConnList = c;
	spinlock_release_irqrestore(&s_list_lock, lf);

	printd(DEBUG_NET, "udp_conn: dialed %u.%u.%u.%u:%u from local port %u\n",
	       NET_IPV4_OCTETS(peer_ip), peer_port, c->local_port);
	return c;
}

// ── Read (blocking; task context only) ──────────────────────────────────────
long udp_conn_read(udp_conn_t* c, void* buf, size_t len)
{
	core_local_storage_t* cls = get_core_local_storage();
	thread_t* self = cls->currentThread;

	for (;;)
	{
		// Terminate outranks the wait — checked at loop top, BEFORE the
		// lock, so a Ctrl+C that woke us exits instead of re-parking.
		if (self->signals.sigind & SIGNALS_TERMINATING)
			return UDP_CONN_ERR_INTERRUPTED;

		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->count > 0)
		{
			uint16_t slot = c->head;
			uint16_t have = c->lens[slot];
			uint16_t n = (len < have) ? (uint16_t)len : have;
			memcpy(buf, c->slots[slot], n);
			// The tail of a too-big datagram DROPS (the classic UDP
			// truncation contract, stated in os64/net.h) — a datagram is
			// a unit; handing out half now and half later would invent a
			// stream where the protocol promises packets.
			c->head = (uint16_t)((c->head + 1) % UDP_CONN_QUEUE_SLOTS);
			c->count--;
			c->rx_delivered++;
			spinlock_release_irqrestore(&c->lock, irqflags);
			return (long)n;
		}

		// Nothing yet: register, drop the lock, park. The sweep may fire
		// between the release and the sigaction — that's the race the
		// backstop tick and the loop-and-retest exist to make harmless.
		c->waiter = self;
		spinlock_release_irqrestore(&c->lock, irqflags);
		sigaction(SIGSLEEP, NULL, kTicksSinceStart + UDP_CONN_BACKSTOP_TICKS, self);
	}
}

// ── Write (one datagram; task context only) ─────────────────────────────────
long udp_conn_write(udp_conn_t* c, const void* buf, size_t len)
{
	if (len == 0 || len > UDP_CONN_MAX_DGRAM)
		return -1;   // a datagram is atomic: oversize is an ERROR, never a loop

	// udp_send's -2 means "next hop unresolved, ARP query just went out" —
	// the documented first-packet behavior. Task context may sleep, so
	// HERE (unlike the RX-context responders) we can simply wait out the
	// resolution: retry for up to ~500ms, which is geological time for an
	// ARP round trip on any link this OS has met.
	for (int tries = 0; tries < 50; tries++)
	{
		int32_t rc = udp_send(c->dev, c->peer_ip, c->local_port, c->peer_port,
		                      buf, (uint16_t)len);
		if (rc == 0)
			return (long)len;
		if (rc != -2)
			return -1;
		wait(10);
	}
	return -1;   // neighbor never answered ARP — the peer's problem is now the caller's
}

// ── Close ───────────────────────────────────────────────────────────────────
void udp_conn_close(udp_conn_t* c)
{
	// Stop arrivals first: after udp_unbind returns, udp.c can never call
	// udp_conn_rx with this conn again, so the frees below race nothing.
	udp_unbind(c->local_port);

	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	udp_conn_t** pp = &kUdpConnList;
	while (*pp != NULL && *pp != c)
		pp = &(*pp)->next;
	if (*pp == c)
		*pp = c->next;
	spinlock_release_irqrestore(&s_list_lock, lf);

	// A task can't be parked in read() and closing at once (one thread),
	// and exit's handle_close_all only runs after the thread left the
	// syscall — so no waiter can exist here. Stated, not assumed silently.
	kfree(c->slots);
	kfree(c);
}
