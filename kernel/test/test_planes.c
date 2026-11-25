#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "gui/plane.h"
#include "smp_core.h"
#include "task.h"
#include "printd.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"

// A window's planes through the calls a syscall reaches (VIDEO_PLANE.md
// § 3.10): the commit is the lock hold, the copy and the edits together, so
// the composer alone (tools/test_plane_compose_host.sh) cannot prove it.
// A kernel task's window: its canvases are kernel memory, and its kept
// pairs are freed at the end, since the kernel task never dies. The window
// sits far off the screen, so nothing shows; the compositor's part is
// driven into a private view.

#define PT_X (-4000)
#define PT_Y (-4000)
#define PT_PIC_W 64
#define PT_PIC_H 48

static bool s_ok;

static void check(bool cond, const char *what, int64_t got)
{
	if (!cond) {
		printd(DEBUG_TESTS, "\tFAIL: test_planes - %s (answered %ld)\n", what, got);
		s_ok = false;
	}
}

static window_t *window_of(int64_t handle)
{
	int64_t err;
	return gui_handle_owned_locked(handle, &err);
}

static os64_gui_plane_edit_t place_edit(uint32_t plane, int32_t x, int32_t y, uint32_t noverlays)
{
	os64_gui_plane_edit_t e = {0};
	e.plane = plane;
	e.origin = (os64_gui_rect_t){x, y, PT_PIC_W, PT_PIC_H};
	e.clip = e.origin;
	e.noverlays = noverlays;
	if (noverlays > 0)
		e.overlay[0] = (os64_gui_rect_t){x, y, PT_PIC_W, 8};
	return e;
}

static os64_gui_plane_edit_t hide_edit(uint32_t plane)
{
	os64_gui_plane_edit_t e = {0};
	e.plane = plane;
	e.hide = 1;
	return e;
}

static uint32_t plane_state(int64_t handle, uint32_t name)
{
	uint64_t irq = gui_lock(GUI_LOCK_OTHER);
	window_t *w = window_of(handle);
	uint32_t s = w ? w->planes[(name & 0xFF) - 1].state : 0xFF;
	gui_unlock(irq);
	return s;
}

// The picture: each pixel its own coordinates, so a misplaced row shows.
static void paint_picture(int64_t handle, uint32_t name)
{
	os64_gui_plane_surface_t s;
	if (gui_plane_surface(handle, name, &s) != 0)
		return;
	for (uint32_t y = 0; y < PT_PIC_H; y++)
		for (uint32_t x = 0; x < PT_PIC_W; x++)
			s.pixels[y * s.pitch_px + x] = 0x00100000u | (y << 8) | x;
}

static void paint_content(int64_t handle, uint32_t value)
{
	surface_t s;
	if (gui_window_get_surface(handle, &s) != 0)
		return;
	for (uint32_t y = 0; y < s.height; y++)
		for (uint32_t x = 0; x < s.width; x++)
			s.pixels[y * s.pitch_px + x] = value;
}

// The compositor's part over the whole content, into a private view whose
// first pixel is the content's; every pixel checked against what § 3.3
// says it must be. A view bigger than the content by a margin of sentinel
// pixels shows anything drawn past it.
static void check_composed(int64_t handle, int32_t ox, int32_t oy, int32_t overlay_rows, uint32_t content_px,
                           const char *what)
{
	enum { MARGIN = 16 };
	uint64_t irq = gui_lock(GUI_LOCK_OTHER);
	window_t *w = window_of(handle);
	if (w == NULL) {
		gui_unlock(irq);
		check(false, what, 0);
		return;
	}
	uint32_t cw = w->content.width, ch = w->content.height;
	gui_unlock(irq);
	uint32_t vw = cw + MARGIN, vh = ch + MARGIN;
	uint32_t *pixels = kmalloc_try((uint64_t)vw * vh * 4);
	if (pixels == NULL) {
		check(false, "no memory for the view", 0);
		return;
	}
	for (uint32_t i = 0; i < vw * vh; i++)
		pixels[i] = 0xDEADBEEF;
	surface_t view = {pixels, vw, vh, vw};
	irq = gui_lock(GUI_LOCK_OTHER);
	w = window_of(handle);
	bool drew = w && gui_planes_compose(w, &view, (rect_t){0, 0, (int32_t)vw, (int32_t)vh},
	                                    (rect_t){0, 0, (int32_t)cw, (int32_t)ch});
	gui_unlock(irq);
	uint32_t bad = 0;
	for (uint32_t y = 0; y < vh; y++) {
		for (uint32_t x = 0; x < vw; x++) {
			uint32_t want;
			int32_t px = (int32_t)x - ox, py = (int32_t)y - oy;
			bool in_pic = px >= 0 && px < PT_PIC_W && py >= 0 && py < PT_PIC_H;
			if (x >= cw || y >= ch)
				want = 0xDEADBEEF;   // past the content: never drawn
			else if (!in_pic)
				want = content_px;
			else if (py < overlay_rows)
				want = plane_blend(content_px, 0x00100000u | ((uint32_t)py << 8) | (uint32_t)px);
			else
				want = 0x00100000u | ((uint32_t)py << 8) | (uint32_t)px;
			if (pixels[y * vw + x] != want)
				bad++;
		}
	}
	kfree(pixels);
	check(drew && bad == 0, what, (int64_t)bad);
}

