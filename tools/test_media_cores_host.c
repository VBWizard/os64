// test_media_cores_host.c — libmedia's three cores for a video player, under
// ASan and UBSan:
//   the picture queue (userland/libmedia/media_pictures.c) against a model,
//     with flushes, stale pushes, the end and a full queue;
//   the clock (media_clock.c) with made-up wall times: each source, each
//     hold and its end, generations, and random events with the readings
//     checked never to step, never to fall back, and to run within the slew;
//   the pace (media_pace.c): the paint's pick, the converter's skip and the
//     reach on the display's grid, and every door with its hold, flapping,
//     the far-behind pair, and no verdict while not judging.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os64/media_clock.h"
#include "os64/media_pace.h"
#include "os64/media_pictures.h"

static int s_failures, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); if (s_failures > 20) exit(1); } } while (0)

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

// ------------------------------------------------------------ the picture queue

typedef struct { uintptr_t handle; int64_t at, duration; } Entry;
static Entry s_model[1100];
static uint32_t s_held;
static uintptr_t s_released[1100];
static uint32_t s_released_count;

static void release(void *context, void *handle)
{
    CHECK(context == (void *)s_released, "a flush hands the release its context");
    s_released[s_released_count++] = (uintptr_t)handle;
}

static void pictures_run(uint32_t capacity, int rounds)
{
    os64_media_pictures_entry_t *slot = malloc(sizeof(*slot) * capacity);
    os64_media_pictures_t q;
    os64_media_pictures_init(&q, slot, capacity);
    s_held = 0;
    uint32_t generation = 0;
    int ended = 0;
    uintptr_t serial = 1;
    for (int round = 0; round < rounds; round++) {
        uint64_t op = rnd() % 100;
        if (op < 50) {
            int stale = rnd() % 12 == 0, null = rnd() % 40 == 0;
            uint32_t g = stale ? generation - 1 - (uint32_t)(rnd() % 3) : generation;
            uintptr_t h = null ? 0 : serial++;
            int64_t at = (int64_t)(rnd() % 100000), duration = (int64_t)(rnd() % 3000);
            int want = stale || ended || null ? OS64_MEDIA_PICTURES_REFUSED
                       : s_held == capacity  ? OS64_MEDIA_PICTURES_FULL
                                             : OS64_MEDIA_PICTURES_TAKEN;
            int got = os64_media_pictures_push(&q, g, (void *)h, at, duration);
            CHECK(got == want, "round %d: push gave %d, want %d", round, got, want);
            if (got == OS64_MEDIA_PICTURES_TAKEN)
                s_model[s_held++] = (Entry){h, at, duration};
        } else if (op < 85) {
            os64_media_pictures_entry_t e = {0};
            int peeked = rnd() % 4 == 0;
            bool got = os64_media_pictures_take(&q, peeked ? NULL : &e);
            CHECK(got == (s_held > 0), "round %d: take answered %d with %u held", round, got, s_held);
            if (got && !peeked)
                CHECK((uintptr_t)e.handle == s_model[0].handle && e.at == s_model[0].at &&
                      e.duration == s_model[0].duration, "round %d: take gave the wrong entry", round);
            if (s_held > 0) {
                memmove(s_model, s_model + 1, sizeof(Entry) * (s_held - 1));
                s_held--;
            }
        } else if (op < 93) {
            s_released_count = 0;
            int given = rnd() % 5 != 0;
            uint32_t g = os64_media_pictures_flush(&q, given ? release : NULL, s_released);
            CHECK(g == generation + 1, "flush gave generation %u, want %u", g, generation + 1);
            if (given) {
                CHECK(s_released_count == s_held, "flush released %u of %u", s_released_count, s_held);
                for (uint32_t i = 0; i < s_released_count && i < s_held; i++)
                    CHECK(s_released[i] == s_model[i].handle, "flush released out of order at %u", i);
            }
            generation = g;
            s_held = 0;
            ended = 0;
        } else {
            int stale = rnd() % 4 == 0;
            bool took = os64_media_pictures_end(&q, stale ? generation - 1 : generation);
            CHECK(took == !stale, "round %d: an end names its generation", round);
            if (!stale)
                ended = 1;
        }
        CHECK(os64_media_pictures_count(&q) == s_held, "round %d: count %u, want %u", round,
              os64_media_pictures_count(&q), s_held);
        CHECK(os64_media_pictures_full(&q) == (s_held == capacity), "round %d: full", round);
        CHECK(os64_media_pictures_generation(&q) == generation, "round %d: generation", round);
        CHECK(os64_media_pictures_newest(&q) == (s_held > 0 ? s_model[s_held - 1].at : -1), "round %d: newest",
              round);
        for (uint32_t i = 0; i <= s_held; i++) {
            const os64_media_pictures_entry_t *e = os64_media_pictures_peek(&q, i);
            if (i == s_held)
                CHECK(e == NULL, "round %d: peek past the back", round);
            else
                CHECK(e != NULL && (uintptr_t)e->handle == s_model[i].handle && e->at == s_model[i].at,
                      "round %d: peek %u", round, i);
        }
        CHECK(os64_media_pictures_ended(&q) == (ended != 0), "round %d: ended", round);
        CHECK(os64_media_pictures_finished(&q) == (ended && s_held == 0), "round %d: finished", round);
    }
    free(slot);
}

static void pictures_edges(void)
{
    os64_media_pictures_t q;
    os64_media_pictures_init(&q, NULL, 8);
    CHECK(os64_media_pictures_push(&q, 0, (void *)1, 0, 0) == OS64_MEDIA_PICTURES_FULL, "no slots is always full");
    CHECK(os64_media_pictures_newest(&q) == -1 && !os64_media_pictures_take(&q, NULL), "no slots holds nothing");
    os64_media_pictures_entry_t slot[2];
    os64_media_pictures_init(&q, slot, 2);
    os64_media_pictures_end(&q, 0);
    CHECK(os64_media_pictures_finished(&q), "ended and empty is finished");
    CHECK(os64_media_pictures_push(&q, 0, (void *)1, 0, 0) == OS64_MEDIA_PICTURES_REFUSED, "a push after the end");
    CHECK(os64_media_pictures_flush(&q, NULL, NULL) == 1 && !os64_media_pictures_ended(&q), "a flush forgets the end");
    CHECK(os64_media_pictures_push(&q, 0, (void *)1, 0, 0) == OS64_MEDIA_PICTURES_REFUSED,
          "a push made before a flush is refused after it");
    CHECK(!os64_media_pictures_end(&q, 0) && !os64_media_pictures_ended(&q),
          "an end decided before a flush does not end the generation after it");
}

