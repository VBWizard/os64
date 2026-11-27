// fullscreentest: the full-screen contract (os64_gui_window_set_full_screen,
// docs/design/pending/VIDEO.md § 9). Run with the desktop up; the windows it
// makes are its own and go when it ends.
//
//   fullscreentest           the state: entering and leaving, what get_state
//                            and the surface say, the black edge a plane sees,
//                            another program taking focus and giving it back
//                            (the window stays full screen and its plane
//                            keeps being shown), and the refusals
//   fullscreentest --chord   waits for the person (or the QEMU driver) to
//                            press Ctrl+Alt+F twice on its window: in, then out
//   fullscreentest --vt      waits for a text terminal to take the glass,
//                            then asks: the answer must be NOT_FOCUSED
#include "os64/os64.h"
#include "os64/gui.h"

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    os64_printf("fullscreentest: FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint32_t s_sw, s_sh;

static bool state_of(int64_t win, os64_gui_window_state_t *st)
{
    return os64_gui_window_get_state(win, st) == 0;
}

static bool flag(int64_t win, uint32_t bit)
{
    os64_gui_window_state_t st;
    return state_of(win, &st) && (st.flags & bit) != 0;
}

// What the window's events said since the last call: a full-screen change, a
// resize.
static void drain(int64_t win, bool *changed, bool *resized)
{
    os64_gui_event_t ev;
    *changed = *resized = false;
    while (os64_gui_event_poll(win, &ev) == 1) {
        if (ev.type == OS64_GUI_EVENT_WINDOW_FULL_SCREEN) *changed = true;
        if (ev.type == OS64_GUI_EVENT_WINDOW_RESIZE) *resized = true;
    }
}

// The cover sweep and the plane's frames run in the compositor's passes.
static void passes(void)
{
    os64_sleep(150);
}

// One frame wait on the plane, with a short patience: FRAME when shown.
static uint32_t plane_wait(int64_t win, uint32_t plane)
{
    os64_gui_plane_frame_t f;
    if (os64_gui_plane_frame_wait(win, plane, 300, &f) != 0)
        return 0;
    return f.why;
}

static void publish_whole(int64_t win)
{
    os64_gui_surface_t s;
    if (os64_gui_window_get_surface(win, &s) != 0)
        return;
    for (uint32_t y = 0; y < s.height; y++)
        for (uint32_t x = 0; x < s.width; x++)
            s.pixels[(size_t)y * s.pitch_px + x] = 0xff203040u;
    (void)os64_gui_window_publish(win, NULL);
}

static int chord_mode(void)
{
    int64_t win = os64_gui_window_create_content("Full screen chord", 60, 60, 320, 200, 0);
    CHECK(win > 0);
    os64_gui_window_state_t before;
    CHECK(state_of(win, &before));
    os64_printf("fullscreentest: press Ctrl+Alt+F on this window, twice\n");
    bool in = false, out = false;
    for (unsigned i = 0; i < 300 && !out; i++) {
        os64_sleep(100);
        bool full = flag(win, OS64_GUI_WINDOW_FULL_SCREEN);
        if (!in && full) {
            in = true;
            publish_whole(win);
        }
        if (in && !full)
            out = true;
    }
    CHECK(in && out);
    os64_gui_window_state_t after;
    CHECK(state_of(win, &after) && after.x == before.x && after.y == before.y &&
          after.width == before.width && after.height == before.height);
    os64_gui_window_destroy(win);
    os64_printf("fullscreentest --chord: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

static int vt_mode(void)
{
    int64_t win = os64_gui_window_create_content("Full screen VT", 60, 60, 320, 200, 0);
    CHECK(win > 0);
    os64_printf("fullscreentest: switch to a text terminal (Ctrl+Alt+F2), then back\n");
    bool asked = false;
    for (unsigned i = 0; i < 300 && !asked; i++) {
        os64_sleep(100);
        if (flag(win, OS64_GUI_WINDOW_COVERED)) {
            CHECK(os64_gui_window_set_full_screen(win, true) == OS64_GUI_ERR_NOT_FOCUSED);
            asked = true;
        }
    }
    CHECK(asked);
    CHECK(!flag(win, OS64_GUI_WINDOW_FULL_SCREEN));
    os64_gui_window_destroy(win);
    os64_printf("fullscreentest --vt: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (os64_gui_screen_info(&s_sw, &s_sh) < 0) {
        os64_printf("fullscreentest: SKIP (GUI required)\n");
        return 0;
    }
    if (argc == 2 && os64_streq(argv[1], "--chord"))
        return chord_mode();
    if (argc == 2 && os64_streq(argv[1], "--vt"))
        return vt_mode();

    // ── Entering ────────────────────────────────────────────────────────
    int64_t a = os64_gui_window_create_content("Full screen A", 60, 60, 400, 300, 0);
    CHECK(a > 0);
    if (a <= 0)
        return 1;
    os64_gui_window_state_t before, st;
    os64_gui_surface_t s0, s1;
    CHECK(state_of(a, &before));
    CHECK(os64_gui_window_get_surface(a, &s0) == 0);
    publish_whole(a);

    // A plane, placed in the window's middle, so the edge can be seen by it.
    int64_t plane = os64_gui_plane_create(a, 160, 90);
    CHECK(plane > 0);
    os64_gui_plane_surface_t ps;
    CHECK(plane > 0 && os64_gui_plane_surface(a, (uint32_t)plane, &ps) == 0);
    if (plane > 0) {
        for (uint32_t i = 0; i < 90; i++)
            for (uint32_t j = 0; j < 160; j++)
                ps.pixels[(size_t)i * ps.pitch_px + j] = 0xff00ff00u;
        CHECK(os64_gui_plane_publish(a, (uint32_t)plane, 160, 90) == 0);
        os64_gui_plane_edit_t e = {.plane = (uint32_t)plane, .origin = {40, 40, 160, 90},
                                   .clip = {40, 40, 160, 90}};
        CHECK(os64_gui_window_publish_edits(a, NULL, &e, 1) == 0);
        passes();
        CHECK(plane_wait(a, (uint32_t)plane) == OS64_GUI_PLANE_FRAME);
    }

    bool changed, resized;
    drain(a, &changed, &resized);
    CHECK((int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_SET_FULL_SCREEN, (uint64_t)a, 2) ==
          OS64_GUI_ERR_BAD_ARGS);
    CHECK(os64_gui_window_set_full_screen(a, true) == 0);
    CHECK(os64_gui_window_set_full_screen(a, true) == 0);   // already: no change
    CHECK(state_of(a, &st) && (st.flags & OS64_GUI_WINDOW_FULL_SCREEN) && st.x == 0 && st.y == 0 &&
          st.width == s_sw && st.height == s_sh);
    // The canvas was reserved at the screen's size: the same pointer and
    // pitch, now the screen's width and height (no allocation).
    CHECK(os64_gui_window_get_surface(a, &s1) == 0 && s1.pixels == s0.pixels &&
          s1.pitch_px == s0.pitch_px && s1.width == s_sw && s1.height == s_sh);
    drain(a, &changed, &resized);
    CHECK(changed && resized);
    // Across the edge, until the window publishes, its plane is not shown.
    if (plane > 0)
        CHECK(plane_wait(a, (uint32_t)plane) == OS64_GUI_PLANE_PATIENCE);
    publish_whole(a);
    passes();
    if (plane > 0)
        CHECK(plane_wait(a, (uint32_t)plane) == OS64_GUI_PLANE_FRAME);

    // ── Another program takes focus, and gives it back ───────────────────
    // (Chris's ruling 1: a full-screen video stays there when he switches.)
    int64_t b = os64_gui_window_create_content("Full screen B", 100, 100, 240, 160, 0);
    CHECK(b > 0);
    passes();
    CHECK(flag(a, OS64_GUI_WINDOW_FULL_SCREEN));
    CHECK(!flag(a, OS64_GUI_WINDOW_COVERED));    // B is small: A still shows around it
    CHECK(!flag(b, OS64_GUI_WINDOW_COVERED));    // and B is over A
    if (plane > 0)
        CHECK(plane_wait(a, (uint32_t)plane) == OS64_GUI_PLANE_FRAME);
    // A is full screen but unfocused now: asking again changes nothing.
    CHECK(os64_gui_window_set_full_screen(a, true) == 0);
    CHECK(os64_gui_window_focus(a) == 0);
    passes();
    CHECK(flag(a, OS64_GUI_WINDOW_FULL_SCREEN));
    CHECK(flag(b, OS64_GUI_WINDOW_COVERED));     // A is over everything again
    if (plane > 0)
        CHECK(plane_wait(a, (uint32_t)plane) == OS64_GUI_PLANE_FRAME);

    // ── Refusals ────────────────────────────────────────────────────────
    CHECK(os64_gui_window_set_full_screen(b, true) == OS64_GUI_ERR_NOT_FOCUSED);
    int64_t popup = os64_gui_popup_create("Full screen popup", a, 20, 20, 100, 60);
    CHECK(popup > 0);
    if (popup > 0) {
        CHECK(os64_gui_window_set_full_screen(popup, true) == OS64_GUI_ERR_BAD_ARGS);
        CHECK(os64_gui_window_destroy(popup) == 0);
    }
    CHECK(os64_gui_window_set_full_screen(-1, true) == OS64_GUI_ERR_INVALID_HANDLE);

    // While full screen, a content size asked is for the frame it goes back to.
    CHECK(os64_gui_window_set_content_size(a, 300, 200) == 0);
    CHECK(os64_gui_window_get_surface(a, &s1) == 0 && s1.width == s_sw && s1.height == s_sh);

    // ── Leaving ─────────────────────────────────────────────────────────
    drain(a, &changed, &resized);
    CHECK(os64_gui_window_set_full_screen(a, false) == 0);
    CHECK(os64_gui_window_set_full_screen(a, false) == 0);   // already: no change
    CHECK(!flag(a, OS64_GUI_WINDOW_FULL_SCREEN));
    CHECK(os64_gui_window_get_surface(a, &s1) == 0 && s1.width == 300 && s1.height == 200);
    CHECK(state_of(a, &st) && st.x == before.x && st.y == before.y);
    drain(a, &changed, &resized);
    CHECK(changed && resized);
    if (plane > 0) {
        os64_gui_plane_edit_t hide = {.plane = (uint32_t)plane, .hide = 1};
        os64_gui_rect_t all = {0, 0, 300, 200};
        CHECK(os64_gui_window_publish_edits(a, &all, &hide, 1) == 0);
        CHECK(os64_gui_plane_destroy(a, (uint32_t)plane) == 0);
    }

    // ── Over a maximized window: maximized again after ────────────────────
    int64_t c = os64_gui_window_create_content("Full screen C", 60, 60, 400, 300,
                                               OS64_GUI_CREATE_MAXIMIZED);
    CHECK(c > 0);
    os64_gui_surface_t mx;
    CHECK(os64_gui_window_get_surface(c, &mx) == 0);
    CHECK(os64_gui_window_set_full_screen(c, true) == 0);
    CHECK(state_of(c, &st) && (st.flags & OS64_GUI_WINDOW_FULL_SCREEN) &&
          (st.flags & OS64_GUI_WINDOW_MAXIMIZED));
    CHECK(os64_gui_window_get_surface(c, &s1) == 0 && s1.width == s_sw && s1.height == s_sh);
    CHECK(os64_gui_window_set_full_screen(c, false) == 0);
    CHECK(state_of(c, &st) && !(st.flags & OS64_GUI_WINDOW_FULL_SCREEN) &&
          (st.flags & OS64_GUI_WINDOW_MAXIMIZED));
    CHECK(os64_gui_window_get_surface(c, &s1) == 0 && s1.width == mx.width && s1.height == mx.height);

    // ── A window whose minimum is larger than the screen ─────────────────
    int64_t d = os64_gui_window_create_content("Full screen D", 0, 0, s_sw + 64, 200, 0);
    CHECK(d > 0);
    if (d > 0) {
        CHECK(os64_gui_window_set_min_size(d, s_sw + 32, 100) == 0);
        CHECK(os64_gui_window_set_full_screen(d, true) == OS64_GUI_ERR_SCREEN_FIT);
        CHECK(!flag(d, OS64_GUI_WINDOW_FULL_SCREEN));
        os64_gui_window_destroy(d);
    }

    os64_gui_window_destroy(c);
    os64_gui_window_destroy(b);
    os64_gui_window_destroy(a);
    os64_printf("fullscreentest: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
