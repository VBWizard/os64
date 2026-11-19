// media_clock.c — what time it is in a playing media (os64/media_clock.h).
//
// UNITS. Inside, a media time is counted in 2500ths of an output frame,
// because then one microsecond of real time is exactly 120 units and the
// slew's twentieth of it exactly 6: every reading is exact integer
// arithmetic, and a reading's rise over one microsecond is between 114 and
// 126 units, never less. With whole frames the rounding of the run and of the
// slew could disagree and a reading could fall back by one frame. Readings
// leave as whole frames, rounded down.
//
// A reading is a function of `t` and of the state set by the last event:
//   PAUSED, STARVED   base
//   RUNNING, AWAITING base + run(t - since)
//   STARTING          base until hold_until, then base + run(t - hold_until)
//   FOLLOWING         heard + run(f - heard_wall) + drift worked toward 0
//                     since `slew_since`: by 6 units a microsecond down to
//                     10 ms, then by one unit every 4 microseconds; `f` is
//                     `t` held at heard_wall + still_us (the ear stopped),
//                     and before `slew_since` the reading is `base` (it was
//                     ahead of the ear by more than the bound)
// Every event first takes the reading at its `now`, so the next state
// carries on from it: that is what keeps a reading from stepping. A seek
// goes where it is told instead, and a report finding the reading more than
// the bound behind the ear steps it forward to the bound.

#include "os64/media_clock.h"

enum { MODE_PAUSED, MODE_RUNNING, MODE_STARTING, MODE_AWAITING, MODE_FOLLOWING, MODE_STARVED };

#define UNITS_PER_FRAME 2500                  // a frame
#define UNITS_PER_US 120                      // 48000 frames a second, in units a microsecond
#define SLEW_UNITS_PER_US (UNITS_PER_US / 20) // a twentieth of real time
// Drift within NEAR of nothing is worked off at a 480th (a unit every 4 us).
// A sink's reports are stamped when it pulls, and it pulls at its own pass,
// up to a tick late: drift that small is the reports' scatter, and working it
// off at a twentieth swung the reading a twentieth fast and slow from one
// report to the next, enough to show two pictures' worth of time in one
// refresh, so a picture went unshown (the P5 dropped a hundred a minute
// following the headphones, six muted). Slowly, the scatter averages out.
#define NEAR_UNITS (480 * UNITS_PER_FRAME)    // 10 ms
#define NEAR_US_PER_UNIT 4
#define BOUND_UNITS ((int64_t)OS64_MEDIA_CLOCK_BOUND_US * UNITS_PER_US)

// Real time `us` long, as media time. The playback rate is this one place.
static int64_t run(int64_t us)
{
    return us * UNITS_PER_US;
}

// Drift of size `a` (> 0) after `us` of being worked toward 0: at the slew
// down to NEAR, then at the near rate. The fast part lasts the whole
// microseconds that leave at least NEAR, so the size never rises.
static int64_t worked_size(int64_t a, int64_t us)
{
    if (a - us * SLEW_UNITS_PER_US >= NEAR_UNITS)
        return a - us * SLEW_UNITS_PER_US;
    int64_t fast_us = a > NEAR_UNITS ? (a - NEAR_UNITS) / SLEW_UNITS_PER_US : 0;
    int64_t left = a - fast_us * SLEW_UNITS_PER_US - (us - fast_us) / NEAR_US_PER_UNIT;
    return left > 0 ? left : 0;
}

// Drift `d` after `us` of being worked toward 0.
static int64_t slewed(int64_t d, int64_t us)
{
    if (d > 0)
        return worked_size(d, us);
    return d < 0 ? -worked_size(-d, us) : 0;
}

// Units to frames, rounded down (a reading may be below 0: a report of a
// sound that began before the media's zero).
static int64_t frames(int64_t units)
{
    int64_t q = units / UNITS_PER_FRAME;
    return units % UNITS_PER_FRAME < 0 ? q - 1 : q;
}

static int64_t at_or_after(const os64_media_clock_t *c, int64_t t)
{
    return t < c->since ? c->since : t;
}

// What the reports alone say at `t`.
static int64_t heard_view(const os64_media_clock_t *c, int64_t t)
{
    return c->heard + run(t - c->heard_wall);
}