// ------------------------------------------------------------ the clock

#define MS 1000
#define SEC 1000000
#define FRAMES_PER_SEC 48000

static int64_t frames_in(int64_t us) { return us * FRAMES_PER_SEC / SEC; }

// An ear moving at real time from `frame` at `wall`: a report every 10 ms
// from `from` to `to`, each made at the moment it describes.
static void ear_reports(os64_media_clock_t *c, int64_t frame, int64_t wall, int64_t from, int64_t to)
{
    for (int64_t t = from; t <= to; t += 10 * MS)
        os64_media_clock_heard(c, os64_media_clock_generation(c), frame + frames_in(t - wall), t, true, t);
}

static void clock_cases(void)
{
    os64_media_clock_t c;

    // Monotonic: paused at 0, then real time, pause and resume exact.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    CHECK(os64_media_clock_read(&c, 5 * SEC) == 0 && os64_media_clock_state(&c, 0) == OS64_MEDIA_CLOCK_PAUSED,
          "a new clock is paused at 0");
    os64_media_clock_play(&c, 10 * SEC);
    CHECK(os64_media_clock_read(&c, 11 * SEC) == 48000, "a second of monotonic is 48000 frames");
    CHECK(os64_media_clock_read(&c, 9 * SEC) == 0, "a time before the play reads as the play");
    os64_media_clock_play(&c, 10500 * MS);
    CHECK(os64_media_clock_read(&c, 11 * SEC) == 48000, "play while playing changes nothing");
    os64_media_clock_pause(&c, 12 * SEC);
    CHECK(os64_media_clock_read(&c, 20 * SEC) == 96000, "pause holds the reading");
    os64_media_clock_play(&c, 30 * SEC);
    CHECK(os64_media_clock_read(&c, 30 * SEC) == 96000 && os64_media_clock_read(&c, 31 * SEC) == 144000,
          "resume carries on from the paused reading");
    uint32_t g = os64_media_clock_seek(&c, 480000, 32 * SEC);
    CHECK(g == 1 && os64_media_clock_read(&c, 32 * SEC) == 480000, "a seek lands exactly");
    CHECK(os64_media_clock_read(&c, 33 * SEC) == 528000, "and runs from there");
    CHECK(!os64_media_clock_heard(&c, 1, 0, 33 * SEC, true, 33 * SEC), "the monotonic source takes no reports");
    os64_media_clock_pause(&c, 34 * SEC);
    g = os64_media_clock_seek(&c, 1000, 35 * SEC);
    CHECK(g == 2 && os64_media_clock_read(&c, 40 * SEC) == 1000, "a seek while paused stays paused, exactly");

    // Heard: a start holds for the first advancing report.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_HEARD);
    g = os64_media_clock_seek(&c, 96000, 0);
    os64_media_clock_play(&c, 1 * SEC);
    CHECK(os64_media_clock_state(&c, 1 * SEC) == OS64_MEDIA_CLOCK_STARTING, "a heard play starts holding");
    CHECK(os64_media_clock_read(&c, 1 * SEC + 200 * MS) == 96000, "the hold holds");
    CHECK(!os64_media_clock_heard(&c, g, 96000, 1 * SEC + 100 * MS, false, 1 * SEC + 100 * MS) &&
          os64_media_clock_read(&c, 1 * SEC + 200 * MS) == 96000, "a report not advancing does not end the hold");
    CHECK(!os64_media_clock_heard(&c, g - 1, 99000, 1 * SEC, true, 1 * SEC + 100 * MS) && c.reports_stale == 1,
          "a report from an earlier generation is refused");
    CHECK(!os64_media_clock_heard(&c, g, 96480, 1 * SEC - 1, true, 1 * SEC + 150 * MS) &&
          os64_media_clock_state(&c, 1 * SEC + 150 * MS) == OS64_MEDIA_CLOCK_STARTING,
          "a report of sound heard before the play does not end its hold");
    // Frame 96000 + 480 heard at 1.21 s, reported at 1.22 s: the ear is 10 ms
    // past it then, so the reports say 96960 while the reading held 96000.
    CHECK(os64_media_clock_heard(&c, g, 96480, 1 * SEC + 210 * MS, true, 1 * SEC + 220 * MS),
          "an advancing report is taken");
    CHECK(os64_media_clock_state(&c, 1 * SEC + 220 * MS) == OS64_MEDIA_CLOCK_FOLLOWING && c.starts_late == 0,
          "and the clock follows, in time");
    CHECK(os64_media_clock_read(&c, 1 * SEC + 220 * MS) == 96000, "taking it does not step the reading");
    CHECK(os64_media_clock_drift(&c, 1 * SEC + 220 * MS) == -960, "the difference is drift, behind the ear");
    CHECK(os64_media_clock_drift(&c, 1 * SEC + 220 * MS + 1) == -960, "drift in frames is rounded down");
    CHECK(c.start_wait_us == 220 * MS, "the start's wait for its first report, kept");
    // A report is trusted for still_us; the slew's own arithmetic below
    // reads seconds past this one report, so it is trusted for longer here.
    os64_media_clock_still(&c, 10 * SEC);
    // 960 frames of drift (20 ms) at a twentieth down to 10 ms: 480 frames
    // worked off in 200 ms. The last 480 go at a 480th, in 4.8 s.
    CHECK(os64_media_clock_read(&c, 1 * SEC + 420 * MS) == 96000 + frames_in(200 * MS) * 21 / 20,
          "the slew runs a twentieth fast");
    CHECK(os64_media_clock_drift(&c, 1 * SEC + 420 * MS) == -480, "down to 10 ms at the slew");
    CHECK(os64_media_clock_drift(&c, 1 * SEC + 620 * MS) == -460 &&
              os64_media_clock_read(&c, 1 * SEC + 620 * MS) == 96960 + frames_in(400 * MS) - 460,
          "the last 10 ms at a 480th: 20 frames in 200 ms");
    CHECK(os64_media_clock_drift(&c, 6 * SEC + 219 * MS) == -1, "a frame short of done");
    CHECK(os64_media_clock_read(&c, 6 * SEC + 220 * MS) == 96960 + frames_in(5 * SEC) &&
              os64_media_clock_drift(&c, 6 * SEC + 220 * MS) == 0,
          "until the drift is gone, then the reading is the ear's");
    CHECK(os64_media_clock_drift(&c, 9 * SEC) == 0 &&
              os64_media_clock_read(&c, 9 * SEC) == 96480 + frames_in(7 * SEC + 790 * MS),
          "and it does not swing past the ear");

    // A start whose device says nothing runs after the bound.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_HEARD);
    os64_media_clock_play(&c, 0);
    CHECK(os64_media_clock_read(&c, OS64_MEDIA_CLOCK_START_HOLD_US - 1) == 0, "held until the bound");
    CHECK(os64_media_clock_state(&c, OS64_MEDIA_CLOCK_START_HOLD_US) == OS64_MEDIA_CLOCK_STARTING_LATE,
          "late at the bound");
    CHECK(os64_media_clock_read(&c, OS64_MEDIA_CLOCK_START_HOLD_US + 1 * SEC) == 48000,
          "then real time from where it held");
    // At 1.3 s it has run 1.05 s: 50400. The ear says 2400 further on.
    CHECK(os64_media_clock_heard(&c, 0, 50400 + 2400, 1300 * MS, true, 1300 * MS) && c.starts_late == 1,
          "a report after the bound is counted late");
    CHECK(os64_media_clock_read(&c, 1300 * MS) == 50400 && os64_media_clock_drift(&c, 1300 * MS) == -2400,
          "and becomes drift without a step");

    // The bound is the player's: a sink with a long delay holds longer.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_HEARD);
    os64_media_clock_hold(&c, 300 * MS);
    os64_media_clock_play(&c, 0);
    CHECK(os64_media_clock_read(&c, 300 * MS - 1) == 0 &&
          os64_media_clock_state(&c, 299 * MS) == OS64_MEDIA_CLOCK_STARTING,
          "a bound of 300 ms holds past the quarter second");
    CHECK(os64_media_clock_read(&c, 1300 * MS) == 48000, "and runs from its end");
    os64_media_clock_hold(&c, -5 * SEC);
    os64_media_clock_seek(&c, 0, 2 * SEC);
    CHECK(os64_media_clock_read(&c, 2 * SEC) == 0 && os64_media_clock_read(&c, 3 * SEC + 1) == 48000,
          "a bound below 1 is 1 microsecond: a start never begins already run");

    // Unmute while running: no hold, real time until the first report.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    os64_media_clock_play(&c, 0);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_HEARD, 1 * SEC);
    CHECK(os64_media_clock_state(&c, 1 * SEC) == OS64_MEDIA_CLOCK_AWAITING, "unmute awaits a report");
    CHECK(os64_media_clock_read(&c, 2 * SEC) == 96000, "running at real time while it waits");
    CHECK(!os64_media_clock_heard(&c, 0, 0, 2 * SEC, false, 2 * SEC) && os64_media_clock_read(&c, 3 * SEC) == 144000,
          "a report not advancing does not stop a picture already moving");
    CHECK(os64_media_clock_heard(&c, 0, 144000 + 4800, 3 * SEC, true, 3 * SEC) &&
          os64_media_clock_drift(&c, 3 * SEC) == -4800, "the first report is drift");

    // Starved: a report not advancing holds, drift and all, for as long as told.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_HEARD);
    os64_media_clock_play(&c, 0);
    os64_media_clock_heard(&c, 0, 0, 100 * MS, true, 100 * MS);
    os64_media_clock_heard(&c, 0, 48000 + 960, 1100 * MS, true, 1100 * MS);
    int64_t before = os64_media_clock_read(&c, 1200 * MS);
    CHECK(os64_media_clock_heard(&c, 0, 48000 + 960 + 4800, 1200 * MS, false, 1200 * MS) && c.starves == 1,
          "a report not advancing is taken");
    CHECK(os64_media_clock_state(&c, 1200 * MS) == OS64_MEDIA_CLOCK_STARVED, "starved");
    CHECK(os64_media_clock_read(&c, 60 * SEC) == before, "the hold lasts as long as the clock is told");
    os64_media_clock_heard(&c, 0, before + 960, 61 * SEC, true, 61 * SEC);
    CHECK(os64_media_clock_read(&c, 61 * SEC) == before && os64_media_clock_drift(&c, 61 * SEC) == -960,
          "an advancing report ends it without a step");
    os64_media_clock_heard(&c, 0, 0, 62 * SEC, false, 62 * SEC);
    int64_t held = os64_media_clock_read(&c, 62 * SEC);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_MONOTONIC, 70 * SEC);
    CHECK(os64_media_clock_read(&c, 70 * SEC) == held && os64_media_clock_read(&c, 71 * SEC) == held + 48000,
          "the monotonic source ends it too, without a step");

    // A change to the source already in use changes nothing: following
    // stays following, its drift kept.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_HEARD);
    os64_media_clock_play(&c, 0);
    os64_media_clock_heard(&c, 0, 4800, 100 * MS, true, 100 * MS);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_HEARD, 120 * MS);
    CHECK(os64_media_clock_state(&c, 120 * MS) == OS64_MEDIA_CLOCK_FOLLOWING &&
          os64_media_clock_drift(&c, 120 * MS) == -4800 + frames_in(20 * MS) / 20,
          "the source it already has: still following, the drift still slewing");

    // A reading between frames is the frame it is in, below 0 too.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    os64_media_clock_seek(&c, -1, 0);
    os64_media_clock_play(&c, 0);
    CHECK(os64_media_clock_read(&c, 10) == -1 && os64_media_clock_read(&c, 21) == 0,
          "a reading is rounded down, not toward 0");

    // A change of source while paused takes effect at play.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_HEARD, 0);
    CHECK(os64_media_clock_state(&c, 0) == OS64_MEDIA_CLOCK_PAUSED, "a source change does not play");
    CHECK(!os64_media_clock_heard(&c, 0, 0, 0, true, 0), "no report is taken while paused");
    os64_media_clock_play(&c, 1 * SEC);
    CHECK(os64_media_clock_state(&c, 1 * SEC) == OS64_MEDIA_CLOCK_STARTING, "a play on the new source holds");

    // THE STILL EAR. A report is trusted for still_us past the moment it
    // describes; then the reading holds where that left it and the state
    // reads starved, until a report says the ear moved.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    CHECK(c.still_us == OS64_MEDIA_CLOCK_STILL_US, "a new clock's trust");
    os64_media_clock_still(&c, 0);
    CHECK(c.still_us == 1, "a trust below 1 is 1");
    os64_media_clock_still(&c, 80 * MS);
    os64_media_clock_play(&c, 0);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_HEARD, 1 * SEC);
    os64_media_clock_heard(&c, 0, 48000, 1 * SEC, true, 1 * SEC);
    CHECK(os64_media_clock_state(&c, 1 * SEC + 80 * MS) == OS64_MEDIA_CLOCK_FOLLOWING &&
              os64_media_clock_read(&c, 1 * SEC + 80 * MS) == 48000 + frames_in(80 * MS),
          "within its trust: following, at real time");
    CHECK(os64_media_clock_state(&c, 1 * SEC + 80 * MS + 1) == OS64_MEDIA_CLOCK_STARVED &&
              os64_media_clock_read(&c, 1 * SEC + 500 * MS) == 48000 + frames_in(80 * MS),
          "past it: the ear is taken as stopped, the reading holds");
    // The ear moved again at 1.3 s, from where it stopped at 1.0 s: the
    // reading is 80 ms ahead, inside the bound, so the slew takes it.
    os64_media_clock_heard(&c, 0, 48000 + frames_in(10 * MS), 1 * SEC + 310 * MS, true, 1 * SEC + 310 * MS);
    CHECK(c.stills == 1 && c.held_ahead == 0, "a report after the trust ran out: a still, counted");
    CHECK(os64_media_clock_state(&c, 1 * SEC + 310 * MS) == OS64_MEDIA_CLOCK_FOLLOWING &&
              os64_media_clock_read(&c, 1 * SEC + 310 * MS) == 48000 + frames_in(80 * MS) &&
              os64_media_clock_drift(&c, 1 * SEC + 310 * MS) == frames_in(70 * MS),
          "following again from where it held: 70 ms ahead of the ear");
    CHECK(c.report_ages[5] == 1, "the report it replaced was 310 ms old: over 120 ms");
    // A trust set while following applies at once.
    os64_media_clock_still(&c, 10 * MS);
    CHECK(os64_media_clock_state(&c, 1 * SEC + 321 * MS) == OS64_MEDIA_CLOCK_STARVED, "a new trust applies at once");

    // A whole second ahead of the ear: the reading holds until the ear is
    // within the bound (0.9 s), then the slew takes the last 100 ms.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    os64_media_clock_play(&c, 0);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_HEARD, 10 * SEC);
    os64_media_clock_heard(&c, 0, 9 * FRAMES_PER_SEC, 10 * SEC, true, 10 * SEC);
    CHECK(c.held_ahead == 1 && c.jumped_behind == 0, "held ahead, counted");
    CHECK(os64_media_clock_drift(&c, 10 * SEC) == 48000, "a second ahead");
    ear_reports(&c, 9 * FRAMES_PER_SEC, 10 * SEC, 10 * SEC + 10 * MS, 10 * SEC + 890 * MS);
    CHECK(os64_media_clock_read(&c, 10 * SEC + 890 * MS) == 10 * FRAMES_PER_SEC, "the reading holds meanwhile");
    ear_reports(&c, 9 * FRAMES_PER_SEC, 10 * SEC, 10 * SEC + 900 * MS, 10 * SEC + 900 * MS);
    CHECK(os64_media_clock_drift(&c, 10 * SEC + 900 * MS) == 4800, "until the ear is within the bound");
    // 4,320 frames at the slew take 1.8 s; the last 480 then take 4.8 s.
    ear_reports(&c, 9 * FRAMES_PER_SEC, 10 * SEC, 10 * SEC + 910 * MS, 12 * SEC + 700 * MS);
    CHECK(os64_media_clock_drift(&c, 12 * SEC + 700 * MS) == 480, "the slew to 10 ms");
    ear_reports(&c, 9 * FRAMES_PER_SEC, 10 * SEC, 12 * SEC + 710 * MS, 17 * SEC + 500 * MS);
    CHECK(os64_media_clock_drift(&c, 17 * SEC + 500 * MS) == 0 &&
              os64_media_clock_read(&c, 17 * SEC + 500 * MS) == 16 * FRAMES_PER_SEC + frames_in(500 * MS),
          "gone after 7.5 s, and the reading is the ear's");
    CHECK(c.held_ahead == 1, "held once");

    // A second BEHIND: one step forward to the bound, then the slew.
    os64_media_clock_init(&c, OS64_MEDIA_CLOCK_MONOTONIC);
    os64_media_clock_play(&c, 0);
    os64_media_clock_source(&c, OS64_MEDIA_CLOCK_HEARD, 10 * SEC);
    os64_media_clock_heard(&c, 0, 11 * FRAMES_PER_SEC, 10 * SEC, true, 10 * SEC);
    CHECK(c.jumped_behind == 1 && c.held_ahead == 0, "jumped, counted");
    CHECK(os64_media_clock_read(&c, 10 * SEC) == 11 * FRAMES_PER_SEC - 4800 &&
              os64_media_clock_drift(&c, 10 * SEC) == -4800,
          "stepped forward to the bound behind");
}

