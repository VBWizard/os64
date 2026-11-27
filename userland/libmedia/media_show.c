// media_show.c — a show thread's decisions for its plane (os64/media_show.h).

#include "os64/media_show.h"

void os64_media_show_init(os64_media_show_t *s, uint32_t cap_w, uint32_t cap_h)
{
    *s = (os64_media_show_t){.cap_w = cap_w, .cap_h = cap_h};
}

os64_media_show_act_t os64_media_show_decide(os64_media_show_t *s, const os64_media_show_input_t *in)
{
    os64_media_show_act_t act = {0};
    if (in->gone) {
        act.end = true;
        return act;
    }
    if (in->closing) {
        if (!s->closing) {
            s->closing = true;
            s->closing_since = in->now;
        }
        if (!in->placed) {
            act.destroy = true;
            act.end = true;
        } else if (in->now - s->closing_since >= OS64_MEDIA_SHOW_CLOSE_BOUND_US) {
            act.end = true;
            s->ended_placed++;
        }
        return act;
    }
    bool stale_was = s->stale, ready_was = s->ready;
    s->target_w = in->target_w;
    s->target_h = in->target_h;
    s->ready = s->pic_w == in->target_w && s->pic_h == in->target_h;
    if (in->have) {
        bool same = in->w == s->pic_w && in->h == s->pic_h;
        bool fresh = in->serial != s->published;
        if (in->placed) {
            // The kernel keeps a placed plane's size: another size waits for
            // the caller's hide.
            if (!same)
                s->stale = true;
            else if (fresh)
                act.publish = true;
        } else {
            bool owed = s->pic_w == 0 || in->wanted || s->stale || !same;
            if (owed && (fresh || !same)) {
                act.recreate = in->w > s->cap_w || in->h > s->cap_h;
                act.publish = true;
            }
        }
    }
    act.look = s->stale != stale_was || s->ready != ready_was;
    return act;
}

bool os64_media_show_published(os64_media_show_t *s, bool done, uint32_t w, uint32_t h, uint32_t serial)
{
    if (!done)
        return false;
    bool stale_was = s->stale, ready_was = s->ready;
    s->pic_w = w;
    s->pic_h = h;
    s->published = serial;
    s->stale = false;
    s->ready = w == s->target_w && h == s->target_h;
    s->publishes++;
    return s->stale != stale_was || s->ready != ready_was;
}

void os64_media_show_recreated(os64_media_show_t *s, uint32_t cap_w, uint32_t cap_h)
{
    s->cap_w = cap_w;
    s->cap_h = cap_h;
    // A new plane holds no picture.
    s->pic_w = s->pic_h = 0;
    s->published = 0;
    if (cap_w != 0)
        s->recreates++;
}
