// bar.c — whether a click answered the question bar (bar.h).

#include "bar.h"

void yonder_bar_raise(yonder_bar_t *bar, uint32_t number)
{
    bar->up = true;
    bar->armed = false;
    bar->number = number;
    bar->pressed = BAR_ELSEWHERE;
}

void yonder_bar_lower(yonder_bar_t *bar)
{
    bar->up = false;
    bar->armed = false;
    bar->pressed = BAR_ELSEWHERE;
}

void yonder_bar_settled(yonder_bar_t *bar)
{
    if (bar->up)
        bar->armed = true;
}

yonder_bar_answer_t yonder_bar_press(yonder_bar_t *bar, yonder_bar_spot_t spot)
{
    // A press before arming is forgotten, not remembered for later: its
    // release must not complete a click that began before the question.
    bar->pressed = bar->up && bar->armed ? spot : BAR_ELSEWHERE;
    return BAR_NOTHING;
}

yonder_bar_answer_t yonder_bar_release(yonder_bar_t *bar, yonder_bar_spot_t spot)
{
    yonder_bar_spot_t pressed = bar->pressed;
    bar->pressed = BAR_ELSEWHERE;
    if (!bar->up || !bar->armed || spot == BAR_ELSEWHERE || spot != pressed)
        return BAR_NOTHING;
    return spot == BAR_ON_YES ? BAR_YES : BAR_NO;
}

yonder_bar_answer_t yonder_bar_escape(yonder_bar_t *bar)
{
    return bar->up ? BAR_NO : BAR_NOTHING;
}
