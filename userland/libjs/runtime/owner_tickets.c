#include "owner_tickets.h"

struct JSOwnerCell { uint64_t serial; size_t tickets, loans; };
struct JSOwnerTokenCell { uint64_t serial, owner; };
typedef struct { size_t tickets, loans, bytes; } OwnerLayout;

static bool span(size_t *at, size_t alignment, size_t count, size_t size, size_t *offset)
{
    size_t remainder = *at % alignment;
    size_t pad = remainder ? alignment - remainder : 0;
    if (pad > SIZE_MAX - *at) return false;
    size_t start = *at + pad;
    if (count > (SIZE_MAX - start) / size) return false;
    *offset = start;
    *at = start + count * size;
    return true;
}
static bool layout(JSOwnerLimits limits, OwnerLayout *out)
{
    if (!limits.owners || limits.owners > OS64_JS_MAX_REALMS || !limits.tickets || !limits.loans)
        return false;
    size_t at = 0, owners;
    if (!span(&at, _Alignof(JSOwnerCell), limits.owners, sizeof(JSOwnerCell), &owners) ||
        !span(&at, _Alignof(JSOwnerTokenCell), limits.tickets, sizeof(JSOwnerTokenCell), &out->tickets) ||
        !span(&at, _Alignof(JSOwnerTokenCell), limits.loans, sizeof(JSOwnerTokenCell), &out->loans))
        return false;
    out->bytes = at;
    return true;
}
JSOwnerStatus js_owner_tickets_size(JSOwnerLimits limits, size_t *bytes)
{
    OwnerLayout space;
    if (!bytes || !layout(limits, &space)) return JS_OWNER_INVALID;
    *bytes = space.bytes;
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_tickets_init(JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerLimits limits, void *storage, size_t bytes)
{
    OwnerLayout space;
    if (!table || table->_incarnation || !incarnation || !storage || !layout(limits, &space) ||
        bytes < space.bytes || (uintptr_t)storage % _Alignof(max_align_t)) return JS_OWNER_INVALID;
    uintptr_t start = (uintptr_t)storage, object = (uintptr_t)table;
    if (bytes > UINTPTR_MAX - start || sizeof(*table) > UINTPTR_MAX - object ||
        (start < object + sizeof(*table) && object < start + bytes)) return JS_OWNER_INVALID;
    JSOwnerCell *owners = storage;
    JSOwnerTokenCell *tickets = (void *)((unsigned char *)storage + space.tickets);
    JSOwnerTokenCell *loans = (void *)((unsigned char *)storage + space.loans);
    for (size_t i = 0; i < limits.owners; i++) owners[i] = (JSOwnerCell){0};
    for (size_t i = 0; i < limits.tickets; i++) tickets[i] = (JSOwnerTokenCell){0};
    for (size_t i = 0; i < limits.loans; i++) loans[i] = (JSOwnerTokenCell){0};
    *table = (JSOwnerTickets){._incarnation=incarnation, ._limits=limits,
        ._owners=owners, ._tickets=tickets, ._loans=loans};
    return JS_OWNER_OK;
}
static JSOwnerStatus live(const JSOwnerTickets *table, uint64_t incarnation)
{
    if (!table || !table->_incarnation || !incarnation) return JS_OWNER_INVALID;
    return incarnation == table->_incarnation ? JS_OWNER_OK : JS_OWNER_FOREIGN;
}
static JSOwnerStatus owner_at(const JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerId owner, size_t *at)
{
    JSOwnerStatus status = live(table, incarnation);
    if (status != JS_OWNER_OK) return status;
    if (!owner.incarnation || !owner.serial) return JS_OWNER_INVALID;
    if (owner.incarnation != incarnation) return JS_OWNER_FOREIGN;
    for (size_t i = 0; i < table->_owners_used; i++)
        if (table->_owners[i].serial == owner.serial) { *at = i; return JS_OWNER_OK; }
    return JS_OWNER_STALE;
}
static JSOwnerStatus token_at(const JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerId owner, uint64_t serial, bool loan, size_t *owner_index, size_t *token_index)
{
    JSOwnerStatus status = owner_at(table, incarnation, owner, owner_index);
    if (status != JS_OWNER_OK) return status;
    if (!serial) return JS_OWNER_INVALID;
    const JSOwnerTokenCell *cells = loan ? table->_loans : table->_tickets;
    size_t count = loan ? table->_limits.loans : table->_limits.tickets;
    for (size_t i = 0; i < count; i++)
        if (cells[i].serial == serial && cells[i].owner == owner.serial) {
            *token_index = i; return JS_OWNER_OK;
        }
    return JS_OWNER_STALE;
}
static size_t free_token(JSOwnerTokenCell *cells, size_t count)
{
    for (size_t i = 0; i < count; i++) if (!cells[i].serial) return i;
    return count;
}
/* Reservation checks precede these writes; no callout can interleave a commit. */
static void put_ticket(JSOwnerTickets *table, size_t owner, size_t at, JSOwnerTicket *out)
{
    uint64_t serial = ++table->_last_token;
    uint64_t key = table->_owners[owner].serial;
    table->_tickets[at] = (JSOwnerTokenCell){serial, key};
    table->_owners[owner].tickets++;
    table->_tickets_used++;
    *out = (JSOwnerTicket){{table->_incarnation, key}, serial};
}
static JSOwnerStatus issue_ticket(JSOwnerTickets *table, size_t owner, JSOwnerTicket *out)
{
    size_t at = free_token(table->_tickets, table->_limits.tickets);
    if (at == table->_limits.tickets) return JS_OWNER_CAPACITY;
    if (table->_last_token == UINT64_MAX) return JS_OWNER_EXHAUSTED;
    put_ticket(table, owner, at, out);
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_ticket_create(JSOwnerTickets *table, uint64_t incarnation, JSOwnerTicket *out)
{
    if (!out) return JS_OWNER_INVALID;
    JSOwnerStatus status = live(table, incarnation);
    if (status != JS_OWNER_OK) return status;
    size_t owner = table->_owners_used;
    size_t at = free_token(table->_tickets, table->_limits.tickets);
    if (owner == table->_limits.owners || at == table->_limits.tickets) return JS_OWNER_CAPACITY;
    if (table->_last_owner == UINT64_MAX || table->_last_token == UINT64_MAX) return JS_OWNER_EXHAUSTED;
    table->_owners[owner] = (JSOwnerCell){.serial=++table->_last_owner};
    table->_owners_used++;
    put_ticket(table, owner, at, out);
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_ticket_duplicate(JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerTicket ticket, JSOwnerTicket *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, ticket.owner, ticket.serial, false, &owner, &at);
    return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;
}
JSOwnerStatus js_owner_reserve(JSOwnerTickets *table, uint64_t incarnation, JSOwnerId *out)
{
    if (!out) return JS_OWNER_INVALID;
    JSOwnerStatus status = live(table, incarnation);
    if (status != JS_OWNER_OK) return status;
    size_t at = table->_owners_used;
    if (at == table->_limits.owners) return JS_OWNER_CAPACITY;
    if (table->_last_owner == UINT64_MAX) return JS_OWNER_EXHAUSTED;
    uint64_t serial = ++table->_last_owner;
    table->_owners[at] = (JSOwnerCell){.serial=serial};
    table->_owners_used++;
    *out = (JSOwnerId){incarnation, serial};
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_ticket_open(JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerId key, JSOwnerTicket *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner;
    JSOwnerStatus status = owner_at(table, incarnation, key, &owner);
    return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;
}
JSOwnerStatus js_owner_ticket_resolve(const JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerTicket ticket, JSOwnerId *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, ticket.owner, ticket.serial, false, &owner, &at);
    if (status == JS_OWNER_OK) *out = ticket.owner;
    return status;
}
JSOwnerStatus js_owner_ticket_close(JSOwnerTickets *table, uint64_t incarnation, JSOwnerTicket ticket)
{
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, ticket.owner, ticket.serial, false, &owner, &at);
    if (status != JS_OWNER_OK) return status;
    table->_tickets[at] = (JSOwnerTokenCell){0};
    table->_owners[owner].tickets--;
    table->_tickets_used--;
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_loan_begin(JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerTicket ticket, JSOwnerLoan *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, ticket.owner, ticket.serial, false, &owner, &at);
    if (status != JS_OWNER_OK) return status;
    at = free_token(table->_loans, table->_limits.loans);
    if (at == table->_limits.loans) return JS_OWNER_CAPACITY;
    if (table->_last_token == UINT64_MAX) return JS_OWNER_EXHAUSTED;
    uint64_t serial = ++table->_last_token;
    table->_loans[at] = (JSOwnerTokenCell){serial, ticket.owner.serial};
    table->_owners[owner].loans++;
    table->_loans_used++;
    *out = (JSOwnerLoan){ticket.owner, serial};
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_loan_owner(const JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerLoan loan, JSOwnerId *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, loan.owner, loan.serial, true, &owner, &at);
    if (status == JS_OWNER_OK) *out = loan.owner;
    return status;
}
JSOwnerStatus js_owner_loan_promote(JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerLoan loan, JSOwnerTicket *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, loan.owner, loan.serial, true, &owner, &at);
    return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;
}
JSOwnerStatus js_owner_loan_end(JSOwnerTickets *table, uint64_t incarnation, JSOwnerLoan loan)
{
    size_t owner, at;
    JSOwnerStatus status = token_at(table, incarnation, loan.owner, loan.serial, true, &owner, &at);
    if (status != JS_OWNER_OK) return status;
    table->_loans[at] = (JSOwnerTokenCell){0};
    table->_owners[owner].loans--;
    table->_loans_used--;
    return JS_OWNER_OK;
}
JSOwnerStatus js_owner_counts(const JSOwnerTickets *table, uint64_t incarnation,
    JSOwnerId key, JSOwnerCounts *out)
{
    if (!out) return JS_OWNER_INVALID;
    size_t owner;
    JSOwnerStatus status = owner_at(table, incarnation, key, &owner);
    if (status == JS_OWNER_OK)
        *out = (JSOwnerCounts){table->_owners[owner].tickets, table->_owners[owner].loans};
    return status;
}
JSOwnerStatus js_owner_summary(const JSOwnerTickets *table, uint64_t incarnation, JSOwnerSummary *out)
{
    if (!out) return JS_OWNER_INVALID;
    JSOwnerStatus status = live(table, incarnation);
    if (status == JS_OWNER_OK)
        *out = (JSOwnerSummary){table->_owners_used, table->_tickets_used, table->_loans_used,
            table->_unknown_escape};
    return status;
}
JSOwnerStatus js_owner_note_unknown_escape(JSOwnerTickets *table, uint64_t incarnation)
{
    JSOwnerStatus status = live(table, incarnation);
    if (status == JS_OWNER_OK) table->_unknown_escape = true;
    return status;
}
