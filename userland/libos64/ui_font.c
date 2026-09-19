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
#include "text_internal.h"    // F2's decoder: cluster boundaries without a layout
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
	// Whose context it is. libui destroys only what libui built; a borrowed
	// one belongs to the application, which outlives every window using it.
	bool                 owns_text;
	os64_font_set_t     *set;
	ui_font_faces_t      faces;
	size_t               live_bytes;
	os64_font_status_t   status;   // why there is no set, or what last failed
	bool                 tried;    // the lazy builtin attempt has happened

	// Non-NULL only while prepare runs. Measurement answers from the
	// candidate through it, which is what lets an app plan its layout with
	// the new metrics before a single live bound moves.
	const ui_font_faces_t *staging;

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
	} else {
		b->owns_text = true;
	}
	ui->font = b;
	return b;
}

// Resolve a set into the faces the widgets ask for. It CAN FAIL, and the
// caller must carry that: a set whose roles or whose tab stop could not be
// resolved is not a set this window can wear, and pretending otherwise
// substitutes measurements from a face nobody chose.
static os64_font_status_t faces_resolve(ui_font_binding_t *b, os64_font_set_t *set,
                                        ui_font_faces_t *faces)
{
	os64_memset(faces, 0, sizeof(*faces));
	if (!set)
		return OS64_FONT_OK;
	for (int role = 0; role < OS64_FONT_ROLE_COUNT; ++role) {
		os64_font_role_view_t *v = &faces->view[role];
		os64_font_status_t status =
			os64_font_set_view(set, (os64_font_role_t)role, v);
		if (status != OS64_FONT_OK)
			return status;

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
		status = os64_text_layout(b->text, (const uint8_t *)" ", 1, &layout, &run);
		if (status != OS64_FONT_OK)
			return status;            // could not measure: say so, do not guess
		status = os64_text_run_view(run, &rv);
		os64_font_pos_t advance = status == OS64_FONT_OK ? rv.advance_x : 0;
		os64_text_run_release(run);
		if (status != OS64_FONT_OK)
			return status;

		// A ZERO-WIDTH SPACE IS A MEASUREMENT, NOT AN ERROR, and it is the
		// one answer that cannot be a tab stop — F2 requires a positive
		// interval, and eight of nothing is nothing. A face like that gets
		// the builtin cell's eight columns as a declared policy. It is the
		// same number the old code reached for when a LAYOUT failed, which
		// is exactly why the two had to stop sharing an exit.
		faces->tab_interval[role] = advance > 0 ? 8 * advance
		                                        : 8 * OS64_FONT_GLYPH_W * OS64_FONT_UNIT;
	}
	return OS64_FONT_OK;
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
	status = faces_resolve(b, b->set, &b->faces);
	if (status != OS64_FONT_OK) {
		// Even the builtin face could not be resolved. Let go of it rather
		// than keep a half-resolved set: the bitmap painter still works.
		os64_font_set_release(b->set);
		b->set = (os64_font_set_t *)0;
		os64_memset(&b->faces, 0, sizeof(b->faces));
		b->status = status;
		return false;
	}
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

// Lay one line out against a role's faces. A NULL `out` run with an OK
// status is how "this window wears no face, draw with the bitmap cell"
// is spelled; any other status is a real failure and its own answer.
static os64_font_status_t role_layout(os64_ui_t *ui, os64_font_role_t role,
                                      const char *s, size_t len,
                                      os64_text_run_t **out)
{
	*out = (os64_text_run_t *)0;
	const os64_font_role_view_t *v = role_view(ui, role);
	if (!v)
		return OS64_FONT_OK;           // unbound: the bitmap cell is the answer
	ui_font_binding_t *b = binding_of(ui);
	os64_text_layout_t layout = {
		.encoding = OS64_TEXT_UTF8_WESTERN_V1,
		.fonts = v->fonts, .font_count = v->font_count,
		.tab_origin = 0, .tab_interval = binding_faces(b)->tab_interval[role],
	};
	os64_font_status_t status =
		os64_text_layout(b->text, (const uint8_t *)s, len, &layout, out);
	if (status != OS64_FONT_OK) {
		*out = (os64_text_run_t *)0;
		b->status = status;            // what os64_ui_font_status will report
	}
	return status;
}

// 26.6 to whole pixels, outward: a width that decides whether text fits must
// never come back short of the ink it promised to hold.
static int32_t px_ceil(os64_font_pos_t v)
{
	return (int32_t)((v + (OS64_FONT_UNIT - 1)) / OS64_FONT_UNIT);
}

os64_font_status_t os64_ui_text_measure(os64_ui_t *ui, os64_font_role_t role,
                                        const char *s, size_t len, int32_t *out)
{
	if (!out)
		return OS64_FONT_BAD_ARGUMENT;
	*out = 0;
	if (!s || !len)
		return OS64_FONT_OK;

	os64_text_run_t *run = (os64_text_run_t *)0;
	os64_font_status_t status = role_layout(ui, role, s, len, &run);
	if (status != OS64_FONT_OK)
		return status;
	if (!run) {
		*out = (int32_t)len * OS64_FONT_GLYPH_W;   // the bitmap cell
		return OS64_FONT_OK;
	}

	os64_text_run_view_t rv;
	status = os64_text_run_view(run, &rv);
	if (status == OS64_FONT_OK)
		*out = px_ceil(rv.advance_x);
	os64_text_run_release(run);
	return status;
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

// Does this retained run still say what the caption says? The run owns a
// copy of the bytes it was laid out from, so the question is answerable
// without trusting the caller to announce a change — and a caption is short
// enough that comparing it beats laying it out again.
static bool run_matches(os64_text_run_t *run, const char *s, size_t len)
{
	os64_text_run_view_t rv;
	if (!run || os64_text_run_view(run, &rv) != OS64_FONT_OK)
		return false;
	if (rv.byte_count != len)
		return false;
	for (size_t i = 0; i < len; ++i)
		if (rv.bytes[i] != (uint8_t)s[i])
			return false;
	return true;
}

// Defined with the run primitives below, because a caller that asks for a
// width and then draws has to come through the same door or pay for two
// layouts and risk two different answers.
static os64_font_status_t slot_run(os64_ui_t *ui, void **run_slot,
                                   os64_font_role_t role,
                                   const char *s, size_t len,
                                   os64_text_run_t **out);

int32_t os64_ui_draw_text(os64_ui_t *ui, void **run_slot,
                          os64_font_role_t role,
                          os64_gui_surface_t *dst, os64_gui_rect_t clip,
                          int32_t x, int32_t top_y, const char *s, size_t len,
                          uint32_t fg, uint32_t bg)
{
	if (!s)
		s = "";
	if (!len)
		return x;

	// The retained run is the usual case: adoption staged it, so the paint
	// that follows a commit allocates nothing. Text that has changed
	// underneath it is re-laid-out here and retained in the same slot — the
	// same door os64_ui_run_width uses, so a painter that asked for a width
	// and then draws gets one layout between them, not two.
	os64_text_run_t *run = (os64_text_run_t *)0;
	if (slot_run(ui, run_slot, role, s, len, &run) != OS64_FONT_OK) {
		// A window wearing a face has no honest way to draw this. The
		// bitmap cell is a DIFFERENT font at a different size, so
		// substituting it would put the wrong glyphs at the wrong widths
		// and call it success. Leave the row as it is; the failure is on
		// the binding for anyone who asks.
		return x;
	}
	bool borrowed = run_slot && *run_slot == (void *)run;

	if (!run)   // unbound window: the painter this layer replaced
		return os64_draw_text_clipped(dst, clip, x, top_y, s, len, fg, bg);

	os64_text_run_view_t rv;
	if (os64_text_run_view(run, &rv) != OS64_FONT_OK) {
		if (!borrowed)
			os64_text_run_release(run);
		return x;
	}

	os64_ui_font_metrics_t m;
	os64_ui_font_metrics(ui, role, &m);
	int32_t advance = px_ceil(rv.advance_x);

	// THE ROW IS THE CEILING AND THE FLOOR. Pitch comes from the primary
	// face and does not grow for a taller fallback or a missing-glyph
	// marker (FONT_PROVIDER.md), so a glyph that overflows the line box is
	// cut here — otherwise an 8px row carrying a 16px marker paints over
	// its neighbours, and the row above belongs to a different widget.
	// Horizontally the caller's clip still rules, overhang included.
	os64_gui_rect_t row = { clip.x, top_y, clip.w, m.row_h };
	os64_gui_rect_t row_clip;
	if (!os64_rect_intersect(clip, row, &row_clip)) {
		// Nothing of this row is visible. The pen still advanced — a
		// caller laying a line out depends on that — but a run made
		// here belongs to nobody now.
		if (!borrowed)
			os64_text_run_release(run);
		return x + advance;
	}

	// The paper before the ink. os64_text_draw paints coverage only, while
	// the painter it replaces filled each cell — so the row's own box is
	// cleared here, or a shortened caption leaves its old tail on screen.
	os64_gui_rect_t box = { x, top_y, advance, m.row_h };
	os64_gui_rect_t painted;
	if (os64_rect_intersect(box, row_clip, &painted))
		os64_draw_fill_rect(dst, painted, bg);

	os64_text_draw(run, dst, row_clip, x, top_y + m.baseline, fg);
	if (!borrowed)
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

os64_font_status_t os64_ui_font_borrow_context(os64_ui_t *ui,
                                               os64_text_context_t *text)
{
	if (!ui || !text)
		return OS64_FONT_BAD_ARGUMENT;
	ui_font_binding_t *b = binding_ensure(ui);
	if (!b)
		return OS64_FONT_NO_MEMORY;
	if (b->text == text)
		return OS64_FONT_OK;
	// Moving a window between engines would strand whatever it is already
	// holding — its set, its widgets' runs — in a context nobody will
	// destroy. A window picks its engine before it has anything in it.
	if (b->set || b->tried)
		return OS64_FONT_BUSY;
	if (b->text && b->owns_text) {
		os64_font_status_t status = os64_text_destroy(b->text);
		if (status != OS64_FONT_OK)
			return status;
	}
	b->text = text;
	b->owns_text = false;
	b->status = OS64_FONT_OK;
	return OS64_FONT_OK;
}

// A set carries the context it was prepared on. Binding one from somewhere
// else would have F2 refuse every layout against it, and this wrapper would
// then measure the bitmap cell and report success — the wrong font, silently,
// at the wrong widths. Ask the question once, here.
static os64_font_status_t set_belongs_here(ui_font_binding_t *b, os64_font_set_t *set)
{
	os64_font_role_view_t view;
	os64_font_status_t status = os64_font_set_view(set, OS64_FONT_ROLE_UI, &view);
	if (status != OS64_FONT_OK)
		return status;
	return view.text == b->text ? OS64_FONT_OK : OS64_FONT_BAD_ARGUMENT;
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
	// Resolve into a LOCAL first: a set that cannot be resolved must leave
	// the window wearing what it had, not half of something new.
	ui_font_faces_t resolved;
	if (set) {
		os64_font_status_t status = set_belongs_here(b, set);
		if (status != OS64_FONT_OK)
			return status;
		status = faces_resolve(b, set, &resolved);
		if (status != OS64_FONT_OK)
			return status;
		status = os64_font_set_retain(set);
		if (status != OS64_FONT_OK)
			return status;
	} else {
		os64_memset(&resolved, 0, sizeof(resolved));
	}
	os64_font_set_release(b->set);
	b->set = set;
	b->faces = resolved;
	b->status = OS64_FONT_OK;
	b->tried = set != (os64_font_set_t *)0;
	os64_ui_font_restamp(ui);
	if (ui->root)
		os64_ui_mark_dirty(ui, ui->root);
	return OS64_FONT_OK;
}

os64_font_status_t os64_ui_font_planner(os64_ui_t *ui,
                          os64_font_status_t (*plan)(os64_ui_t *ui, void *user, void **out),
                          void (*commit)(os64_ui_t *ui, void *user, void *plan),
                          void (*discard)(os64_ui_t *ui, void *user, void *plan),
                          void *user)
{
	if (!ui)
		return OS64_FONT_BAD_ARGUMENT;
	// A staged plan that can be applied but not thrown away leaks on every
	// refusal, and one that can be thrown away but not applied is not a
	// plan. Either all three doors exist or none do.
	bool any = plan || commit || discard;
	bool all = plan && commit && discard;
	if (any && !all)
		return OS64_FONT_BAD_ARGUMENT;

	// No allocation: the window already owns this storage, so a registered
	// planner is a registered planner.
	ui->font_plan = plan;
	ui->font_plan_commit = commit;
	ui->font_plan_discard = discard;
	ui->font_plan_user = user;
	return OS64_FONT_OK;
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

static void restamp_tree(os64_ui_widget_t *w, os64_ui_t *ui);
static void commit_tree_bounds(os64_ui_widget_t *w);
static void unstage_tree_bounds(os64_ui_widget_t *w);

// ── staging a tree's text against a candidate ───────────────────────────────
//
// A commit cannot allocate, and a paint has nowhere to report a failure to,
// so every run the window will need is laid out HERE — while failing is
// still allowed and still means "the old face stays".

// Make the slot hold a run for exactly these bytes, laying one out only if
// what is there says something else. Borrowed from the slot afterwards.
static os64_font_status_t slot_run(os64_ui_t *ui, void **run_slot,
                                   os64_font_role_t role,
                                   const char *s, size_t len,
                                   os64_text_run_t **out)
{
	if (run_slot && run_matches((os64_text_run_t *)*run_slot, s, len)) {
		*out = (os64_text_run_t *)*run_slot;
		return OS64_FONT_OK;
	}
	os64_text_run_t *run = (os64_text_run_t *)0;
	os64_font_status_t status = role_layout(ui, role, s, len, &run);
	if (status != OS64_FONT_OK) {
		*out = (os64_text_run_t *)0;
		return status;
	}
	if (run && run_slot) {
		os64_text_run_release((os64_text_run_t *)*run_slot);
		*run_slot = run;
	}
	*out = run;
	return OS64_FONT_OK;
}

os64_font_status_t os64_ui_run_width(os64_ui_t *ui, void **run_slot,
                                     os64_font_role_t role,
                                     const char *s, size_t len, int32_t *out)
{
	if (!out)
		return OS64_FONT_BAD_ARGUMENT;
	*out = 0;
	if (!s || !len)
		return OS64_FONT_OK;

	os64_text_run_t *run = (os64_text_run_t *)0;
	os64_font_status_t status = slot_run(ui, run_slot, role, s, len, &run);
	if (status != OS64_FONT_OK)
		return status;
	if (!run) {
		*out = (int32_t)len * OS64_FONT_GLYPH_W;   // unbound: the bitmap cell
		return OS64_FONT_OK;
	}
	os64_text_run_view_t rv;
	status = os64_text_run_view(run, &rv);
	if (status == OS64_FONT_OK)
		*out = px_ceil(rv.advance_x);
	// The run belongs to the slot when there is one; otherwise it was ours.
	if (!run_slot)
		os64_text_run_release(run);
	return status;
}

os64_font_status_t os64_ui_run_layout(os64_ui_t *ui, os64_font_role_t role,
                                      const char *s, size_t len, void **out)
{
	os64_text_run_t *run = (os64_text_run_t *)0;
	os64_font_status_t status = role_layout(ui, role, s, len, &run);
	*out = run;
	return status;
}

void os64_ui_run_release(void *run)
{
	os64_text_run_release((os64_text_run_t *)run);
}

bool os64_ui_run_matches(void *run, const char *s, size_t len)
{
	return run_matches((os64_text_run_t *)run, s, len);
}

// ── asking a run where things are ───────────────────────────────────────────
// F2 answers all of these in 26.6; an editor works in whole pixels, and the
// contract's quantization is floor((pos + 32)/64) so a caret and the glyph
// beside it round the same way. px_ceil is for EXTENTS, which must not come
// back short; a position is not an extent.

static int32_t px_round(os64_font_pos_t v)
{
	return (int32_t)((v + OS64_FONT_UNIT / 2) / OS64_FONT_UNIT);
}

os64_font_status_t os64_ui_run_caret(void *run, size_t byte_offset, bool after,
                                     int32_t *out_x)
{
	if (!run || !out_x)
		return OS64_FONT_BAD_ARGUMENT;
	os64_text_caret_t caret;
	os64_font_status_t status =
		os64_text_caret((os64_text_run_t *)run, byte_offset,
		                after ? OS64_TEXT_AFTER : OS64_TEXT_BEFORE, &caret);
	if (status == OS64_FONT_OK)
		*out_x = px_round(caret.x);
	return status;
}

os64_font_status_t os64_ui_run_hit(void *run, int32_t x, size_t *out_offset)
{
	if (!run || !out_offset)
		return OS64_FONT_BAD_ARGUMENT;
	os64_text_caret_t caret;
	os64_font_status_t status =
		os64_text_hit((os64_text_run_t *)run, (os64_font_pos_t)x * OS64_FONT_UNIT,
		              &caret);
	if (status == OS64_FONT_OK)
		*out_offset = caret.byte_offset;
	return status;
}

os64_font_status_t os64_ui_run_selection(void *run, size_t begin, size_t end,
                                         os64_gui_rect_t *out)
{
	if (!run || !out)
		return OS64_FONT_BAD_ARGUMENT;
	os64_font_rect_t rect;
	os64_font_status_t status =
		os64_text_selection((os64_text_run_t *)run, begin, end, &rect);
	if (status != OS64_FONT_OK)
		return status;
	int32_t x0 = px_round(rect.x0), x1 = px_round(rect.x1);
	int32_t y0 = px_round(rect.y0), y1 = px_round(rect.y1);
	*out = (os64_gui_rect_t){ x0, y0, x1 - x0, y1 - y0 };
	return OS64_FONT_OK;
}

// ── cluster boundaries, read from the bytes ─────────────────────────────────
// WHERE A CLUSTER ENDS BELONGS TO THE ENCODING PROFILE, NOT TO THE FACE. F2
// lays a line out by walking its decoder one cluster at a time and publishes
// a caret at each end, so the carets of any run are exactly the boundaries
// that decoder finds, in whatever face the run was laid out. Asking the
// decoder directly gets the same answer with no layout: nothing to allocate,
// nothing to refuse. That is what keeps Backspace, Delete, Left and Right
// correct when memory is short. The alternative was falling back to one
// byte, which is not a smaller cluster but a way of cutting one in half.
//
// The walk starts at the line's first byte, because a combining mark decides
// where the cluster BEFORE it ends and the decoder cannot run backwards. It
// is the same order of work as the layout the edit goes on to cause.

static size_t cluster_end(const char *s, size_t len, size_t at)
{
	// The profile role_layout lays every widget's text out in.
	return text_decode((const uint8_t *)s, len, at,
	                   OS64_TEXT_UTF8_WESTERN_V1, false).end;
}

size_t os64_ui_text_step(const char *s, size_t len, size_t offset, bool forward)
{
	if (offset > len)
		offset = len;
	size_t at = 0;
	if (forward) {
		while (at < len && at <= offset)
			at = cluster_end(s, len, at);
		return at;
	}
	size_t before = 0;
	while (at < offset) {
		before = at;
		at = cluster_end(s, len, at);
	}
	return before;
}

size_t os64_ui_text_snap(const char *s, size_t len, size_t offset, bool after)
{
	if (offset >= len)
		return len;
	size_t at = 0;
	while (at < offset) {
		size_t end = cluster_end(s, len, at);
		if (end > offset)
			return after ? end : at;
		at = end;
	}
	return at;
}

// The one-caption default. A widget with no text stages nothing and drops
// whatever it held, because that run describes a face on its way out.
os64_font_status_t os64_ui_stage_caption(os64_ui_widget_t *w, os64_ui_t *ui)
{
	size_t len = 0;
	if (w->text)
		while (w->text[len])
			++len;
	if (!len)
		return OS64_FONT_OK;
	void *run = (void *)0;
	os64_font_status_t status =
		os64_ui_run_layout(ui, OS64_FONT_ROLE_UI, w->text, len, &run);
	if (status != OS64_FONT_OK)
		return status;
	os64_ui_run_release(w->run_staged);
	w->run_staged = run;
	return OS64_FONT_OK;
}

void os64_ui_commit_caption(os64_ui_widget_t *w)
{
	os64_ui_run_release(w->run);
	w->run = w->run_staged;
	w->run_staged = (void *)0;
}

void os64_ui_discard_caption(os64_ui_widget_t *w)
{
	os64_ui_run_release(w->run_staged);
	w->run_staged = (void *)0;
}

void os64_ui_widget_stage_bounds(os64_ui_widget_t *w, os64_gui_rect_t bounds)
{
	w->bounds_staged = bounds;
	w->bounds_staged_valid = true;
}

os64_gui_rect_t os64_ui_widget_planned_bounds(const os64_ui_widget_t *w)
{
	return w->bounds_staged_valid ? w->bounds_staged : w->bounds;
}

static void commit_tree_bounds(os64_ui_widget_t *w)
{
	if (!w)
		return;
	if (w->bounds_staged_valid) {
		w->bounds = w->bounds_staged;
		w->bounds_staged_valid = false;
	}
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		commit_tree_bounds(c);
}

static void unstage_tree_bounds(os64_ui_widget_t *w)
{
	if (!w)
		return;
	w->bounds_staged_valid = false;
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		unstage_tree_bounds(c);
}

static os64_font_status_t stage_tree_runs(os64_ui_t *ui, os64_ui_widget_t *w)
{
	if (!w)
		return OS64_FONT_OK;
	if (w->cls && w->cls->prepare) {
		os64_font_status_t status = w->cls->prepare(w, ui);
		if (status != OS64_FONT_OK)
			return status;
	}
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling) {
		os64_font_status_t status = stage_tree_runs(ui, c);
		if (status != OS64_FONT_OK)
			return status;
	}
	return OS64_FONT_OK;
}

static void commit_tree_runs(os64_ui_widget_t *w)
{
	if (!w)
		return;
	if (w->cls && w->cls->commit)
		w->cls->commit(w);
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		commit_tree_runs(c);
}

static void discard_tree_runs(os64_ui_widget_t *w)
{
	if (!w)
		return;
	if (w->cls && w->cls->discard)
		w->cls->discard(w);
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		discard_tree_runs(c);
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

	os64_font_status_t status = set_belongs_here(b, candidate);
	if (status != OS64_FONT_OK) {
		os64_free(plan);
		return status;
	}
	status = os64_font_set_retain(candidate);
	if (status != OS64_FONT_OK) {
		os64_free(plan);
		return status;
	}
	plan->candidate = candidate;
	status = faces_resolve(b, candidate, &plan->faces);
	if (status != OS64_FONT_OK) {
		os64_font_set_release(plan->candidate);
		os64_free(plan);
		return status;
	}

	// Everything from here measures the candidate: the natural heights the
	// widgets report, whatever the application measures inside its planner,
	// and the runs staged after it. A failure costs only this staging.
	b->staging = &plan->faces;

	// THE ORDER IS THE POINT. Natural metrics come before the planner, so
	// it sizes against the face it is about to get; the planner comes
	// before the widgets' text, because a widget preparing its runs has to
	// know how much room it is ABOUT to have. A list the new layout makes
	// taller can show rows it cannot show today, and staging against live
	// bounds would leave those to be laid out by the first paint, where
	// failing is not allowed.
	restamp_tree(ui->root, ui);
	if (ui->font_plan) {
		status = ui->font_plan(ui, ui->font_plan_user, &plan->app_plan);
		if (status == OS64_FONT_OK)
			plan->app_planned = true;
	}
	if (status == OS64_FONT_OK)
		status = stage_tree_runs(ui, ui->root);
	b->staging = (const ui_font_faces_t *)0;

	if (status != OS64_FONT_OK) {
		if (plan->app_planned && ui->font_plan_discard)
			ui->font_plan_discard(ui, ui->font_plan_user, plan->app_plan);
		discard_tree_runs(ui->root);
		unstage_tree_bounds(ui->root);
		restamp_tree(ui->root, ui);       // back to the face still installed
		os64_font_set_release(plan->candidate);
		os64_free(plan);
		return status;
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
	// Prepare already stamped them from the candidate, which is now the
	// installed set; this re-runs against it and swaps in the staged runs,
	// and neither allocates.
	// The staged rectangles become the real ones, then the staged runs do.
	// Neither allocates.
	commit_tree_bounds(ui->root);
	commit_tree_runs(ui->root);
	os64_ui_font_restamp(ui);
	if (plan->app_planned && ui->font_plan_commit)
		ui->font_plan_commit(ui, ui->font_plan_user, plan->app_plan);

	// The whole window repaints: every run in it was laid out against the
	// set just retired.
	if (ui->root)
		os64_ui_mark_dirty(ui, ui->root);
	os64_free(plan);
}

static void consumer_abort(void *user, void *plan_ptr)
{
	os64_ui_t *ui = user;
	ui_font_plan_t *plan = plan_ptr;

	if (plan->app_planned && ui->font_plan_discard)
		ui->font_plan_discard(ui, ui->font_plan_user, plan->app_plan);
	discard_tree_runs(ui->root);
	unstage_tree_bounds(ui->root);
	restamp_tree(ui->root, ui);           // the face still installed
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

// Drop every run this tree is holding, so teardown is not defeated by a
// caption the window happened to have painted.
static void release_tree_runs(os64_ui_widget_t *w)
{
	if (!w)
		return;
	// Storage only the class can see goes first: a listbox keeps one run per
	// visible row in its own array, and a run kept anywhere at all holds the
	// text context BUSY for good.
	if (w->cls && w->cls->destroy)
		w->cls->destroy(w);
	os64_text_run_release((os64_text_run_t *)w->run);
	os64_text_run_release((os64_text_run_t *)w->run_staged);
	w->run = w->run_staged = (void *)0;
	w->bounds_staged_valid = false;
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		release_tree_runs(c);
}

os64_font_status_t os64_ui_font_release(os64_ui_t *ui)
{
	if (!ui)
		return OS64_FONT_BAD_ARGUMENT;
	ui_font_binding_t *b = binding_of(ui);
	if (!b)
		return OS64_FONT_OK;

	release_tree_runs(ui->root);
	os64_font_set_release(b->set);
	b->set = (os64_font_set_t *)0;
	os64_memset(&b->faces, 0, sizeof(b->faces));

	// THE BINDING IS THE CONTEXT'S ALLOCATOR. A context that refuses to die
	// is still holding somebody's set or run, and those call back through
	// these callbacks when they are finally released — so the owner has to
	// outlive them. BUSY leaves the window intact and says come back.
	if (b->text && b->owns_text) {
		os64_font_status_t status = os64_text_destroy(b->text);
		if (status != OS64_FONT_OK) {
			b->status = status;
			b->tried = false;   // a retry may rebuild the builtin set
			return status;
		}
	}
	ui->font = (void *)0;
	os64_free(b);
	return OS64_FONT_OK;
}
