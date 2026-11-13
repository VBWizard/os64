// test_xhci_service_host.c — drive kernel/src/driver/system/usb/xhci_service.c
// against a model of xHCI controllers.
//
// THE MODEL. Time is a counter that moves only when a handler "costs" some.
// Each controller has an event ring, a producer that posts events at
// scheduled times (stamping each, which a real controller does not), and a
// register file that records every ERDP write. model_pass() is the glue the
// kernel's xhci.c wraps round a drain: drain, ask what ERDP is owed, read
// the register if asked, write it if told, account the wait.
//
// WHAT IS ASSERTED is XHCI_INTERRUPT.md § The design, D11's list: the
// deadline bounds a visit to itself plus one handler; the cursor rotates;
// idle passes write ERDP zero times; an early stop names the last event
// handled and what is left survives, across a wrap; an empty stop names the
// empty slot; the busy bit is retired once and only when it reads set; and
// the reported wait covers every event's real wait, a late visit after a
// prompt wake among them.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/system/usb/xhci_service.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAX_CONTROLLERS 4
#define MAX_EVENTS      4096
#define MAX_WRITES      4096

typedef struct {
	xhci_evt_ring_t ring;
	xhci_wait_t     wait;
	// The producer.
	uint32_t enqueue;        // next slot it writes
	uint32_t prod_cycle;
	uint32_t posted;         // events ever posted
	uint64_t posted_at[MAX_EVENTS];
	// The consumer's record.
	uint32_t handled;        // events ever handled, which is also the next id expected
	uint32_t acked;          // events the controller knows are consumed: those before the slot ERDP names
	uint64_t handled_at[MAX_EVENTS];
	uint64_t cost;           // what one handler costs
	// The register file.
	uint64_t erdp;           // as last written (EHB bit as the model holds it)
	bool     ehb;            // the busy bit
	uint32_t writes;
	uint64_t write_value[MAX_WRITES];
	uint32_t reads;
	// A flood: whenever the ring has room, the producer posts.
	bool     flood;
	// The interrupter. An event arriving with the busy bit clear sets
	// USBSTS.EINT and, with IE set, sends one message; so does an ERDP
	// write that clears the busy bit with the ring not empty. While the
	// busy bit is set nothing is sent. (With MSI the pending bit clears
	// itself when the message goes; EINT does not.)
	uint32_t usbsts;         // EINT and the register's other write-one-to-clear bits
	bool     ie;
	uint32_t messages;       // sent
	uint32_t answered;       // of which the top half has run for
	bool     owed;           // the top half's mark, taken by the pass that answers it
	uint32_t irq_writes;
	xhci_irq_reg_t irq_write_reg[MAX_WRITES];
	uint32_t irq_write_value[MAX_WRITES];
} controller_t;

static controller_t hc[MAX_CONTROLLERS];
static uint32_t     hc_count;
static uint64_t     now;                    // model time
static const uint64_t kEdges[3] = { 20, 200, 2000 };

// Scheduled posts: (time, controller), in time order.
typedef struct { uint64_t at; uint32_t c; } post_t;
static post_t  posts[MAX_EVENTS];
static uint32_t post_count, post_next;

static void model_reset(uint32_t controllers, uint32_t ring_size)
{
	for (uint32_t c = 0; c < MAX_CONTROLLERS; c++)
		free(hc[c].ring.trb);
	memset(hc, 0, sizeof(hc));
	hc_count = controllers;
	now = 1;   // 0 means "never" to xhci_wait_t
	post_count = post_next = 0;
	for (uint32_t c = 0; c < controllers; c++) {
		hc[c].ring.trb = calloc(ring_size, sizeof(xhci_trb_t));
		if (!hc[c].ring.trb) { perror("calloc"); exit(2); }
		hc[c].ring.phys = 0x10000000ull * (c + 1);
		hc[c].ring.size = ring_size;
		hc[c].ring.cycle = 1;
		hc[c].prod_cycle = 1;
		hc[c].erdp = hc[c].ring.phys;
		hc[c].cost = 1;
	}
}

// The controller's view of the ring is ERDP, not what the software has
// handled: a slot is free again only once ERDP has moved past it. Full when
// one free slot is left.
static bool ring_has_room(const controller_t *k)
{
	return k->posted - k->acked < k->ring.size - 1;
}

static void produce(uint32_t c, uint64_t at)
{
	controller_t *k = &hc[c];
	CHECK(ring_has_room(k), "model posted into a full ring on controller %u", c);
	CHECK(k->posted < MAX_EVENTS, "model ran out of event slots");
	xhci_trb_t *t = &k->ring.trb[k->enqueue];
	t->param = k->posted;                       // the event's id
	t->status = 0;
	t->control = (k->prod_cycle ? XHCI_TRB_CYCLE : 0);
	k->posted_at[k->posted++] = at;
	if (++k->enqueue == k->ring.size) { k->enqueue = 0; k->prod_cycle ^= 1; }
	if (!k->ehb) {
		k->usbsts |= XHCI_USBSTS_EINT;
		if (k->ie) k->messages++;
	}
	k->ehb = true;                              // an event sets the busy bit
}

// Post everything due, and top up any flooding controller.
static void model_tick(void)
{
	while (post_next < post_count && posts[post_next].at <= now) {
		produce(posts[post_next].c, posts[post_next].at);
		post_next++;
	}
	for (uint32_t c = 0; c < hc_count; c++)
		while (hc[c].flood && ring_has_room(&hc[c]) && hc[c].posted < MAX_EVENTS)
			produce(c, now);
}

static void schedule(uint64_t at, uint32_t c)
{
	posts[post_count].at = at;
	posts[post_count].c = c;
	post_count++;
}

static uint64_t model_clock(void *ctx)
{
	(void)ctx;
	model_tick();
	return now;
}

