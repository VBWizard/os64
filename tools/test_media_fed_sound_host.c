// libmedia's fed sound placement (userland/libmedia/media_fed_sound.c) on
// the host: the properties os64/media_fed_sound.h promises, the rules case
// by case, and random streams checked against invariants. Its mutants:
// tools/test_media_fed_sound_mutants.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "os64/media_fed_sound.h"
#include "os64/media_track.h"

static unsigned long s_checks, s_failed;
static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok && ++s_failed <= 30)
        printf("FAIL: %s\n", what);
}

static int64_t ns_of(int64_t ticks, uint32_t timescale)
{
    int64_t ns = 0;
    if (!os64_media_ticks_to_ns(ticks, timescale, &ns)) abort();
    return ns;
}

// An hour of contiguous frames of `len` samples at `rate`, their times in
// `timescale` ticks rounded to nearest as a packager writes them.
static void contiguous_hour(const char *name, uint32_t rate, uint32_t len, uint32_t timescale)
{
    os64_media_fed_sound_t s;
    os64_media_fed_sound_start(&s, rate, 2 * (int64_t)len * 1000000000 / rate, OS64_MEDIA_NO_TIME);
    int64_t frames = (int64_t)rate * 3600 / len, total = 0, silence = 0, cut = 0, restarts = 0;
    for (int64_t i = 0; i < frames; i++) {
        __int128 exact = (__int128)i * len * timescale;
        int64_t ticks = (int64_t)((exact + rate / 2) / rate);
        os64_media_fed_place_t p;
        os64_media_fed_sound_place(&s, ns_of(ticks, timescale), len, &p);
        silence += p.silence;
        cut += p.skip;
        restarts += p.restarted;
        if (p.at != total) break;
        total += p.keep;
    }
    char what[160];
    snprintf(what, sizeof(what), "%s: an hour plays by count (silence %lld, cut %lld, restarts %lld, %lld of %lld samples)",
             name, (long long)silence, (long long)cut, (long long)restarts, (long long)total,
             (long long)(frames * len));
    check(silence == 0 && cut == 0 && restarts == 0 && total == frames * len, what);
}