// FOLLOWING: the time the reading is taken at. The ear's last report is
// trusted for still_us past the moment it describes; after that the ear is
// taken as stopped, and the reading holds where that left it, drift and
// all, until a report says the ear moved. (Never before the last event.)
static int64_t follow_t(const os64_media_clock_t *c, int64_t t)
{
    int64_t until = c->heard_wall + c->still_us;
    if (t > until)
        t = until;
    return at_or_after(c, t);
}

static bool ear_stopped(const os64_media_clock_t *c, int64_t t)
{
    return t > c->heard_wall + c->still_us;
}

static int64_t reading(const os64_media_clock_t *c, int64_t t)
{
    t = at_or_after(c, t);
    switch (c->mode) {
    case MODE_RUNNING:
    case MODE_AWAITING:
        return c->base + run(t - c->since);
    case MODE_STARTING:
        return t < c->hold_until ? c->base : c->base + run(t - c->hold_until);
    case MODE_FOLLOWING: {
        int64_t f = follow_t(c, t);
        // Ahead of the ear by more than the bound at the last report: held
        // at `base` while the ear closes the gap (BOUND, in the header).
        if (f < c->slew_since)
            return c->base;
        return heard_view(c, f) + slewed(c->drift, f - c->slew_since);
    }
    default:
        return c->base;
    }
}

// Leaving STARTING: counted when its hold had already run out.
static void leave_start(os64_media_clock_t *c, int64_t now)
{
    if (c->mode == MODE_STARTING && now >= c->hold_until)
        c->starts_late++;
}

// Running from `value` at `now`: a hold first on the heard source.
static void start(os64_media_clock_t *c, int64_t value, int64_t now)
{
    c->base = value;
    c->since = now;
    if (c->source == OS64_MEDIA_CLOCK_HEARD) {
        c->mode = MODE_STARTING;
        c->hold_until = now + c->hold_us;
    } else {
        c->mode = MODE_RUNNING;
    }
}

void os64_media_clock_init(os64_media_clock_t *c, int source)
{
    *c = (os64_media_clock_t){0};
    c->mode = MODE_PAUSED;
    c->source = source == OS64_MEDIA_CLOCK_HEARD ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC;
    c->hold_us = OS64_MEDIA_CLOCK_START_HOLD_US;
    c->still_us = OS64_MEDIA_CLOCK_STILL_US;
}

void os64_media_clock_hold(os64_media_clock_t *c, int64_t us)
{
    c->hold_us = us > 0 ? us : 1;
}

void os64_media_clock_still(os64_media_clock_t *c, int64_t us)
{
    c->still_us = us > 0 ? us : 1;
}

// How old the last report had grown when the next replaced it, by bucket.
static void count_age(os64_media_clock_t *c, int64_t age)
{
    static const int64_t edges[OS64_MEDIA_CLOCK_AGE_BUCKETS - 1] = {20000, 40000, 60000, 80000, 120000};
    int i = 0;
    while (i < OS64_MEDIA_CLOCK_AGE_BUCKETS - 1 && age > edges[i])
        i++;
    c->report_ages[i]++;
}

void os64_media_clock_play(os64_media_clock_t *c, int64_t now)
{
    if (c->mode != MODE_PAUSED)
        return;
    start(c, c->base, at_or_after(c, now));
}

void os64_media_clock_pause(os64_media_clock_t *c, int64_t now)
{
    if (c->mode == MODE_PAUSED)
        return;
    now = at_or_after(c, now);
    leave_start(c, now);
    c->base = reading(c, now);
    c->since = now;
    c->mode = MODE_PAUSED;
}

uint32_t os64_media_clock_seek(os64_media_clock_t *c, int64_t frame, int64_t now)
{
    now = at_or_after(c, now);
    leave_start(c, now);
    int64_t value = frame * UNITS_PER_FRAME;
    if (c->mode == MODE_PAUSED) {
        c->base = value;
        c->since = now;
    } else {
        start(c, value, now);
    }
    return ++c->generation;
}

