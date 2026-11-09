// Run in an isolated GUI guest. No input injection is needed: resize events
// fill the input ring, and sibling threads exercise the blocking wait path.
#include "os64/os64.h"
#include "os64/ui.h"

#define DOORBELL_OK   0x600B0000
#define DOORBELL_FAIL 0x600B0001
#define DOORBELL_SKIP 0x600B0002

static unsigned failures;
#define CHECK(c) do { if (!(c)) { os64_printf("doorbelltest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

typedef struct {
    int64_t win;
    uint32_t mask;
    bool finished;
} notification_t;

static int64_t notify(void *arg)
{
    notification_t *n = arg;
    os64_sleep(100);
    return os64_gui_event_ring(n->win, n->mask);
}

// Turn a lost wake into a failed wait instead of a permanently hung fixture.
static int64_t watchdog(void *arg)
{
    notification_t *n = arg;
    for (unsigned i = 0; i < 200; ++i) {
        if (__atomic_load_n(&n->finished, __ATOMIC_ACQUIRE)) return 0;
        os64_sleep(100);
    }
    os64_gui_window_destroy(n->win);
    return -1;
}

static void join_worker(int64_t h)
{
    if (h < 0) return;
    int64_t result = -1;
    CHECK(os64_thread_join((int32_t)h, &result) == 0 && result == 0);
    CHECK(os64_close((int32_t)h) == 0);
}

static uint32_t drain(int64_t win, unsigned *bells)
{
    os64_gui_event_t ev;
    uint32_t mask = 0;
    *bells = 0;
    while (os64_gui_event_poll(win, &ev) == 1) {
        if (ev.type != OS64_GUI_EVENT_DOORBELL) continue;
        ++*bells;
        mask |= ev.doorbell.mask;
    }
    return mask;
}

static void on_doorbell(os64_ui_t *ui, const os64_gui_event_t *ev)
{
    CHECK(ev->type == OS64_GUI_EVENT_DOORBELL && ev->doorbell.mask == 16);
    ui->quit = true;
}

static void wait_case(int64_t win, bool peek, bool toolkit)
{
    unsigned bells;
    drain(win, &bells);
    notification_t n = {.win = win, .mask = toolkit ? 16 : peek ? 8 : 4};
    int64_t guard = os64_thread(watchdog, &n);
    CHECK(guard >= 0);
    if (guard < 0) return;
    int64_t worker = os64_thread(notify, &n);
    CHECK(worker >= 0);
    if (worker >= 0) {
        if (toolkit) {
            os64_ui_t ui = {0};
            ui.on_doorbell = on_doorbell;
            os64_ui_run(&ui, win, NULL);
            CHECK(ui.quit);
        } else {
            os64_gui_event_t ev;
            bool received = false;
            for (;;) {
                int64_t rc = os64_gui_event_wait(win, peek ? NULL : &ev);
                if (rc == OS64_INTERRUPTED) continue;
                CHECK(rc == 1);
                if (rc != 1) break;
                if (peek) {
                    rc = os64_gui_event_poll(win, &ev);
                    CHECK(rc == 1);
                    if (rc != 1) break;
                }
                if (ev.type != OS64_GUI_EVENT_DOORBELL) continue;
                CHECK(ev.doorbell.mask == n.mask);
                received = true;
                break;
            }
            CHECK(received);
        }
    }
    __atomic_store_n(&n.finished, true, __ATOMIC_RELEASE);
    join_worker(worker);
    join_worker(guard);
    CHECK(drain(win, &bells) == 0 && bells == 0);
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "--foreign")) {
        int64_t win = 0;
        for (const char *p = argv[2]; *p; ++p) win = win * 10 + (*p - '0');
        return os64_gui_event_ring(win, 1) == OS64_GUI_ERR_NOT_OWNER ? 0 : 1;
    }
    int64_t win = os64_gui_window_create("Doorbell test", 32, 32, 160, 120, 0);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("doorbelltest: SKIP (requires GUI)\n");
        return DOORBELL_SKIP;
    }
    CHECK(win > 0);
    if (win <= 0) return DOORBELL_FAIL;
    unsigned bells;
    drain(win, &bells);
    CHECK(os64_gui_event_ring(0, 1) == OS64_GUI_ERR_INVALID_HANDLE);
    CHECK(os64_gui_event_ring(-1, 1) == OS64_GUI_ERR_INVALID_HANDLE);
    CHECK(os64_gui_event_ring(win, 0) == OS64_GUI_ERR_BAD_ARGS);
    CHECK((int64_t)os64_syscall2(SYSCALL_GUI_EVENT_RING, (uint64_t)win,
          (1ull << 32) | 1) == OS64_GUI_ERR_BAD_ARGS);
    char number[24];
    os64_snprintf(number, sizeof(number), "%ld", win);
    char *args[] = {"/tests/doorbelltest", "--foreign", number, NULL};
    int64_t child = os64_spawn(args[0], args);
    int32_t code = -1;
    CHECK(child > 0);
    if (child > 0) CHECK(os64_wait(child, &code) == child && code == 0);
    CHECK(drain(win, &bells) == 0 && bells == 0);

    // More successful resizes than the ring can hold, without polling.
    for (unsigned i = 0; i < 100; ++i)
        CHECK(os64_gui_window_set_min_size(win, 200 + i, 120) == 0);
    for (unsigned i = 0; i < 1000; ++i)
        CHECK(os64_gui_event_ring(win, 1u << (i % 3)) == 0);
    CHECK(drain(win, &bells) == 7 && bells == 1);
    CHECK(os64_gui_event_ring(win, 0x80000000u) == 0);
    CHECK(drain(win, &bells) == 0x80000000u && bells == 1);
    wait_case(win, false, false);
    wait_case(win, true, false);
    wait_case(win, false, true);
    CHECK(os64_gui_window_destroy(win) == 0);
    CHECK(os64_gui_event_ring(win, 1) == OS64_GUI_ERR_INVALID_HANDLE);
    os64_printf("doorbelltest: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? DOORBELL_FAIL : DOORBELL_OK;
}
