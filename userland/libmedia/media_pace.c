// media_pace.c — which picture to show, and what to do when late
// (os64/media_pace.h).

#include "os64/media_pace.h"

void os64_media_pace_init(os64_media_pace_t *p, uint32_t depth, int64_t period)
{
    *p = (os64_media_pace_t){0};
    p->depth = depth;
    p->period = period > 0 ? period : 0;
    p->in_since = p->out_since = -1;
    p->state = OS64_MEDIA_PACE_ON_TIME;
    p->sound_armed = true;
}

void os64_media_pace_period(os64_media_pace_t *p, int64_t period)
{
    p->period = period > 0 ? period : 0;
}

// Into `state` at `now`, its entry counted; both holds start again.
static void enter(os64_media_pace_t *p, int state, int64_t now)
{
    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND && now > p->far_since)
        p->far_behind_us += (uint64_t)(now - p->far_since);
    p->state = state;
    p->entered[state]++;
    p->in_since = p->out_since = -1;
    p->skip_unused = state != OS64_MEDIA_PACE_ON_TIME;
    if (state == OS64_MEDIA_PACE_FAR_BEHIND) {
        p->far_since = now;
        p->resync_asked = true;
        p->resyncs++;
    }
}

void os64_media_pace_reset(os64_media_pace_t *p, int64_t now)
{
    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND && now > p->far_since)
        p->far_behind_us += (uint64_t)(now - p->far_since);
    p->state = OS64_MEDIA_PACE_ON_TIME;
    p->skip_unused = false;
    p->resync_asked = false;
    p->sound_armed = true;
    p->in_since = p->out_since = -1;
}

bool os64_media_pace_resync_take(os64_media_pace_t *p)
{
    bool asked = p->resync_asked;
    p->resync_asked = false;
    return asked;
}

os64_media_pace_pick_t os64_media_pace_pick(os64_media_pace_t *p, const os64_media_pictures_t *ready, int64_t clock,
                                            const os64_media_pictures_entry_t *shown, bool judging)
{
    os64_media_pace_pick_t pick = {0};
    uint32_t due = 0;
    const os64_media_pictures_entry_t *e;
    while ((e = os64_media_pictures_peek(ready, due)) != 0 && e->at <= clock)
        due++;
    if (due > 0) {
        pick.show = true;
        pick.drop = due - 1;
        p->shown++;
        p->dropped += pick.drop;
    } else if (judging && os64_media_pictures_count(ready) == 0 && shown != 0) {
        int64_t lasts = shown->duration > 0 ? shown->duration : p->period;
        if (lasts > 0 && clock >= shown->at + lasts) {
            pick.starved = true;
            p->starved++;
        }
    }
    return pick;
}

int64_t os64_media_pace_reach(int64_t now, int64_t cost, int64_t blank, int64_t period_us)
{
    int64_t target = now + (cost > 0 ? cost : 0);
    if (period_us <= 0)
        return target;
    // The least k with blank + k * period_us >= target, k of either sign.
    int64_t gap = target - blank;
    int64_t k = gap / period_us;
    if (k * period_us < gap)
        k++;
    return blank + k * period_us;
}

bool os64_media_pace_skip(os64_media_pace_t *p, const os64_media_pictures_t *decoded, int64_t clock)
{
    const os64_media_pictures_entry_t *next = os64_media_pictures_peek(decoded, 1);
    if (next == 0 || next->at > clock)
        return false;
    p->skipped++;
    return true;
}

int os64_media_pace_judge(os64_media_pace_t *p, const os64_media_pace_measure_t *m)
{
    if (!m->judging) {
        p->in_since = p->out_since = -1;
        return p->state;
    }
    // The sound's door re-arms once the sound is back at its floor: a
    // shortfall a resync did not cure is not the picture decoder's, and
    // another resync would only cost the pictures again.
    bool short_of_sound = m->sound_floor > 0 && m->sound_frames < m->sound_floor;
    if (!short_of_sound)
        p->sound_armed = true;
    bool lead_known = p->period > 0 && m->newest >= 0;
    if (lead_known)
        p->lead = m->newest - m->clock;
    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND)
        return p->state;
    bool by_sound = short_of_sound && p->sound_armed;
    if (by_sound || (lead_known && p->lead < -(int64_t)OS64_MEDIA_PACE_FAR_PERIODS * p->period)) {
        if (by_sound) {
            p->far_by_sound++;
            p->sound_armed = false;
        }
        enter(p, OS64_MEDIA_PACE_FAR_BEHIND, m->now);
        return p->state;
    }
    if (!lead_known) {
        p->in_since = p->out_since = -1;
        return p->state;
    }
    int64_t lead = p->lead;
    int64_t depth = (int64_t)p->depth * p->period;   // the queue's depth in media time
    if (p->state == OS64_MEDIA_PACE_ON_TIME) {
        if (lead * 8 < OS64_MEDIA_PACE_IN_EIGHTHS * depth) {
            if (p->in_since < 0)
                p->in_since = m->now;
            if (m->now - p->in_since >= OS64_MEDIA_PACE_IN_HOLD_US)
                enter(p, OS64_MEDIA_PACE_DECODING_LATE, m->now);
        } else {
            p->in_since = -1;
        }
    } else {
        if (lead * 8 >= OS64_MEDIA_PACE_OUT_EIGHTHS * depth) {
            if (p->out_since < 0)
                p->out_since = m->now;
            if (m->now - p->out_since >= OS64_MEDIA_PACE_OUT_HOLD_US)
                enter(p, OS64_MEDIA_PACE_ON_TIME, m->now);
        } else {
            p->out_since = -1;
        }
    }
    return p->state;
}

void os64_media_pace_resynced(os64_media_pace_t *p, int64_t now)
{
    if (p->state == OS64_MEDIA_PACE_FAR_BEHIND)
        enter(p, OS64_MEDIA_PACE_DECODING_LATE, now);
}