// ------------------------------------------------------------ the storm

#define STILL (80 * MS)
#define BOUND_FRAMES 4800 // OS64_MEDIA_CLOCK_BOUND_US

// A sound with gaps: it plays at real time from `start` except during each
// gap [gap_at[i], gap_at[i] + gap), when it stands still.
typedef struct {
    int64_t start;
    const int64_t *gap_at;
    int gaps;
    int64_t gap;
} sound_t;

// Where the ear is at wall `t`, in frames.
static int64_t ear_at(const sound_t *s, int64_t t)
{
    int64_t played = t - s->start;
    for (int i = 0; i < s->gaps; i++) {
        if (t <= s->gap_at[i])
            break;
        int64_t in = t - s->gap_at[i];
        played -= in < s->gap ? in : s->gap;
    }
    return frames_in(played);
}

static bool in_gap(const sound_t *s, int64_t t)
{
    for (int i = 0; i < s->gaps; i++)
        if (t >= s->gap_at[i] && t < s->gap_at[i] + s->gap)
            return true;
    return false;
}

// Plays `s` to its end at `end_frame` against a clock started at its
// start, reporting as gplay does: every 10 ms while the ear moves, each
// report describing the ear up to 10 ms before it was made (the pull's
// stamp); none in a gap. Checks every millisecond that the reading stays
// within the bound of the ear's view, and returns the worst it got ahead
// of the true ear and the reading when the sound ended.
static void play_storm(const sound_t *s, int64_t end_frame, int64_t *worst_ahead, int64_t *at_end,
                       os64_media_clock_t *c)
{
    os64_media_clock_init(c, OS64_MEDIA_CLOCK_HEARD);
    os64_media_clock_still(c, STILL);
    os64_media_clock_play(c, s->start);
    *worst_ahead = 0;
    *at_end = -1;
    int64_t last_frame = -1;
    for (int64_t t = s->start; ; t += MS) {
        if ((t - s->start) % (10 * MS) == 0 && !in_gap(s, t)) {
            int64_t stamp = t - (int64_t)(rnd() % (10 * MS));
            if (stamp < s->start)
                stamp = s->start;
            int64_t frame = ear_at(s, stamp);
            if (frame > last_frame && stamp >= s->start + 1)
                os64_media_clock_heard(c, 0, frame, stamp, true, t);
            if (frame > last_frame)
                last_frame = frame;
        }
        int64_t r = os64_media_clock_read(c, t);
        int64_t ear = ear_at(s, t);
        if (r - ear > *worst_ahead)
            *worst_ahead = r - ear;
        if (os64_media_clock_state(c, t) == OS64_MEDIA_CLOCK_FOLLOWING ||
            os64_media_clock_state(c, t) == OS64_MEDIA_CLOCK_STARVED) {
            // Within the bound, except while held ahead after a gap let the
            // reading run the trust past where the ear stopped.
            int64_t d = os64_media_clock_drift(c, t);
            int64_t over = t < c->slew_since ? frames_in(STILL + 20 * MS) : 1;
            CHECK(d <= BOUND_FRAMES + over && d >= -BOUND_FRAMES - 1, "%lld us: %lld frames from the ear's view",
                  (long long)t, (long long)d);
        }
        if (ear >= end_frame) {
            *at_end = r;
            return;
        }
    }
}

