// test_strip_host.c: yonder's strip states (userland/apps/yonder/strip.c),
// on the host under ASan and UBSan: the ruled table as named cases, then
// random sequences of inputs, moves and times against a model written from
// VIDEO_V6_DESIGN.md § 2 (with Fable's answers) apart from the code.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../userland/apps/yonder/strip.h"

static int s_failures, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); if (s_failures > 20) exit(1); } } while (0)

static yonder_strip_t fresh(void)
{
    yonder_strip_t s;
    yonder_strip_init(&s);
    return s;
}

static void cases(void)
{
    yonder_strip_t s = fresh();
    CHECK(!yonder_strip_drawn(&s), "a strip made is hidden until it settles");
    s.paused = true;
    CHECK(yonder_strip_settle(&s, 0) && s.state == YONDER_STRIP_PINNED, "paused pins it");
    CHECK(yonder_strip_next(&s) == UINT64_MAX, "pinned arms nothing");

    // Play with the pointer outside: hidden at once.
    s.paused = false;
    CHECK(yonder_strip_settle(&s, 10) && !yonder_strip_drawn(&s), "playing, pointer outside: hidden");

    // A move over the box: shown 3 s from the move, then hidden.
    s.over_box = true;
    yonder_strip_moved(&s, 100);
    CHECK(!yonder_strip_settle(&s, 100) && s.state == YONDER_STRIP_TIMED, "a move shows it (it was shown by the move)");
    CHECK(yonder_strip_next(&s) == 100 + YONDER_STRIP_IDLE_MS, "timed arms the deadline");
    CHECK(!yonder_strip_settle(&s, 100 + YONDER_STRIP_IDLE_MS - 1) && yonder_strip_drawn(&s), "shown up to the deadline");
    CHECK(yonder_strip_settle(&s, 100 + YONDER_STRIP_IDLE_MS) && !yonder_strip_drawn(&s), "hidden at the deadline");
    CHECK(yonder_strip_next(&s) == UINT64_MAX, "hidden arms nothing");

    // Another move puts it back; leaving the box hides it at once.
    yonder_strip_moved(&s, 5000);
    yonder_strip_settle(&s, 5000);
    s.over_box = false;
    CHECK(yonder_strip_settle(&s, 5001) && !yonder_strip_drawn(&s), "leaving the box hides it");
    CHECK(yonder_strip_next(&s) == UINT64_MAX, "outside the box, nothing is armed");

    // Buffering holds a shown strip past its deadline, and never shows a hidden one.
    s = fresh();
    s.over_box = true;
    yonder_strip_moved(&s, 0);
    yonder_strip_settle(&s, 0);
    s.buffering = true;
    CHECK(!yonder_strip_settle(&s, YONDER_STRIP_IDLE_MS + 50) && yonder_strip_drawn(&s), "buffering holds it past its deadline");
    CHECK(yonder_strip_next(&s) == YONDER_STRIP_IDLE_MS + 50 + YONDER_STRIP_MARK_MS, "while buffering only the mark is armed");
    s.buffering = false;
    CHECK(yonder_strip_settle(&s, YONDER_STRIP_IDLE_MS + 70) && !yonder_strip_drawn(&s), "buffering over: hidden");
    s = fresh();
    s.over_box = true;
    yonder_strip_moved(&s, 0);
    yonder_strip_settle(&s, 0);
    s.buffering = true;
    s.over_box = false;
    CHECK(yonder_strip_settle(&s, 10) && !yonder_strip_drawn(&s), "leaving the box hides it, buffering or not");
    s.buffering = true;
    yonder_strip_settle(&s, 9000);
    CHECK(!yonder_strip_drawn(&s), "buffering never shows a hidden strip");

    // The pins: over the strip, focus, a drag (even outside the box).
    s = fresh();
    s.over_strip = true;
    yonder_strip_settle(&s, 0);
    CHECK(s.state == YONDER_STRIP_PINNED, "the pointer over the strip pins it");
    s.over_strip = false;
    s.focused = true;
    yonder_strip_settle(&s, 0);
    CHECK(s.state == YONDER_STRIP_PINNED, "focus pins it");
    s.focused = false;
    s.dragging = true;
    s.over_box = false;
    yonder_strip_settle(&s, 0);
    CHECK(s.state == YONDER_STRIP_PINNED, "a drag that left the box stays pinned");
    s.dragging = false;
    CHECK(yonder_strip_settle(&s, 1) && !yonder_strip_drawn(&s), "released outside the box: hidden");

    // Unpinned with the pointer inside: timed from the last move.
    s = fresh();
    s.paused = true;
    s.over_box = true;
    yonder_strip_moved(&s, 1000);
    yonder_strip_settle(&s, 1000);
    s.paused = false;               // a click on play, at 2000
    yonder_strip_moved(&s, 2000);
    yonder_strip_settle(&s, 2000);
    CHECK(s.state == YONDER_STRIP_TIMED && s.deadline == 2000 + YONDER_STRIP_IDLE_MS, "a play pressed: timed from the press");
    s = fresh();
    s.paused = true;
    s.over_box = true;
    yonder_strip_moved(&s, 0);
    yonder_strip_settle(&s, 0);
    s.paused = false;               // played by script long after the last move
    CHECK(yonder_strip_settle(&s, 10000) && !yonder_strip_drawn(&s), "unpinned past the deadline: hidden at once");

    // An autoplay with no pointer: never drawn.
    s = fresh();
    for (uint64_t t = 0; t < 20000; t += 250)
        CHECK(!yonder_strip_settle(&s, t) && !yonder_strip_drawn(&s), "an autoplay nobody hovered is never drawn");

    // The mark: half a second of buffering shows it; less shows nothing.
    s = fresh();
    s.buffering = true;
    CHECK(!yonder_strip_settle(&s, 100) && s.mark == YONDER_MARK_PENDING, "buffering: the mark pending");
    CHECK(!yonder_strip_settle(&s, 100 + YONDER_STRIP_MARK_MS - 1) && s.mark == YONDER_MARK_PENDING, "not yet");
    CHECK(yonder_strip_settle(&s, 100 + YONDER_STRIP_MARK_MS) && s.mark == YONDER_MARK_SHOWN, "shown at half a second");
    s.buffering = false;
    CHECK(yonder_strip_settle(&s, 2000) && s.mark == YONDER_MARK_NONE, "gone when buffering ends");
    s.buffering = true;
    yonder_strip_settle(&s, 3000);
    s.buffering = false;
    CHECK(!yonder_strip_settle(&s, 3200) && s.mark == YONDER_MARK_NONE, "a 200 ms buffering shows nothing");
}