static void model_handle(void *ctx, const xhci_trb_t *ev)
{
	controller_t *k = ctx;
	CHECK(ev->param == k->handled, "controller handled event %lu, expected %u",
	      (unsigned long)ev->param, k->handled);
	if (k->handled < MAX_EVENTS)
		k->handled_at[k->handled] = now;
	k->handled++;
	now += k->cost;
	model_tick();
}

// The glue xhci.c wraps round a drain. `owed`: the top half acknowledged an
// interrupt since ERDP was last written.
static xhci_evt_drain_t model_pass(uint32_t c, bool use_deadline, uint64_t deadline, bool owed)
{
	controller_t *k = &hc[c];
	model_tick();
	uint32_t before = k->handled;
	// Always with the clock: the stamp for "seen empty" is the drain's own.
	xhci_evt_drain_t d = xhci_evt_drain(&k->ring, model_handle, k, model_clock, NULL,
	                                    use_deadline ? deadline : UINT64_MAX);
	CHECK(d.handled == k->handled - before, "drain said %u handled, the handler ran %u times",
	      d.handled, k->handled - before);

	bool reads_set = false;
	if (xhci_evt_erdp_wants_read(d, owed)) {
		k->reads++;
		reads_set = k->ehb;
	}
	xhci_erdp_t e = xhci_evt_erdp(&k->ring, d, owed, reads_set);
	if (e.write) {
		CHECK(k->writes < MAX_WRITES, "model ran out of write slots");
		CHECK((e.value & XHCI_ERDP_EHB) != 0, "an ERDP write without the busy bit");
		// The acknowledgement must not cover an event nobody handled. The
		// slot written is the empty slot (every posted event handled) or
		// the last event handled.
		uint64_t slot = ((e.value & ~0xFull) - k->ring.phys) / sizeof(xhci_trb_t);
		if (d.handled > 0 && d.work_left)
			CHECK(slot == (k->handled - 1) % k->ring.size,
			      "early stop wrote slot %lu, last handled is in slot %u",
			      (unsigned long)slot, (k->handled - 1) % k->ring.size);
		else
			CHECK(slot == k->handled % k->ring.size,
			      "wrote slot %lu, the next slot is %u", (unsigned long)slot,
			      k->handled % k->ring.size);
		// An unchanged pointer is rewritten for one reason only: retiring
		// the busy bit after an acknowledged interrupt.
		if (k->writes > 0 && k->write_value[k->writes - 1] == e.value)
			CHECK(d.handled == 0 && owed, "the same ERDP value %lx was written twice running",
			      (unsigned long)e.value);
		k->write_value[k->writes++] = e.value;
		xhci_evt_published(&k->ring, e);
		// An early stop names the last event handled, which the controller
		// therefore still counts as occupying its slot.
		if (d.handled > 0)
			k->acked = d.work_left ? k->handled - 1 : k->handled;
		k->erdp = e.value & ~XHCI_ERDP_EHB;
		k->ehb = false;
		// A ring not empty at the write would signal again; the model's
		// producer sets the bit again on its next event either way.
		if (k->posted != k->handled) {
			k->ehb = true;
			k->usbsts |= XHCI_USBSTS_EINT;
			if (k->ie) k->messages++;
		}
	}

	// The wait bound must cover every event this drain handled.
	uint64_t seen_empty = k->wait.seen_empty;
	uint64_t max_before = k->wait.max;
	xhci_wait_account(&k->wait, d, now, kEdges);
	if (d.handled > 0 && seen_empty != 0) {
		for (uint32_t id = before; id < k->handled && id < MAX_EVENTS; id++) {
			CHECK(k->posted_at[id] >= seen_empty,
			      "event %u was posted at %lu, before the ring was seen empty at %lu",
			      id, (unsigned long)k->posted_at[id], (unsigned long)seen_empty);
			// The event's REAL wait, post to handler, is inside the bound.
			CHECK(k->handled_at[id] - k->posted_at[id] <= k->wait.max,
			      "event %u waited %lu, the reported maximum is %lu", id,
			      (unsigned long)(k->handled_at[id] - k->posted_at[id]), (unsigned long)k->wait.max);
		}
		CHECK(k->wait.max >= now - seen_empty && k->wait.max >= max_before,
		      "the reported maximum went backwards");
	}
	return d;
}

// The interrupter's two registers as the top half sees them.
static uint32_t irq_read(void *ctx, xhci_irq_reg_t reg)
{
	controller_t *k = ctx;
	return reg == XHCI_REG_USBSTS ? k->usbsts : (k->ie ? XHCI_IMAN_IE : 0);
}

static void irq_write(void *ctx, xhci_irq_reg_t reg, uint32_t value)
{
	controller_t *k = ctx;
	CHECK(k->irq_writes < MAX_WRITES, "model ran out of interrupt write slots");
	k->irq_write_reg[k->irq_writes] = reg;
	k->irq_write_value[k->irq_writes++] = value;
	if (reg == XHCI_REG_USBSTS)
		k->usbsts &= ~value;                    // write-one-to-clear, all of it
	else
		k->ie = (value & XHCI_IMAN_IE) != 0;    // IE is read-write: a write without it turns it off
}

// The top half, for every controller, as one entry of the shared vector.
// Returns whether any controller had signalled (else the entry is spurious).
static bool model_irq(void)
{
	bool any = false;
	for (uint32_t c = 0; c < hc_count; c++) {
		hc[c].answered = hc[c].messages;
		if (xhci_irq_acknowledge(irq_read, irq_write, &hc[c])) {
			hc[c].owed = true;
			any = true;
		}
	}
	return any;
}

// A pass as kusb makes it for an interrupt-driven controller: the mark is
// taken before the drain.
static xhci_evt_drain_t model_irq_pass(uint32_t c, bool use_deadline, uint64_t deadline)
{
	bool owed = hc[c].owed;
	hc[c].owed = false;
	return model_pass(c, use_deadline, deadline, owed);
}