static void clock_storm(void)
{
    os64_media_clock_t c;
    int64_t worst, at_end;

    // One 150 ms gap: the picture holds through it (after the reading's
    // trust in the last report runs out) and heals.
    static const int64_t one[] = {5 * SEC};
    sound_t s1 = {.start = 0, .gap_at = one, .gaps = 1, .gap = 150 * MS};
    play_storm(&s1, frames_in(20 * SEC), &worst, &at_end, &c);
    CHECK(c.stills >= 1, "the gap was held through (%llu)", (unsigned long long)c.stills);
    CHECK(worst <= frames_in(STILL + 20 * MS), "ahead by at most the trust and a report's scatter (%lld)",
          (long long)worst);
    CHECK(at_end - frames_in(20 * SEC) <= frames_in(20 * MS) && at_end - frames_in(20 * SEC) >= -frames_in(20 * MS),
          "healed by the end (%lld frames off)", (long long)(at_end - frames_in(20 * SEC)));
    CHECK(c.held_ahead == 0 && c.jumped_behind == 0, "one gap needs no bound");

    // The P5's storm: a 150 ms gap every 3 s for a minute, then one every
    // second for a minute.
    static int64_t storm[80];
    int n = 0;
    for (int64_t t = 3 * SEC; t < 60 * SEC; t += 3 * SEC)
        storm[n++] = t;
    for (int64_t t = 60 * SEC; t < 120 * SEC; t += SEC)
        storm[n++] = t;
    sound_t s2 = {.start = 0, .gap_at = storm, .gaps = n, .gap = 150 * MS};
    int64_t end = frames_in(130 * SEC) - frames_in((int64_t)n * 150 * MS);
    play_storm(&s2, end, &worst, &at_end, &c);
    CHECK(worst <= BOUND_FRAMES + frames_in(STILL + 20 * MS),
          "the storm: never further ahead of the true ear than the bound and the trust (%lld)", (long long)worst);
    CHECK(at_end - end <= BOUND_FRAMES && at_end - end >= -BOUND_FRAMES,
          "the picture ends within the bound of the sound (%lld frames)", (long long)(at_end - end));
    CHECK(c.stills >= (uint64_t)n - 2, "every gap held through (%llu of %d)", (unsigned long long)c.stills, n);
    printf("  the storm (%d gaps of 150 ms): ahead of the true ear by %lld ms at worst, the picture's end %lld ms "
           "from the sound's; %llu stills, %llu holds ahead, %llu steps behind\n",
           n, (long long)(worst * 1000 / FRAMES_PER_SEC), (long long)((at_end - end) * 1000 / FRAMES_PER_SEC),
           (unsigned long long)c.stills, (unsigned long long)c.held_ahead, (unsigned long long)c.jumped_behind);
}

