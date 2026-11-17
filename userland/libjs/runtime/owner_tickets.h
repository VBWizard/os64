#ifndef JS_OWNER_TICKETS_H
#define JS_OWNER_TICKETS_H
#include "os64/js.h"

/* Private, owner-thread metadata. Identity validation grants no lifecycle,
 * execution, security or reclamation authority. No engine/native owner
 * pointers, roots, callbacks or allocation are managed by this unit. */
typedef enum {
    JS_OWNER_OK, JS_OWNER_INVALID, JS_OWNER_FOREIGN, JS_OWNER_STALE,
    JS_OWNER_CAPACITY, JS_OWNER_EXHAUSTED
} JSOwnerStatus;
typedef struct { uint64_t incarnation, serial; } JSOwnerId;
typedef struct { JSOwnerId owner; uint64_t serial; } JSOwnerTicket;
typedef struct { JSOwnerId owner; uint64_t serial; } JSOwnerLoan;
typedef struct { size_t owners, tickets, loans; } JSOwnerLimits;
typedef struct { size_t tickets, loans; } JSOwnerCounts;
typedef struct { size_t owners, tickets, loans; bool unknown_escape; } JSOwnerSummary;
typedef struct JSOwnerCell JSOwnerCell;
typedef struct JSOwnerTokenCell JSOwnerTokenCell;

/* Caller-owned state: zero-initialize before first init; do not copy, edit or
 * reset during its lifetime. Fields are private, exposed for stack storage.
 * Owner rows remain cumulatively reserved. Ticket and loan cells share one
 * non-reusing serial issuer, despite having distinct token types/pools. */
typedef struct {
    uint64_t _incarnation, _last_owner, _last_token;
    JSOwnerLimits _limits;
    JSOwnerCell *_owners;
    JSOwnerTokenCell *_tickets, *_loans;
    size_t _owners_used, _tickets_used, _loans_used;
    bool _unknown_escape;
} JSOwnerTickets;

/* Storage size excludes sizeof(JSOwnerTickets). Init takes malloc-like
 * untyped storage aligned for max_align_t, disjoint from the table; both must
 * remain live through use. Size/range arithmetic is checked before writes.
 * All capacities are positive; owners cannot exceed OS64_JS_MAX_REALMS.
 * Refusal leaves state/storage and output arguments unchanged. Outputs must
 * be caller objects outside the table/storage. Inputs are passed by value.
 *
 * Integration supplies a nonzero live incarnation from a proven fresh issuer
 * and the live table belonging to that runtime. This unit checks that numeric
 * binding; it neither issues incarnations nor validates arbitrary pointers.
 * Distinct test incarnations do not prove production issuer uniqueness or
 * admission charging. No token contains a dereferenceable runtime pointer. */
JSOwnerStatus js_owner_tickets_size(JSOwnerLimits, size_t *bytes);
JSOwnerStatus js_owner_tickets_init(JSOwnerTickets *, uint64_t live_incarnation,
    JSOwnerLimits, void *storage, size_t bytes);
/* Create reserves one metadata owner row and first ticket, not a realm or
 * native resource. There is no owner-row erase/reuse operation in this unit. */
JSOwnerStatus js_owner_ticket_create(JSOwnerTickets *, uint64_t, JSOwnerTicket *);
JSOwnerStatus js_owner_ticket_duplicate(JSOwnerTickets *, uint64_t, JSOwnerTicket, JSOwnerTicket *);
JSOwnerStatus js_owner_ticket_resolve(const JSOwnerTickets *, uint64_t, JSOwnerTicket, JSOwnerId *);
JSOwnerStatus js_owner_ticket_close(JSOwnerTickets *, uint64_t, JSOwnerTicket);
JSOwnerStatus js_owner_loan_begin(JSOwnerTickets *, uint64_t, JSOwnerTicket, JSOwnerLoan *);
JSOwnerStatus js_owner_loan_owner(const JSOwnerTickets *, uint64_t, JSOwnerLoan, JSOwnerId *);
JSOwnerStatus js_owner_loan_promote(JSOwnerTickets *, uint64_t, JSOwnerLoan, JSOwnerTicket *);
/* End only after the surrounding engine/settings/value cleanup has unwound.
 * Closing a ticket does not end admitted loans; metadata close/end perform no
 * root release, callback, collection, provider detach or quota uncharge. */
JSOwnerStatus js_owner_loan_end(JSOwnerTickets *, uint64_t, JSOwnerLoan);
JSOwnerStatus js_owner_counts(const JSOwnerTickets *, uint64_t, JSOwnerId, JSOwnerCounts *);
JSOwnerStatus js_owner_summary(const JSOwnerTickets *, uint64_t, JSOwnerSummary *);
/* Sticky retention metadata, not engine failure/activity/F16 state. Zero
 * holders or absence of this flag is not a release certificate. */
JSOwnerStatus js_owner_note_unknown_escape(JSOwnerTickets *, uint64_t);
#endif