// ── The cases ───────────────────────────────────────────────────────────────

static void test_drive_choice(void)
{
	for (int msi = 0; msi < 2; msi++)
		for (int msix = 0; msix < 2; msix++) {
			xhci_drive_choice_t lit = xhci_drive_choose(msi, msix, true);
			CHECK(lit.driven == XHCI_DRIVEN_TICK && lit.why_tick == XHCI_TICK_FLASHLIGHT,
			      "the flashlight did not force the tick (msi %d msix %d)", msi, msix);
			CHECK(strcmp(xhci_drive_text(lit), "tick (USBPOLL)") == 0, "flashlight text");
			xhci_drive_choice_t c = xhci_drive_choose(msi, msix, false);
			xhci_driven_t want = msi ? XHCI_DRIVEN_MSI : msix ? XHCI_DRIVEN_MSIX : XHCI_DRIVEN_TICK;
			CHECK(c.driven == want, "msi %d msix %d chose %d", msi, msix, (int)c.driven);
			CHECK((c.driven == XHCI_DRIVEN_TICK) == (c.why_tick == XHCI_TICK_NO_CAPABILITY) &&
			      (c.driven != XHCI_DRIVEN_TICK) == (c.why_tick == XHCI_TICK_BY_CHOICE_NONE),
			      "msi %d msix %d: the reason does not match the choice", msi, msix);
			const char *text = msi ? "msi" : msix ? "msix" : "tick (no capability)";
			CHECK(strcmp(xhci_drive_text(c), text) == 0, "msi %d msix %d reads %s", msi, msix, xhci_drive_text(c));
		}
	xhci_drive_choice_t failed = { XHCI_DRIVEN_TICK, XHCI_TICK_DID_NOT_TAKE };
	CHECK(strcmp(xhci_drive_text(failed), "tick (did not take)") == 0, "did-not-take text");
	xhci_drive_choice_t undecided = { XHCI_DRIVEN_TICK, XHCI_TICK_NOT_ASKED };
	CHECK(strcmp(xhci_drive_text(undecided), "tick") == 0, "undecided text");
}

static void test_irq_acknowledge(void)
{
	model_reset(1, 8);
	controller_t *k = &hc[0];
	k->ie = true;
	// Not this controller's interrupt: nothing is written, and the
	// register's other bits are not touched either.
	k->usbsts = (1u << 4) | (1u << 2);
	CHECK(!xhci_irq_acknowledge(irq_read, irq_write, k) && k->irq_writes == 0,
	      "EINT clear, yet the top half wrote %u register(s)", k->irq_writes);
	// Its interrupt: USBSTS with EINT alone, THEN IMAN with IP and IE.
	produce(0, now);
	CHECK(k->messages == 1 && (k->usbsts & XHCI_USBSTS_EINT), "the model did not signal");
	CHECK(xhci_irq_acknowledge(irq_read, irq_write, k), "EINT set, not acknowledged");
	CHECK(k->irq_writes == 2, "%u writes, wanted two", k->irq_writes);
	CHECK(k->irq_write_reg[0] == XHCI_REG_USBSTS && k->irq_write_value[0] == XHCI_USBSTS_EINT,
	      "first write: register %d value %x", (int)k->irq_write_reg[0], k->irq_write_value[0]);
	CHECK(k->irq_write_reg[1] == XHCI_REG_IMAN &&
	      k->irq_write_value[1] == (XHCI_IMAN_IP | XHCI_IMAN_IE),
	      "second write: register %d value %x", (int)k->irq_write_reg[1], k->irq_write_value[1]);
	CHECK(k->usbsts == ((1u << 4) | (1u << 2)), "USBSTS's other bits were cleared: %x", k->usbsts);
	CHECK(k->ie, "the acknowledgement turned the interrupt off");
	// Acknowledged once: a second entry finds nothing.
	CHECK(!xhci_irq_acknowledge(irq_read, irq_write, k) && k->irq_writes == 2, "acknowledged twice");
}

