// yonder's animation frames (userland/apps/yonder/frame_plan.c) on the host
// under ASan and UBSan: each rule by a case of its own, then a long random
// walk of looks, answers, refusals, samples and closes held to the
// invariants — at most one request out, none while a frame is open, the
// time handed to animations never earlier than the last, skips counted only
// for refreshes yonder was asking for.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame_plan.h"

static int s_checks, s_failed;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

static void look_cases(void)
{
    frame_plan_t p = {0};
    frame_plan_step_t s = frame_plan_step(&p, FRAME_PLAN_NEVER, 100);
    check(!s.ask && s.wake_ms == FRAME_PLAN_NEVER && p.asks == 0, "look: nothing due asks nothing, wakes nothing");
    s = frame_plan_step(&p, 150, 100);
    check(!s.ask && s.wake_ms == 134 && !p.asked,
          "look: work due past the next blank wakes the loop a period before the blank that carries it");
    s = frame_plan_step(&p, 110, 100);
    check(s.ask && p.asked, "look: work due by the next blank asks now");
    frame_plan_refused(&p);
    p.refused = 0;
    s = frame_plan_step(&p, 100, 100);
    check(s.ask && s.wake_ms == FRAME_PLAN_NEVER && p.asked && p.asks == 1, "look: work due now asks");
    s = frame_plan_step(&p, 90, 101);
    check(!s.ask && p.asks == 1, "look: no second ask while one is out");
    uint64_t t = 0;
    check(frame_plan_answer(&p, 7, 100500, 16666667, &t) && t == 100500 && p.open && !p.asked,
          "answer: opens at its blank");
    s = frame_plan_step(&p, 50, 102);
    check(!s.ask && p.asks == 1, "look: no ask while a frame is open");
    s = frame_plan_step(&p, 200, 102);
    check(!s.ask && s.wake_ms == 184,
          "look: an open frame still hands a later time to the ticker, on the answer's grid");
    frame_plan_close(&p);
    check(!p.open && p.closed_now, "close: the frame closes");
    s = frame_plan_step(&p, 102, 103);
    check(s.ask && p.chained && !p.closed_now, "look: the look after a close asks, chained");
    frame_plan_t q = {0};
    frame_plan_close(&q);
    check(!q.closed_now, "close: with no frame open, nothing closes");
    s = frame_plan_step(&q, 0, 1);
    check(s.ask && !q.chained, "look: an ask after a close of nothing is not chained");
}

static void refusal_cases(void)
{
    frame_plan_t p = {0};
    (void)frame_plan_step(&p, 0, 10);
    frame_plan_refused(&p);
    check(!p.asked && p.asks == 0 && p.refused == 1, "refused: none out, not counted as asked");
    frame_plan_refused(&p);
    check(p.refused == 1, "refused: with none out, nothing to undo");
    frame_plan_step_t s = frame_plan_step(&p, 0, 11);
    check(s.ask && p.asks == 1, "refused: the next look asks again");
    uint64_t t = 99;
    frame_plan_t q = {0};
    check(!frame_plan_answer(&q, 3, 1000, 16666667, &t) && !q.open && t == 99,
          "answer: one nothing asked for opens nothing");
}

static void time_cases(void)
{
    frame_plan_t p = {0};
    check(frame_plan_sample(&p, 1000) == 1000, "sample: between frames, now");
    (void)frame_plan_step(&p, 0, 1);
    uint64_t t = 0;
    check(frame_plan_answer(&p, 1, 900, 16666667, &t) && t == 1000,
          "answer: a blank before the last sample takes the sample (never backward)");
    check(frame_plan_sample(&p, 1100) == 1000, "sample: inside a frame, the frame's time");
    frame_plan_close(&p);
    check(frame_plan_sample(&p, 1200) == 1200, "sample: after the frame, now again");
    check(frame_plan_sample(&p, 1150) == 1200, "sample: a clock read earlier than the last is lifted");
    (void)frame_plan_step(&p, 0, 2);
    check(frame_plan_answer(&p, 2, 1180, 16666667, &t) && t == 1200, "answer: lifted to the last sample");
    frame_plan_close(&p);
    (void)frame_plan_step(&p, 0, 3);
    check(frame_plan_answer(&p, 3, 20000, 16666667, &t) && t == 20000, "answer: a later blank is its own time");
    frame_plan_close(&p);
    check(frame_plan_sample(&p, 19000) == 20000, "sample: a clock read before the last frame's time takes that time");
}

