// gui_demo_keys.c — the /gkeys demo app: echoes keystrokes and clicks.
//
// Client-style code (gui_client.h only), like gui_demo_bounce.c. What it
// proves: keys land ONLY in the focused window, per-window event queues
// deliver in order, and mouse coordinates arrive content-local.

#include "gui/gui_client.h"
#include "gui/gui_demos.h"
#include "gui/window.h"   // GUI_WINDOW_START_UNFOCUSED — decline the boot focus race
#include "gui/surface.h"

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "signals.h"
#include "smp_core.h"
#include "sprintf.h"
#include "thread.h"

#define KEYS_BG      0xfff4f2ea   // warm paper white
#define KEYS_INK     GUI_COLOR_BLACK
#define KEYS_ACCENT  0xff2a62b8
#define LINE_MAX     36           // chars that fit the 330px-wide content

bool gkeys_thread(bool daemon)
{
	(void)daemon;
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	int64_t win = gui_window_create("keys", 620, 100, 330, 180, GUI_WINDOW_START_UNFOCUSED);
	if (win <= 0) {
		printd(DEBUG_GUI, "gkeys: window create failed (%ld)\n", win);
		return false;
	}
	surface_t content;
	gui_window_get_surface(win, &content);

	rect_t all = {0, 0, (int32_t)content.width, (int32_t)content.height};
	surface_fill_rect(&content, all, KEYS_BG);
	const char prompt[] = "click me, then type:";
	surface_draw_text(&content, 10, 10, prompt, sizeof(prompt) - 1, KEYS_ACCENT, KEYS_BG);
	gui_window_publish(win, NULL);

	char line[LINE_MAX + 1] = {0};
	size_t line_len = 0;
	char status[LINE_MAX + 1] = {0};

	while (1) {
		bool dirty = false;

		input_event_t ev;
		while (gui_event_poll(win, &ev) == 1) {
			switch (ev.type) {
			case INPUT_EVENT_KEY_DOWN:
				if (ev.key.ascii == '\b') {
					if (line_len)
						line[--line_len] = '\0';
					dirty = true;
				} else if (ev.key.ascii == '\n') {
					line_len = 0;
					line[0] = '\0';
					dirty = true;
				} else if (ev.key.ascii >= ' ' && ev.key.ascii < 127) {
					if (line_len < LINE_MAX) {
						line[line_len++] = ev.key.ascii;
						line[line_len] = '\0';
					}
					dirty = true;
				}
				break;
			case INPUT_EVENT_MOUSE_BUTTON_DOWN:
				// Content-local coordinates, straight from the router.
				sprintf(status, "click at (%d,%d)", ev.mouse.x, ev.mouse.y);
				dirty = true;
				break;
			default:
				break;
			}
		}

		if (dirty) {
			// Repaint the two text lines (typed input + last click).
			rect_t text_area = {0, 40, (int32_t)content.width, 48};
			surface_fill_rect(&content, text_area, KEYS_BG);
			surface_draw_text(&content, 10, 44, line, line_len, KEYS_INK, KEYS_BG);
			size_t status_len = 0;
			while (status[status_len])
				status_len++;
			surface_draw_text(&content, 10, 68, status, status_len, KEYS_ACCENT, KEYS_BG);
			gui_window_publish(win, &text_area);
		}

		sigaction(SIGSLEEP, NULL, kTicksSinceStart + 1, self);
	}
}
