// uiprobe — libui's fixture app (the gfxprobe tradition, one layer up).
//
// Exercises the whole L2 contract from ring 3: a panel root, labels, three
// buttons, the vertical stack layout, dispatch (press/track/release, drag-off
// cancel), the dirty-union repaint, and the theme table — every color and
// metric this window shows came through os64_ui_theme_t, so a
// /home/theme.conf edit re-skins it with zero recompile, which is the ruling
// this app exists to prove.
//
// Deliberately a FIXTURE, not a product: the counter proves click→state→
// repaint, [bounce] proves a widget can launch a real task, [quit] proves
// the loop's exit contract. The first real libui APP is Chris's to write —
// the labor treaty (toolkit mine, apps his) applies from this line onward.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/ui.h"
#include "os64/fmt.h"
#include "os64/proc.h"

#define WIN_W 200u
#define WIN_H 190u

static os64_ui_t      gUi;
static os64_ui_widget_t gRoot, gTitle, gCount, gBtnCount, gBtnBounce, gBtnQuit;
static char           gCountText[24];
static uint32_t       gClicks = 0;
static volatile bool  gRunning = true;

static void refresh_count(void)
{
	os64_snprintf(gCountText, sizeof(gCountText), "clicks: %u", gClicks);
}

static void on_count(os64_ui_widget_t *w, void *user)
{
	(void)w; (void)user;
	gClicks++;
	refresh_count();
	os64_ui_mark_dirty(&gUi, &gCount);
}

static void on_bounce(os64_ui_widget_t *w, void *user)
{
	(void)w; (void)user;
	// A widget launching a citizen: the embryo of every launcher to come.
	char *argv[] = { "/bin/gbounce", (char *)0 };
	int64_t pid = os64_spawn("/bin/gbounce", argv);
	if (pid < 0)
		os64_printf("uiprobe: gbounce launch failed (%ld)\n", (long)pid);
}

static void on_quit(os64_ui_widget_t *w, void *user)
{
	(void)w; (void)user;
	gRunning = false;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	// Decline the boot focus grab like the demos do (the no-mouse race that
	// earned the flag) — a click focuses us the honest way.
	int64_t win = os64_gui_window_create("uiprobe", 700, 80, WIN_W, WIN_H,
	                                     OS64_GUI_WINDOW_START_UNFOCUSED);
	if (win <= 0) {
		os64_printf("uiprobe: no GUI (window_create %ld) — nothing to probe\n",
		            (long)win);
		return 1;
	}

	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0) {
		os64_printf("uiprobe: get_surface failed\n");
		return 1;
	}

	os64_ui_init(&gUi, &ctx);

	os64_ui_panel(&gRoot);
	gRoot.bounds = (os64_gui_rect_t){0, 0, (int32_t)ctx.surf.width,
	                                 (int32_t)ctx.surf.height};

	os64_ui_label(&gTitle, "os64 libui");
	refresh_count();
	os64_ui_label(&gCount, gCountText);
	os64_ui_button(&gBtnCount,  "count",  on_count,  (void *)0);
	os64_ui_button(&gBtnBounce, "bounce", on_bounce, (void *)0);
	os64_ui_button(&gBtnQuit,   "quit",   on_quit,   (void *)0);

	gTitle.bounds.h = gUi.theme.font_h;   // labels: one text row
	gCount.bounds.h = gUi.theme.font_h;

	os64_ui_add_child(&gRoot, &gTitle);
	os64_ui_add_child(&gRoot, &gCount);
	os64_ui_add_child(&gRoot, &gBtnCount);
	os64_ui_add_child(&gRoot, &gBtnBounce);
	os64_ui_add_child(&gRoot, &gBtnQuit);

	os64_ui_set_root(&gUi, &gRoot);
	os64_ui_stack_vertical(&gUi, &gRoot);

	os64_ui_run(&gUi, win, &gRunning);

	os64_gui_window_destroy(win);
	return 0;
}