static void skip_cases(void)
{
    frame_plan_t p = {0};
    uint64_t t;
    (void)frame_plan_step(&p, 0, 1);
    (void)frame_plan_answer(&p, 10, 1000, 16666667, &t);
    frame_plan_close(&p);
    (void)frame_plan_step(&p, 0, 2);
    (void)frame_plan_answer(&p, 11, 17667, 16666667, &t);
    check(p.skipped == 0, "skips: the next refresh skips none");
    frame_plan_close(&p);
    (void)frame_plan_step(&p, 0, 3);
    (void)frame_plan_answer(&p, 14, 67667, 16666667, &t);
    check(p.skipped == 2, "skips: asked as the frame closed, refreshes between are skipped");
    frame_plan_close(&p);
    (void)frame_plan_step(&p, FRAME_PLAN_NEVER, 4);
    (void)frame_plan_step(&p, 0, 500);
    check(!p.chained, "skips: an ask after an idle look is not chained");
    (void)frame_plan_answer(&p, 40, 500000, 16666667, &t);
    check(p.skipped == 2, "skips: refreshes nobody asked for are not skipped");
    frame_plan_t q = {0};
    (void)frame_plan_step(&q, 0, 1);
    (void)frame_plan_answer(&q, 50, 1000, 16666667, &t);
    check(q.skipped == 0, "skips: the first answer has nothing to compare with");
}

static void blank_cases(void)
{
    frame_plan_t p = {0};
    check(frame_plan_next_blank_us(&p, 5000) == 5000 + FRAME_PLAN_PERIOD_GUESS_US,
          "next blank: before any answer, a guessed period away");
    uint64_t t;
    (void)frame_plan_step(&p, 0, 1);
    (void)frame_plan_answer(&p, 1, 1000, 16667000, &t);
    check(p.period_us == 16667, "next blank: the period from the answer");
    check(frame_plan_next_blank_us(&p, 1000) == 17667, "next blank: at a blank, the one after");
    check(frame_plan_next_blank_us(&p, 17666) == 17667, "next blank: just before one, that one");
    check(frame_plan_next_blank_us(&p, 17667) == 34334, "next blank: at the second, the third");
    check(frame_plan_next_blank_us(&p, 500) == 1000, "next blank: before the last answer's, that one");
    (void)frame_plan_step(&p, 0, 2);
    frame_plan_close(&p);
    (void)frame_plan_step(&p, 0, 2);
    (void)frame_plan_answer(&p, 2, 17667, 0, &t);
    check(p.period_us == 16667, "next blank: an answer with no period keeps the last");
    frame_plan_close(&p);
    (void)frame_plan_step(&p, 0, 3);
    (void)frame_plan_answer(&p, 3, 34334, 16666667, &t);
    check(p.period_us == 16667, "next blank: the period rounds to the nearest microsecond");
}

static void half_cases(void)
{
    check(frame_plan_half_due(FRAME_PLAN_NEVER, true, 0, 0, 2, 200000) == FRAME_PLAN_NEVER,
          "half: nothing due is never");
    check(frame_plan_half_due(50, false, 0, 300000, 2, 200000) == FRAME_PLAN_NEVER,
          "half: a cost past the limit stops it, frame or none");
    check(frame_plan_half_due(50, true, 40, UINT64_MAX, 1, 200000) == FRAME_PLAN_NEVER,
          "half: a failed frame is past every limit");
    check(frame_plan_half_due(50, true, 40, 200000, 1, 200000) == 240, "half: at the limit it rests");
    check(frame_plan_half_due(50, false, 40, 9000, 2, 200000) == 50, "half: before a frame, no rest");
    check(frame_plan_half_due(50, true, 100, 1500, 2, 200000) == 104,
          "half: twice the cost, rounded up to the millisecond, after it ended");
    check(frame_plan_half_due(50, true, 100, 1000, 1, 200000) == 101, "half: once the cost for ticks");
    check(frame_plan_half_due(200, true, 100, 1500, 2, 200000) == 200, "half: a due past the rest stands");
    check(frame_plan_half_due(0, true, 100, 0, 2, 200000) == 100, "half: a free frame rests nothing");
    check(frame_plan_half_due(5, true, UINT64_MAX - 3, 9000, 2, 200000) == UINT64_MAX - 1,
          "half: the rest saturates");
}

