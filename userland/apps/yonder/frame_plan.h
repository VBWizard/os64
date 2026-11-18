// frame_plan.h — when yonder's animation frames are asked for and what time
// each one is (PILE3.md § The animations' frames, on the display's beat).
//
// A frame is the display's: yonder asks for the next one
// (os64_gui_frame_request) while the shown page has frame work due — CSS
// animations laid out or ticked, the Web Animations step,
// requestAnimationFrame's callbacks, a ResizeObserver step, the pointer's
// re-hit — and the work runs when the answer arrives, at one time for all of
// it: the answer's blank instant. Work due at a time of its own waits for
// the ticker; frame work whose time is still to come is waited for there
// too, and asks in time for the blank that will carry it, so a long delay
// or a costly frame's rest asks for no frame nobody needs.
//
// Pure: no os64 headers, so tools/test_frame_plan_host.sh runs it, by cases
// and a random walk held to its invariants. yonder's clock is milliseconds
// (yonder_now_ms) and the answer's is microseconds; both are os64_micros',
// so a millisecond is the microsecond time divided by a thousand.

#ifndef YONDER_FRAME_PLAN_H
#define YONDER_FRAME_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#define FRAME_PLAN_NEVER UINT64_MAX

// The period assumed before any answer has said the display's (60 Hz).
#define FRAME_PLAN_PERIOD_GUESS_US 16667u

typedef struct {
    bool asked;          // a request is out and unanswered
    bool open;           // an answer arrived; its frame's work runs until it closes
    bool closed_now;     // the frame closed since the last look (frame_plan_step)
    bool chained;        // the request out went in the look right after a frame closed
    uint64_t number;     // the last answer's number of refreshes; 0 for none yet
    uint64_t blank_us;   // when the last answer's blank began
    uint64_t period_us;  // the display's period, by the last answer; 0 for none yet
    uint64_t time_us;    // the open frame's time
    uint64_t sampled_us; // the latest time any animation was handed
    uint64_t asks, answers, skipped, refused;
} frame_plan_t;

// What the loop does about frames after a look: ask now, and/or have the
// ticker wake it at `wake_ms` (FRAME_PLAN_NEVER for no wake).
typedef struct {
    bool ask;
    uint64_t wake_ms;
} frame_plan_step_t;

// A look, given the earliest frame work's "not before" time (`due_ms`,
// FRAME_PLAN_NEVER for none) and now. A frame asked for now is answered at
// the next blank, so work due by that blank asks now, unless a request is
// out or a frame is open (the frame in hand meets it, or the look after it
// closes asks). Work due later is the ticker's wake, a period before the
// blank that will carry it (on the last answer's grid), where that look
// asks: so a rest or a delay costs no refresh beyond its own, whatever the
// ticker's wake rounds to. An ask is counted as made: say so if the
// request was refused (frame_plan_refused).
frame_plan_step_t frame_plan_step(frame_plan_t *p, uint64_t due_ms, uint64_t now_ms);

// The request a look asked for was refused: none is out.
void frame_plan_refused(frame_plan_t *p);

// An answer: the frame opens, and its time is returned — the blank
// instant, or the latest time an animation was already handed when that is
// later, so no animation ever goes back in time. Refreshes that went by
// unanswered while yonder asked frame after frame are counted skipped. An
// answer nothing asked for (false) opens nothing.
bool frame_plan_answer(frame_plan_t *p, uint64_t number, uint64_t blank_us, uint32_t period_ns,
                       uint64_t *time_us);

// The time an animation sampled now is handed: the open frame's, else now —
// no earlier than the latest handed already, which it becomes.
uint64_t frame_plan_sample(frame_plan_t *p, uint64_t now_us);

// The frame's work is done (its paint, or nothing to paint): it closes.
void frame_plan_close(frame_plan_t *p);

// When the next blank after `now_us` is expected, from the last answer
// (its blank plus whole periods), or a guessed period from now before any.
uint64_t frame_plan_next_blank_us(const frame_plan_t *p, uint64_t now_us);

// A half of the CSS animations held to its cost rule: the time its work is
// next due (`due_ms`, its own answer), no sooner than `rest` times its last
// frame's cost after that frame ended, and never once that cost passed
// `stop_us` (a cost of UINT64_MAX, a frame that failed, passes any lower
// limit; a `stop_us` of UINT64_MAX stops nothing, for a caller that holds
// its half to the rule itself). `had` is whether the half has had a frame.
uint64_t frame_plan_half_due(uint64_t due_ms, bool had, uint64_t ended_ms, uint64_t cost_us, uint32_t rest,
                             uint64_t stop_us);

#endif
