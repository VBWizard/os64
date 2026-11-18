// frametest — the display's frame for ring 3 (FRAME_CLOCK.md § 6):
// os64_gui_frame_request (syscall 69), its answer OS64_GUI_EVENT_FRAME, and
// os64_gui_frame_take (70), as a program sees them.
//
//   - a request is answered within a bound, with a period;
//   - the frame numbers rise from one answer to the next;
//   - asking twice before the answer is one request: one answer;
//   - OS64_GUI_FRAME_IF_DRAWN answers IDLE, and asks nothing, until the
//     window publishes, and then asks;
//   - an unknown flag is BAD_ARGS, a window that is not there is
//     INVALID_HANDLE, and taking with nothing waiting is 0;
//   - take returns the answer and leaves the program's other events queued;
//   - a program that asks and never reads holds at most one answer, and no
//     input is lost for it;
//   - a window that asks and dies with its task, its request unanswered,
//     leaves the frames working for everyone else: this window's next
//     request is still answered. (A request is a flag on its window and
//     goes with it, so there is nothing to leak; what could go wrong is
//     the teardown under a waiting request.)
//
// A covered window is answered nothing until it is shown (the design's
// rule), so the fixture SKIPS when its window is covered (a text terminal
// holding the screen, say) rather than waiting for a frame that is rightly
// never coming.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"

#define FRAME_OK   0xF4A30000
#define FRAME_FAIL 0xF4A30001
#define FRAME_SKIP 0xF4A30002

#define ANSWER_BOUND_MS 2000   // a clock-off boot answers at its passes' pace

static int failures;
#define CHECK(c) do { if (!(c)) { os64_printf("frametest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static bool covered(int64_t win)
{
    os64_gui_window_state_t st;
    return os64_gui_window_get_state(win, &st) == 0 && (st.flags & OS64_GUI_WINDOW_COVERED);
}

// Poll the queue until a frame answer comes or the bound passes, counting
// what else was seen.
static bool wait_answer(int64_t win, os64_gui_event_t *out, uint32_t bound_ms, uint32_t *others)
{
    int64_t deadline = os64_micros() + (int64_t)bound_ms * 1000;
    os64_gui_event_t ev;
    while (os64_micros() < deadline) {
        while (os64_gui_event_poll(win, &ev) == 1) {
            if (ev.type == OS64_GUI_EVENT_FRAME) {
                *out = ev;
                return true;
            }
            if (others)
                (*others)++;
        }
        os64_sleep(5);
    }
    return false;
}

static uint32_t frames_waiting(int64_t win, uint32_t *lost)
{
    uint32_t n = 0;
    os64_gui_event_t ev;
    while (os64_gui_event_poll(win, &ev) == 1) {
        if (ev.type == OS64_GUI_EVENT_FRAME)
            n++;
        else if (ev.type == OS64_GUI_EVENT_INPUT_LOST && lost)
            (*lost)++;
    }
    return n;
}

static void publish(os64_draw_ctx_t *ctx, uint32_t color)
{
    os64_draw_fill_rect(&ctx->surf, (os64_gui_rect_t){ 0, 0, (int32_t)ctx->surf.width,
                        (int32_t)ctx->surf.height }, color);
    os64_draw_publish(ctx, NULL);
}

