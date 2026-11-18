// frame_plan.c — when yonder's animation frames are asked for and what time
// each one is (frame_plan.h).

#include "frame_plan.h"

static uint64_t later(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static uint64_t sum(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX - 1 : a + b;
}

// The display's period by the last answer, or the guess before any.
static uint64_t period_of(const frame_plan_t *p)
{
    return p->number != 0 && p->period_us != 0 ? p->period_us : FRAME_PLAN_PERIOD_GUESS_US;
}

// The first blank at or after `at_us`, on the last answer's grid (or a
// guessed one from `now_us` before any).
static uint64_t blank_at_or_after(const frame_plan_t *p, uint64_t at_us, uint64_t now_us)
{
    uint64_t period = period_of(p), base = p->number != 0 && p->period_us != 0 ? p->blank_us : now_us;
    if (at_us <= base)
        return base;
    uint64_t periods = (at_us - base + period - 1) / period;
    return periods > (UINT64_MAX - base) / period ? UINT64_MAX - 1 : base + periods * period;
}

frame_plan_step_t frame_plan_step(frame_plan_t *p, uint64_t due_ms, uint64_t now_ms)
{
    frame_plan_step_t step = {false, FRAME_PLAN_NEVER};
    bool closed_now = p->closed_now;
    p->closed_now = false;
    if (due_ms == FRAME_PLAN_NEVER)
        return step;
    uint64_t now_us = now_ms > UINT64_MAX / 1000 ? UINT64_MAX - 1 : now_ms * 1000;
    uint64_t due_us = due_ms > UINT64_MAX / 1000 ? UINT64_MAX - 1 : due_ms * 1000;
    // A frame asked for now is answered at the next blank, so work due by
    // then asks now; work due later wakes the loop a period before the
    // blank that will carry it, in time to ask for that one.
    if (due_us > frame_plan_next_blank_us(p, now_us)) {
        uint64_t wake_us = blank_at_or_after(p, due_us, now_us) - period_of(p);
        step.wake_ms = (wake_us + 999) / 1000;
        return step;
    }
    if (p->asked || p->open)
        return step;
    step.ask = true;
    p->asked = true;
    p->chained = closed_now;
    p->asks++;
    return step;
}

void frame_plan_refused(frame_plan_t *p)
{
    if (!p->asked)
        return;
    p->asked = false;
    p->chained = false;
    p->asks--;
    p->refused++;
}

bool frame_plan_answer(frame_plan_t *p, uint64_t number, uint64_t blank_us, uint32_t period_ns,
                       uint64_t *time_us)
{
    if (!p->asked)
        return false;
    // Refreshes went by unanswered only if yonder asked as its last frame
    // closed: a request made later was never for the refreshes before it.
    // (A chained request follows an answer, so `p->number` is that one's.)
    if (p->chained && number > p->number + 1)
        p->skipped += number - p->number - 1;
    p->asked = false;
    p->chained = false;
    p->open = true;
    p->answers++;
    p->number = number;
    p->blank_us = blank_us;
    if (period_ns != 0)
        p->period_us = ((uint64_t)period_ns + 500) / 1000;
    p->time_us = p->sampled_us = later(blank_us, p->sampled_us);
    *time_us = p->time_us;
    return true;
}

uint64_t frame_plan_sample(frame_plan_t *p, uint64_t now_us)
{
    if (p->open)
        return p->time_us;
    p->sampled_us = later(now_us, p->sampled_us);
    return p->sampled_us;
}

void frame_plan_close(frame_plan_t *p)
{
    if (!p->open)
        return;
    p->open = false;
    p->closed_now = true;
}

uint64_t frame_plan_next_blank_us(const frame_plan_t *p, uint64_t now_us)
{
    if (p->number == 0 || p->period_us == 0)
        return sum(now_us, FRAME_PLAN_PERIOD_GUESS_US);
    if (now_us < p->blank_us)
        return p->blank_us;
    uint64_t periods = (now_us - p->blank_us) / p->period_us + 1;
    if (periods > (UINT64_MAX - p->blank_us) / p->period_us)
        return UINT64_MAX - 1;
    return p->blank_us + periods * p->period_us;
}

uint64_t frame_plan_half_due(uint64_t due_ms, bool had, uint64_t ended_ms, uint64_t cost_us, uint32_t rest,
                             uint64_t stop_us)
{
    if (due_ms == FRAME_PLAN_NEVER || cost_us > stop_us)
        return FRAME_PLAN_NEVER;
    if (!had)
        return due_ms;
    uint64_t cost_ms = cost_us / 1000 + (cost_us % 1000 != 0);
    uint64_t rested = cost_ms > (UINT64_MAX - 1) / (rest != 0 ? rest : 1) ? UINT64_MAX - 1
                                                                          : sum(ended_ms, cost_ms * rest);
    return later(due_ms, rested);
}
