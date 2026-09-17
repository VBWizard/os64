// The window's font binding: one F2 text context and one immutable role set,
// so widget measurement and painting go through measured runs instead of a
// fixed 8x16 cell.
//
// WHY PER-WINDOW AND NOT PER-PROCESS: a window is the unit that adopts a font
// (FONT_PROVIDER.md's coherent adoption group), and the appearance workshop
// already draws a preview beside the live article. A process-wide face would
// make the preview indistinguishable from a publication.
//
// WHY LAZY: every GUI program links libui, and a program that only draws
// pixels never asks it to measure a string. The engine, its glyph cache and
// the builtin role set cost nothing until the first piece of text.
//
// WHEN THERE IS NO BINDING the widgets draw through the 8x16 bitmap painter
// they always used — a window that has not drawn text yet, or one whose
// engine refused a memory cap. That is a fallback, not a degradation: the
// builtin role set IS that face, so printable text lands on the same pixels
// either way (the host suite compares them). The two paths part only where
// a byte has a MEANING: the old painter drew the glyph at that index and
// advanced one cell, while a run obeys F0 — a tab advances to the next stop
// and any other control byte becomes one missing-glyph marker.

#include "os64/ui.h"
#include "ui_internal.h"
#include "os64/font_provider.h"
#include "os64/font_adopt.h"
#include "os64/text_draw.h"
#include "os64/draw.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/font_psf1.h"   // the bitmap cell the fallback path draws

// A role set resolved into the shapes the widgets actually ask for: the
// borrowed font list, the row geometry, and the tab stop that has to be
// measured because no metric carries it.
typedef struct
{
	os64_font_role_view_t view[OS64_FONT_ROLE_COUNT];
	os64_font_pos_t       tab_interval[OS64_FONT_ROLE_COUNT];
} ui_font_faces_t;

// What prepare stages and commit swaps in. It holds the candidate's own
// reference, so an aborted adoption releases exactly what it retained.
typedef struct ui_font_plan
{
	os64_font_set_t *candidate;
	ui_font_faces_t  faces;
	void            *app_plan;
	bool             app_planned;
} ui_font_plan_t;

typedef struct ui_font_binding
{
	os64_text_context_t *text;
	os64_font_set_t     *set;
	ui_font_faces_t      faces;
	size_t               live_bytes;
	os64_font_status_t   status;   // why there is no set, when there is none
	bool                 tried;    // the lazy builtin attempt has happened

	// Non-NULL only while prepare runs. Measurement answers from the
	// candidate through it, which is what lets an app plan its layout with
	// the new metrics before a single live bound moves.
	const ui_font_faces_t *staging;

	// The app's layout planner, if it registered one.
	os64_font_status_t (*plan)(os64_ui_t *ui, void *user, void **out);
	void  (*plan_commit)(os64_ui_t *ui, void *user, void *plan);
	void  (*plan_discard)(os64_ui_t *ui, void *user, void *plan);
	void  *plan_user;
} ui_font_binding_t;

// The engine allocates through libos64's heap, wrapped only to keep a
// live-byte count: F2 hands the original size back on free, so the count is
// exact and a fixture can assert a released binding owes nothing.
static void *binding_alloc(void *user, size_t bytes)
{
	ui_font_binding_t *b = user;
	void *p = os64_malloc(bytes);
	if (p)
		b->live_bytes += bytes;
	return p;
}

static void binding_free(void *user, void *allocation, size_t bytes)
{
	ui_font_binding_t *b = user;
	b->live_bytes -= bytes;
	os64_free(allocation);
}

// ── the binding's own lifetime ──────────────────────────────────────────────

static ui_font_binding_t *binding_of(os64_ui_t *ui)
{
	return (ui_font_binding_t *)ui->font;
}

