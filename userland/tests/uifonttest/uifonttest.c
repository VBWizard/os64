// uifonttest — libui's widgets under a real face, on the glass.
//
// The host suite proves the arithmetic; this proves the WINDOW. It builds an
// ordinary widget tree, then replaces the window's font through the same
// adoption transaction F5 will use, so what you see is the production path
// with fixture-supplied bytes rather than a demonstration of its own.
//
// The keys:
//   1..4   builtin 8x16, DejaVu Sans, Source Sans 3, Source Code Pro
//   + / -  nominal pixel size
//   r      arm a refusal: the next change is rejected during planning, and
//          the window must keep the layout and face it already had
//   q      quit
//
// WHY THE FONTS COME FROM /tests AND NOT A CONFIG FILE: which fonts os64
// ships with, and how a person chooses one, is F5's question. A fixture
// reading fixture bytes pre-empts none of it.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/ui.h"
#include "os64/fmt.h"
#include "os64/str.h"
#include "os64/slurp.h"
#include "os64/font_provider.h"
#include "os64/font_adopt.h"

#define WIN_W 520u
#define WIN_H 560u
#define FONT_MAX (4u * 1024u * 1024u)

static os64_ui_t        gUi;
static os64_ui_widget_t gRoot, gTitle, gStatus, gSample, gBtnA, gBtnB;
static os64_ui_checkbox_t gCheck;
static os64_ui_listbox_t  gList;
static char             gStatusText[96];
static volatile bool    gRunning = true;
static bool             gRefuseNext;
static int32_t          gLastNeeded, gLastHave;

static const char *const kFaces[] = {
	"builtin 8x16", "DejaVuSans.ttf", "SourceSans3-Regular.otf",
	"SourceCodePro-Regular.otf",
};
static size_t   gFace = 0;
static uint32_t gSize = 16;

static const char *const kRows[] = {
	"Waltz, bad nymph, for quick jigs vex",
	"iiiii vs WWWWW",
	"kerning: AV To Ta We",
	"digits 0123456789",
};

static const char *row_label(size_t index, void *user)
{
	(void)user;
	return index < sizeof(kRows) / sizeof(*kRows) ? kRows[index] : "";
}

// ── the app's layout, planned against whatever face is being adopted ───────
//
// Heights come from the live row metric, so this one function serves startup
// and every later replacement. During adoption libui answers with the
// CANDIDATE, which is what makes the "does it fit?" question honest.

typedef struct { int32_t list_h, needed, have; } layout_plan_t;

static void apply_layout(const layout_plan_t *plan)
{
	// The labels, the checkbox and the buttons keep bounds.h at zero and let
	// libui size them from the face — which is the point of the exercise.
	gList.w.bounds.h = plan->list_h;
	os64_ui_stack_vertical(&gUi, &gRoot);
}

// THE LIST IS THE ELASTIC PART. Everything else needs exactly what the face
// makes it need; whatever is left over is the list's, down to a floor of one
// row. A window is a fixed thing and a face is not, so something has to give
// — refusing outright is the answer only when even the rigid rows no longer
// fit, and that is what this returns LIMIT for.
static os64_font_status_t measure_layout(os64_ui_t *ui, layout_plan_t *out)
{
	int32_t label_h = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
	int32_t control_h = os64_ui_control_min_height(ui);
	int32_t row_h = label_h + 8;                 // ui_list's own padding
	int32_t gap = ui->theme.gap, pad = ui->theme.pad;
	int32_t rigid = 2 * pad + 3 * (label_h + gap) + (control_h + gap) +
	                2 * (control_h + gap) + gap;

	out->have = gRoot.bounds.h;
	out->list_h = out->have - rigid;
	if (out->list_h > row_h * 4 + 8)
		out->list_h = row_h * 4 + 8;             // never taller than its rows
	out->needed = rigid + row_h + 8;             // the floor: one visible row
	return out->needed <= out->have ? OS64_FONT_OK : OS64_FONT_LIMIT;
}

static os64_font_status_t plan_layout(os64_ui_t *ui, void *user, void **out)
{
	(void)user;
	if (gRefuseNext) {
		gRefuseNext = false;
		return OS64_FONT_LIMIT;
	}
	layout_plan_t measured;
	os64_font_status_t status = measure_layout(ui, &measured);
	if (status != OS64_FONT_OK) {
		gLastNeeded = measured.needed;
		gLastHave = measured.have;
		return status;
	}
	layout_plan_t *staged = os64_malloc(sizeof(*staged));
	if (!staged)
		return OS64_FONT_NO_MEMORY;
	*staged = measured;
	*out = staged;
	return OS64_FONT_OK;
}