static void test_irq_protocol(void)
{
	// One event, one message, one pass: handled, ERDP written, quiet again.
	model_reset(2, 8);
	hc[0].ie = hc[1].ie = true;
	produce(0, now);
	CHECK(model_irq() && hc[0].owed && !hc[1].owed, "the wrong controller was marked");
	model_irq_pass(0, false, 0);
	CHECK(hc[0].handled == 1 && hc[0].writes == 1 && !hc[0].ehb && !hc[0].owed, "one event");

	// While the busy bit is set the controller says nothing more: three
	// events, one message. The pass that answers it takes all three.
	produce(0, now); produce(0, now); produce(0, now);
	CHECK(hc[0].messages == 2, "%u messages for a burst behind one busy bit", hc[0].messages);
	model_irq();
	model_irq_pass(0, false, 0);
	CHECK(hc[0].handled == 4 && hc[0].messages == 2, "the burst");

	// An event between the acknowledgement and the drain is the drain's.
	produce(0, now);
	model_irq();
	produce(0, now);
	model_irq_pass(0, false, 0);
	CHECK(hc[0].handled == 6 && hc[0].messages == 3 && !hc[0].ehb, "an event after the acknowledgement");

	// A drain stopped at the deadline with events left: its ERDP write
	// clears the busy bit on a ring that is not empty, and the controller
	// signals again by itself.
	hc[0].cost = 10;
	produce(0, now); produce(0, now); produce(0, now);
	model_irq();
	xhci_evt_drain_t d = model_irq_pass(0, true, now + 11);
	CHECK(d.work_left && hc[0].handled == 8, "the deadline did not stop the drain after two: %u", hc[0].handled);
	CHECK(hc[0].messages == 5 && (hc[0].usbsts & XHCI_USBSTS_EINT), "no second signal after an early stop");
	model_irq();
	model_irq_pass(0, false, 0);
	CHECK(hc[0].handled == 9 && !hc[0].ehb, "the rest was not taken");
	// Stopped after ONE event from a ring last published empty, nothing is
	// written (the pointer would be the one the register holds), so the
	// busy bit stays and the controller says nothing: the pass's own
	// "work left" is what brings the daemon back.
	produce(0, now); produce(0, now);
	model_irq();
	uint32_t messages = hc[0].messages;
	d = model_irq_pass(0, true, now + 1);
	CHECK(d.work_left && hc[0].handled == 10 && hc[0].ehb && hc[0].messages == messages,
	      "a one-event stop: handled %u, busy %d, %u new message(s)", hc[0].handled, hc[0].ehb,
	      hc[0].messages - messages);
	model_irq_pass(0, false, 0);
	CHECK(hc[0].handled == 11 && !hc[0].ehb, "the event left by a one-event stop");
	hc[0].cost = 1;

	// A tick's pass takes the event before the top half has run. The top
	// half still marks; the pass that answers finds nothing, reads ERDP,
	// finds the bit clear and writes nothing.
	produce(0, now);
	model_pass(0, false, 0, false);
	uint32_t writes = hc[0].writes, reads = hc[0].reads;
	CHECK(model_irq() && hc[0].owed, "the late top half did not mark");
	model_irq_pass(0, false, 0);
	CHECK(hc[0].writes == writes && hc[0].reads == reads + 1, "an already-serviced interrupt: %u writes %u reads",
	      hc[0].writes - writes, hc[0].reads - reads);

	// An event pending when the interrupt is turned on never signals:
	// enabling marks the controller once, and that first pass takes it.
	model_reset(1, 8);
	produce(0, now);
	CHECK(hc[0].messages == 0, "a message with the interrupt off");
	hc[0].ie = true;
	hc[0].owed = true;
	model_irq_pass(0, false, 0);
	CHECK(hc[0].handled == 1 && !hc[0].ehb, "an event pending at enabling was stranded");
	produce(0, now);
	CHECK(hc[0].messages == 1, "no message for the first event after enabling");

	// A spurious entry: nobody had signalled.
	CHECK(!model_irq() || hc[0].owed, "setup");
	model_irq_pass(0, false, 0);
	CHECK(!model_irq(), "an entry with nothing pending was not called spurious");
}

// Interrupts ALONE: no tick ever comes. A pass runs only because the top
// half marked a controller or because the last pass said work was left.
// Whatever the bursts, costs and deadlines, nothing may be stranded.
static void test_irq_random(int rounds)
{
	uint32_t seed = 0x0BADCAFE;
#define RND(n) ((seed = seed * 1664525u + 1013904223u) >> 8) % (n)
	for (int round = 0; round < rounds; round++) {
		uint32_t controllers = 1 + RND(MAX_CONTROLLERS);
		uint32_t size = 4 + RND(13);
		model_reset(controllers, size);
		for (uint32_t c = 0; c < controllers; c++) {
			hc[c].cost = 1 + RND(6);
			hc[c].ie = true;
		}
		bool raised = false;
		for (int step = 0; step < 300; step++) {
			if (step < 200 && RND(3) == 0) {
				uint32_t c = RND(controllers);
				uint32_t burst = 1 + RND(size);
				for (uint32_t i = 0; i < burst && ring_has_room(&hc[c]); i++) produce(c, now);
			}
			// The top half runs when a message is undelivered, and
			// sometimes late: after the next pass has already been by.
			bool pending = false;
			for (uint32_t c = 0; c < controllers; c++) pending |= hc[c].answered != hc[c].messages;
			bool fired = pending && RND(4) != 0 && model_irq();
			if (!fired && !raised) { now += RND(5); continue; }
			raised = false;
			uint64_t deadline = now + 1 + RND(20);
			for (uint32_t c = 0; c < controllers; c++) {
				xhci_evt_drain_t d = model_irq_pass(c, true, deadline);
				if (d.work_left) { raised = true; break; }
			}
			now += RND(5);
		}
		// Quiet now: let every outstanding message and raised bell be answered.
		for (int settle = 0; settle < 64; settle++) {
			bool fired = model_irq();
			if (!fired && !raised) break;
			raised = false;
			for (uint32_t c = 0; c < controllers; c++)
				if (model_irq_pass(c, true, now + 1000).work_left) raised = true;
		}
		for (uint32_t c = 0; c < controllers; c++) {
			CHECK(hc[c].handled == hc[c].posted, "round %d: controller %u has %u of %u handled with no message outstanding",
			      round, c, hc[c].handled, hc[c].posted);
			CHECK(!hc[c].ehb, "round %d: controller %u is left with its busy bit set: it will never signal again", round, c);
			CHECK(hc[c].ie, "round %d: controller %u's interrupt was turned off", round, c);
		}
	}
#undef RND
}

static void test_drain_to_empty(void)
{
	model_reset(1, 8);
	for (int i = 0; i < 5; i++) produce(0, now);
	xhci_evt_drain_t d = model_pass(0, false, 0, false);
	CHECK(d.handled == 5 && !d.work_left, "handled %u work_left %d", d.handled, d.work_left);
	CHECK(hc[0].writes == 1, "%u ERDP writes for one drain", hc[0].writes);
	CHECK(hc[0].write_value[0] == (hc[0].ring.phys + 5 * 16) + XHCI_ERDP_EHB, "empty stop names slot 5");
	CHECK(hc[0].reads == 0, "a drain that made progress read ERDP");
}