// Create the context on demand. A failure is remembered in `status` so the
// next paint does not retry an engine that already refused; os64_ui_font_bind
// clears it, which is how an app retries deliberately.
static ui_font_binding_t *binding_ensure(os64_ui_t *ui)
{
	ui_font_binding_t *b = binding_of(ui);
	if (b)
		return b;

	b = os64_malloc(sizeof(*b));
	if (!b)
		return (ui_font_binding_t *)0;
	os64_memset(b, 0, sizeof(*b));

	os64_text_options_t options = {
		.memory = { b, binding_alloc, binding_free },
	};
	os64_font_status_t status = os64_font_context_create(&options, &b->text);
	if (status != OS64_FONT_OK) {
		// Keep the record rather than the wreckage: the widgets fall back to
		// the bitmap painter and os64_ui_font_status names why.
		b->text = (os64_text_context_t *)0;
		b->status = status;
	}
	ui->font = b;
	return b;
}

// Resolve a set into the faces the widgets ask for. A role whose view fails
// is left unbound and the others stay usable — one refused role must not
// cost a window every glyph it can still draw.
static void faces_resolve(ui_font_binding_t *b, os64_font_set_t *set,
                          ui_font_faces_t *faces)
{
	os64_memset(faces, 0, sizeof(*faces));
	if (!set)
		return;
	for (int role = 0; role < OS64_FONT_ROLE_COUNT; ++role) {
		os64_font_role_view_t *v = &faces->view[role];
		if (os64_font_set_view(set, (os64_font_role_t)role, v) != OS64_FONT_OK) {
			os64_memset(v, 0, sizeof(*v));
			continue;
		}
		// Eight columns of the primary's space: the editor's 1970s tab stop,
		// said in the only unit a proportional face has. Measured, because
		// no metric in the view carries it.
		os64_text_layout_t layout = {
			.encoding = OS64_TEXT_UTF8_WESTERN_V1,
			.fonts = v->fonts, .font_count = v->font_count,
			.tab_origin = 0, .tab_interval = 8 * OS64_FONT_UNIT,
		};
		os64_text_run_t *run = (os64_text_run_t *)0;
		os64_text_run_view_t rv;
		if (os64_text_layout(b->text, (const uint8_t *)" ", 1, &layout, &run) == OS64_FONT_OK) {
			if (os64_text_run_view(run, &rv) == OS64_FONT_OK && rv.advance_x > 0)
				faces->tab_interval[role] = 8 * rv.advance_x;
			os64_text_run_release(run);
		}
		if (faces->tab_interval[role] <= 0)
			faces->tab_interval[role] = 8 * OS64_FONT_GLYPH_W * OS64_FONT_UNIT;
	}
}

// The builtin role set, prepared once on first use. NULL specs are the
// provider's shorthand for builtin at 16, which is the face the widgets drew
// with before this layer existed.
static bool binding_ensure_builtin(ui_font_binding_t *b)
{
	if (b->set)
		return true;
	if (b->tried || !b->text)
		return false;
	b->tried = true;
	os64_font_status_t status =
		os64_font_set_prepare(b->text, (const os64_font_role_spec_t *)0, &b->set);
	if (status != OS64_FONT_OK) {
		b->set = (os64_font_set_t *)0;
		b->status = status;
		return false;
	}
	faces_resolve(b, b->set, &b->faces);
	return true;
}

// The faces measurement and painting answer from: the CANDIDATE while an
// adoption is planning, the installed set otherwise. One accessor, so an
// app's planner and libui's own staging cannot disagree about which font
// they are measuring.
static const ui_font_faces_t *binding_faces(ui_font_binding_t *b)
{
	if (b->staging)
		return b->staging;
	return binding_ensure_builtin(b) ? &b->faces : (const ui_font_faces_t *)0;
}

// The same question asked WITHOUT building anything: what this window is
// already wearing, or nothing.
//
// WHY THE METRICS PATH TAKES THIS ONE: a widget re-derives its geometry the
// moment it joins a tree, long before anything is drawn, and a window that
// asks how tall a row is has not yet asked for a glyph. Answering from the
// 8x16 fallback costs no engine, and it is the same answer the builtin set
// gives — same face, same ascent — which is what lets a tree be laid out
// before the first paint decides a font is really needed. The host suite
// pins that equality so the shortcut cannot quietly start lying.
static const ui_font_faces_t *binding_faces_current(os64_ui_t *ui)
{
	ui_font_binding_t *b = ui ? binding_of(ui) : (ui_font_binding_t *)0;
	if (!b)
		return (const ui_font_faces_t *)0;
	if (b->staging)
		return b->staging;
	return b->set ? &b->faces : (const ui_font_faces_t *)0;
}