static void commit_layout(os64_ui_t *ui, void *user, void *plan)
{
	(void)ui; (void)user;
	apply_layout((const layout_plan_t *)plan);
	os64_free(plan);
}

static void discard_layout(os64_ui_t *ui, void *user, void *plan)
{
	(void)ui; (void)user;
	os64_free(plan);
}

// ── choosing a face ────────────────────────────────────────────────────────

static void status_show(const char *outcome)
{
	os64_ui_font_metrics_t m;
	os64_ui_font_metrics(&gUi, OS64_FONT_ROLE_UI, &m);
	os64_snprintf(gStatusText, sizeof(gStatusText), "%s %upx  row %d base %d  %s",
	              kFaces[gFace], gSize, (int)m.row_h, (int)m.baseline, outcome);
	os64_ui_mark_dirty(&gUi, &gRoot);
}

// Build the three role specs from one file. The terminal role must be
// fixed-width or the provider refuses the whole set, so it keeps the mono
// face whatever the UI is wearing — this fixture has no terminal to show,
// but a set with a hole in it is not a set.
static os64_font_status_t adopt_face(void)
{
	os64_text_context_t *text = os64_ui_font_context(&gUi);
	if (!text)
		return OS64_FONT_NO_MEMORY;

	uint8_t *ui_bytes = (uint8_t *)0, *mono_bytes = (uint8_t *)0;
	size_t ui_len = 0, mono_len = 0;
	os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
	char path[96];

	if (gFace != 0) {
		os64_snprintf(path, sizeof(path), "/tests/fonts/%s", kFaces[gFace]);
		if (os64_slurp(path, FONT_MAX, &ui_bytes, &ui_len) != OS64_SLURP_OK) {
			os64_printf("uifonttest: cannot read %s\n", path);
			return OS64_FONT_MISSING;
		}
	}
	if (os64_slurp("/tests/fonts/DejaVuSansMono.ttf", FONT_MAX,
	               &mono_bytes, &mono_len) != OS64_SLURP_OK) {
		os64_free(ui_bytes);
		return OS64_FONT_MISSING;
	}

	for (int role = 0; role < OS64_FONT_ROLE_COUNT; ++role) {
		specs[role].pixel_height = gFace == 0 ? 16 : gSize;
		if (gFace != 0)
			specs[role].primary =
				(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, ui_bytes, ui_len};
	}
	specs[OS64_FONT_ROLE_TERMINAL].pixel_height = gFace == 0 ? 16 : gSize;
	specs[OS64_FONT_ROLE_TERMINAL].primary =
		(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, mono_bytes, mono_len};

	os64_font_set_t *candidate = (os64_font_set_t *)0;
	os64_font_problem_t problem;
	os64_font_status_t status =
		os64_font_set_prepare_checked(text, specs, &candidate, &problem);
	os64_free(ui_bytes);
	os64_free(mono_bytes);
	if (status != OS64_FONT_OK) {
		os64_printf("uifonttest: prepare failed (status %d, role %d source %lu)\n",
		            (int)status, (int)problem.role, (unsigned long)problem.source_index);
		return status;
	}

	os64_font_consumer_t consumer;
	os64_ui_font_consumer(&gUi, &consumer);
	size_t failed = 0;
	status = os64_font_adopt(candidate, &consumer, 1, &failed);
	os64_font_set_release(candidate);
	return status;
}

static void choose(size_t face, uint32_t size)
{
	size_t was_face = gFace;
	uint32_t was_size = gSize;
	gFace = face;
	gSize = size;
	os64_font_status_t status = adopt_face();
	if (status != OS64_FONT_OK) {
		// The window keeps the face and the layout it had; only the label
		// tells you an attempt was made and turned down.
		gFace = was_face;
		gSize = was_size;
		if (status == OS64_FONT_LIMIT) {
			char why[48];
			os64_snprintf(why, sizeof(why), "REFUSED need %d have %d",
			              (int)gLastNeeded, (int)gLastHave);
			status_show(why);
		} else {
			status_show("FAILED (kept old)");
		}
		return;
	}
	status_show("ok");
}