// Random events, with readings checked at random times between them.
static void clock_random(int rounds)
{
    os64_media_clock_t c;
    os64_media_clock_init(&c, rnd() % 2 ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC);
    int64_t now = 0, ear = 0;     // the made-up ear: where the sound is, in frames
    for (int round = 0; round < rounds; round++) {
        int64_t step = (int64_t)(rnd() % (300 * MS)) + 1;
        // Between events: readings never fall back, and run at most a
        // twentieth fast and, while not holding, at least a twentieth slow.
        // Following, a reading holds past the last report's trust and while
        // ahead by more than the bound, and is never further from the
        // ear's view than the bound.
        int64_t t0 = now, r0 = os64_media_clock_read(&c, t0);
        int state = os64_media_clock_state(&c, t0);
        int64_t trusted_until = c.heard_wall + c.still_us, slewing_from = c.slew_since;
        for (int i = 0; i < 4; i++) {
            int64_t t = t0 + (int64_t)(rnd() % (uint64_t)step) + 1;
            int64_t r = os64_media_clock_read(&c, t);
            CHECK(r >= r0, "round %d: the reading fell back from %lld to %lld", round, (long long)r0, (long long)r);
            int64_t real = frames_in(t - t0);
            CHECK(r - r0 <= real + real / 20 + 2, "round %d: ran faster than the slew (%lld for %lld)", round,
                  (long long)(r - r0), (long long)real);
            if (state == OS64_MEDIA_CLOCK_RUNNING || state == OS64_MEDIA_CLOCK_AWAITING)
                CHECK(r - r0 >= real - 1 && r - r0 <= real + 1, "round %d: monotonic is real time", round);
            if (state == OS64_MEDIA_CLOCK_FOLLOWING && t <= trusted_until && t0 >= slewing_from)
                CHECK(r - r0 >= real - real / 20 - 2, "round %d: ran slower than the slew", round);
            if (state == OS64_MEDIA_CLOCK_FOLLOWING || state == OS64_MEDIA_CLOCK_STARVED) {
                int64_t d = os64_media_clock_drift(&c, t);
                CHECK(d >= -4801 && (d <= 4801 || t < slewing_from), "round %d: %lld frames from the ear's view",
                      round, (long long)d);
            }
            if (state == OS64_MEDIA_CLOCK_PAUSED || state == OS64_MEDIA_CLOCK_STARVED)
                CHECK(r == r0, "round %d: a hold moved", round);
        }
        now += step;
        ear += frames_in(step) + (int64_t)(rnd() % 200) - 100;
        int64_t before = os64_media_clock_read(&c, now);
        uint64_t jumps = c.jumped_behind;
        uint64_t op = rnd() % 100;
        uint32_t generation = os64_media_clock_generation(&c);
        if (op < 50) {
            int advancing = rnd() % 8 != 0;
            int stale = rnd() % 16 == 0;
            os64_media_clock_heard(&c, stale ? generation + 1 : generation, ear, now - (int64_t)(rnd() % (20 * MS)),
                                   advancing, now);
        } else if (op < 62) {
            os64_media_clock_play(&c, now);
        } else if (op < 70) {
            os64_media_clock_pause(&c, now);
        } else if (op < 85) {
            os64_media_clock_source(&c, rnd() % 2 ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC, now);
        } else if (op < 92) {
            int64_t frame = (int64_t)(rnd() % (600 * FRAMES_PER_SEC));
            uint32_t g = os64_media_clock_seek(&c, frame, now);
            CHECK(g == generation + 1, "round %d: a seek moves the generation", round);
            CHECK(os64_media_clock_read(&c, now) == frame, "round %d: a seek lands exactly", round);
            ear = frame;
            continue;
        } else {
            continue;
        }
        if (c.jumped_behind != jumps)
            CHECK(os64_media_clock_read(&c, now) >= before && os64_media_clock_drift(&c, now) == -4800,
                  "round %d: a report behind by more than the bound steps forward to it", round);
        else
            CHECK(os64_media_clock_read(&c, now) == before, "round %d: an event stepped the reading (%lld to %lld)",
                  round, (long long)before, (long long)os64_media_clock_read(&c, now));
    }
}

