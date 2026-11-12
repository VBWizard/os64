// xhci_service.h — the consumer's side of an xHCI event ring, and the order
// a daemon visits several of them in (XHCI_INTERRUPT.md § The design, D2,
// D6, D10).
//
// PURE: no kernel headers, no hardware. tools/test_xhci_service_host.sh
// drives it against a model controller whose producer stamps its events and
// whose register file records every write, with handlers that cost time.
// What is here is the part of the driver that is arithmetic about a ring and
// a clock; the registers themselves stay in xhci.c.

#ifndef XHCI_SERVICE_H
#define XHCI_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

// A Transfer Request Block: the unit of every xHCI ring.
typedef struct {
	uint64_t param;
	uint32_t status;
	uint32_t control;
} __attribute__((packed)) xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "an xHCI TRB is sixteen bytes");

#define XHCI_TRB_CYCLE  (1u << 0)    // control bit 0: who owns the slot
#define XHCI_ERDP_EHB   (1ull << 3)  // Event Handler Busy, write-one-to-clear

// One event ring segment as its consumer sees it.
typedef struct {
	xhci_trb_t *trb;      // the segment
	uint64_t    phys;     // its address as the controller knows it
	uint32_t    size;     // TRBs in the segment
	uint32_t    dequeue;  // the next slot to look at
	uint32_t    cycle;    // 1 or 0: the cycle bit a slot carries once the controller has written it
	uint32_t    published; // the slot ERDP was last written with (0 at setup, where ERDP names the segment's start)
} xhci_evt_ring_t;

// Handle one event. The event is the consumer's until this returns.
typedef void (*xhci_evt_handle_t)(void *ctx, const xhci_trb_t *ev);
// The clock the deadline is read against, in whatever unit the caller's
// deadline is in.
typedef uint64_t (*xhci_clock_t)(void *ctx);

typedef struct {
	uint32_t handled;    // events handled
	// Why the drain stopped. false: the next slot is not the controller's
	// to read yet, so the ring is empty. true: the deadline passed and the
	// next slot holds an event nobody has handled.
	bool     work_left;
} xhci_evt_drain_t;

// Handle events until the ring is empty or `deadline` has passed. The clock
// is read before EVERY event, so a drain overruns its deadline by at most
// one handler. `clock` NULL means no deadline: drain to empty.
xhci_evt_drain_t xhci_evt_drain(xhci_evt_ring_t *r, xhci_evt_handle_t handle, void *ctx,
                                xhci_clock_t clock, void *clock_ctx, uint64_t deadline);

// What a drain owes the controller's ERDP register.
typedef struct {
	bool     write;   // false: write nothing
	uint64_t value;   // the dequeue pointer with EHB set
} xhci_erdp_t;

// True when xhci_evt_erdp needs to know whether ERDP's EHB bit reads set:
// the drain handled nothing, and the top half has acknowledged an interrupt
// for this ring since ERDP was last written.
bool xhci_evt_erdp_wants_read(xhci_evt_drain_t drain, bool ehb_owed);

// ERDP is written for two reasons and never for neither. PROGRESS: the
// drain handled events. The pointer names the empty slot when the ring was
// drained to empty, and the LAST EVENT HANDLED when the deadline stopped it
// with work left, so the acknowledgement never covers an event nobody has
// handled. One such stop has nothing new to say: the ring was last
// published empty at slot N and the drain handled the one event at N
// before its deadline. The last event handled IS slot N, the pointer the
// register already holds, and it is not written again; the next drain
// publishes further. RETIRING THE BUSY BIT: nothing was handled, an
// interrupt was acknowledged (`ehb_owed`), and the bit reads set
// (`ehb_reads_set`); the pointer is written back unchanged, with the bit,
// once. Otherwise nothing: an unchanged pointer is not rewritten.
xhci_erdp_t xhci_evt_erdp(const xhci_evt_ring_t *r, xhci_evt_drain_t drain,
                          bool ehb_owed, bool ehb_reads_set);

// The caller wrote `erdp` to the register: remember what it names.
void xhci_evt_published(xhci_evt_ring_t *r, xhci_erdp_t erdp);

// ── How long an event may have waited ───────────────────────────────────────
//
// The controller does not stamp its events, so the wait is bounded, not
// measured: an event found by a drain arrived after the last moment this
// ring was SEEN EMPTY and was handled before the drain ENDED. That interval
// includes whatever the daemon did before it reached this ring and the
// handlers that ran ahead of the event inside the drain, and it keeps
// growing across wakes while work is carried over, because a drain stopped
// by its deadline does not move the stamp.
typedef struct {
	uint64_t seen_empty;    // when a drain last found this ring empty (0 = never)
	uint64_t max;           // the longest bound of any drain that handled events
	uint64_t recent_max;    // the same, since the owner last zeroed it (a report's "since last read")
	uint64_t within[3];     // drains whose bound was within edge[0], edge[1], edge[2]
	uint64_t over;          // and beyond the last edge
} xhci_wait_t;

// Account for one drain that ended at `ended` on the clock the edges are
// in. `edge` is three ascending bounds.
void xhci_wait_account(xhci_wait_t *w, xhci_evt_drain_t drain, uint64_t ended,
                       const uint64_t edge[3]);

// ── The order of a visit ────────────────────────────────────────────────────

typedef struct {
	uint32_t count;    // controllers
	uint32_t cursor;   // the controller the next visit starts at
} xhci_visit_t;

// Drain controller `index` against `deadline`.
typedef xhci_evt_drain_t (*xhci_visit_drain_t)(void *ctx, uint32_t index, uint64_t deadline);

typedef struct {
	uint32_t handled;    // events, all controllers
	uint32_t visited;    // controllers drained this visit
	// A controller was stopped by the deadline, or was not reached because
	// one before it was. The caller comes back without waiting to be told.
	bool     work_left;
} xhci_visit_result_t;

// Visit each controller once, starting at the cursor. A controller stopped
// by the deadline ends the visit and moves the cursor to the one after it:
// the controller that used the time is served last next visit, and the ones
// not reached are served first.
xhci_visit_result_t xhci_visit(xhci_visit_t *v, xhci_visit_drain_t drain, void *ctx,
                               uint64_t deadline);

#endif
