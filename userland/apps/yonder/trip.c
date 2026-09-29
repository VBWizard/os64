// trip.c — a navigation, run on a worker (trip.h).

#include "trip.h"
#include "os64/gui.h"
#include "os64/mem.h"
#include "os64/str.h"

// What the leg's face needs, alive for the run: the mailbox, and the pool's
// own cancellation predicate — the only one, so that Stop, a new
// navigation, the close box and the pool's own failures all reach a fetch
// and a question alike.
typedef struct {
    yonder_trip_t *trip;
    bool (*cancelled)(void *ctx);
    void *ctx;
} Run;

static bool run_cancelled(void *ctx)
{
    Run *r = ctx;
    return r->cancelled(r->ctx);
}

static void run_progress(void *ctx, const char *sentence)
{
    Run *r = ctx;
    yonder_mail_progress(r->trip->mail, sentence);
    (void)os64_gui_event_ring(r->trip->window, r->trip->mail_bell);
}

// Asked mid-fetch, on this worker, by libfetch's downgrade hop: the window
// asks the person and the answer comes back down the mailbox's pipe.
static bool run_confirm(void *ctx, const char *question, bool security, const char *refused)
{
    (void)security;             // every question here is answered by a person, freshly
    (void)refused;              // the fetch's own reason says why it stopped
    Run *r = ctx;
    uint32_t number = yonder_mail_ask(r->trip->mail, question);
    (void)os64_gui_event_ring(r->trip->window, r->trip->mail_bell);
    return yonder_mail_wait(r->trip->mail, number, r->cancelled, r->ctx);
}

int64_t yonder_trip_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_trip_t *trip = job;
    yonder_arrival_t *a = os64_calloc(1, sizeof(*a));
    if (a == NULL)
        return -1;
    Run r = {trip, cancelled, ctx};
    way_leg_t leg = way_leg(trip->session);
    leg.face = (way_face_t){&r, run_confirm, run_cancelled, run_progress};
    a->loaded = way_load(&leg, trip->url, trip->has_request ? &trip->request : NULL, &a->page,
                         &a->why);
    os64_strcopy(a->status, sizeof(a->status), leg.status);
    *out = a;
    return a->loaded ? 1 : 0;
}

void yonder_trip_release(void *job, void *product)
{
    yonder_arrival_t *a = product;
    if (a != NULL) {
        way_page_clear(&a->page);
        os64_free(a);
    }
    yonder_trip_t *trip = job;
    if (trip != NULL) {
        os64_page_request_free(&trip->request);
        yonder_mail_drop(trip->mail);
        os64_free(trip);
    }
}
