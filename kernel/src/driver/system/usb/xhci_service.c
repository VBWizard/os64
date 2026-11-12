// xhci_service.c — draining an xHCI event ring against a deadline, what the
// drain owes ERDP, the bound on an event's wait, and the order of a visit.
// xhci_service.h is the contract and says why each rule is what it is.

#include "driver/system/usb/xhci_service.h"

#include <stddef.h>

xhci_evt_drain_t xhci_evt_drain(xhci_evt_ring_t *r, xhci_evt_handle_t handle, void *ctx,
                                xhci_clock_t clock, void *clock_ctx, uint64_t deadline)
{
	xhci_evt_drain_t result = { 0, false };

	for (;;) {
		const xhci_trb_t *ev = &r->trb[r->dequeue];
		// The controller writes the cycle bit last: a slot whose bit does
		// not match is one it has not finished, or not started.
		if ((ev->control & XHCI_TRB_CYCLE) != (r->cycle ? XHCI_TRB_CYCLE : 0))
			return result;
		if (clock != NULL && clock(clock_ctx) >= deadline) {
			result.work_left = true;
			return result;
		}

		handle(ctx, ev);

		r->dequeue++;
		if (r->dequeue == r->size) {
			// The segment is the whole ring: wrapping it is what flips the
			// cycle the controller writes next.
			r->dequeue = 0;
			r->cycle ^= 1;
		}
		result.handled++;
	}
}

bool xhci_evt_erdp_wants_read(xhci_evt_drain_t drain, bool ehb_owed)
{
	return drain.handled == 0 && ehb_owed;
}

xhci_erdp_t xhci_evt_erdp(const xhci_evt_ring_t *r, xhci_evt_drain_t drain,
                          bool ehb_owed, bool ehb_reads_set)
{
	xhci_erdp_t erdp = { false, 0 };
	uint32_t slot = r->dequeue;

	if (drain.handled > 0) {
		// Stopped with work left: name the last event handled, the slot
		// before the cursor (the last slot of the segment when the cursor
		// has just wrapped).
		if (drain.work_left)
			slot = (slot == 0 ? r->size : slot) - 1;
		// The one stop with nothing new to publish (xhci_service.h): the
		// last event handled is the slot the register already names.
		erdp.write = !(drain.work_left && slot == r->published);
	} else if (ehb_owed && ehb_reads_set) {
		erdp.write = true;
	}

	erdp.value = (r->phys + (uint64_t)slot * sizeof(xhci_trb_t)) | XHCI_ERDP_EHB;
	return erdp;
}

void xhci_evt_published(xhci_evt_ring_t *r, xhci_erdp_t erdp)
{
	r->published = (uint32_t)(((erdp.value & ~0xFull) - r->phys) / sizeof(xhci_trb_t));
}

void xhci_wait_account(xhci_wait_t *w, xhci_evt_drain_t drain, uint64_t ended,
                       const uint64_t edge[3])
{
	// A ring never yet seen empty has no bound to give: boot left events
	// on it for an unknown time.
	if (drain.handled > 0 && w->seen_empty != 0) {
		// To the drain's END: the last event it handled waited through the
		// handlers ahead of it.
		uint64_t bound = ended - w->seen_empty;
		if (bound > w->max)
			w->max = bound;
		if (bound > w->recent_max)
			w->recent_max = bound;
		if (bound <= edge[0]) w->within[0]++;
		else if (bound <= edge[1]) w->within[1]++;
		else if (bound <= edge[2]) w->within[2]++;
		else w->over++;
	}
	// The drain's last look found the ring empty at `ended`. A drain the
	// deadline stopped leaves the stamp where it was: the events still on
	// the ring are as old as they were.
	if (!drain.work_left)
		w->seen_empty = ended;
}

xhci_visit_result_t xhci_visit(xhci_visit_t *v, xhci_visit_drain_t drain, void *ctx,
                               uint64_t deadline)
{
	xhci_visit_result_t result = { 0, 0, false };
	if (v->count == 0)
		return result;

	// Taken modulo the count below, so a cursor left over from a larger
	// count still names a controller.
	uint32_t first = v->cursor;
	for (uint32_t n = 0; n < v->count; n++) {
		uint32_t index = (first + n) % v->count;
		xhci_evt_drain_t d = drain(ctx, index, deadline);
		result.handled += d.handled;
		result.visited++;
		if (d.work_left) {
			v->cursor = (index + 1) % v->count;
			result.work_left = true;
			return result;
		}
	}
	return result;
}
