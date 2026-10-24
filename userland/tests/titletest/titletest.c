// titletest — os64_gui_window_set_title (syscall 65): an owner renames its
// window, the encoding flag it passes is the one get_state reports after, the
// geometry stays put, and every refusal create makes of a title is made here
// too, leaving the encoding (the half ring 3 can read back) as it was.
// Another task's window is NOT_OWNER. Skips on a boot without the GUI.
//
// `titletest --hold` is the eyes-on half: it opens a window named
// "titletest: before", renames it to "titletest: AFTER" three seconds later,
// and stays up until it is closed or `q` is pressed, so a screenshot can show
// the compositor drawing the new name.
#include "os64/os64.h"
#include "os64/draw.h"

#define TITLE_OK   0x711E0000
#define TITLE_FAIL 0x711E0001
#define TITLE_SKIP 0x711E0002

static unsigned failures;
#define CHECK(c) do { if (!(c)) { os64_printf("titletest: FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static uint32_t state_flags(int64_t win)
{
    os64_gui_window_state_t st;
    CHECK(os64_gui_window_get_state(win, &st) == 0);
    return st.flags;
}

static bool same_place(int64_t win, const os64_gui_window_state_t *was)
{
    os64_gui_window_state_t st;
    return os64_gui_window_get_state(win, &st) == 0 && st.x == was->x && st.y == was->y &&
           st.width == was->width && st.height == was->height;
}

// Events a rename must not cause: a resize or a placement.
static unsigned geometry_events(int64_t win)
{
    unsigned n = 0;
    os64_gui_event_t ev;
    while (os64_gui_event_poll(win, &ev) == 1)
        if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE || ev.type == OS64_GUI_EVENT_WINDOW_PLACEMENT)
            ++n;
    return n;
}

static int hold(int64_t win)
{
    os64_draw_ctx_t ctx;
    if (os64_draw_ctx_init(&ctx, win) != 0) {
        os64_gui_window_destroy(win);
        return TITLE_FAIL;
    }
    const char *text = "The title bar is renamed in 3 seconds.";
    os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){0, 0, (int32_t)ctx.surf.width, (int32_t)ctx.surf.height}, 0xff172334);
    os64_draw_text(&ctx.surf, 16, 24, text, os64_strlen(text), 0xffeeeeee, 0xff172334);
    os64_gui_window_publish(win, NULL);
    os64_sleep(3000);
    int64_t rc = os64_gui_window_set_title(win, "titletest: AFTER", 0);
    os64_printf("titletest: renamed (%ld)\n", rc);
    for (;;) {
        os64_gui_event_t ev;
        int64_t got = os64_gui_event_wait(win, &ev);
        if (got == OS64_INTERRUPTED)
            continue;
        if (got != 1 || ev.type == OS64_GUI_EVENT_WINDOW_CLOSE ||
            (ev.type == OS64_GUI_EVENT_KEY_DOWN && ev.key.ascii == 'q'))
            break;
    }
    os64_gui_window_destroy(win);
    return rc == 0 ? TITLE_OK : TITLE_FAIL;
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "--foreign")) {
        int64_t win = 0;
        for (const char *p = argv[2]; *p; ++p) win = win * 10 + (*p - '0');
        return os64_gui_window_set_title(win, "stolen", 0) == OS64_GUI_ERR_NOT_OWNER ? 0 : 1;
    }
    bool holding = argc == 2 && os64_streq(argv[1], "--hold");
    int64_t win = holding ? os64_gui_window_create("titletest: before", 120, 90, 420, 160, 0)
                          : os64_gui_window_create("titletest", 40, 40, 240, 120, 0);
    if (win == OS64_GUI_ERR_NOT_RUNNING) {
        os64_printf("titletest: SKIP (requires GUI)\n");
        return TITLE_SKIP;
    }
    CHECK(win > 0);
    if (win <= 0) return TITLE_FAIL;
    if (holding)
        return hold(win);

    os64_gui_window_state_t was;
    CHECK(os64_gui_window_get_state(win, &was) == 0);
    CHECK((was.flags & OS64_GUI_WINDOW_TITLE_UTF8) == 0);
    geometry_events(win);

    // Renamed, and the encoding passed is the one in force.
    CHECK(os64_gui_window_set_title(win, "Renamed", 0) == 0);
    CHECK(os64_gui_window_set_title(win, "Caf\xc3\xa9 UTF-8", OS64_GUI_WINDOW_TITLE_UTF8) == 0);
    CHECK((state_flags(win) & OS64_GUI_WINDOW_TITLE_UTF8) != 0);
    CHECK(os64_gui_window_set_title(win, "Latin-1 again", 0) == 0);
    CHECK((state_flags(win) & OS64_GUI_WINDOW_TITLE_UTF8) == 0);
    CHECK(os64_gui_window_set_title(win, "", 0) == 0);    // create takes an empty title too
    CHECK(os64_gui_window_set_title(win, "", 0) == 0);    // and the same one twice is no error

    // The capacity is OS64_GUI_TITLE_MAX with its NUL: the longest title
    // fits, one byte more is refused, never cut.
    char longest[OS64_GUI_TITLE_MAX], over[OS64_GUI_TITLE_MAX + 1];
    os64_memset(longest, 'L', sizeof(longest) - 1);
    longest[sizeof(longest) - 1] = '\0';
    os64_memset(over, 'O', sizeof(over) - 1);
    over[sizeof(over) - 1] = '\0';
    CHECK(os64_gui_window_set_title(win, longest, 0) == 0);

    // Every refusal leaves the encoding (and so the title) as it was: each
    // asks for UTF-8, and the window stays Latin-1.
    CHECK(os64_gui_window_set_title(win, over, OS64_GUI_WINDOW_TITLE_UTF8) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_title(win, NULL, OS64_GUI_WINDOW_TITLE_UTF8) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_title(win, (const char *)8, OS64_GUI_WINDOW_TITLE_UTF8) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_title(win, (const char *)0xffffffff80000000ull,
                                    OS64_GUI_WINDOW_TITLE_UTF8) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_title(win, "flags", OS64_GUI_WINDOW_TITLE_UTF8 | 1u) == OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_title(win, "flags", OS64_GUI_WINDOW_TITLE_UTF8 | (1ull << 32)) ==
          OS64_GUI_ERR_BAD_ARGS);
    CHECK((state_flags(win) & OS64_GUI_WINDOW_TITLE_UTF8) == 0);
    CHECK(os64_gui_window_set_title(0, "nobody", 0) == OS64_GUI_ERR_INVALID_HANDLE);
    CHECK(os64_gui_window_set_title(-1, "nobody", 0) == OS64_GUI_ERR_INVALID_HANDLE);

    // Another task may not rename it.
    char number[24];
    os64_snprintf(number, sizeof(number), "%ld", win);
    char *args[] = {"/tests/titletest", "--foreign", number, NULL};
    int64_t child = os64_spawn(args[0], args);
    int32_t code = -1;
    CHECK(child > 0);
    if (child > 0) CHECK(os64_wait(child, &code) == child && code == 0);

    // A rename moves nothing.
    CHECK(same_place(win, &was));
    CHECK(geometry_events(win) == 0);

    CHECK(os64_gui_window_destroy(win) == 0);
    CHECK(os64_gui_window_set_title(win, "gone", 0) == OS64_GUI_ERR_INVALID_HANDLE);
    os64_printf("titletest: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? TITLE_FAIL : TITLE_OK;
}
