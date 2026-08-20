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

#define WIN_W 90u
#define WIN_H 48u
#define MAX_CLOCK_CHARS 40

static char clockText[MAX_CLOCK_CHARS];
static os64_ui_widget_t gRoot, gLblClockText;
static os64_ui_t gUi;
static bool separatorsShown = false;

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

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	// [1] A window. Flags 0 = normal birth: on top, takes focus.
	int64_t win = os64_gui_window_create("gclock", 280, 10,
	                                     WIN_W,
	                                     WIN_H, 0);
	if (win <= 0)
	{
		os64_printf("gclock: no GUI here (window_create %ld)\n", (long)win);
		return 1;
	}

	// [2] The draw context: the window's shared canvas plus drawing state.
	//     Retained mode still stands on it — libui paints THROUGH this.
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0)
	{
		os64_printf("gclock: get_surface failed\n");
		os64_gui_window_destroy(win);
		return 1;
	}

    // [3] The UI context. This is also where the THEME loads — defaults,
    //     then /home/theme.conf on top — so every color and metric this
    //     window shows is the user's to change without a recompile.
    os64_ui_init(&gUi, &ctx);

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
    while (1 == 1)
    {
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