int main(int argc, char **argv)
{
    unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 2;

    // The properties: an hour of contiguous sound gives no silence, no
    // cut and no drift, whatever timescale its times were written in.
    contiguous_hour("AAC 44.1 kHz, timescale 44100", 44100, 1024, 44100);
    contiguous_hour("AAC 44.1 kHz, timescale 1000", 44100, 1024, 1000);
    contiguous_hour("AAC 44.1 kHz, timescale 90000", 44100, 1024, 90000);
    contiguous_hour("AAC 48 kHz, timescale 90000", 48000, 1024, 90000);
    contiguous_hour("AAC 48 kHz, timescale 10000000", 48000, 1024, 10000000);
    contiguous_hour("HE-AAC 24 kHz core, timescale 1000", 24000, 1024, 1000);

    // The rules, case by case, at 48 kHz (a millisecond is 48 samples),
    // with a join of 0.1 s (4800 samples).
    {
        os64_media_fed_sound_t s;
        os64_media_fed_place_t p;
        os64_media_fed_sound_start(&s, 48000, 100000000, OS64_MEDIA_NO_TIME);
        check(s.tolerance == 48 && s.bound == 4800, "a millisecond's tolerance; the join's bound");
        os64_media_fed_sound_place(&s, 1000000000, 1024, &p);
        check(p.at == 48000 && p.keep == 1024 && !p.silence && !p.skip && !p.restarted,
              "the first frame sets the count at its own time");
        // 48 samples late: within the tolerance, played at the count.
        os64_media_fed_sound_place(&s, 1000000000 + (int64_t)(1024 + 48) * 1000000000 / 48000, 1024, &p);
        check(p.at == 49024 && !p.silence && !p.skip, "48 samples late plays on at the count");
        // 49 samples early: an overlap past the tolerance, cut.
        int64_t count = 49024 + 1024;
        os64_media_fed_sound_place(&s, (count - 49) * 1000000000 / 48000, 1024, &p);
        check(p.skip == 49 && p.keep == 975 && p.at == count, "49 samples early: those 49 cut");
        count += 975;
        // 49 samples late: a gap, filled with silence; the frame at its time.
        os64_media_fed_sound_place(&s, (count + 49) * 1000000000 / 48000, 1024, &p);
        check(p.silence == 49 && p.at == count + 49 && p.keep == 1024, "49 samples late: 49 of silence");
        count += 49 + 1024;
        // A gap of exactly the bound is filled; one sample more restarts.
        os64_media_fed_sound_place(&s, (count + 4800) * 1000000000 / 48000, 1024, &p);
        check(p.silence == 4800 && !p.restarted, "a gap of the bound is filled");
        count += 4800 + 1024;
        os64_media_fed_sound_place(&s, (count + 4801) * 1000000000 / 48000, 1024, &p);
        check(p.restarted && !p.silence && p.at == count + 4801, "a gap past the bound restarts the count, unfilled");
        count += 4801 + 1024;
        // A frame wholly before the count goes whole.
        os64_media_fed_sound_place(&s, (count - 3000) * 1000000000 / 48000, 1024, &p);
        check(p.skip == 1024 && p.keep == 0 && p.at == count, "a frame wholly before the count is cut whole");
    }
    // A seek: the count starts at the target, the run-in is cut, and the
    // frame across the target is trimmed to start exactly there.
    {
        os64_media_fed_sound_t s;
        os64_media_fed_place_t p;
        os64_media_fed_sound_start(&s, 48000, 100000000, 2000000000);   // 2 s: sample 96000
        int64_t cut = 0, at = -1;
        for (int i = 0; i < 12; i++) {                                    // frames from 96000 - 10240
            int64_t first = 96000 - 10240 + 1024LL * i;
            os64_media_fed_sound_place(&s, first * 1000000000 / 48000, 1024, &p);
            cut += p.skip;
            if (p.keep > 0 && at < 0) at = p.at;
            check(!p.silence && !p.restarted, "the run-in is never silence");
        }
        check(cut == 10240 && at == 96000, "a seek's run-in is cut to the target exactly");
    }

    // Random streams: frames of random lengths with times jittered, gaps
    // and overlaps of random sizes, and now and then a seek. Invariants:
    // what is kept is contiguous unless restarted; the cut and kept parts
    // make up the frame; silence never passes the bound; and the first
    // kept sample is where the frame's time says to within the tolerance,
    // exactly when anything was filled or cut (a frame cut whole has none:
    // it lay wholly before the count).
    unsigned long seed = 12345, ops = 0;
    time_t t0 = time(NULL);
    while ((unsigned)(time(NULL) - t0) < seconds) {
        uint32_t rates[4] = { 44100, 48000, 24000, 8000 };
        uint32_t rate = rates[(seed = seed * 6364136223846793005ul + 1442695040888963407ul) >> 62];
        os64_media_fed_sound_t s;
        int64_t join = (int64_t)((seed >> 20) % 500 + 1) * 1000000;
        os64_media_fed_sound_start(&s, rate, join, OS64_MEDIA_NO_TIME);
        int64_t truth = (int64_t)((seed >> 33) % 100000), count = 0;
        bool first = true;
        for (int i = 0; i < 2000; i++, ops++) {
            seed = seed * 6364136223846793005ul + 1442695040888963407ul;
            uint32_t len = 64 + (uint32_t)((seed >> 40) % 2048);
            int kind = (int)((seed >> 10) % 20);
            int64_t place = truth;
            if (kind == 0) place += 1 + (int64_t)((seed >> 30) % (uint64_t)(2 * s.bound + 2));   // a gap
            else if (kind == 1) place -= 1 + (int64_t)((seed >> 30) % 3000);                      // an overlap
            else place += (int64_t)((seed >> 30) % (uint64_t)(2 * s.tolerance + 1)) - s.tolerance;  // jitter
            os64_media_fed_place_t p;
            int64_t pts = (int64_t)(((__int128)place * 1000000000 + rate / 2) / rate);
            os64_media_fed_sound_place(&s, pts, len, &p);
            int64_t where = os64_media_fed_sound_samples(pts, rate);
            check(p.skip + p.keep == len, "the cut and the kept make up the frame");
            check(p.silence >= 0 && p.silence <= s.bound, "silence within the bound");
            if (!first && !p.restarted) check(p.at == count + p.silence, "what is kept is contiguous");
            int64_t err = p.at - (where + p.skip);
            if (p.keep == 0) check(p.at == count && where + (int64_t)len <= count, "a frame cut whole lay before the count");
            else if (p.silence || p.skip || p.restarted) check(err == 0, "placed exactly when filled, cut or restarted");
            else check(err >= -s.tolerance && err <= s.tolerance, "within the tolerance otherwise");
            count = p.at + p.keep;
            truth = place + len;
            first = false;
        }
    }
    printf("random streams: %lu frames\n", ops);
    printf("media fed sound: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
