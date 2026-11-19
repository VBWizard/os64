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
//   FOLLOWING         heard + run(t - heard_wall) + drift worked toward 0
//                     by 6 units for every microsecond since `since`
// Every event but a seek first takes the reading at its `now`, so the next
// state carries on from it: that is what keeps a reading from stepping.

#include "os64/media_clock.h"

enum { MODE_PAUSED, MODE_RUNNING, MODE_STARTING, MODE_AWAITING, MODE_FOLLOWING, MODE_STARVED };

#define UNITS_PER_FRAME 2500                  // a frame
#define UNITS_PER_US 120                      // 48000 frames a second, in units a microsecond
#define SLEW_UNITS_PER_US (UNITS_PER_US / 20) // a twentieth of real time

// Real time `us` long, as media time. The playback rate is this one place.
static int64_t run(int64_t us)
{
    return us * UNITS_PER_US;
}

// Drift `d` after `us` of being worked toward 0.
static int64_t slewed(int64_t d, int64_t us)
{
    int64_t worked = us * SLEW_UNITS_PER_US;
    if (d > 0)
        return d > worked ? d - worked : 0;
    return -d > worked ? d + worked : 0;
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

static int64_t reading(const os64_media_clock_t *c, int64_t t)
{
    t = at_or_after(c, t);
    switch (c->mode) {
    case MODE_RUNNING:
    case MODE_AWAITING:
        return c->base + run(t - c->since);
    case MODE_STARTING:
        return t < c->hold_until ? c->base : c->base + run(t - c->hold_until);
    case MODE_FOLLOWING:
        return heard_view(c, t) + slewed(c->drift, t - c->since);
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
}

void os64_media_clock_hold(os64_media_clock_t *c, int64_t us)
{
    c->hold_us = us > 0 ? us : 1;
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
    leave_start(c, now);
    int64_t was = reading(c, now);
    c->heard = frame * UNITS_PER_FRAME;
    c->heard_wall = frame_wall;
    c->drift = was - heard_view(c, now);
    c->since = now;
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
        return OS64_MEDIA_CLOCK_FOLLOWING;
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
    return frames(slewed(c->drift, at_or_after(c, t) - c->since));
}