bool test_planes(void)
{
	int64_t handle = gui_window_create("planes", PT_X, PT_Y, 320, 200, GUI_WINDOW_START_UNFOCUSED);
	if (handle == GUI_ERR_NOT_RUNNING) {
		printd(DEBUG_TESTS, "\tSKIP: test_planes (no GUI on this boot)\n");
		return true;
	}
	if (handle <= 0) {
		printd(DEBUG_TESTS, "\tFAIL: test_planes - window create answered %ld\n", handle);
		return false;
	}
	s_ok = true;
	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls->task;
	thread_t *self = cls->currentThread;

	// Sizes: none, and past the largest class.
	check(gui_plane_create(handle, 0, 10) == GUI_ERR_BAD_ARGS, "a zero size refused", 0);
	check(gui_plane_create(handle, 4000, 100) == OS64_GUI_ERR_ABOVE_CAPACITY, "past 3840 refused", 0);

	// Made, drawn, published: hidden until a window publish places it.
	int64_t a = gui_plane_create(handle, PT_PIC_W, PT_PIC_H);
	check(a > 0, "create", a);
	if (a <= 0)
		goto out;
	uint32_t pa = (uint32_t)a;
	os64_gui_plane_surface_t surf;
	int64_t rc = gui_plane_surface(handle, pa, &surf);
	check(rc == 0 && surf.cap_w == 640 && surf.cap_h == 360 && surf.pitch_px == 640, "surface: the 640x360 class", rc);
	os64_gui_plane_edit_t e = place_edit(pa, 20, 30, 1);
	rc = gui_window_publish_edits(handle, NULL, &e, 1);
	check(rc == OS64_GUI_ERR_PLANE_PICTURE, "a place before any picture refused (rule 4)", rc);
	paint_picture(handle, pa);
	check(gui_plane_publish(handle, pa, PT_PIC_W, PT_PIC_H) == 0, "publish", 0);
	check(plane_state(handle, pa) == GUI_PLANE_PICTURED, "published, still hidden", plane_state(handle, pa));

	// Rule 1: a name that is no plane.
	os64_gui_plane_edit_t bogus = place_edit(pa + 1, 20, 30, 0);
	rc = gui_window_publish_edits(handle, NULL, &bogus, 1);
	check(rc == OS64_GUI_ERR_NO_PLANE, "an unknown name refused (rule 1)", rc);
	// Rule 4: an origin not the picture's size.
	os64_gui_plane_edit_t wrong = place_edit(pa, 20, 30, 0);
	wrong.origin.w = PT_PIC_W - 1;
	rc = gui_window_publish_edits(handle, NULL, &wrong, 1);
	check(rc == OS64_GUI_ERR_PLANE_PICTURE, "an origin of another size refused (rule 4)", rc);
	// Rule 2: damage short of the new clip; nothing changes.
	os64_gui_rect_t short_damage = {0, 0, 40, 40};
	rc = gui_window_publish_edits(handle, &short_damage, &e, 1);
	check(rc == OS64_GUI_ERR_PLANE_DAMAGE, "damage short of the clip refused (rule 2)", rc);
	check(plane_state(handle, pa) == GUI_PLANE_PICTURED, "a refused commit changed nothing", 0);
	// An overlay outside its clip.
	os64_gui_plane_edit_t out_of_clip = place_edit(pa, 20, 30, 1);
	out_of_clip.overlay[0].x = 0;
	rc = gui_window_publish_edits(handle, NULL, &out_of_clip, 1);
	check(rc == OS64_GUI_ERR_PLANE_OVERLAP, "an overlay outside its clip refused", rc);

	// Placed, with the content's pixels in the same commit.
	uint32_t content_px = 0x80404040u;   // half covering, premultiplied
	paint_content(handle, content_px);
	rc = gui_window_publish_edits(handle, NULL, &e, 1);
	check(rc == 0 && plane_state(handle, pa) == GUI_PLANE_PLACED, "placed", rc);
	check_composed(handle, 20, 30, 8, content_px, "composed: picture, overlay blended, content around");

	// While placed: the picture cannot change size, and the plane cannot go.
	rc = gui_plane_publish(handle, pa, PT_PIC_W - 2, PT_PIC_H);
	check(rc == OS64_GUI_ERR_PLANE_PICTURE, "a resize while placed refused", rc);
	rc = gui_plane_destroy(handle, pa);
	check(rc == GUI_ERR_BUSY, "destroy while placed refused", rc);
	rc = gui_window_destroy(handle);
	check(rc == GUI_ERR_BUSY, "a window with planes refuses destroy", rc);

	// A publish that paints elsewhere leaves the place as it was.
	rc = gui_window_publish(handle, NULL);
	check(rc == 0 && plane_state(handle, pa) == GUI_PLANE_PLACED, "a plain publish keeps the place", rc);

	// The creator rule: anyone else is refused.
	uint64_t irq = gui_lock(GUI_LOCK_OTHER);
	window_t *w = window_of(handle);
	gui_plane_t *p = w ? &w->planes[(pa & 0xFF) - 1] : NULL;
	if (p)
		p->creator = NULL;
	gui_unlock(irq);
	rc = gui_plane_publish(handle, pa, PT_PIC_W, PT_PIC_H);
	check(rc == OS64_GUI_ERR_NOT_CREATOR, "another thread's publish refused", rc);
	irq = gui_lock(GUI_LOCK_OTHER);
	if (p)
		p->creator = self;
	gui_unlock(irq);

	// Rule 3: a second plane whose clip meets the first's.
	int64_t b = gui_plane_create(handle, PT_PIC_W, PT_PIC_H);
	check(b > 0, "a second plane", b);
	uint32_t pb = b > 0 ? (uint32_t)b : 0;
	os64_gui_plane_surface_t surf_b = {0};
	gui_plane_surface(handle, pb, &surf_b);
	paint_picture(handle, pb);
	gui_plane_publish(handle, pb, PT_PIC_W, PT_PIC_H);
	os64_gui_plane_edit_t meets = place_edit(pb, 40, 40, 0);
	rc = gui_window_publish_edits(handle, NULL, &meets, 1);
	check(rc == OS64_GUI_ERR_PLANE_OVERLAP, "clips that meet refused (rule 3)", rc);
	// Moving the first away in the same commit makes room: whole or nothing.
	os64_gui_plane_edit_t both[2] = {place_edit(pa, 200, 100, 0), meets};
	rc = gui_window_publish_edits(handle, NULL, both, 2);
	check(rc == 0 && plane_state(handle, pb) == GUI_PLANE_PLACED, "two edits in one commit", rc);

	// A window made smaller after the commit: the clip is cut to the content
	// as it stands, and nothing past it is read or drawn.
	irq = gui_lock(GUI_LOCK_OTHER);
	w = window_of(handle);
	if (w) {
		rect_t f = w->frame;
		(void)wm_resize(w, (rect_t){f.x, f.y, f.w - 100, f.h - 60});
	}
	gui_unlock(irq);
	os64_gui_plane_edit_t hide_b = hide_edit(pb);
	rc = gui_window_publish_edits(handle, NULL, &hide_b, 1);
	check(rc == 0 && plane_state(handle, pb) == GUI_PLANE_PICTURED, "hidden after the shrink", rc);
	check_composed(handle, 200, 100, 0, content_px, "composed after a shrink: the clip cut to the content");

	// The creator's exit takes its planes: hidden, then gone, pairs kept.
	gui_thread_planes_exit(self);
	check(plane_state(handle, pa) == GUI_PLANE_FREE && plane_state(handle, pb) == GUI_PLANE_FREE,
	      "a creator's exit took its planes", 0);
	rc = gui_plane_publish(handle, pa, PT_PIC_W, PT_PIC_H);
	check(rc == OS64_GUI_ERR_NO_PLANE, "the old name refused after", rc);

	// Kept pairs serve the next plane of their class: one of the two
	// canvases, and no new pair.
	gui_plane_status_t row;
	uint64_t refused[GUI_PLANE_REFUSALS], pairs0, kept0, bytes0, pairs1, kept1, bytes1;
	gui_planes_status(&row, 0, refused, &pairs0, &kept0, &bytes0);
	int64_t c = gui_plane_create(handle, 100, 100);
	os64_gui_plane_surface_t surf2 = {0};
	rc = c > 0 ? gui_plane_surface(handle, (uint32_t)c, &surf2) : c;
	gui_planes_status(&row, 0, refused, &pairs1, &kept1, &bytes1);
	check(rc == 0 && (surf2.pixels == surf.pixels || surf2.pixels == surf_b.pixels) && pairs1 == pairs0 &&
	      kept1 + 1 == kept0, "a kept pair reused", rc);
	check(c > 0 && (uint32_t)c != pa, "a new name for the slot", c);

	// The budget: set below the next pair, refused; put back.
	if (c > 0)
		gui_plane_destroy(handle, (uint32_t)c);
	gui_plane_release_kept_for_test(task);
	gui_plane_budget_for_test(1);
	rc = gui_plane_create(handle, PT_PIC_W, PT_PIC_H);
	check(rc == OS64_GUI_ERR_OVER_BUDGET, "over the budget refused", rc);
	gui_plane_budget_for_test(0);

out:
	// Every plane gone (a test that failed midway may have left some), the
	// kept pairs freed, the window destroyed.
	gui_thread_planes_exit(self);
	gui_plane_release_kept_for_test(task);
	rc = gui_window_destroy(handle);
	check(rc == 0, "the window destroyed at the end", rc);
	if (s_ok)
		printd(DEBUG_TESTS, "\tPASS: test_planes\n");
	return s_ok;
}