// ------------------------------------------------------------ the pace

#define PERIOD 800      // a 60 a second picture, in output frames
#define DEPTH 8         // in at under 2400 frames of lead, out at 4800 or more

static void pace_pick_cases(void)
{
    os64_media_pace_t p;
    os64_media_pace_init(&p, DEPTH, PERIOD);
    os64_media_pictures_entry_t slot[4];
    os64_media_pictures_t ready;
    os64_media_pictures_init(&ready, slot, 4);
    os64_media_pictures_push(&ready, 0, (void *)1, 100, 100);
    os64_media_pictures_push(&ready, 0, (void *)2, 200, 100);
    os64_media_pictures_push(&ready, 0, (void *)3, 300, 100);
    os64_media_pace_pick_t k = os64_media_pace_pick(&p, &ready, 50, NULL, true);
    CHECK(!k.show && k.drop == 0 && !k.starved, "nothing due: nothing changes");
    k = os64_media_pace_pick(&p, &ready, 100, NULL, true);
    CHECK(k.show && k.drop == 0, "a picture due at the blank is shown");
    k = os64_media_pace_pick(&p, &ready, 250, NULL, true);
    CHECK(k.show && k.drop == 1, "the latest due is shown and the one before dropped");
    k = os64_media_pace_pick(&p, &ready, 10000, NULL, false);
    CHECK(k.show && k.drop == 2, "all due: the last shown, every one before dropped, judging or not");
    CHECK(p.shown == 3 && p.dropped == 3 && os64_media_pictures_count(&ready) == 3, "counted; the queue untouched");

    os64_media_pictures_t empty;
    os64_media_pictures_init(&empty, slot, 4);
    os64_media_pictures_entry_t shown = {(void *)9, 1000, 100};
    CHECK(!os64_media_pace_pick(&p, &empty, 1099, &shown, true).starved, "within the picture's time: not starved");
    CHECK(os64_media_pace_pick(&p, &empty, 1100, &shown, true).starved && p.starved == 1,
          "past it, nothing ready: starved");
    CHECK(!os64_media_pace_pick(&p, &empty, 5000, &shown, false).starved, "not judging: not starved");
    CHECK(!os64_media_pace_pick(&p, &empty, 5000, NULL, true).starved, "nothing shown yet: not starved");
    shown.duration = 0;
    CHECK(!os64_media_pace_pick(&p, &empty, 1000 + PERIOD - 1, &shown, true).starved &&
          os64_media_pace_pick(&p, &empty, 1000 + PERIOD, &shown, true).starved,
          "a picture with no duration lasts a period");
    os64_media_pace_period(&p, 0);
    CHECK(!os64_media_pace_pick(&p, &empty, 1000000, &shown, true).starved, "no duration and no period: never starved");
    os64_media_pictures_push(&empty, 0, (void *)4, 9000, 100);
    os64_media_pace_period(&p, PERIOD);
    CHECK(!os64_media_pace_pick(&p, &empty, 5000, &shown, true).starved, "a picture ready but early: not starved");

    // The converter's skip: by the successor.
    os64_media_pictures_t decoded;
    os64_media_pictures_init(&decoded, slot, 4);
    os64_media_pictures_push(&decoded, 0, (void *)1, 0, 0);
    CHECK(!os64_media_pace_skip(&p, &decoded, 1000000), "no successor decoded: convert it");
    os64_media_pictures_push(&decoded, 0, (void *)2, 800, 0);
    CHECK(!os64_media_pace_skip(&p, &decoded, 799), "the successor not yet due: convert it");
    CHECK(os64_media_pace_skip(&p, &decoded, 800) && p.skipped == 1, "the successor due: skip it");

    // The reach, on the display's grid.
    CHECK(os64_media_pace_reach(1000, 5000, 10000, 16667) == 10000, "before the next blank: that blank");
    CHECK(os64_media_pace_reach(1000, 9000, 10000, 16667) == 10000, "on a blank: that blank");
    CHECK(os64_media_pace_reach(1000, 29000, 10000, 16667) == 43334, "past it: the first after");
    CHECK(os64_media_pace_reach(50000, 0, 100000, 16667) == 66666, "a blank earlier than the one given");
    CHECK(os64_media_pace_reach(50000, -5, 100000, 0) == 50000, "no grid: now, a cost below 0 as none");
}