int main(int argc, char **argv)
{
    // The orphan: ask, and exit without reading or destroying anything.
    if (argc == 2 && os64_streq(argv[1], "--orphan")) {
        int64_t w = os64_gui_window_create("frametest orphan", 60, 60, 160, 80, 0);
        if (w <= 0)
            return 1;
        return os64_gui_frame_request(w, 0) == 0 ? 0 : 1;
    }

    int64_t win = os64_gui_window_create("frametest", 40, 40, 240, 120, 0);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("frametest: SKIP (requires GUI)\n");
        return FRAME_SKIP;
    }
    CHECK(win > 0);
    if (win <= 0)
        return FRAME_FAIL;
    os64_draw_ctx_t ctx;
    CHECK(os64_draw_ctx_init(&ctx, win) == 0);
    publish(&ctx, 0xFF204060U);
    os64_sleep(100);   // the cover sweep's verdict on the new window
    if (covered(win)) {
        os64_printf("frametest: SKIP (its window is covered: nobody to show a frame to)\n");
        os64_gui_window_destroy(win);
        return FRAME_SKIP;
    }
    (void)frames_waiting(win, NULL);

    // Answered, with a period.
    os64_gui_event_t a, b;
    CHECK(os64_gui_frame_request(win, 0) == 0);
    bool got = wait_answer(win, &a, ANSWER_BOUND_MS, NULL);
    CHECK(got);
    if (!got) {
        os64_gui_window_destroy(win);
        return FRAME_FAIL;
    }
    CHECK(a.frame.period_ns > 1000000 && a.frame.period_ns < 100000000);
    CHECK(os64_gui_frame_blank_us(&a) > 0 && os64_gui_frame_blank_us(&a) <= (uint64_t)os64_micros());

    // The numbers rise.
    CHECK(os64_gui_frame_request(win, 0) == 0);
    CHECK(wait_answer(win, &b, ANSWER_BOUND_MS, NULL));
    CHECK(os64_gui_frame_number(&b) > os64_gui_frame_number(&a));
    CHECK(os64_gui_frame_blank_us(&b) > os64_gui_frame_blank_us(&a));

    // Asking twice is one request.
    CHECK(os64_gui_frame_request(win, 0) == 0);
    CHECK(os64_gui_frame_request(win, 0) == 0);
    CHECK(wait_answer(win, &a, ANSWER_BOUND_MS, NULL));
    os64_sleep(200);
    CHECK(frames_waiting(win, NULL) == 0);

    // IF_DRAWN: nothing published since the last request, so IDLE and
    // nothing asked; after a publish, asked.
    CHECK(os64_gui_frame_request(win, OS64_GUI_FRAME_IF_DRAWN) == OS64_GUI_FRAME_IDLE);
    os64_sleep(100);
    CHECK(frames_waiting(win, NULL) == 0);
    publish(&ctx, 0xFF406020U);
    CHECK(os64_gui_frame_request(win, OS64_GUI_FRAME_IF_DRAWN) == 0);
    CHECK(wait_answer(win, &a, ANSWER_BOUND_MS, NULL));

    // Refusals, and an empty take.
    CHECK(os64_gui_frame_request(win, 2) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_frame_request(0, 0) == OS64_GUI_ERR_INVALID_HANDLE);
    CHECK(os64_gui_frame_take(win, &a) == 0);
    CHECK(os64_gui_frame_take(0, &a) == OS64_GUI_ERR_INVALID_HANDLE);

    // Take returns the answer and leaves the program's other events queued.
    CHECK(os64_gui_event_ring(win, 0x5) == 0);
    CHECK(os64_gui_frame_request(win, 0) == 0);
    int64_t deadline = os64_micros() + ANSWER_BOUND_MS * 1000;
    int64_t took = 0;
    while (took == 0 && os64_micros() < deadline) {
        took = os64_gui_frame_take(win, &a);
        if (took == 0)
            os64_sleep(5);
    }
    CHECK(took == 1 && a.type == OS64_GUI_EVENT_FRAME);
    os64_gui_event_t ev;
    CHECK(os64_gui_event_poll(win, &ev) == 1 && ev.type == OS64_GUI_EVENT_DOORBELL && ev.doorbell.mask == 0x5);
    CHECK(frames_waiting(win, NULL) == 0);

    // A program that asks and never reads: one answer waits, the newest.
    for (int i = 0; i < 60; i++) {
        CHECK(os64_gui_frame_request(win, 0) == 0);
        os64_sleep(20);
    }
    os64_sleep(100);
    uint32_t lost = 0;
    CHECK(os64_gui_frame_take(win, &a) == 1);
    CHECK(frames_waiting(win, &lost) == 0);
    CHECK(lost == 0);

    // A window that asks and dies with its task, unanswered: the frames
    // still work for this one.
    char *args[] = { "/tests/frametest", "--orphan", NULL };
    int64_t child = os64_spawn(args[0], args);
    int32_t code = -1;
    CHECK(child > 0);
    if (child > 0)
        CHECK(os64_wait(child, &code) == child && code == 0);
    publish(&ctx, 0xFF602040U);
    CHECK(os64_gui_frame_request(win, 0) == 0);
    CHECK(wait_answer(win, &a, ANSWER_BOUND_MS, NULL));

    os64_gui_window_destroy(win);
    if (failures) {
        os64_printf("frametest: %d check(s) failed\n", failures);
        return FRAME_FAIL;
    }
    os64_printf("frametest: PASS\n");
    return FRAME_OK;
}