static void test_idle_passes_write_nothing(void)
{
	model_reset(1, 8);
	for (int i = 0; i < 100; i++) {
		xhci_evt_drain_t d = model_pass(0, true, now + 10, false);
		CHECK(d.handled == 0 && !d.work_left, "an idle pass handled something");
		now += 10;
	}
	CHECK(hc[0].writes == 0 && hc[0].reads == 0, "100 idle passes: %u writes, %u reads",
	      hc[0].writes, hc[0].reads);
	// And after real work, idle passes still write nothing more.
	produce(0, now);
	model_pass(0, true, now + 10, false);
	for (int i = 0; i < 100; i++) { now += 10; model_pass(0, true, now + 10, false); }
	CHECK(hc[0].writes == 1, "idle passes after one event: %u writes", hc[0].writes);
}

static void test_early_stop(void)
{
	model_reset(1, 8);
	hc[0].cost = 4;
	for (int i = 0; i < 5; i++) produce(0, now);
	uint64_t deadline = now + 10;
	xhci_evt_drain_t d = model_pass(0, true, deadline, false);
	// Handlers at +0, +4, +8; the clock reads +12 before the fourth.
	CHECK(d.handled == 3 && d.work_left, "handled %u work_left %d", d.handled, d.work_left);
	CHECK(hc[0].writes == 1 && hc[0].write_value[0] == (hc[0].ring.phys + 2 * 16) + XHCI_ERDP_EHB,
	      "early stop names slot 2, the last handled");
	CHECK(now <= deadline + hc[0].cost, "the drain overran its deadline by more than one handler");
	// What was left survives, in order.
	d = model_pass(0, false, 0, false);
	CHECK(d.handled == 2 && !d.work_left && hc[0].handled == 5, "the events left did not survive");
	CHECK(hc[0].write_value[1] == (hc[0].ring.phys + 5 * 16) + XHCI_ERDP_EHB, "then the empty slot");
}

// A drain's later events wait through the handlers ahead of them: the
// bound runs to the drain's end, not its start. (Codex, #559: seen empty at
// 100, two events at 101, the drain begins at 105, handlers cost 6; the
// second event is handled at 111 and waited 10, where a bound to the
// drain's start said 5.)
static void test_wait_includes_the_drain(void)
{
	model_reset(1, 16);
	now = 100;
	model_pass(0, false, 0, false);            // seen empty at 100
	hc[0].cost = 6;
	produce(0, 101); produce(0, 101);
	now = 105;
	model_pass(0, false, 0, false);
	CHECK(hc[0].handled_at[1] - hc[0].posted_at[1] == 10, "setup: the second event waited %lu",
	      (unsigned long)(hc[0].handled_at[1] - hc[0].posted_at[1]));
	CHECK(hc[0].wait.max == 17, "the bound runs from 100 to the drain's end at 117: %lu",
	      (unsigned long)hc[0].wait.max);
}

// The ring is looked at and found empty, and THEN, before the caller gets
// round to accounting, an event arrives. The stamp for "seen empty" must be
// from before the look, or it is later than the event and the bound comes
// out shorter than the wait. (Codex, #559 round 2: empty check at 100, an
// arrival at 101, accounting at 200, handling at 205; a stamp taken at
// accounting reported 5 for a wait of 104.)
static void test_wait_stamp_precedes_the_look(void)
{
	model_reset(1, 16);
	controller_t *k = &hc[0];
	now = 100;
	xhci_evt_drain_t d = xhci_evt_drain(&k->ring, model_handle, k, model_clock, NULL, UINT64_MAX);
	CHECK(d.handled == 0 && !d.work_left && d.empty_at == 100, "setup: empty_at %lu", (unsigned long)d.empty_at);
	now = 101;
	produce(0, now);                           // arrives after the look
	now = 200;                                 // the caller was preempted
	xhci_wait_account(&k->wait, d, now, kEdges);
	CHECK(k->wait.seen_empty == 100, "the stamp is %lu, not the look's 100", (unsigned long)k->wait.seen_empty);
	k->cost = 5;
	d = xhci_evt_drain(&k->ring, model_handle, k, model_clock, NULL, UINT64_MAX);
	xhci_wait_account(&k->wait, d, now, kEdges);
	CHECK(k->handled_at[0] - k->posted_at[0] == 99, "setup: the event waited %lu",
	      (unsigned long)(k->handled_at[0] - k->posted_at[0]));
	CHECK(k->wait.max == 105 && k->wait.max >= k->handled_at[0] - k->posted_at[0],
	      "the bound %lu does not cover the wait", (unsigned long)k->wait.max);
	// A drain with no clock has no stamp to give, and says so with zero.
	d = xhci_evt_drain(&k->ring, model_handle, k, NULL, NULL, 0);
	CHECK(d.empty_at == 0 && !d.work_left, "a clockless drain stamped %lu", (unsigned long)d.empty_at);
}

// A clock that moves on every reading, and a producer that posts whatever
// has come due at the START of a reading: so an event can arrive after one
// reading (and the look that follows it) and before the next.
static uint64_t stepping_clock(void *ctx)
{
	(void)ctx;
	model_tick();
	uint64_t t = now;
	now += 2;
	return t;
}

// The same rule against the drain itself: the stamp is the reading taken
// before the look, not one taken after it. An event due between the two
// readings arrives after the look; a stamp from the second reading would be
// later than the event.
static void test_wait_stamp_is_the_reading_before_the_look(void)
{
	model_reset(1, 16);
	controller_t *k = &hc[0];
	now = 100;
	schedule(101, 0);
	xhci_evt_drain_t d = xhci_evt_drain(&k->ring, model_handle, k, stepping_clock, NULL, UINT64_MAX);
	CHECK(d.handled == 0 && !d.work_left, "setup: the look should find the ring empty");
	CHECK(d.empty_at == 100, "the stamp is %lu, not the reading before the look (100)",
	      (unsigned long)d.empty_at);
	xhci_wait_account(&k->wait, d, now, kEdges);
	d = xhci_evt_drain(&k->ring, model_handle, k, stepping_clock, NULL, UINT64_MAX);
	CHECK(d.handled == 1 && k->posted_at[0] >= k->wait.seen_empty,
	      "the event posted at %lu is older than the seen-empty stamp %lu",
	      (unsigned long)k->posted_at[0], (unsigned long)k->wait.seen_empty);
}

