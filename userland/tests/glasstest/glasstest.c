// glasstest — /dev/glass, the read side (REMOTE.md section 3, os64/glass.h).
//
//   1. The first reads after an open compose the whole screen, in whole-row
//      bands, each record exactly its header and its pixels.
//   2. A buffer sized for ten rows gets ten rows, and the rest follows.
//   3. A buffer that cannot hold the header and one row is refused, and the
//      pending change survives the refusal.
//   4. A window painted a known colour reaches a viewer, pixels and all.
//   5. A still screen times out under os64_read_for's patience.
//   6. Closing a viewer under a parked reader ends the wait; it never reads
//      freed memory (the kernel would panic, which is the failure this
//      catches).
//
// `glasstest watch` prints every record's header for thirty seconds instead:
// the hands-on half, for what no fixture can do alone — switch terminals and
// watch OS64_GLASS_TEXT_VT come and go.
//
// Exit codes: 0x61A55000 success, 0x61A55001 SKIP (no desktop on this boot),
// 0x61A55Fnn the step that failed.

#include "os64/os64.h"
#include "os64/draw.h"
#include "os64/glass.h"

#define GLASSTEST_OK        0x61A55000
#define GLASSTEST_SKIP      0x61A55001
#define GLASSTEST_OPEN      0x61A55F01   // a desktop exists and /dev/glass refused
#define GLASSTEST_FULL      0x61A55F02   // the first reads did not compose the whole screen
#define GLASSTEST_BAND      0x61A55F03   // a ten-row buffer did not get ten rows
#define GLASSTEST_SMALL     0x61A55F04   // a too-small buffer was not refused, or lost the change
#define GLASSTEST_WINDOW    0x61A55F05   // a painted window never reached the viewer
#define GLASSTEST_STILL     0x61A55F06   // a still screen never timed out
#define GLASSTEST_CLOSE     0x61A55F07   // a close under a parked reader went wrong

#define PAINT 0x0012AB34u

static uint8_t buf[1024 * 1024];
static uint32_t screen_w, screen_h;

// One record: its header, and a check that the length is exactly the header
// and its pixels. Returns the read's result.
static int64_t record(int32_t h, uint8_t *into, size_t cap, uint64_t patience, os64_glass_rect_t *head)
{
	int64_t n = patience ? os64_read_for(h, into, cap, patience) : os64_read(h, into, cap);
	if (n <= 0)
		return n;
	os64_memcpy(head, into, sizeof(*head));
	if ((uint64_t)n != sizeof(*head) + (uint64_t)head->w * head->h * 4 ||
	    head->screen_w != screen_w || head->screen_h != screen_h ||
	    (uint32_t)head->x + head->w > screen_w || (uint32_t)head->y + head->h > screen_h)
	{
		os64_printf("glasstest: malformed record %u,%u %ux%u (%ld bytes)\n",
		            head->x, head->y, head->w, head->h, (long)n);
		return -1000;
	}
	return n;
}

static int watch(void)
{
	os64_gui_screen_info(&screen_w, &screen_h);   // what record() checks each header against
	int64_t h = os64_open("/dev/glass", "r");
	if (h < 0)
	{
		os64_printf("glasstest watch: /dev/glass refused (no desktop?)\n");
		return 1;
	}
	for (int second = 0; second < 30; )
	{
		os64_glass_rect_t head;
		int64_t n = record((int32_t)h, buf, sizeof(buf), 1000, &head);
		if (n == OS64_ERR_TIMEOUT) { second++; continue; }
		if (n <= 0) break;
		os64_printf("glasstest watch: %u,%u %ux%u%s\n", head.x, head.y, head.w, head.h,
		            (head.flags & OS64_GLASS_TEXT_VT) ? " TEXT_VT" : "");
	}
	os64_close((int32_t)h);
	return 0;
}

static int64_t park_and_read(void *arg)
{
	int32_t h = (int32_t)(int64_t)arg;
	static uint8_t mine[64 * 1024];
	return os64_read(h, mine, sizeof(mine));
}