// The model: the rules as the table states them, kept apart from strip.c.
typedef struct {
    int state;              // 0 hidden, 1 timed, 2 pinned
    uint64_t last_move;
    bool moved;
    int mark;               // 0 none, 1 pending, 2 shown
    uint64_t buffering_since;
} model_t;

static void model_settle(model_t *m, const yonder_strip_t *in, uint64_t now)
{
    bool pin = in->paused || in->over_strip || in->focused || in->dragging;
    if (pin)
        m->state = 2;
    else if (m->state != 0) {
        bool expired = !m->moved || now >= m->last_move + YONDER_STRIP_IDLE_MS;
        m->state = (!in->over_box || (!in->buffering && expired)) ? 0 : 1;
    }
    if (!in->buffering)
        m->mark = 0;
    else {
        if (m->mark == 0) {
            m->mark = 1;
            m->buffering_since = now;
        }
        if (m->mark == 1 && now >= m->buffering_since + YONDER_STRIP_MARK_MS)
            m->mark = 2;
    }
}

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

static void random_rounds(int rounds)
{
    for (int r = 0; r < rounds; r++) {
        yonder_strip_t s = fresh();
        model_t m = {0};
        uint64_t now = 0;
        for (int step = 0; step < 200; step++) {
            now += rnd() % 1500;
            switch (rnd() % 8) {
                case 0: s.paused = !s.paused; break;
                case 1: s.over_box = !s.over_box; if (!s.over_box) s.over_strip = false; break;
                case 2: if (s.over_box && yonder_strip_drawn(&s)) s.over_strip = !s.over_strip; break;
                case 3: s.focused = !s.focused; break;
                case 4: s.dragging = !s.dragging; break;
                case 5: s.buffering = !s.buffering; break;
                default:
                    if (s.over_box) {
                        yonder_strip_moved(&s, now);
                        m.last_move = now;
                        m.moved = true;
                        if (m.state == 0)
                            m.state = 1;
                    }
                    break;
            }
            int was_state = m.state != 0;
            int was_mark = m.mark == 2;
            model_settle(&m, &s, now);
            bool changed = yonder_strip_settle(&s, now);
            CHECK((int)s.state == m.state, "round %d step %d: state %d, model %d", r, step, (int)s.state, m.state);
            CHECK((int)s.mark == m.mark, "round %d step %d: mark %d, model %d", r, step, (int)s.mark, m.mark);
            CHECK(changed == ((was_state != (m.state != 0)) || (was_mark != (m.mark == 2))),
                  "round %d step %d: settle's answer", r, step);
            uint64_t next = yonder_strip_next(&s);
            uint64_t want = UINT64_MAX;
            if (m.state == 1 && !s.buffering && s.over_box)
                want = m.last_move + YONDER_STRIP_IDLE_MS;
            if (m.mark == 1 && m.buffering_since + YONDER_STRIP_MARK_MS < want)
                want = m.buffering_since + YONDER_STRIP_MARK_MS;
            CHECK(next == want, "round %d step %d: next %llu, model %llu", r, step, (unsigned long long)next,
                  (unsigned long long)want);
        }
    }
}

int main(int argc, char **argv)
{
    cases();
    random_rounds(argc > 1 ? atoi(argv[1]) : 2000);
    if (s_failures) {
        fprintf(stderr, "FAIL strip: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS strip: %d checks\n", s_checks);
    return 0;
}