// The view a role draws with, or NULL when this window has no usable set and
// the caller should take the bitmap path.
static const os64_font_role_view_t *role_view(os64_ui_t *ui, os64_font_role_t role)
{
	// An unattached widget has no window and therefore no binding; it draws
	// through the bitmap painter rather than refusing to draw at all.
	if (!ui || (unsigned)role >= OS64_FONT_ROLE_COUNT)
		return (const os64_font_role_view_t *)0;
	ui_font_binding_t *b = binding_ensure(ui);
	if (!b)
		return (const os64_font_role_view_t *)0;
	const ui_font_faces_t *faces = binding_faces(b);
	if (!faces)
		return (const os64_font_role_view_t *)0;
	const os64_font_role_view_t *v = &faces->view[role];
	return v->font_count ? v : (const os64_font_role_view_t *)0;
}

// ── measuring and painting ──────────────────────────────────────────────────

// Lay one line out. The run is the caller's to release.
static os64_text_run_t *role_run(os64_ui_t *ui, os64_font_role_t role,
                                 const char *s, size_t len)
{
	const os64_font_role_view_t *v = role_view(ui, role);
	if (!v)
		return (os64_text_run_t *)0;
	ui_font_binding_t *b = binding_of(ui);
	os64_text_layout_t layout = {
		.encoding = OS64_TEXT_UTF8_WESTERN_V1,
		.fonts = v->fonts, .font_count = v->font_count,
		.tab_origin = 0, .tab_interval = binding_faces(b)->tab_interval[role],
	};
	os64_text_run_t *run = (os64_text_run_t *)0;
	if (os64_text_layout(b->text, (const uint8_t *)s, len, &layout, &run) != OS64_FONT_OK)
		return (os64_text_run_t *)0;
	return run;
}

// 26.6 to whole pixels, outward: a width that decides whether text fits must
// never come back short of the ink it promised to hold.
static int32_t px_ceil(os64_font_pos_t v)
{
	return (int32_t)((v + (OS64_FONT_UNIT - 1)) / OS64_FONT_UNIT);
}

int32_t os64_ui_text_width(os64_ui_t *ui, os64_font_role_t role,
                           const char *s, size_t len)
{
	if (!s || !len)
		return 0;
	os64_text_run_t *run = role_run(ui, role, s, len);
	if (!run) {
		// The bitmap cell, which is what this window is still drawing with.
		return (int32_t)len * OS64_FONT_GLYPH_W;
	}
	os64_text_run_view_t rv;
	int32_t width = os64_text_run_view(run, &rv) == OS64_FONT_OK ? px_ceil(rv.advance_x)
	                                                             : (int32_t)len * OS64_FONT_GLYPH_W;
	os64_text_run_release(run);
	return width;
}

bool os64_ui_font_metrics(os64_ui_t *ui, os64_font_role_t role,
                          os64_ui_font_metrics_t *out)
{
	const ui_font_faces_t *faces =
		(unsigned)role < OS64_FONT_ROLE_COUNT ? binding_faces_current(ui)
		                                      : (const ui_font_faces_t *)0;
	const os64_font_role_view_t *v =
		faces && faces->view[role].font_count ? &faces->view[role]
		                                      : (const os64_font_role_view_t *)0;
	if (!v) {
		// The bitmap painter's own row: os64_text_font_bitmap reports the
		// same face as ascent 12 of a 16-pixel line box, so a window that
		// falls back here keeps the geometry it would have had.
		out->row_h = OS64_FONT_GLYPH_H;
		out->baseline = 12;
		out->cell_w = OS64_FONT_GLYPH_W;
		return false;
	}
	out->row_h = v->row_height_px;
	out->baseline = v->baseline_px;
	out->cell_w = v->cell_width_px;
	return true;
}

int32_t os64_ui_font_row_height(os64_ui_t *ui, os64_font_role_t role)
{
	os64_ui_font_metrics_t m;
	os64_ui_font_metrics(ui, role, &m);
	return m.row_h;
}

