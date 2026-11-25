// test_media_show_host.c — libmedia's show-thread core (media_show.c) on the
// host: the design's scenarios (VIDEO_PLANE.md §§ 4.1, 4.3, 4.4), then every
// rule held as a property over random sequences of passes.

#include "os64/media_show.h"

#include <stdio.h>
#include <stdlib.h>

static int s_failed;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            s_failed++;                                                                \
        }                                                                              \
    } while (0)

static uint64_t s_seed = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return (uint32_t)s_seed;
}

// A pass whose pick is of the size asked for.
static os64_media_show_input_t pass(int64_t now, bool placed, bool wanted, uint32_t w, uint32_t h, uint32_t serial)
{
    return (os64_media_show_input_t){.now = now, .placed = placed, .wanted = wanted, .have = true,
                                     .w = w, .h = h, .serial = serial, .target_w = w, .target_h = h};
}

// The capacity class the kernel gives a picture (os64/gui.h § Planes).
static void class_of(uint32_t w, uint32_t h, uint32_t *cw, uint32_t *ch)
{
    static const uint32_t classes[][2] = {{640, 360}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (unsigned i = 0; i < 5; i++)
        if (w <= classes[i][0] && h <= classes[i][1]) {
            *cw = classes[i][0];
            *ch = classes[i][1];
            return;
        }
    *cw = *ch = 0;
}

// The caller carries out a publish and a recreate as asked, every one taken.
static os64_media_show_act_t run(os64_media_show_t *s, os64_media_show_input_t in)
{
    os64_media_show_act_t a = os64_media_show_decide(s, &in);
    if (a.recreate) {
        uint32_t cw, ch;
        class_of(in.w, in.h, &cw, &ch);
        os64_media_show_recreated(s, cw, ch);
    }
    if (a.publish)
        a.look |= os64_media_show_published(s, true, in.w, in.h, in.serial);
    return a;
}

static void first_and_wanted(void)
{
    os64_media_show_t s;
    os64_media_show_init(&s, 640, 360);
    // The first picture is published, placed or not, wanted or not.
    os64_media_show_act_t a = run(&s, (os64_media_show_input_t){.have = true, .w = 480, .h = 270, .target_w = 480, .target_h = 270, .serial = 1});
    CHECK(a.publish && !a.recreate && s.ready && !s.stale && a.look);
    // Not placed, not wanted: the pictures go by the clock.
    a = run(&s, (os64_media_show_input_t){.have = true, .w = 480, .h = 270, .target_w = 480, .target_h = 270, .serial = 2});
    CHECK(!a.publish && s.published == 1);
    // Wanted: every new pick.
    a = run(&s, (os64_media_show_input_t){.wanted = true, .have = true, .w = 480, .h = 270, .target_w = 480, .target_h = 270, .serial = 3});
    CHECK(a.publish && s.published == 3);
    a = run(&s, (os64_media_show_input_t){.wanted = true, .have = true, .w = 480, .h = 270, .target_w = 480, .target_h = 270, .serial = 3});
    CHECK(!a.publish);   // the same pick twice is published once
    // Placed: every new pick of its size.
    a = run(&s, pass(0, true, false, 480, 270, 4));
    CHECK(a.publish && s.published == 4);
    // No pick: nothing.
    a = run(&s, (os64_media_show_input_t){.placed = true});
    CHECK(!a.publish && !a.end);
}

// § 4.4: a new size, through its five steps.
static void new_size(void)
{
    os64_media_show_t s;
    os64_media_show_init(&s, 640, 360);
    run(&s, pass(0, false, false, 480, 270, 1));   // the first picture, before the place
    CHECK(s.ready && s.pic_w == 480);
    // Step 2: a pick of the new size while placed: stale, nothing published.
    os64_media_show_act_t a = run(&s, pass(0, true, false, 1280, 720, 2));
    CHECK(!a.publish && s.stale && !s.ready && a.look);
    a = run(&s, pass(0, true, false, 1280, 720, 3));
    CHECK(!a.publish && s.stale && !a.look);   // stays stale, rung once
    // Step 4: the place empty (the caller hid it): a new plane of the bigger
    // class, then the current pick published; ready, and rung.
    a = run(&s, pass(0, false, false, 1280, 720, 4));
    CHECK(a.recreate && a.publish && s.cap_w == 1280 && s.ready && !s.stale && a.look && s.recreates == 1);
    // Step 5: wanted, then placed again: every pick of the new size.
    a = run(&s, pass(0, true, true, 1280, 720, 5));
    CHECK(a.publish && !a.recreate);
    // Asked for a new size: not ready until a picture of it is published,
    // even while the pick is still the old size.
    os64_media_show_input_t asked = pass(0, true, true, 1280, 720, 6);
    asked.target_w = 640;
    asked.target_h = 360;
    a = run(&s, asked);
    CHECK(!s.ready && a.look);
    // A smaller size fits the class: hidden, published with no recreate.
    run(&s, pass(0, true, false, 640, 360, 6));
    CHECK(s.stale);
    a = run(&s, pass(0, false, false, 640, 360, 7));
    CHECK(a.publish && !a.recreate && s.ready);
}

// § 4.3: closing ends after a hide, or at the bound with the plane placed.
static void closing(void)
{
    os64_media_show_t s;
    os64_media_show_init(&s, 640, 360);
    run(&s, pass(0, true, false, 480, 270, 1));
    os64_media_show_input_t in = pass(1000, true, false, 480, 270, 2);
    in.closing = true;
    os64_media_show_act_t a = run(&s, in);
    CHECK(!a.publish && !a.end && !a.destroy && s.closing && s.closing_since == 1000);
    in.now = 1000 + OS64_MEDIA_SHOW_CLOSE_BOUND_US - 1;
    a = run(&s, in);
    CHECK(!a.end);   // placed, before the bound: it waits
    in.placed = false;
    a = run(&s, in);
    CHECK(a.destroy && a.end && s.ended_placed == 0);

    os64_media_show_init(&s, 640, 360);
    in = pass(5000, true, false, 480, 270, 1);
    in.closing = true;
    a = run(&s, in);
    CHECK(!a.end);
    in.now = 5000 + OS64_MEDIA_SHOW_CLOSE_BOUND_US;
    a = run(&s, in);
    CHECK(a.end && !a.destroy && s.ended_placed == 1);   // the kernel's exit takes the plane out

    // A window gone: the end, nothing to destroy.
    os64_media_show_init(&s, 640, 360);
    a = run(&s, (os64_media_show_input_t){.gone = true, .placed = true});
    CHECK(a.end && !a.destroy && !a.publish);
}

// A publish the kernel refused leaves the plane as it was.
static void refused(void)
{
    os64_media_show_t s;
    os64_media_show_init(&s, 640, 360);
    os64_media_show_input_t in = pass(0, false, false, 480, 270, 1);
    os64_media_show_act_t a = os64_media_show_decide(&s, &in);
    CHECK(a.publish);
    CHECK(!os64_media_show_published(&s, false, 480, 270, 1) && s.pic_w == 0 && s.published == 0);
    a = os64_media_show_decide(&s, &in);
    CHECK(a.publish);   // owed again
    os64_media_show_recreated(&s, 0, 0);
    CHECK(s.cap_w == 0 && s.recreates == 0);

    // A new plane holds nothing, whatever the old one held: made, its first
    // publish refused, then the old size again is owed a publish.
    os64_media_show_init(&s, 640, 360);
    run(&s, pass(0, false, false, 480, 270, 1));
    in = pass(0, false, false, 1280, 720, 2);
    a = os64_media_show_decide(&s, &in);
    CHECK(a.recreate && a.publish);
    os64_media_show_recreated(&s, 1280, 720);
    os64_media_show_published(&s, false, 1280, 720, 2);
    in = pass(0, false, false, 480, 270, 3);
    a = os64_media_show_decide(&s, &in);
    CHECK(a.publish && !a.recreate && !s.ready);
}

// Every rule, as a property of random sequences.
static void properties(unsigned rounds)
{
    // Portrait and 4:3 among them: a side can outgrow the plane alone.
    static const uint32_t sizes[][2] = {{480, 270}, {640, 360}, {1280, 720}, {1920, 1080}, {3840, 2160},
                                        {720, 1280}, {1024, 768}, {360, 640}};
    for (unsigned r = 0; r < rounds; r++) {
        os64_media_show_t s;
        os64_media_show_init(&s, 640, 360);
        int64_t now = 0;
        uint32_t serial = 0;
        bool closing = false, ended = false;
        int64_t closing_at = 0;
        for (int i = 0; i < 200 && !ended; i++) {
            now += rnd() % 40000;
            if (rnd() % 3)
                serial++;
            unsigned k = rnd() % 8;
            if (!closing && rnd() % 100 == 0) {
                closing = true;
                closing_at = now;
            }
            os64_media_show_input_t in = {.now = now, .placed = rnd() % 2, .wanted = rnd() % 2,
                                          .closing = closing, .gone = rnd() % 500 == 0, .have = rnd() % 8 != 0,
                                          .w = sizes[k][0], .h = sizes[k][1], .serial = serial,
                                          .target_w = sizes[rnd() % 2 ? k : rnd() % 8][0]};
            in.target_h = in.target_w == in.w ? in.h : sizes[rnd() % 8][1];
            os64_media_show_t before = s;
            os64_media_show_act_t a = os64_media_show_decide(&s, &in);
            bool same = in.w == before.pic_w && in.h == before.pic_h;
            // A placed plane is never published another size, never destroyed,
            // never recreated.
            CHECK(!(in.placed && a.publish && !same));
            CHECK(!(in.placed && (a.destroy || a.recreate)));
            // Closing or gone: nothing published.
            CHECK(!((in.closing || in.gone) && a.publish));
            // A recreate only for a pick past the plane's capacity, and always
            // with its publish; and no publish of a picture past the capacity
            // without one (it would overrun the canvas).
            CHECK(!a.recreate || (a.publish && (in.w > before.cap_w || in.h > before.cap_h)));
            CHECK(!a.publish || a.recreate || (in.w <= before.cap_w && in.h <= before.cap_h));
            // Ready is the plane holding the size asked for.
            CHECK(in.closing || in.gone || s.ready == (before.pic_w == in.target_w && before.pic_h == in.target_h));
            // Placed, its size, a new pick: published.
            if (in.have && in.placed && !in.closing && !in.gone && same && in.serial != before.published)
                CHECK(a.publish);
            // Not placed, not wanted, not stale, holding its size: nothing.
            if (in.have && !in.placed && !in.wanted && !before.stale && same && before.pic_w != 0)
                CHECK(!a.publish);
            // The same pick is never published twice.
            CHECK(!(a.publish && in.serial == before.published && same));
            // Closing: no end before the bound while placed; at or past it, an end.
            if (in.closing && !in.gone && in.placed)
                CHECK(a.end == (now - (before.closing ? before.closing_since : now) >= OS64_MEDIA_SHOW_CLOSE_BOUND_US));
            if (in.closing && !in.gone && !in.placed)
                CHECK(a.end && a.destroy);
            // Rung exactly when stale or ready moved.
            CHECK(a.look == (s.stale != before.stale || s.ready != before.ready));
            if (a.recreate) {
                uint32_t cw, ch;
                class_of(in.w, in.h, &cw, &ch);
                os64_media_show_recreated(&s, cw, ch);
            }
            if (a.publish)
                os64_media_show_published(&s, rnd() % 10 != 0, in.w, in.h, in.serial);
            ended = a.end;
            (void)closing_at;
        }
    }
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    first_and_wanted();
    new_size();
    closing();
    refused();
    properties(rounds);
    if (s_failed != 0) {
        fprintf(stderr, "media show: %d failed\n", s_failed);
        return 1;
    }
    printf("media show: all cases passed (%u random sequences)\n", rounds);
    return 0;
}
