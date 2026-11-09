#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "gui/compositor.h"
#include "strcmp.h"
#include "printd.h"
#include "CONFIG.h"

// What /tests/titletest cannot see from ring 3: the title text itself, and
// the damage a rename leaves. A real window (made through the client API, so
// the handle path is the one a syscall takes) is renamed with the lock held
// and the pending damage taken out around each call, so the test sees
// exactly what that call damaged: the titlebar strip for a new title or a
// new encoding, nothing for the same one again. A refused call must leave
// both as they were. The window sits far off the screen, so nothing shows.

#define TITLE_TEST_X (-4000)
#define TITLE_TEST_Y (-4000)
#define TITLE_TEST_DAMAGE_MAX 64

static rect_t s_saved[TITLE_TEST_DAMAGE_MAX];
static rect_t s_seen[TITLE_TEST_DAMAGE_MAX];

static bool same_rect(rect_t a, rect_t b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The one rename under test: exactly `damaged` rects added, and when there
// is one, it is the titlebar strip.
static bool renamed(window_t *w, const char *title, bool utf8, uint32_t damaged)
{
    wm_set_title(w, title, utf8);
    uint32_t n = gui_damage_take_locked(s_seen, TITLE_TEST_DAMAGE_MAX);
    rect_t strip = {w->frame.x, w->frame.y, w->frame.w, wm_chrome_top(w->flags)};
    bool ok = strncmp(w->title, title, GUI_WINDOW_TITLE_MAX) == 0 &&
              ((w->flags & GUI_WINDOW_TITLE_UTF8) != 0) == utf8 &&
              n == damaged && (n == 0 || same_rect(s_seen[0], strip));
    if (!ok)
        printd(DEBUG_TESTS, "\tFAIL: test_window_title - '%s' utf8 %d: title '%s' flags 0x%x, "
               "%u rects (wanted %u), first %d,%d %dx%d\n", title, utf8, w->title, w->flags,
               n, damaged, n ? s_seen[0].x : 0, n ? s_seen[0].y : 0, n ? s_seen[0].w : 0,
               n ? s_seen[0].h : 0);
    return ok;
}

bool test_window_title(void)
{
    int64_t handle = gui_window_create("title: before", TITLE_TEST_X, TITLE_TEST_Y, 200, 100,
                                       GUI_WINDOW_START_UNFOCUSED);
    if (handle == GUI_ERR_NOT_RUNNING) {
        printd(DEBUG_TESTS, "\tSKIP: test_window_title (no GUI on this boot)\n");
        return true;
    }
    if (handle <= 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_window_title - create answered %ld\n", handle);
        return false;
    }

    bool ok = true;
    uint64_t irq = gui_lock(GUI_LOCK_OTHER);
    window_t *w = wm_topmost_at(TITLE_TEST_X + 10, TITLE_TEST_Y + 10);
    if (w == NULL || w->client_handle != (uint16_t)handle) {
        printd(DEBUG_TESTS, "\tFAIL: test_window_title - the window is not where it was made\n");
        ok = false;
        w = NULL;
    }
    uint32_t saved = gui_damage_take_locked(s_saved, TITLE_TEST_DAMAGE_MAX);
    if (w != NULL) {
        ok &= renamed(w, "title: after", false, 1);   // a new title, same encoding
        ok &= renamed(w, "title: after", true, 1);    // the same title, a new encoding
        ok &= renamed(w, "title: after", true, 0);    // nothing new: no repaint
    }
    for (uint32_t i = 0; i < saved; i++)
        gui_damage_add_locked(s_saved[i]);
    gui_unlock(irq);

    // Through the handle, as the syscall reaches it: a stray flag is refused
    // and changes nothing; the same call without it renames.
    if (w != NULL) {
        int64_t refused = gui_window_set_title(handle, "title: stray", GUI_WINDOW_TITLE_UTF8 | 1u);
        irq = gui_lock(GUI_LOCK_OTHER);
        bool kept = strncmp(w->title, "title: after", GUI_WINDOW_TITLE_MAX) == 0 &&
                    (w->flags & GUI_WINDOW_TITLE_UTF8) != 0;
        gui_unlock(irq);
        int64_t taken = gui_window_set_title(handle, "title: handle", 0);
        irq = gui_lock(GUI_LOCK_OTHER);
        bool took = strncmp(w->title, "title: handle", GUI_WINDOW_TITLE_MAX) == 0 &&
                    (w->flags & GUI_WINDOW_TITLE_UTF8) == 0;
        gui_unlock(irq);
        if (refused != GUI_ERR_BAD_ARGS || !kept || taken != 0 || !took) {
            printd(DEBUG_TESTS, "\tFAIL: test_window_title - by handle: stray flag %ld (kept %d), "
                   "rename %ld (took %d)\n", refused, kept, taken, took);
            ok = false;
        }
    }

    if (gui_window_destroy(handle) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_window_title - destroy refused\n");
        ok = false;
    }
    if (ok)
        printd(DEBUG_TESTS, "\tPASS: test_window_title\n");
    return ok;
}