int32_t os64_ui_draw_text(os64_ui_t *ui, os64_font_role_t role,
                          os64_gui_surface_t *dst, os64_gui_rect_t clip,
                          int32_t x, int32_t top_y, const char *s, size_t len,
                          uint32_t fg, uint32_t bg)
{
	if (!s)
		s = "";
	os64_text_run_t *run = len ? role_run(ui, role, s, len) : (os64_text_run_t *)0;
	if (!run) {
		if (!len)
			return x;
		return os64_draw_text_clipped(dst, clip, x, top_y, s, len, fg, bg);
	}

	os64_text_run_view_t rv;
	if (os64_text_run_view(run, &rv) != OS64_FONT_OK) {
		os64_text_run_release(run);
		return os64_draw_text_clipped(dst, clip, x, top_y, s, len, fg, bg);
	}

	os64_ui_font_metrics_t m;
	os64_ui_font_metrics(ui, role, &m);
	int32_t advance = px_ceil(rv.advance_x);

	// The paper before the ink. os64_text_draw paints coverage only, while
	// the painter it replaces filled each cell — so the row's own box is
	// cleared here, or a shortened caption leaves its old tail on screen.
	os64_gui_rect_t box = { x, top_y, advance, m.row_h };
	os64_gui_rect_t painted;
	if (os64_rect_intersect(box, clip, &painted))
		os64_draw_fill_rect(dst, painted, bg);

	os64_text_draw(run, dst, clip, x, top_y + m.baseline, fg);
	os64_text_run_release(run);
	return x + advance;
}

// ── the public binding surface ──────────────────────────────────────────────

os64_text_context_t *os64_ui_font_context(os64_ui_t *ui)
{
	if (!ui)
		return (os64_text_context_t *)0;
	ui_font_binding_t *b = binding_ensure(ui);
	return b ? b->text : (os64_text_context_t *)0;
}

os64_font_set_t *os64_ui_font_set(os64_ui_t *ui)
{
	if (!ui)
		return (os64_font_set_t *)0;
	ui_font_binding_t *b = binding_of(ui);
	return b ? b->set : (os64_font_set_t *)0;
}

os64_font_status_t os64_ui_font_status(const os64_ui_t *ui)
{
	const ui_font_binding_t *b = ui ? (const ui_font_binding_t *)ui->font
	                               : (const ui_font_binding_t *)0;
	return b ? b->status : OS64_FONT_OK;
}

size_t os64_ui_font_live_bytes(const os64_ui_t *ui)
{
	const ui_font_binding_t *b = ui ? (const ui_font_binding_t *)ui->font
	                               : (const ui_font_binding_t *)0;
	return b ? b->live_bytes : 0;
}

os64_font_status_t os64_ui_font_bind(os64_ui_t *ui, os64_font_set_t *set)
{
	if (!ui)
		return OS64_FONT_BAD_ARGUMENT;
	ui_font_binding_t *b = binding_ensure(ui);
	if (!b)
		return OS64_FONT_NO_MEMORY;
	if (!b->text)
		return b->status;
	if (set) {
		os64_font_status_t status = os64_font_set_retain(set);
		if (status != OS64_FONT_OK)
			return status;
	}
	os64_font_set_release(b->set);
	b->set = set;
	b->status = OS64_FONT_OK;
	b->tried = set != (os64_font_set_t *)0;
	faces_resolve(b, b->set, &b->faces);
	os64_ui_font_restamp(ui);
	if (ui->root)
		os64_ui_mark_dirty(ui, ui->root);
	return OS64_FONT_OK;
}

void os64_ui_font_planner(os64_ui_t *ui,
                          os64_font_status_t (*plan)(os64_ui_t *ui, void *user, void **out),
                          void (*commit)(os64_ui_t *ui, void *user, void *plan),
                          void (*discard)(os64_ui_t *ui, void *user, void *plan),
                          void *user)
{
	ui_font_binding_t *b = ui ? binding_ensure(ui) : (ui_font_binding_t *)0;
	if (!b)
		return;
	b->plan = plan;
	b->plan_commit = commit;
	b->plan_discard = discard;
	b->plan_user = user;
}