int main(int argc, char **argv)
{
	if (argc == 2 && os64_streq(argv[1], "watch"))
		return watch();

	bool desktop = os64_gui_screen_info(&screen_w, &screen_h) == 0 && screen_w && screen_h;
	int64_t h = os64_open("/dev/glass", "r");
	if (h < 0)
	{
		if (!desktop)
		{
			os64_printf("glasstest: SKIP (no desktop on this boot)\n");
			return GLASSTEST_SKIP;
		}
		return GLASSTEST_OPEN;
	}

	// 1. The whole screen, top to bottom, in whole-row bands.
	uint32_t next_row = 0;
	while (next_row < screen_h)
	{
		os64_glass_rect_t head;
		int64_t n = record((int32_t)h, buf, sizeof(buf), 1000, &head);
		if (n <= 0 || head.x != 0 || head.w != screen_w || head.y != next_row)
		{
			os64_printf("glasstest: full frame broke at row %u (read %ld)\n", next_row, (long)n);
			return GLASSTEST_FULL;
		}
		next_row += head.h;
	}

	// 2 and 3, on a second viewer so its first change is the whole screen.
	int64_t h2 = os64_open("/dev/glass", "r");
	if (h2 < 0)
		return GLASSTEST_OPEN;
	if (os64_read((int32_t)h2, buf, sizeof(os64_glass_rect_t) + 4) >= 0)
		return GLASSTEST_SMALL;
	os64_glass_rect_t head;
	size_t ten = sizeof(os64_glass_rect_t) + (size_t)screen_w * 4 * 10;
	if (record((int32_t)h2, buf, ten, 1000, &head) <= 0 || head.y != 0 || head.h != 10)
		return GLASSTEST_BAND;
	if (record((int32_t)h2, buf, ten, 1000, &head) <= 0 || head.y != 10 || head.h != 10)
		return GLASSTEST_BAND;
	os64_close((int32_t)h2);

	// 4. A window of a known colour, seen through the glass.
	int64_t win = os64_gui_window_create("glasstest", 120, 120, 240, 180, 0);
	os64_draw_ctx_t ctx;
	if (win <= 0 || os64_draw_ctx_init(&ctx, win) != 0)
		return GLASSTEST_WINDOW;
	os64_draw_fill_rect(&ctx.surf, (os64_gui_rect_t){0, 0, (int32_t)ctx.surf.width, (int32_t)ctx.surf.height},
	                    0xFF000000u | PAINT);
	os64_gui_window_publish(win, NULL);
	os64_gui_window_state_t st;
	os64_gui_window_get_state(win, &st);
	uint32_t cx = (uint32_t)st.x + st.width / 2, cy = (uint32_t)st.y + st.height / 2;
	bool seen = false;
	for (int tries = 0; tries < 200 && !seen; tries++)
	{
		int64_t n = record((int32_t)h, buf, sizeof(buf), 3000, &head);
		if (n <= 0)
			break;
		if (cx >= head.x && cx < (uint32_t)head.x + head.w && cy >= head.y && cy < (uint32_t)head.y + head.h)
		{
			uint32_t pixel;
			os64_memcpy(&pixel, buf + sizeof(head) + (((size_t)(cy - head.y) * head.w + (cx - head.x)) * 4), 4);
			seen = (pixel & 0x00FFFFFFu) == PAINT;
		}
	}
	os64_gui_window_destroy(win);
	if (!seen)
		return GLASSTEST_WINDOW;

	// 5. Stillness: drain, then wait for a read that times out. Anything on
	// the desktop that animates is drained and waited out again.
	bool still = false;
	for (int tries = 0; tries < 50 && !still; tries++)
	{
		int64_t n = record((int32_t)h, buf, sizeof(buf), 300, &head);
		if (n == OS64_ERR_TIMEOUT)
			still = true;
		else if (n <= 0)
			break;
	}
	if (!still)
		return GLASSTEST_STILL;

	// 6. A reader parked in a viewer whose handle a sibling closes. It comes
	// back — EOF, or a record if the screen moved first — and the machine is
	// still here to say so.
	int64_t h3 = os64_open("/dev/glass", "r");
	if (h3 < 0)
		return GLASSTEST_OPEN;
	while (record((int32_t)h3, buf, sizeof(buf), 300, &head) > 0)
		;
	int64_t th = os64_thread(park_and_read, (void *)h3);
	if (th < 0)
		return GLASSTEST_CLOSE;
	os64_sleep(200);
	os64_close((int32_t)h3);
	int64_t got = -1;
	if (os64_thread_join((int32_t)th, &got) != 0 || got < 0)
		return GLASSTEST_CLOSE;

	os64_close((int32_t)h);
	os64_printf("glasstest: the screen came through /dev/glass: whole, banded, refused, painted, still, closed\n");
	return GLASSTEST_OK;
}
