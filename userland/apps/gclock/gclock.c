//gclock - Retained mode example ... with blinking separators!
//
// The RETAINED-MODE template (glogo is the immediate-mode twin; LIBDRAW.md's
// "Start here" section is the map). In retained mode you describe FURNITURE
// once — widgets, in a tree, with bounds — and then your entire job per tick
// is three verbs: change state, mark dirty, os64_ui_paint(). The toolkit
// draws and publishes for you; you never call fill/draw/publish yourself
// (doing so paints over your own furniture — ask this file's first draft).
//
// Also the model for TIME-driven UIs: os64_ui_run blocks in event_wait, and
// a clock has no events — so a ticking app runs the loop itself, exactly as
// below. The first user-authored g-app, and it found a window-manager bug
// (the third titlebar door) before it could tell time.
//
// Every numbered step is load-bearing; swap the label for your furniture and
// the tick for your logic, and you have a new app.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/fmt.h"
#include "os64/ui.h"
#include "os64/conf.h"

#define WIN_W 90u
#define WIN_H 48u
// A decorated frame's 20px titlebar includes the top border; an undecorated
// frame keeps that 1px border. Removing the bar therefore removes 19px while
// preserving the clock's content height exactly.
#define TITLEBAR_FRAME_DELTA 19u
#define MAX_CLOCK_CHARS 40

static int64_t gClockWin = 0;
static char clockText[MAX_CLOCK_CHARS];
static os64_ui_widget_t gRoot, gLblClockText;
static os64_ui_t gUi;
static bool separatorsShown = false;
static bool gRunning = true;

typedef struct {
    int32_t x, y;
    bool titlebar;
    bool pinned;
    const char *path;
} gclock_conf_t;

