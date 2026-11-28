// libmedia's placement of a fed media's sound (os64/media_fed_sound.h says
// the rules). Pure: host-tested by tools/test_media_fed_sound_host.sh.
#include "os64/media_fed_sound.h"
#include "os64/media_track.h"

// Whole seconds and a remainder, so nothing needs 128-bit division (the
// library links no compiler runtime): the remainder times a rate fits in 64
// bits. Nearest, a half rounded up.
int64_t os64_media_fed_sound_samples(int64_t ns, uint32_t rate)
{
    int64_t q = ns / 1000000000, r = ns % 1000000000;
    if (r < 0) {
        r += 1000000000;
        q--;
    }
    return q * (int64_t)rate + (int64_t)(((uint64_t)r * rate + 500000000u) / 1000000000u);
}

void os64_media_fed_sound_start(os64_media_fed_sound_t *s, uint32_t rate, int64_t join_ns, int64_t target_ns)
{
    *s = (os64_media_fed_sound_t){ .rate = rate };
    s->tolerance = (int64_t)rate * OS64_MEDIA_FED_SOUND_TOLERANCE_US / 1000000;
    if (s->tolerance < 1)
        s->tolerance = 1;
    s->bound = join_ns > 0 ? os64_media_fed_sound_samples(join_ns, rate) : 0;
    if (target_ns != OS64_MEDIA_NO_TIME) {
        s->count = os64_media_fed_sound_samples(target_ns, rate);
        s->placed = true;
    }
}

void os64_media_fed_sound_place(os64_media_fed_sound_t *s, int64_t pts_ns, uint32_t samples,
                                os64_media_fed_place_t *out)
{
    int64_t place = os64_media_fed_sound_samples(pts_ns, s->rate);
    *out = (os64_media_fed_place_t){ .keep = samples };
    if (!s->placed) {
        s->count = place;
        s->placed = true;
    }
    int64_t d = place - s->count;       // positive: a gap; negative: an overlap
    if (d > s->tolerance) {
        if (d <= s->bound) {
            out->silence = d;
            s->count = place;
        } else {
            out->restarted = true;
            s->count = place;
        }
    } else if (d < -s->tolerance) {
        int64_t over = -d;
        out->skip = over >= samples ? samples : (uint32_t)over;
        out->keep = samples - out->skip;
    }
    out->at = s->count;
    s->count += out->keep;
}