static uint64_t s_rand = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(uint64_t n)
{
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 7;
    s_rand ^= s_rand << 17;
    return s_rand % n;
}

static void walk_cases(void)
{
    frame_plan_t p = {0};
    uint64_t now_us = 1000000, handed = 0, number = 0, refused = 0, opened_by_answer = 0;
    int ok_one_out = 1, ok_no_ask_open = 1, ok_monotonic = 1, ok_counts = 1, ok_skips = 1, ok_wake = 1;
    for (int i = 0; i < 200000; i++) {
        now_us += rnd(9000);
        uint64_t now_ms = now_us / 1000;
        switch (rnd(6)) {
        case 0:
        case 1: {
            uint64_t due = rnd(4) == 0 ? FRAME_PLAN_NEVER : now_ms + rnd(80) - 20;
            int was_asked = p.asked, was_open = p.open;
            uint64_t next_blank = frame_plan_next_blank_us(&p, now_ms * 1000);
            frame_plan_step_t s = frame_plan_step(&p, due, now_ms);
            if (s.ask && (was_asked || was_open))
                ok_no_ask_open = 0;
            // An ask is for work due by the next blank; a wake comes before
            // the work is due, after now, and the look then would ask.
            if (s.ask && due * 1000 > next_blank)
                ok_wake = 0;
            if (due != FRAME_PLAN_NEVER && due * 1000 > next_blank &&
                (s.wake_ms == FRAME_PLAN_NEVER || s.wake_ms > due || s.wake_ms <= now_ms))
                ok_wake = 0;
            if (s.wake_ms != FRAME_PLAN_NEVER && due * 1000 > next_blank) {
                frame_plan_t later = p;
                later.closed_now = false;
                if (due * 1000 > frame_plan_next_blank_us(&later, s.wake_ms * 1000))
                    ok_wake = 0;
            }
            if (s.ask && rnd(10) == 0) {
                frame_plan_refused(&p);
                refused++;
            }
            break;
        }
        case 2: {
            uint64_t t = 0, skipped = p.skipped;
            int chained = p.chained, asked = p.asked;
            uint64_t step = 1 + rnd(4), was = p.number;
            number += step;
            uint64_t blank = now_us - rnd(5000);
            bool opened = frame_plan_answer(&p, number, blank, 16666667, &t);
            if (opened != asked)
                ok_counts = 0;
            if (opened) {
                opened_by_answer++;
                if (t < handed || t < blank)
                    ok_monotonic = 0;
                handed = t;
                uint64_t want = chained && was != 0 && number > was + 1 ? number - was - 1 : 0;
                if (p.skipped - skipped != want)
                    ok_skips = 0;
            }
            break;
        }
        case 3: {
            uint64_t t = frame_plan_sample(&p, now_us - rnd(3000));
            if (t < handed)
                ok_monotonic = 0;
            handed = t;
            break;
        }
        default:
            frame_plan_close(&p);
            break;
        }
        if (p.asked && p.open)
            ok_one_out = 0;
        if (p.asks != p.answers + (p.asked ? 1 : 0) || p.refused != refused || p.answers != opened_by_answer)
            ok_counts = 0;
    }
    check(ok_one_out, "walk: never a request out with a frame open");
    check(ok_no_ask_open, "walk: never an ask with one out or a frame open");
    check(ok_monotonic, "walk: the time handed never goes back, and a frame is never before its blank");
    check(ok_counts, "walk: asks are answers plus the one out; refusals and answers counted");
    check(ok_skips, "walk: skips are the refreshes between chained answers");
    check(ok_wake, "walk: an ask only for work due by the next blank; a wake before the work, after now, "
                   "and at it the work is due by the next blank");
}

int main(void)
{
    look_cases();
    refusal_cases();
    time_cases();
    skip_cases();
    blank_cases();
    half_cases();
    walk_cases();
    printf("frame_plan: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