static bool parse_i32(const char **text, int32_t *out)
{
    const char *p = *text;
    bool negative = false;
    uint64_t value = 0;
    uint64_t limit;

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }
    if (*p < '0' || *p > '9')
        return false;
    limit = negative ? 2147483648ULL : 2147483647ULL;
    while (*p >= '0' && *p <= '9') {
        uint64_t digit = (uint64_t)(*p++ - '0');
        if (value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *out = negative ? (int32_t)(-(int64_t)value) : (int32_t)value;
    *text = p;
    return true;
}

static bool parse_position(const char *value, int32_t *x, int32_t *y)
{
    int32_t px, py;
    const char *p = value;

    if (!parse_i32(&p, &px) || *p++ != ',' || !parse_i32(&p, &py) || *p != '\0')
        return false;
    *x = px;
    *y = py;
    return true;
}

static bool conf_line(const char *key, const char *value, void *user)
{
    gclock_conf_t *conf = (gclock_conf_t *)user;

    if (key == NULL) {
        os64_hprintf(OS64_STDERR, "gclock: %s: expected 'key = value' - ignored: %s\n",
                     conf->path, value);
    } else if (os64_streq_nocase(key, "position")) {
        if (!parse_position(value, &conf->x, &conf->y))
            os64_hprintf(OS64_STDERR, "gclock: %s: Position must be x,y - ignored: %s\n",
                         conf->path, value);
    } else if (os64_streq_nocase(key, "titlebar")) {
        if (os64_streq_nocase(value, "on"))
            conf->titlebar = true;
        else if (os64_streq_nocase(value, "off"))
            conf->titlebar = false;
        else
            os64_hprintf(OS64_STDERR, "gclock: %s: Titlebar must be on or off - ignored: %s\n",
                         conf->path, value);
    } else if (os64_streq_nocase(key, "pinned")) {
        if (os64_streq_nocase(value, "true"))
            conf->pinned = true;
        else if (os64_streq_nocase(value, "false"))
            conf->pinned = false;
        else
            os64_hprintf(OS64_STDERR, "gclock: %s: Pinned must be true or false - ignored: %s\n",
                         conf->path, value);
    } else {
        os64_hprintf(OS64_STDERR, "gclock: %s: unknown setting '%s' - ignored\n",
                     conf->path, key);
    }
    return true;
}

static void refresh_clock_text(void)
{
    os64_date_t now;

    separatorsShown = !separatorsShown;
    os64_date_now(&now, NULL);
    os64_memset(clockText, 0, MAX_CLOCK_CHARS);
    if (separatorsShown)
        os64_snprintf(clockText, 40, "%02d:%02d:%02d",
                        now.hour, now.minute, now.second);
    else
        os64_snprintf(clockText, 40, "%02d %02d %02d",
                      now.hour, now.minute, now.second);
}

static void on_close_request(os64_ui_t *ui)
{
    (void)ui;
    os64_gui_window_state_t st;
    if (os64_gui_window_get_state(gClockWin, &st) == 0)
    {
        char pos[32];
        os64_snprintf(pos, sizeof pos, "%d,%d", st.x, st.y);
        const os64_conf_pair_t save[] = {
            {"titlebar", (st.flags & OS64_GUI_WINDOW_NO_DECORATIONS) ? "off" : "on"},
            {"pinned", (st.flags & OS64_GUI_WINDOW_PINNED) ? "true" : "false"},
            {"position", pos},
        };
        size_t saveCount = (st.flags & OS64_GUI_WINDOW_MAXIMIZED) ? 2 : 3;
        //don't save position if window is maximized
        if (os64_conf_write("gclock.conf", save, saveCount)) // → /home/gclock.conf, merged, atomic
            os64_hprintf(1, "Error: Unable to save configuration to gclock.conf");
    }
    gRunning = false;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	gclock_conf_t conf = { .x = 280, .y = 10, .titlebar = true, .pinned = false };
	char conf_path[OS64_CONF_PATH_MAX];
	conf.path = conf_path;
	int64_t conf_rc = os64_conf_find_read("gclock.conf", conf_line, &conf,
	                                      conf_path, sizeof(conf_path));
	if (conf_rc == OS64_CONF_TRUNCATED)
		os64_hprintf(OS64_STDERR, "gclock: %s: file exceeds %d bytes; trailing settings ignored\n",
		             conf_path, OS64_CONF_MAX - 1);
	else if (conf_rc == OS64_CONF_NO_MEMORY)
		os64_hprintf(OS64_STDERR, "gclock: could not allocate config reader buffer; using defaults\n");

	uint64_t flags = 0;
	if (!conf.titlebar)
		flags |= OS64_GUI_WINDOW_NO_DECORATIONS;
	if (conf.pinned)
		flags |= OS64_GUI_WINDOW_PINNED;
	uint32_t win_h = conf.titlebar ? WIN_H : WIN_H - TITLEBAR_FRAME_DELTA;

	// [1] A window, born directly in its configured position and WM state.
	gClockWin = os64_gui_window_create("gclock", conf.x, conf.y,
	                                     WIN_W,
	                                     win_h, flags);
	if (gClockWin <= 0)
	{
		os64_printf("gclock: no GUI here (window_create %ld)\n", (long)gClockWin);
		return 1;
	}

	// [2] The draw context: the window's shared canvas plus drawing state.
	//     Retained mode still stands on it — libui paints THROUGH this.
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, gClockWin) != 0)
	{
		os64_printf("gclock: get_surface failed\n");
		os64_gui_window_destroy(gClockWin);
		return 1;
	}

    // [3] The UI context. This is also where the THEME loads — defaults,
    //     then /home/theme.conf on top — so every color and metric this
    //     window shows is the user's to change without a recompile.
    os64_ui_init(&gUi, &ctx);

    gUi.on_close = on_close_request;
    // [4] The furniture. App-owned structs (libui never allocates): a panel
    //     as the root — it paints the themed background — and a label whose
    //     text member POINTS AT our buffer, so refreshing the buffer is
    //     refreshing the widget.
    os64_ui_panel(&gRoot);
    gRoot.bounds = (os64_gui_rect_t){0, 0, (int32_t)ctx.surf.width,
                                     (int32_t)ctx.surf.height};
    os64_ui_set_root(&gUi, &gRoot);
    os64_ui_label(&gLblClockText, clockText);
    os64_ui_add_child(&gRoot, &gLblClockText);

    // [5] Layout: give the label a height (one font row); the vertical stack
    //     assigns x/y/width inside the panel, honoring the theme's padding.
    //     A widget without real bounds paints nowhere — the first draft's
    //     black-window lesson, preserved here as a warning label.
    gLblClockText.bounds.h = gUi.theme.font_h;
    os64_ui_stack_vertical(&gUi, &gRoot);

    // [6] First paint: deliver everything set_root/stack marked dirty.
    //     Without it the window shows its birth-gray until the first tick.
    os64_ui_paint(&gUi);

    // [7] The tick loop — retained mode's whole runtime, three verbs long:
    //     change state, mark dirty, paint (which publishes just the dirty
    //     rect itself). frame_wait anchors to the last frame BOUNDARY and
    //     sleeps only the remainder, so the blink keeps honest time no
    //     matter what the work costs.
    os64_frame_clock_t frameClock;
    os64_frame_clock_init(&frameClock);
    while (gRunning)
    {
        os64_gui_event_t ev;
        while (os64_gui_event_poll(gClockWin, &ev) == 1)
        os64_ui_dispatch(&gUi, &ev);
        // Sync to frames instead of blindly sleeping for 500 ms!
        os64_frame_wait(&frameClock, 500);
        refresh_clock_text();
        os64_ui_mark_dirty(&gUi, &gLblClockText);
        os64_ui_paint(&gUi);
    }
    // [8] No exit path, and that is honest: a clock has no quit gesture yet.
    //     Ctrl+C from the launching terminal ends it, and the kernel's exit
    //     sweep reclaims the window — deliberate, not forgotten.
	return 0;
}