// The ring was last published empty at slot N; the next drain handles the
// one event at N and its deadline stops it with another waiting. The last
// event handled is slot N, which the register already names: writing it
// again would be the same ERDP value twice running (xHCI 1.2b § 4.9.4).
// Nothing is written, and the next drain publishes past it. (Codex, #559.)
static void test_one_event_partial_drain(void)
{
	model_reset(1, 8);
	hc[0].cost = 20;
	for (int i = 0; i < 3; i++) produce(0, now);
	model_pass(0, false, 0, false);            // drained: ERDP names slot 3, empty
	CHECK(hc[0].writes == 1 && hc[0].ring.published == 3, "setup: published slot %u", hc[0].ring.published);
	produce(0, now); produce(0, now);          // slots 3 and 4
	xhci_evt_drain_t d = model_pass(0, true, now + 1, false);
	CHECK(d.handled == 1 && d.work_left, "setup: handled %u work_left %d", d.handled, d.work_left);
	CHECK(hc[0].writes == 1, "the unchanged pointer was written again (%u writes)", hc[0].writes);
	// The next drain goes on and publishes: the empty slot, 5.
	d = model_pass(0, false, 0, false);
	CHECK(d.handled == 1 && hc[0].writes == 2 && hc[0].ring.published == 5,
	      "after the partial drain: %u writes, published %u", hc[0].writes, hc[0].ring.published);
	// And a partial drain that DID move past the published slot is written.
	produce(0, now); produce(0, now); produce(0, now);   // slots 5, 6, 7
	hc[0].cost = 1;
	d = model_pass(0, true, now + 2, false);   // handles two, stops with one left
	CHECK(d.handled == 2 && d.work_left && hc[0].writes == 3 && hc[0].ring.published == 6,
	      "a partial drain of two: %u writes, published %u", hc[0].writes, hc[0].ring.published);
}

static void test_deadline_already_passed(void)
{
	model_reset(1, 8);
	produce(0, now);
	xhci_evt_drain_t d = model_pass(0, true, now, false);
	CHECK(d.handled == 0 && d.work_left, "a passed deadline handled %u", d.handled);
	CHECK(hc[0].writes == 0, "nothing handled, nothing owed, yet ERDP was written");
	// Not seen empty: the stamp does not move.
	CHECK(hc[0].wait.seen_empty == 0, "a drain stopped by its deadline moved the seen-empty stamp");
}

static void test_wrap(void)
{
	// Early stop with the cursor just wrapped: the last handled event is in
	// the segment's last slot.
	model_reset(1, 4);
	hc[0].cost = 5;
	for (int i = 0; i < 3; i++) produce(0, now);          // slots 0,1,2
	model_pass(0, false, 0, false);                        // drained; cursor 3
	produce(0, now); produce(0, now);                      // slots 3 and 0 (cycle flipped)
	xhci_evt_drain_t d = model_pass(0, true, now + 1, false);
	CHECK(d.handled == 1 && d.work_left, "wrap early stop: handled %u work_left %d", d.handled, d.work_left);
	CHECK(hc[0].ring.dequeue == 0 && hc[0].ring.cycle == 0, "the cursor did not wrap");
	CHECK(hc[0].write_value[hc[0].writes - 1] == (hc[0].ring.phys + 3 * 16) + XHCI_ERDP_EHB,
	      "early stop after a wrap names the last slot");
	d = model_pass(0, false, 0, false);
	CHECK(d.handled == 1 && hc[0].handled == 5, "the event past the wrap did not survive");

	// Empty stop exactly at the wrap: the empty slot is slot 0.
	model_reset(1, 4);
	for (int i = 0; i < 3; i++) produce(0, now);
	model_pass(0, false, 0, false);
	produce(0, now);                                       // slot 3
	d = model_pass(0, false, 0, false);
	CHECK(d.handled == 1 && !d.work_left && hc[0].ring.dequeue == 0, "did not stop empty at the wrap");
	CHECK(hc[0].write_value[hc[0].writes - 1] == hc[0].ring.phys + XHCI_ERDP_EHB,
	      "empty stop at the wrap names slot 0");

	// A full ring drained to empty, many times round.
	model_reset(1, 4);
	for (int lap = 0; lap < 50; lap++) {
		for (int i = 0; i < 3; i++) produce(0, now);
		d = model_pass(0, false, 0, false);
		CHECK(d.handled == 3 && !d.work_left, "lap %d handled %u", lap, d.handled);
	}
	CHECK(hc[0].writes == 50 && hc[0].handled == 150, "50 full laps: %u writes, %u handled",
	      hc[0].writes, hc[0].handled);
}