void os64_media_clock_source(os64_media_clock_t *c, int source, int64_t now)
{
    source = source == OS64_MEDIA_CLOCK_HEARD ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC;
    if (source == c->source)
        return;
    c->source = source;
    if (c->mode == MODE_PAUSED)
        return;
    now = at_or_after(c, now);
    leave_start(c, now);
    c->base = reading(c, now);
    c->since = now;
    c->mode = source == OS64_MEDIA_CLOCK_HEARD ? MODE_AWAITING : MODE_RUNNING;
}

bool os64_media_clock_heard(os64_media_clock_t *c, uint32_t generation, int64_t frame, int64_t frame_wall,
                            bool advancing, int64_t now)
{
    if (generation != c->generation) {
        c->reports_stale++;
        return false;
    }
    if (c->mode == MODE_PAUSED || c->mode == MODE_RUNNING)
        return false;
    bool waiting = c->mode == MODE_STARTING || c->mode == MODE_AWAITING;
    // A wait ends on a report of sound heard since it began: one measured
    // before a pause and delivered after the play would end a start's hold
    // before the device has made anything heard.
    if (waiting && (!advancing || frame_wall < c->since))
        return false;
    now = at_or_after(c, now);
    c->reports++;
    if (!advancing) {
        if (c->mode == MODE_FOLLOWING) {
            c->base = reading(c, now);
            c->since = now;
            c->mode = MODE_STARVED;
            c->starves++;
        }
        return true;
    }
    if (c->mode == MODE_STARTING)
        c->start_wait_us = now - c->since;
    bool holding = c->mode == MODE_FOLLOWING && now < c->slew_since;
    if (c->mode == MODE_FOLLOWING) {
        count_age(c, now - c->heard_wall);
        if (ear_stopped(c, now))
            c->stills++;
    }
    leave_start(c, now);
    int64_t was = reading(c, now);
    c->heard = frame * UNITS_PER_FRAME;
    c->heard_wall = frame_wall;
    int64_t d = was - heard_view(c, now);
    c->base = was;
    c->since = now;
    c->slew_since = now;
    if (d > BOUND_UNITS) {
        // Ahead by more than the bound: the reading holds until the ear is
        // within it (the ear's view rises a unit of drift per unit of real
        // time while the reading stands), then the slew takes the rest.
        c->slew_since = now + (d - BOUND_UNITS + UNITS_PER_US - 1) / UNITS_PER_US;
        d = BOUND_UNITS;
        if (!holding)
            c->held_ahead++;          // a hold, counted once however many reports it spans
    } else if (d < -BOUND_UNITS) {
        // Behind by more: a step forward to the bound (the pace drops what
        // it passes); the slew takes the rest.
        d = -BOUND_UNITS;
        c->jumped_behind++;
    }
    c->drift = d;
    c->mode = MODE_FOLLOWING;
    return true;
}

int64_t os64_media_clock_read(const os64_media_clock_t *c, int64_t t)
{
    return frames(reading(c, t));
}

uint32_t os64_media_clock_generation(const os64_media_clock_t *c)
{
    return c->generation;
}

int os64_media_clock_state(const os64_media_clock_t *c, int64_t t)
{
    switch (c->mode) {
    case MODE_RUNNING:
        return OS64_MEDIA_CLOCK_RUNNING;
    case MODE_STARTING:
        return at_or_after(c, t) < c->hold_until ? OS64_MEDIA_CLOCK_STARTING : OS64_MEDIA_CLOCK_STARTING_LATE;
    case MODE_AWAITING:
        return OS64_MEDIA_CLOCK_AWAITING;
    case MODE_FOLLOWING:
        return ear_stopped(c, at_or_after(c, t)) ? OS64_MEDIA_CLOCK_STARVED : OS64_MEDIA_CLOCK_FOLLOWING;
    case MODE_STARVED:
        return OS64_MEDIA_CLOCK_STARVED;
    default:
        return OS64_MEDIA_CLOCK_PAUSED;
    }
}

int64_t os64_media_clock_drift(const os64_media_clock_t *c, int64_t t)
{
    if (c->mode != MODE_FOLLOWING)
        return 0;
    t = at_or_after(c, t);
    return frames(reading(c, t) - heard_view(c, follow_t(c, t)));
}