// ── the adoption consumer ───────────────────────────────────────────────────
// One window is one consumer: its widgets and the application layout that
// positions them change together or not at all.

static void restamp_tree(os64_ui_widget_t *w, os64_ui_t *ui)
{
	if (!w)
		return;
	if (w->cls && w->cls->metrics)
		w->cls->metrics(w, ui);
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		restamp_tree(c, ui);
}

void os64_ui_font_restamp(os64_ui_t *ui)
{
	if (!ui)
		return;
	restamp_tree(ui->root, ui);
}

static os64_font_status_t consumer_prepare(void *user, os64_font_set_t *candidate,
                                           void **plan_out)
{
	os64_ui_t *ui = user;
	*plan_out = (void *)0;
	ui_font_binding_t *b = binding_ensure(ui);
	if (!b || !b->text)
		return b ? b->status : OS64_FONT_NO_MEMORY;

	ui_font_plan_t *plan = os64_malloc(sizeof(*plan));
	if (!plan)
		return OS64_FONT_NO_MEMORY;
	os64_memset(plan, 0, sizeof(*plan));

	os64_font_status_t status = os64_font_set_retain(candidate);
	if (status != OS64_FONT_OK) {
		os64_free(plan);
		return status;
	}
	plan->candidate = candidate;
	faces_resolve(b, candidate, &plan->faces);

	// Everything from here measures the candidate. The app's planner runs
	// inside that window, so the layout it returns is the one the new face
	// will actually get — and a failure costs only this staging.
	if (b->plan) {
		b->staging = &plan->faces;
		status = b->plan(ui, b->plan_user, &plan->app_plan);
		b->staging = (const ui_font_faces_t *)0;
		if (status != OS64_FONT_OK) {
			os64_font_set_release(plan->candidate);
			os64_free(plan);
			return status;
		}
		plan->app_planned = true;
	}

	*plan_out = plan;
	return OS64_FONT_OK;
}

static void consumer_commit(void *user, void *plan_ptr)
{
	os64_ui_t *ui = user;
	ui_font_binding_t *b = binding_of(ui);
	ui_font_plan_t *plan = plan_ptr;

	// The candidate is installed before the old set goes, so no reader can
	// see faces that name a released set's fonts.
	os64_font_set_t *retired = b->set;
	b->set = plan->candidate;          // the plan's reference becomes ours
	b->faces = plan->faces;
	b->status = OS64_FONT_OK;
	b->tried = true;
	os64_font_set_release(retired);

	// WIDGET GEOMETRY FIRST, THEN THE APP'S. A layout arranges widgets by
	// the heights they report, so the app's commit has to see the heights
	// this face gives them — restamping afterwards arranges the window
	// against the face it just stopped using, and the rows overlap.
	os64_ui_font_restamp(ui);
	if (plan->app_planned && b->plan_commit)
		b->plan_commit(ui, b->plan_user, plan->app_plan);

	// The whole window repaints: every run in it was laid out against the
	// set just retired.
	if (ui->root)
		os64_ui_mark_dirty(ui, ui->root);
	os64_free(plan);
}

static void consumer_abort(void *user, void *plan_ptr)
{
	os64_ui_t *ui = user;
	ui_font_binding_t *b = binding_of(ui);
	ui_font_plan_t *plan = plan_ptr;

	if (plan->app_planned && b->plan_discard)
		b->plan_discard(ui, b->plan_user, plan->app_plan);
	os64_font_set_release(plan->candidate);
	os64_free(plan);
}

void os64_ui_font_consumer(os64_ui_t *ui, os64_font_consumer_t *out)
{
	out->user = ui;
	out->prepare = consumer_prepare;
	out->barrier = (os64_font_status_t (*)(void *, void *))0;
	out->commit = consumer_commit;
	out->abort = consumer_abort;
}

void os64_ui_font_release(os64_ui_t *ui)
{
	if (!ui)
		return;
	ui_font_binding_t *b = binding_of(ui);
	if (!b)
		return;
	ui->font = (void *)0;
	os64_font_set_release(b->set);
	if (b->text)
		os64_text_destroy(b->text);
	os64_free(b);
}