static int judge(os64_media_pace_t *p, int64_t now, int64_t lead, bool judging)
{
    os64_media_pace_measure_t m = {now, 1000000, judging, 1000000 + lead, 0, 0};
    return os64_media_pace_judge(p, &m);
}

static void pace_door_cases(void)
{
    os64_media_pace_t p;
    os64_media_pace_init(&p, DEPTH, PERIOD);
    int64_t t = 0;
    CHECK(judge(&p, t, 2400, true) == OS64_MEDIA_PACE_ON_TIME &&
          judge(&p, t + OS64_MEDIA_PACE_IN_HOLD_US, 2400, true) == OS64_MEDIA_PACE_ON_TIME,
          "at the in door, not under it, for the whole hold: on time");
    t += 2 * OS64_MEDIA_PACE_IN_HOLD_US;
    CHECK(judge(&p, t, 2399, true) == OS64_MEDIA_PACE_ON_TIME, "under it: the hold begins");
    CHECK(judge(&p, t + OS64_MEDIA_PACE_IN_HOLD_US - 1, 0, true) == OS64_MEDIA_PACE_ON_TIME, "not before the hold");
    CHECK(judge(&p, t + OS64_MEDIA_PACE_IN_HOLD_US, 0, true) == OS64_MEDIA_PACE_DECODING_LATE && p.skip_unused,
          "after it: decoding late, the unused left out");
    CHECK(p.entered[OS64_MEDIA_PACE_DECODING_LATE] == 1 && p.lead == 0, "entered once; the lead kept");
    t += OS64_MEDIA_PACE_IN_HOLD_US;
    CHECK(judge(&p, t, 4799, true) == OS64_MEDIA_PACE_DECODING_LATE, "under the out door: stays");
    CHECK(judge(&p, t + 1, 4800, true) == OS64_MEDIA_PACE_DECODING_LATE, "at it: the hold begins");
    CHECK(judge(&p, t + OS64_MEDIA_PACE_OUT_HOLD_US, 9999, true) == OS64_MEDIA_PACE_DECODING_LATE,
          "not before the hold");
    CHECK(judge(&p, t + 1 + OS64_MEDIA_PACE_OUT_HOLD_US, 9999, true) == OS64_MEDIA_PACE_ON_TIME && !p.skip_unused,
          "after it: on time, every picture decoded");
    t += 1 + OS64_MEDIA_PACE_OUT_HOLD_US;
    CHECK(judge(&p, t + 1, 0, true) == OS64_MEDIA_PACE_ON_TIME &&
          judge(&p, t + OS64_MEDIA_PACE_IN_HOLD_US, 0, true) == OS64_MEDIA_PACE_ON_TIME,
          "back on time, the in door's hold runs again from the start");

    // Flapping round a door moves nothing while no hold completes.
    os64_media_pace_init(&p, DEPTH, PERIOD);
    for (int i = 0; i < 100; i++)
        judge(&p, (int64_t)i * 100000, i % 2 ? 0 : 3000, true);
    CHECK(p.state == OS64_MEDIA_PACE_ON_TIME && p.entered[OS64_MEDIA_PACE_DECODING_LATE] == 0,
          "a lead alternating round the in door every 100 ms never enters");
    os64_media_pace_init(&p, DEPTH, PERIOD);
    judge(&p, 0, 0, true);
    judge(&p, OS64_MEDIA_PACE_IN_HOLD_US, 0, true);
    for (int i = 0; i < 100; i++)
        judge(&p, OS64_MEDIA_PACE_IN_HOLD_US + (int64_t)i * 900000, i % 2 ? 0 : 6000, true);
    CHECK(p.state == OS64_MEDIA_PACE_DECODING_LATE && p.entered[OS64_MEDIA_PACE_ON_TIME] == 0,
          "a lead alternating round the out door every 0.9 s never leaves");

    // Not judging: no door moves, no hold runs.
    os64_media_pace_init(&p, DEPTH, PERIOD);
    judge(&p, 0, 0, true);
    CHECK(judge(&p, OS64_MEDIA_PACE_IN_HOLD_US, -1000000, false) == OS64_MEDIA_PACE_ON_TIME,
          "not judging, however far behind: nothing moves");
    CHECK(judge(&p, OS64_MEDIA_PACE_IN_HOLD_US + 1, 0, true) == OS64_MEDIA_PACE_ON_TIME,
          "a hold interrupted by not judging starts again");
    CHECK(judge(&p, 2 * OS64_MEDIA_PACE_IN_HOLD_US + 1, 0, true) == OS64_MEDIA_PACE_DECODING_LATE,
          "and runs its whole length");
    os64_media_pace_init(&p, DEPTH, 0);
    judge(&p, 0, -1000000, true);
    CHECK(judge(&p, 10 * SEC, -1000000, true) == OS64_MEDIA_PACE_ON_TIME, "no period known: nothing moves");
    os64_media_pace_init(&p, DEPTH, PERIOD);
    os64_media_pace_measure_t none = {0, 1000000, true, -1, 0, 0};
    os64_media_pace_judge(&p, &none);
    none.now = 10 * SEC;
    CHECK(os64_media_pace_judge(&p, &none) == OS64_MEDIA_PACE_ON_TIME, "nothing decoded yet: nothing moves");

    // Far behind, at once, by the picture's lead or the sound's floor.
    os64_media_pace_init(&p, DEPTH, PERIOD);
    CHECK(judge(&p, 0, -30 * PERIOD, true) == OS64_MEDIA_PACE_ON_TIME && p.resyncs == 0,
          "thirty periods behind is not yet far");
    CHECK(judge(&p, 1, -30 * PERIOD - 1, true) == OS64_MEDIA_PACE_FAR_BEHIND && p.resyncs == 1 && p.skip_unused,
          "past thirty: far behind at once, a resync asked");
    CHECK(judge(&p, 2, -1000000, true) == OS64_MEDIA_PACE_FAR_BEHIND && p.resyncs == 1,
          "far behind asks once, and waits for the resync");
    os64_media_pace_resynced(&p, 1 * SEC + 1);
    CHECK(p.state == OS64_MEDIA_PACE_DECODING_LATE && p.skip_unused && p.far_behind_us == 1 * SEC,
          "the first picture after it: decoding late, the time far behind counted");
    os64_media_pace_resynced(&p, 2 * SEC);
    CHECK(p.state == OS64_MEDIA_PACE_DECODING_LATE && p.entered[OS64_MEDIA_PACE_DECODING_LATE] == 1,
          "a resync reported when not far behind changes nothing");
    CHECK(judge(&p, 3 * SEC, -30 * PERIOD - 1, true) == OS64_MEDIA_PACE_FAR_BEHIND && p.resyncs == 2,
          "far behind from decoding late too");

    os64_media_pace_init(&p, DEPTH, PERIOD);
    os64_media_pace_measure_t m = {0, 1000000, true, 1000000 + 9000, 4799, 4800};
    CHECK(os64_media_pace_judge(&p, &m) == OS64_MEDIA_PACE_FAR_BEHIND && p.far_by_sound == 1,
          "the sound under its floor is far behind, however well the picture leads");
    os64_media_pace_reset(&p, 500 * MS);
    CHECK(p.state == OS64_MEDIA_PACE_ON_TIME && !p.skip_unused && p.far_behind_us == 500 * MS && p.resyncs == 1,
          "a reset: on time, the time far behind counted, the counters kept");
    m.sound_frames = 4800;
    m.now = 600 * MS;
    CHECK(os64_media_pace_judge(&p, &m) == OS64_MEDIA_PACE_ON_TIME, "at the floor: not far");
    m.sound_frames = 0;
    m.sound_floor = 0;
    CHECK(os64_media_pace_judge(&p, &m) == OS64_MEDIA_PACE_ON_TIME, "no floor: the sound protects nothing");

    // The sound's door fires once, and re-arms only when the sound has been
    // back at its floor.
    os64_media_pace_init(&p, DEPTH, PERIOD);
    os64_media_pace_measure_t s = {0, 1000000, true, 1000000 + 9000, 100, 4800};
    CHECK(os64_media_pace_judge(&p, &s) == OS64_MEDIA_PACE_FAR_BEHIND && p.resyncs == 1, "short of sound: far");
    os64_media_pace_resynced(&p, 100 * MS);
    s.now = 200 * MS;
    CHECK(os64_media_pace_judge(&p, &s) == OS64_MEDIA_PACE_DECODING_LATE && p.resyncs == 1,
          "still short after the resync: no second resync");
    s.now = 300 * MS;
    s.sound_frames = 4800;
    os64_media_pace_judge(&p, &s);
    s.now = 400 * MS;
    s.sound_frames = 100;
    CHECK(os64_media_pace_judge(&p, &s) == OS64_MEDIA_PACE_FAR_BEHIND && p.resyncs == 2 && p.far_by_sound == 2,
          "back at its floor, then short again: far again, from decoding late");
    os64_media_pace_init(&p, DEPTH, 0);
    os64_media_pace_measure_t q = {0, 1000000, true, -1, 100, 4800};
    CHECK(os64_media_pace_judge(&p, &q) == OS64_MEDIA_PACE_FAR_BEHIND,
          "the sound's door needs no period and no picture decoded");
    q.judging = false;
    os64_media_pace_init(&p, DEPTH, PERIOD);
    CHECK(os64_media_pace_judge(&p, &q) == OS64_MEDIA_PACE_ON_TIME, "nor does it move while not judging");

    // A resync is asked, then taken; a reset withdraws one not taken.
    os64_media_pace_init(&p, DEPTH, PERIOD);
    CHECK(!os64_media_pace_resync_take(&p), "nothing asked");
    judge(&p, 0, -1000000, true);
    CHECK(os64_media_pace_resync_take(&p) && !os64_media_pace_resync_take(&p), "asked once, taken once");
    os64_media_pace_reset(&p, 1);
    judge(&p, 2, -1000000, true);
    os64_media_pace_reset(&p, 3);
    CHECK(!os64_media_pace_resync_take(&p) && p.resyncs == 2, "a reset withdraws a resync not taken; still counted");
    os64_media_pace_init(&p, DEPTH, PERIOD);
    s.now = 0;
    s.sound_frames = 100;
    os64_media_pace_judge(&p, &s);
    os64_media_pace_reset(&p, 1);
    s.now = 2;
    CHECK(os64_media_pace_judge(&p, &s) == OS64_MEDIA_PACE_FAR_BEHIND && p.far_by_sound == 2,
          "a reset arms the sound's door again");

    // The doors scale with the depth.
    os64_media_pace_init(&p, 16, PERIOD);
    judge(&p, 0, 4799, true);
    CHECK(judge(&p, OS64_MEDIA_PACE_IN_HOLD_US, 4799, true) == OS64_MEDIA_PACE_DECODING_LATE,
          "sixteen deep: 4799 frames is under three eighths");
}

int main(void)
{
    pictures_edges();
    uint32_t capacities[] = {1, 2, 3, 8, 1000};
    for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++)
        pictures_run(capacities[i], 20000);
    clock_cases();
    clock_storm();
    for (int i = 0; i < 40; i++)
        clock_random(2000);
    pace_pick_cases();
    pace_door_cases();
    if (s_failures) {
        fprintf(stderr, "FAIL media cores: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS media cores: %d checks (the picture queue, the clock, the pace)\n", s_checks);
    return 0;
}
