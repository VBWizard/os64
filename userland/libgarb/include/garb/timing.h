#ifndef GARB_TIMING_H
#define GARB_TIMING_H

// timing.h — an animation's timing, in one place (WEB_ANIMATIONS.md § 2):
// Easing 1's curves and steps, and Web Animations 1's arithmetic (§ 4.5
// to § 4.10, the 2023-06-05 Working Draft's numbers) from an effect's local
// time to its phase, its iteration and its progress.
// Pure: no page, no tree, no allocation. libflow renders CSS animations and
// a script's with it; libdom answers an Animation's currentTime, playState
// and finish with it, so the two can never disagree about where an
// animation is.

#include <stdbool.h>
#include <stdint.h>

#include "garb/values.h"

// Exported from libgarb.so, which builds with hidden visibility.
#pragma GCC visibility push(default)

// An easing (Easing 1 § 2): linear; a cubic Bézier through (0, 0),
// (p[0], p[1]), (p[2], p[3]) and (1, 1), which the keywords ease, ease-in,
// ease-out and ease-in-out are — `named` when one of them is how it was
// written, which is another easing than the same curve written as
// cubic-bezier() where keyframes are grouped by easing (Chrome's answer);
// or `steps` steps jumping as `jump` says, which step-start and step-end
// are, and are the same easing as.
typedef enum { GARB_EASE_LINEAR = 0, GARB_EASE_CUBIC, GARB_EASE_STEPS } garb_ease_kind_t;
typedef enum { GARB_JUMP_END = 0, GARB_JUMP_START, GARB_JUMP_NONE, GARB_JUMP_BOTH } garb_jump_t;
typedef struct {
    garb_ease_kind_t kind;
    double p[4];
    int32_t steps;
    garb_jump_t jump;
    bool named;
} garb_easing_t;

typedef enum {
    GARB_DIRECTION_NORMAL = 0,
    GARB_DIRECTION_REVERSE,
    GARB_DIRECTION_ALTERNATE,
    GARB_DIRECTION_ALTERNATE_REVERSE,
} garb_direction_t;
typedef enum { GARB_FILL_NONE = 0, GARB_FILL_FORWARDS, GARB_FILL_BACKWARDS, GARB_FILL_BOTH } garb_fill_t;

// An easing as libgarb read it (values.h): a keyword, cubic-bezier() or
// steps(). False for any other value, `*out` then unwritten.
bool garb_easing_of(const garb_val_t *v, garb_easing_t *out);

// The easing's output progress at input `x` (Easing 1 § 2). Outside [0, 1]
// a cubic follows the tangent at the nearer end (§ 2.2.1); `before` is a
// step easing's before flag (§ 2.3.1). NULL is linear.
double garb_ease(const garb_easing_t *e, double x, bool before);

// An effect's timing (§ 6.5.1, EffectTiming): times in
// milliseconds, `iterations` INFINITY for `infinite`, `easing` the
// effect's own, applied to the whole iteration (a CSS animation's is
// linear: its animation-timing-function is per keyframe interval).
typedef struct {
    double duration_ms, delay_ms, end_delay_ms;
    double iterations, iteration_start;
    garb_direction_t direction;
    garb_fill_t fill;
    garb_easing_t easing;
} garb_timing_t;

typedef enum { GARB_PHASE_BEFORE, GARB_PHASE_ACTIVE, GARB_PHASE_AFTER } garb_phase_t;

// Where an effect is at a local time (§ 4.5.4, Local time).
typedef struct {
    garb_phase_t phase;
    double active_duration, end_time;   // § 4.8.2; § 4.5.3, The active interval
    double before_end, active_end;      // the phase boundaries, in local time
    bool in_effect;                     // it has an active time: it shows a value
    double active_time;                 // § 4.8.3.1; 0 when not in effect
    double iteration;                   // the current iteration (§ 4.8.4)
    // Where the current iteration starts, in local time; INFINITY after an
    // infinite run.
    double iteration_start;
    bool forwards;                      // the current direction (§ 4.9)
    bool before;                        // the easing's before flag (§ 4.10.1)
    double directed;                    // the directed progress (§ 4.9.1)
    double progress;                    // the transformed progress (§ 4.10.1)
} garb_clock_t;

// The effect `t` at local time `local`, its animation playing forwards, or
// backwards (a negative playback rate), which moves which boundary is
// inside the active phase (§ 4.5.5, Animation effect phases and states). Not in effect: `in_effect` false and
// the progresses 0.
garb_clock_t garb_clock_at(const garb_timing_t *t, double local, bool backwards);

// A script's keyframe effect on an element (WEB_ANIMATIONS.md § 2), as
// libdom's Animation hands it to libflow: its keyframes as libgarb read
// them (an offset in [0, 1], an easing of its own or the linear one, the
// longhands it sets); its timing; and its clock. The clock is the
// animation's start on the face's clock (libflow's `now_ms`), or the local
// time it holds while `held`, and its playback rate: the local time at
// `now` is `hold_ms` when held, else (now - start_ms) * rate. That is all
// libflow needs to work where the effect is at any time itself, so a
// running one ticks with no call into script per frame; those fields are
// the whole contract between libdom and libflow, and a change to them is a
// change to this sentence.
typedef struct {
    double offset;
    bool has_easing;
    garb_easing_t easing;
    const garb_set_t *sets;
    int32_t nsets;
} garb_effect_keyframe_t;
typedef struct garb_effect {
    const garb_effect_keyframe_t *frames;
    int32_t nframes;
    garb_timing_t timing;
    double start_ms, hold_ms, rate;
    bool held;
} garb_effect_t;

#pragma GCC visibility pop

#endif
