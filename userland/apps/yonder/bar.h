#ifndef YONDER_BAR_H
#define YONDER_BAR_H

// The question bar's answer, decided (YONDER.md § Y3: what makes an answer
// fresh). Pure: the window feeds it events and it says whether one of them
// answered, so the host harness can feed it the sequences that must not.

#include <stdbool.h>
#include <stdint.h>

typedef enum { BAR_ELSEWHERE = 0, BAR_ON_YES, BAR_ON_NO } yonder_bar_spot_t;
typedef enum { BAR_NOTHING = 0, BAR_YES, BAR_NO } yonder_bar_answer_t;

typedef struct {
    bool up;                    // a question is showing
    bool armed;                 // it may be answered
    uint32_t number;            // the question it is showing
    yonder_bar_spot_t pressed;  // where an ARMED press went down
} yonder_bar_t;

// A question appears, disarmed: everything already queued must reach it
// before anything can answer it.
void yonder_bar_raise(yonder_bar_t *bar, uint32_t number);
void yonder_bar_lower(yonder_bar_t *bar);

// The window has seen its event queue empty since the question appeared:
// whatever arrives now was done with the question on screen.
void yonder_bar_settled(yonder_bar_t *bar);

// A press and a release, and where each landed. An answer is a press AND a
// release on the same button, both while armed.
yonder_bar_answer_t yonder_bar_press(yonder_bar_t *bar, yonder_bar_spot_t spot);
yonder_bar_answer_t yonder_bar_release(yonder_bar_t *bar, yonder_bar_spot_t spot);

// Escape: No, armed or not — the safe direction needs no proof of
// freshness.
yonder_bar_answer_t yonder_bar_escape(yonder_bar_t *bar);

#endif