static void on_quit(os64_ui_widget_t *w, void *user)
{
	(void)w; (void)user;
	gRunning = false;
}

static void on_resize(os64_ui_t *ui)
{
	layout_plan_t plan;
	if (measure_layout(ui, &plan) == OS64_FONT_OK)
		apply_layout(&plan);
}

static bool shortcut(const os64_gui_event_t *ev)
{
	if (ev->type != OS64_GUI_EVENT_KEY_DOWN)
		return false;
	char a = ev->key.ascii;
	if (a >= '1' && a <= '4') {
		choose((size_t)(a - '1'), gSize);
		return true;
	}
	switch (a) {
	case '+': case '=': choose(gFace, gSize < 72 ? gSize + 4 : gSize); return true;
	case '-': case '_': choose(gFace, gSize > 8 ? gSize - 4 : gSize); return true;
	case 'r': gRefuseNext = true; status_show("refusal armed"); return true;
	case 'q': gRunning = false; return true;
	default: return false;
	}
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	int64_t win = os64_gui_window_create("uifonttest", 220, 60, WIN_W, WIN_H,
	                                     OS64_GUI_WINDOW_START_UNFOCUSED);
	if (win <= 0) {
		os64_printf("uifonttest: no GUI (window_create %ld)\n", (long)win);
		return 1;
	}
	os64_draw_ctx_t ctx;
	if (os64_draw_ctx_init(&ctx, win) != 0) {
		os64_printf("uifonttest: get_surface failed\n");
		return 1;
	}

	os64_ui_init(&gUi, &ctx);
	gUi.on_resize = on_resize;

	os64_ui_panel(&gRoot);
	gRoot.bounds = (os64_gui_rect_t){0, 0, (int32_t)ctx.surf.width,
	                                 (int32_t)ctx.surf.height};
	os64_ui_label(&gTitle, "libui under a measured face");
	os64_ui_label(&gStatus, gStatusText);
	os64_ui_label(&gSample, "AV To Ta We - iiii vs WWWW");
	os64_ui_checkbox(&gCheck, "a caption beside its box", true, (void *)0, (void *)0);
	os64_ui_listbox(&gList, sizeof(kRows) / sizeof(*kRows), row_label,
	                (void *)0, (void *)0);
	os64_ui_button(&gBtnA, "1-4 face   +/- size", (void *)0, (void *)0);
	os64_ui_button(&gBtnB, "quit", on_quit, (void *)0);

	os64_ui_add_child(&gRoot, &gTitle);
	os64_ui_add_child(&gRoot, &gStatus);
	os64_ui_add_child(&gRoot, &gSample);
	os64_ui_add_child(&gRoot, &gCheck.w);
	os64_ui_add_child(&gRoot, &gList.w);
	os64_ui_add_child(&gRoot, &gBtnA);
	os64_ui_add_child(&gRoot, &gBtnB);
	os64_ui_set_root(&gUi, &gRoot);

	if (os64_ui_font_planner(&gUi, plan_layout, commit_layout, discard_layout,
	                         (void *)0) != OS64_FONT_OK) {
		// Without a registered planner an adoption would install a face and
		// leave this window's layout untouched, which is exactly the silent
		// half-change the transaction exists to prevent.
		os64_printf("uifonttest: planner registration failed\n");
		return 1;
	}
	layout_plan_t startup;
	if (measure_layout(&gUi, &startup) == OS64_FONT_OK)
		apply_layout(&startup);
	status_show("startup");

	// One printed line so a headless run still says what it found.
	os64_printf("uifonttest: builtin row %d, keys 1-4 face, +/- size, r refuse, q quit\n",
	            (int)os64_ui_font_row_height(&gUi, OS64_FONT_ROLE_UI));

	os64_ui_paint(&gUi);
	while (gRunning) {
		os64_gui_event_t ev;
		if (os64_gui_event_wait(win, &ev) != 1)
			break;
		do {
			if (!shortcut(&ev))
				os64_ui_dispatch(&gUi, &ev);
		} while (gRunning && os64_gui_event_poll(win, &ev) == 1);
		os64_ui_paint(&gUi);
	}

	os64_font_status_t released = os64_ui_font_release(&gUi);
	if (released != OS64_FONT_OK)
		os64_printf("uifonttest: font release refused (%d)\n", (int)released);
	os64_gui_window_destroy(win);
	return 0;
}