static void test_busy_bit(void)
{
	// An interrupt was acknowledged, but an earlier pass already took its
	// event and wrote ERDP: the bit reads clear, so nothing is written.
	model_reset(1, 8);
	produce(0, now);
	model_pass(0, false, 0, false);
	CHECK(hc[0].writes == 1 && !hc[0].ehb, "setup");
	xhci_evt_drain_t d = model_pass(0, false, 0, true);
	CHECK(d.handled == 0 && hc[0].reads == 1 && hc[0].writes == 1,
	      "owed, bit clear: %u reads %u writes", hc[0].reads, hc[0].writes);

	// The bit set with nothing to handle (boot leftovers): one read, one
	// write of the unchanged pointer, and the bit is retired.
	hc[0].ehb = true;
	model_pass(0, false, 0, true);
	CHECK(hc[0].reads == 2 && hc[0].writes == 2 && !hc[0].ehb, "owed, bit set: not retired");
	CHECK(hc[0].write_value[1] == hc[0].write_value[0], "retiring the bit moved the pointer");

	// Not owed: the register is not even read.
	hc[0].ehb = true;
	model_pass(0, false, 0, false);
	CHECK(hc[0].reads == 2 && hc[0].writes == 2, "not owed, yet ERDP was read or written");

	// The rule itself, asked directly: the bit reading set is not enough
	// without an acknowledged interrupt. (The glue never asks this way,
	// because it reads the register only when one is owed.)
	xhci_evt_drain_t nothing = { 0, false, 0 };
	CHECK(!xhci_evt_erdp(&hc[0].ring, nothing, false, true).write, "an unowed busy bit was retired");
	CHECK(xhci_evt_erdp(&hc[0].ring, nothing, true, true).write, "an owed, set busy bit was not retired");
	CHECK(!xhci_evt_erdp(&hc[0].ring, nothing, true, false).write, "an owed, clear busy bit was written");

	// Owed and progress: progress decides, no read.
	produce(0, now);
	model_pass(0, false, 0, true);
	CHECK(hc[0].reads == 2 && hc[0].writes == 3, "owed with progress: %u reads %u writes",
	      hc[0].reads, hc[0].writes);
}

static xhci_evt_drain_t visit_drain(void *ctx, uint32_t index, uint64_t deadline)
{
	(void)ctx;
	return model_pass(index, true, deadline, false);
}

// The review's counterexample: three controllers, 4 ms of work each, a
// 10 ms budget. Checked between whole rounds the timers were reached at
// 12 ms; checked before every event they are reached by the deadline plus
// one handler.
static void test_visit_deadline(void)
{
	for (uint64_t cost = 1; cost <= 4; cost++) {
		model_reset(3, 16);
		for (uint32_t c = 0; c < 3; c++) {
			hc[c].cost = cost;
			for (uint64_t i = 0; i < 4 / cost + (4 % cost != 0); i++) produce(c, now);
		}
		xhci_visit_t v = { 3, 0 };
		uint64_t began = now, deadline = now + 10;
		xhci_visit_result_t r = xhci_visit(&v, visit_drain, NULL, deadline);
		CHECK(now - began <= 10 + cost, "cost %lu: the visit took %lu against a budget of 10",
		      (unsigned long)cost, (unsigned long)(now - began));
		CHECK(r.work_left == (hc[0].posted + hc[1].posted + hc[2].posted != r.handled),
		      "cost %lu: work_left %d with %u of %u handled", (unsigned long)cost, r.work_left,
		      r.handled, hc[0].posted + hc[1].posted + hc[2].posted);
	}
}

// A flooded controller must not starve the others.
static void test_visit_rotation(void)
{
	model_reset(3, 16);
	hc[0].flood = true;
	produce(1, now);
	produce(2, now);
	xhci_visit_t v = { 3, 0 };
	xhci_visit_result_t r = xhci_visit(&v, visit_drain, NULL, now + 10);
	CHECK(r.work_left && r.visited == 1 && v.cursor == 1, "flood: visited %u cursor %u", r.visited, v.cursor);
	CHECK(hc[1].handled == 0 && hc[2].handled == 0, "setup: the others were reached under a flood");
	r = xhci_visit(&v, visit_drain, NULL, now + 10);
	CHECK(hc[1].handled == 1 && hc[2].handled == 1, "the second visit did not serve the others first");
	CHECK(r.work_left && v.cursor == 1, "the flooded controller was not last: cursor %u", v.cursor);

	// Two flooded controllers take turns; the third is served every other visit at worst.
	model_reset(3, 16);
	hc[0].flood = hc[1].flood = true;
	v = (xhci_visit_t){ 3, 0 };
	for (int i = 0; i < 6; i++) {
		if (i % 2 == 0) produce(2, now);
		xhci_visit(&v, visit_drain, NULL, now + 10);
	}
	CHECK(hc[0].handled > 0 && hc[1].handled > 0, "a flooded controller was never served");
	CHECK(hc[2].handled >= 2, "the quiet controller was served %u times in six visits", hc[2].handled);

	// No controllers, and a cursor past the end.
	v = (xhci_visit_t){ 0, 0 };
	r = xhci_visit(&v, visit_drain, NULL, now + 10);
	CHECK(r.visited == 0 && !r.work_left, "a visit of nothing did something");
	model_reset(2, 8);
	v = (xhci_visit_t){ 2, 7 };
	r = xhci_visit(&v, visit_drain, NULL, now + 10);
	CHECK(r.visited == 2, "a cursor past the end visited %u", r.visited);
}

