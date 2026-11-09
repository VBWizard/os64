// Explicit session-mutating fixture; run in an isolated GUI guest. It restores
// the initial usable theme before exiting, but consumes publication generations.
#include "os64/os64.h"
#include "os64/ui.h"
#include "os64/appearance.h"

static int failures;
#define CHECK(c) do { if (!(c)) { os64_complain("appearancetest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static int64_t snapshot(char *out, uint64_t *generation)
{
    int64_t fd = os64_open(OS64_APPEARANCE_PATH, "r");
    if (fd < 0) return -1;
    size_t used = 0;
    for (;;) {
        int64_t n = os64_read((int32_t)fd, out + used, OS64_APPEARANCE_MAX + 1 - used);
        if (n < 0 || n > (int64_t)(OS64_APPEARANCE_MAX - used)) {
            os64_close((int32_t)fd); return -1;
        }
        if (!n) break;
        used += (size_t)n;
    }
    os64_close((int32_t)fd);
    size_t body;
    if (!os64_appearance_header_read(out, used, generation, &body)) return -1;
    return (int64_t)used;
}
static int64_t command(const char *bytes, size_t n)
{
    int64_t fd = os64_open(OS64_APPEARANCE_PATH, "w");
    if (fd < 0) return fd;
    int64_t result = os64_write((int32_t)fd, bytes, n);
    os64_close((int32_t)fd);
    return result;
}
static bool receive(int64_t win, os64_ui_t *ui, uint64_t generation)
{
    for (unsigned tries = 0; tries < 100; ++tries) {
        os64_gui_event_t ev;
        while (os64_gui_event_poll(win, &ev) == 1) {
            if (ev.type != OS64_GUI_EVENT_APPEARANCE) continue;
            if (os64_gui_appearance_generation(&ev) != generation) return false;
            os64_ui_dispatch(ui, &ev);
            return true;
        }
        os64_sleep(10);
    }
    return false;
}
typedef struct {
    int64_t win;
    os64_ui_t *ui;
    uint64_t expected;
    unsigned ready;
} waiter_t;
static int64_t wait_for_appearance(void *arg)
{
    waiter_t *w = arg;
    __atomic_store_n(&w->ready, 1u, __ATOMIC_RELEASE);
    for (;;) {
        os64_gui_event_t ev;
        if (os64_gui_event_wait(w->win, &ev) != 1) return -1;
        if (ev.type != OS64_GUI_EVENT_APPEARANCE) continue;
        if (os64_gui_appearance_generation(&ev) != w->expected) return -1;
        os64_ui_dispatch(w->ui, &ev);
        return 0;
    }
}
int main(void)
{
    os64_ui_theme_t original, draft;
    os64_ui_theme_init(&original);
    draft = original;
    char before[OS64_APPEARANCE_MAX + 1], after[OS64_APPEARANCE_MAX + 1];
    uint64_t generation = 0, next = 0;
    int64_t n = snapshot(before, &generation);
    if (n < 0) { os64_complain("appearancetest: /sys/appearance unavailable\n"); return 1; }
    int64_t a = os64_gui_window_create("Appearance test A", 30, 40, 180, 100, 0);
    int64_t b = os64_gui_window_create("Appearance test B", 240, 40, 180, 100, 0);
    if (a <= 0 || b <= 0) {
        if (a > 0) os64_gui_window_destroy(a);
        if (b > 0) os64_gui_window_destroy(b);
        os64_complain("appearancetest: GUI required\n"); return 1;
    }
    os64_draw_ctx_t ac, bc;
    CHECK(os64_draw_ctx_init(&ac, a) == 0);
    CHECK(os64_draw_ctx_init(&bc, b) == 0);
    os64_ui_t au, bu;
    os64_ui_init(&au, &ac); os64_ui_init(&bu, &bc);
    au.theme.pad = 17; bu.theme.pad = 3;
    int64_t held = os64_open(OS64_APPEARANCE_PATH, "r");
    CHECK(held >= 0);
    CHECK(os64_open(OS64_APPEARANCE_PATH, "a") < 0);
    CHECK(os64_open("/sys/appearance/child", "r") < 0);
    CHECK(command("generation = -1\n", 16) < 0);
    CHECK(snapshot(after, &next) == n && next == generation);
    os64_dirent_t entry;
    CHECK(os64_stat(OS64_APPEARANCE_PATH, &entry) == 0 && entry.size == (uint64_t)n);
    int64_t dir = os64_opendir("/sys");
    bool listed = false;
    if (dir >= 0) {
        while (os64_readdir((int32_t)dir, &entry) == 1)
            if (os64_streq(entry.name, "appearance")) listed = true;
        os64_close((int32_t)dir);
    }
    CHECK(listed);
    waiter_t waiter = {.win=b, .ui=&bu, .expected=generation+1};
    int64_t thread = os64_thread(wait_for_appearance, &waiter);
    CHECK(thread >= 0);
    if (thread >= 0) {
        while (!__atomic_load_n(&waiter.ready, __ATOMIC_ACQUIRE)) os64_yield();
        os64_sleep(30);
    }
    os64_ui_theme_palette(&draft, OS64_UI_PALETTE_PAPER);
    draft.button_bevel = 3;
    CHECK(os64_ui_theme_apply(&draft, 3, &next) == 0 && next == generation + 1);
    CHECK(receive(a, &au, next));
    if (thread >= 0) {
        int64_t result = -1;
        CHECK(os64_thread_join((int32_t)thread, &result) == 0 && result == 0);
        os64_close((int32_t)thread);
    } else CHECK(receive(b, &bu, next));
    CHECK(au.appearance_generation == next && bu.appearance_generation == next);
    CHECK(au.theme.panel_bg == draft.panel_bg && bu.theme.panel_bg == draft.panel_bg);
    CHECK(au.theme.pad == 17 && bu.theme.pad == 3);
    if (held >= 0) {
        CHECK(os64_read((int32_t)held, after, sizeof(after)) == n);
        bool same = true;
        for (size_t i = 0; i < (size_t)n; ++i)
            if (before[i] != after[i]) same = false;
        CHECK(same);
        CHECK(os64_write((int32_t)held, before, (size_t)n) < 0);
        os64_close((int32_t)held);
    }
    // This writer's expected generation is stale; the active theme survives.
    CHECK(command(before, (size_t)n) < 0);
    CHECK(snapshot(after, &generation) > 0 && generation == next);
    // Kernel payload is opaque; libui must retain the last usable theme.
    size_t head = os64_appearance_header_write(before, generation);
    const char bad[] = "unsupported = 1\n";
    os64_memcpy(before + head, bad, sizeof(bad) - 1);
    CHECK(command(before, head + sizeof(bad) - 1) > 0);
    CHECK(receive(a, &au, generation + 1) && receive(b, &bu, generation + 1));
    CHECK(au.appearance_generation == generation && bu.appearance_generation == generation);
    CHECK(au.theme.panel_bg == draft.panel_bg && bu.theme.panel_bg == draft.panel_bg);
    CHECK(os64_ui_theme_apply(&original, 3, &next) == 0);
    CHECK(receive(a, &au, next) && receive(b, &bu, next));
    CHECK(au.theme.panel_bg == original.panel_bg && bu.theme.panel_bg == original.panel_bg);
    os64_gui_window_destroy(a); os64_gui_window_destroy(b);
    os64_complain("appearancetest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
