// Run in an isolated GUI guest. --hold leaves an interactive resize fixture;
// the default invocation checks the syscall and destroys its window.
#include "os64/os64.h"
#include "os64/draw.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { os64_printf("windowmintest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static unsigned resize_events(int64_t win)
{
    unsigned n = 0;
    os64_gui_event_t ev;
    while (os64_gui_event_poll(win, &ev) == 1)
        if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE) ++n;
    return n;
}

static void paint(os64_draw_ctx_t *ctx, unsigned width, unsigned height)
{
    os64_gui_window_state_t st;
    CHECK(os64_draw_ctx_refresh(ctx) == 0);
    CHECK(os64_gui_window_get_state(ctx->win, &st) == 0);
    CHECK(ctx->surf.width >= width && ctx->surf.height >= height);
    os64_printf("windowmintest: content %ux%u minimum %ux%u frame %d,%d %ux%u flags %u\n",
                ctx->surf.width, ctx->surf.height, width, height,
                st.x, st.y, st.width, st.height, st.flags);
    os64_draw_fill_rect(&ctx->surf, (os64_gui_rect_t){0, 0, (int32_t)ctx->surf.width, (int32_t)ctx->surf.height}, 0xff172334);
    char text[128];
    os64_snprintf(text, sizeof(text), "Content: %ux%u    Minimum: %ux%u", ctx->surf.width, ctx->surf.height, width, height);
    os64_draw_text(&ctx->surf, 16, 24, text, os64_strlen(text), 0xffeeeeee, 0xff172334);
    const char *help = "Ctrl+Alt+right-drag; M maximize; T titlebar (with Ctrl+Alt)";
    os64_draw_text(&ctx->surf, 16, 56, help, os64_strlen(help), 0xffeeeeee, 0xff172334);
    help = "r: reset   l: 640x480   s: 958x696   g: set large after 3s   q: quit";
    os64_draw_text(&ctx->surf, 16, 88, help, os64_strlen(help), 0xffeeeeee, 0xff172334);
    CHECK(os64_gui_window_publish(ctx->win, NULL) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "--foreign")) {
        int64_t win = 0;
        for (const char *p = argv[2]; *p; ++p) win = win * 10 + (*p - '0');
        return os64_gui_window_set_min_size(win, 640, 480) == OS64_GUI_ERR_NOT_OWNER ? 0 : 1;
    }
    uint32_t screen_w, screen_h;
    if (os64_gui_screen_info(&screen_w, &screen_h) != 0 || screen_w < 640 || screen_h < 480) {
        os64_printf("windowmintest: SKIP (requires GUI and at least 640x480)\n");
        return 0;
    }
    int64_t win = os64_gui_window_create("Minimum size test", 32, 24, 360, 240, 0);
    CHECK(win > 0);
    if (win <= 0) return 1;
    os64_draw_ctx_t ctx;
    CHECK(os64_draw_ctx_init(&ctx, win) == 0);
    if (failures) { os64_gui_window_destroy(win); return 1; }
    uint32_t *pixels = ctx.surf.pixels;
    uint32_t pitch = ctx.surf.pitch_px;
    CHECK(os64_gui_window_set_min_size(0, 640, 480) == OS64_GUI_ERR_INVALID_HANDLE);
    char number[24];
    os64_snprintf(number, sizeof(number), "%ld", win);
    char *args[] = {"/tests/windowmintest", "--foreign", number, NULL};
    int64_t child = os64_spawn(args[0], args);
    int32_t code = -1;
    CHECK(child > 0);
    if (child > 0) CHECK(os64_wait(child, &code) == child && code == 0);
    CHECK(os64_draw_ctx_refresh(&ctx) == 0 && ctx.surf.width == 358 && ctx.surf.height == 219);
    resize_events(win);
    CHECK(os64_gui_window_set_min_size(win, 640, 480) == 0);
    CHECK(resize_events(win) == 1);
    CHECK(os64_draw_ctx_refresh(&ctx) == 0);
    CHECK(ctx.surf.width == 640 && ctx.surf.height == 480);
    CHECK(ctx.surf.pixels == pixels && ctx.surf.pitch_px == pitch);
    CHECK(os64_gui_window_set_min_size(win, 640, 480) == 0 && resize_events(win) == 0);
    CHECK(os64_gui_window_set_min_size(win, UINT32_MAX, 500) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_min_size(win, 700, UINT32_MAX) == OS64_GUI_ERR_BAD_ARGS);
    CHECK((int64_t)os64_syscall3(SYSCALL_GUI_WINDOW_SET_MIN_SIZE, (uint64_t)win,
                                (1ULL << 32) | 700, 500) == OS64_GUI_ERR_BAD_ARGS);
    CHECK((int64_t)os64_syscall3(SYSCALL_GUI_WINDOW_SET_MIN_SIZE, (uint64_t)win,
                                700, (1ULL << 32) | 500) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_draw_ctx_refresh(&ctx) == 0 && ctx.surf.width == 640 && ctx.surf.height == 480);
    CHECK(resize_events(win) == 0);
    CHECK(os64_gui_window_set_min_size(win, 0, 0) == 0);
    CHECK(os64_draw_ctx_refresh(&ctx) == 0 && ctx.surf.width == 640 && ctx.surf.height == 480);
    CHECK(resize_events(win) == 0);
    unsigned min_w = 64, min_h = 32;
    if (argc > 1 && os64_streq(argv[1], "--hold") && screen_w >= 958 && screen_h >= 696) {
        CHECK(os64_gui_window_set_min_size(win, min_w = 958, min_h = 696) == 0);
        paint(&ctx, min_w, min_h);
        bool quit = false;
        uint8_t client_buttons = 0;
        unsigned presses = 0, releases = 0;
        while (!quit) {
            os64_gui_event_t ev;
            int64_t rc = os64_gui_event_wait(win, &ev);
            if (rc == OS64_INTERRUPTED) continue;
            if (rc != 1) { CHECK(false); break; }
            if (ev.type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN) {
                client_buttons |= (uint8_t)(1u << ev.mouse.button);
                ++presses;
            }
            if (ev.type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) {
                // WM-owned presses must not produce client-only releases.
                CHECK(client_buttons & (1u << ev.mouse.button));
                client_buttons &= (uint8_t)~(1u << ev.mouse.button);
                ++releases;
            }
            if (ev.type == OS64_GUI_EVENT_WINDOW_CLOSE) quit = true;
            if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE) paint(&ctx, min_w, min_h);
            if (ev.type != OS64_GUI_EVENT_KEY_DOWN) continue;
            char c = ev.key.ascii;
            if (c == 'q') quit = true;
            if (c == 'd') {
                // Let the driver begin a WM resize, then replace its target.
                os64_sleep(3000);
                CHECK(os64_gui_window_destroy(win) == 0);
                win = os64_gui_window_create("Replacement window", 32, 24, 960, 717, 0);
                CHECK(win > 0);
                if (win <= 0) return 1;
                CHECK(os64_draw_ctx_init(&ctx, win) == 0);
                min_w = 64; min_h = 32; client_buttons = 0;
                paint(&ctx, min_w, min_h);
                continue;
            }
            if (c != 'r' && c != 'l' && c != 's' && c != 'g') continue;
            if (c == 'g') os64_sleep(3000);
            min_w = c == 'r' ? 64 : c == 'l' ? 640 : 958;
            min_h = c == 'r' ? 32 : c == 'l' ? 480 : 696;
            CHECK(os64_gui_window_set_min_size(win, min_w, min_h) == 0);
            paint(&ctx, min_w, min_h);
        }
        os64_printf("windowmintest: client mouse presses %u releases %u\n", presses, releases);
    }
    CHECK(os64_gui_window_destroy(win) == 0);
    CHECK(os64_gui_window_set_min_size(win, 0, 0) == OS64_GUI_ERR_INVALID_HANDLE);
    os64_printf("windowmintest: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