// The review's trace: wakes at 0 and 10, an event at 9, its controller
// reached at 22. The gap between the wakes is 10; the event waited 13; the
// reported bound must cover it.
static void test_wait_covers_late_visit(void)
{
	model_reset(2, 16);
	now = 100;
	model_pass(0, true, now + 10, false);      // both seen empty at 100
	model_pass(1, true, now + 10, false);
	schedule(109, 1);
	hc[0].cost = 12;
	now = 110;                                 // the wake is on time
	produce(0, now);                           // and controller 0 has 12 ms of work
	model_pass(0, true, now + 20, false);
	CHECK(now == 122, "setup: controller 1 reached at %lu", (unsigned long)now);
	model_pass(1, true, now + 20, false);
	uint64_t waited = hc[1].handled_at[0] - hc[1].posted_at[0];
	CHECK(waited == 13, "setup: the event waited %lu", (unsigned long)waited);
	CHECK(hc[1].wait.max >= waited, "the reported bound %lu does not cover a wait of %lu",
	      (unsigned long)hc[1].wait.max, (unsigned long)waited);
	CHECK(hc[1].wait.max == 23, "the bound is from seen-empty (100) to the drain's end (123): %lu",
	      (unsigned long)hc[1].wait.max);
	CHECK(hc[1].wait.within[1] == 1 && hc[1].wait.within[0] == 0, "23 belongs in the second bucket");
	// The recent maximum is the owner's to zero; the lifetime one is not touched by that.
	CHECK(hc[1].wait.recent_max == 23, "recent_max %lu", (unsigned long)hc[1].wait.recent_max);
	hc[1].wait.recent_max = 0;
	produce(1, now);
	now += 3;
	model_pass(1, true, now + 20, false);
	CHECK(hc[1].wait.recent_max == 4 && hc[1].wait.max == 23, "after a reset: recent %lu max %lu",
	      (unsigned long)hc[1].wait.recent_max, (unsigned long)hc[1].wait.max);
}

// A ring that has never been seen empty has no bound to give: what boot
// left on it has waited an unknown time, and that must not be reported as
// a wait since time zero.
static void test_wait_before_first_empty(void)
{
	model_reset(1, 16);
	now = 5000;
	produce(0, now);
	model_pass(0, true, now + 10, false);
	CHECK(hc[0].handled == 1 && hc[0].wait.max == 0 && hc[0].wait.over == 0 &&
	      hc[0].wait.within[0] + hc[0].wait.within[1] + hc[0].wait.within[2] == 0,
	      "a ring never seen empty reported a wait of %lu", (unsigned long)hc[0].wait.max);
	CHECK(hc[0].wait.seen_empty != 0, "the first empty stop did not set the stamp");
}

// Work carried over is as old as it was: a drain stopped by its deadline
// does not move the stamp.
static void test_wait_carries_over(void)
{
	model_reset(1, 16);
	now = 100;
	model_pass(0, true, now + 10, false);      // seen empty at 100
	hc[0].cost = 6;
	produce(0, 101); produce(0, 101); produce(0, 101);
	now = 105;
	model_pass(0, true, now + 10, false);      // handles two (105, 111), stops at 117
	CHECK(hc[0].handled == 2 && hc[0].wait.seen_empty == 100, "the stamp moved: %lu",
	      (unsigned long)hc[0].wait.seen_empty);
	now = 3000;
	model_pass(0, true, now + 10, false);
	CHECK(hc[0].handled == 3 && hc[0].wait.max == 2906 && hc[0].wait.over == 1,
	      "carried-over work reported as %lu", (unsigned long)hc[0].wait.max);
	CHECK(hc[0].wait.seen_empty == now, "an empty stop did not move the stamp");
}

// Random load: every event handled once and in order (model_handle), no
// acknowledgement past what was handled and every wait covered
// (model_pass), every visit within its deadline plus one handler.
static void test_random(int rounds)
{
	uint32_t seed = 0x00C0FFEE;
#define RND(n) ((seed = seed * 1664525u + 1013904223u) >> 8) % (n)
	for (int round = 0; round < rounds; round++) {
		uint32_t controllers = 1 + RND(MAX_CONTROLLERS);
		uint32_t size = 4 + RND(13);
		model_reset(controllers, size);
		uint64_t max_cost = 1;
		for (uint32_t c = 0; c < controllers; c++) {
			hc[c].cost = 1 + RND(6);
			if (hc[c].cost > max_cost) max_cost = hc[c].cost;
		}
		xhci_visit_t v = { controllers, RND(controllers) };
		uint32_t posted = 0;
		for (int step = 0; step < 200; step++) {
			// A burst now and then, never past a ring's room.
			if (RND(3) == 0) {
				uint32_t c = RND(controllers);
				uint32_t burst = 1 + RND(size);
				for (uint32_t i = 0; i < burst && ring_has_room(&hc[c]); i++) { produce(c, now); posted++; }
			}
			uint64_t budget = 1 + RND(20);
			uint64_t began = now;
			xhci_visit_result_t r = xhci_visit(&v, visit_drain, NULL, now + budget);
			CHECK(now - began <= budget + max_cost, "round %d: a visit of budget %lu took %lu",
			      round, (unsigned long)budget, (unsigned long)(now - began));
			uint32_t handled = 0;
			for (uint32_t c = 0; c < controllers; c++) handled += hc[c].handled;
			CHECK(r.work_left || handled == posted, "round %d: no work left, yet %u of %u handled",
			      round, handled, posted);
			now += RND(15);
		}
		// Drain to empty: nothing was lost.
		for (uint32_t c = 0; c < controllers; c++) {
			model_pass(c, false, 0, false);
			CHECK(hc[c].handled == hc[c].posted, "round %d: controller %u lost events (%u of %u)",
			      round, c, hc[c].handled, hc[c].posted);
		}
	}
#undef RND
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 2000;
	test_drain_to_empty();
	test_idle_passes_write_nothing();
	test_early_stop();
	test_wait_includes_the_drain();
	test_wait_stamp_precedes_the_look();
	test_wait_stamp_is_the_reading_before_the_look();
	test_one_event_partial_drain();
	test_deadline_already_passed();
	test_wrap();
	test_busy_bit();
	test_visit_deadline();
	test_visit_rotation();
	test_wait_covers_late_visit();
	test_wait_before_first_empty();
	test_wait_carries_over();
	test_random(rounds);
	test_drive_choice();
	test_irq_acknowledge();
	test_irq_protocol();
	test_irq_random(rounds);
	for (uint32_t c = 0; c < MAX_CONTROLLERS; c++)
		free(hc[c].ring.trb);
	if (failures) {
		printf("xhci_service: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("xhci_service: all checks passed (%d random rounds)\n", rounds);
	return 0;
}
