// trip.c — a navigation's fetch, run on a worker (trip.h).

#include "trip.h"
#include "os64/gui.h"
#include "os64/mem.h"
#include "os64/str.h"

// What the leg's face needs, alive for the run: the mailbox, and the pool's
// own cancellation predicate — the only one, so that Stop, a new
// navigation, the close box and the pool's own failures all reach a fetch,
// a question and a post alike.
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

static void ring(const yonder_trip_t *trip)
{
    (void)os64_gui_event_ring(trip->window, trip->mail_bell);
}

static void run_progress(void *ctx, const char *sentence)
{
    Run *r = ctx;
    yonder_mail_progress(r->trip->mail, sentence);
    ring(r->trip);
}

// Asked mid-fetch, on this worker, by libfetch's downgrade hop: the window
// asks the person and the answer comes back down the mailbox's pipe.
static bool run_confirm(void *ctx, const char *question, bool security, const char *refused)
{
    (void)security;             // every question here is answered by a person, freshly
    (void)refused;              // the fetch's own reason says why it stopped
    Run *r = ctx;
    uint32_t number = yonder_mail_ask(r->trip->mail, question);
    ring(r->trip);
    return yonder_mail_wait(r->trip->mail, number, r->cancelled, r->ctx);
}

int64_t yonder_trip_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    (void)out;                  // no product: the verdict rides the mailbox
    yonder_trip_t *trip = job;
    Run r = {trip, cancelled, ctx};
    // Not way_leg, which reads the session's agent: Settings may be writing
    // it on the window's thread while this runs.
    way_leg_t leg = way_leg_as(trip->session, trip->agent);
    os64_strcopy(leg.referrer, sizeof(leg.referrer), trip->referrer);
    leg.face = (way_face_t){&r, run_confirm, run_cancelled, run_progress};

    yonder_verdict_t verdict;
    os64_memset(&verdict, 0, sizeof(verdict));
    way_opening_t o;
    if (!way_open(&leg, trip->url, trip->has_request ? &trip->request : NULL, &o, &verdict.fetch)) {
        // No page: libway's sentence says what there was instead.
        os64_strcopy(verdict.reason, sizeof(verdict.reason), leg.status);
        yonder_mail_post_verdict(trip->mail, &verdict);
        ring(trip);
        return 0;
    }
    yonder_mail_post_head(trip->mail, &o.head);
    ring(trip);
    // The body, a chunk at a time. A post that cannot find room waits, and
    // while it waits nothing is read from the socket: a window that parses
    // slower than the wire delivers slows the wire.
    uint8_t buf[YONDER_STREAM_CHUNK];
    for (;;) {
        int64_t n = way_read(&leg, &o, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (!yonder_mail_post(trip->mail, buf, (size_t)n, cancelled, ctx)) {
            os64_fetch_close(o.fetch);
            return -1;
        }
        ring(trip);
    }
    verdict.page = true;
    verdict.fetch = os64_fetch_status(o.fetch);
    os64_strcopy(verdict.reason, sizeof(verdict.reason), os64_fetch_reason(o.fetch));
    os64_fetch_close(o.fetch);
    yonder_mail_post_verdict(trip->mail, &verdict);
    ring(trip);
    return 1;
}

void yonder_trip_release(void *job, void *product)
{
    (void)product;
    yonder_trip_t *trip = job;
    if (trip != NULL) {
        os64_page_request_free(&trip->request);
        yonder_mail_drop(trip->mail);
        os64_free(trip);
    }
}
