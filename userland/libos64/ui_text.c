// Text widgets: scrollbars, text fields, and text views. Behavior and
// painting are separate; the app owns the widget and its text model.
//
// THE CONTAINER PATTERN (ui.h documents it): these widgets need state the
// base struct doesn't carry, so each embeds os64_ui_widget_t as its FIRST
// member and casts back. libui threads the base; the app owns the whole.

#include "os64/ui.h"
#include "ui_internal.h"
#include "os64/str.h"
#include "os64/io.h"     // the clipboard is a FILE — open/read/write/close
#include "os64/clip.h"   // ...at OS64_CLIPBOARD_PATH
#include "os64/mem.h"    // the visible lines' run arrays
#include "os64/font_psf1.h"   // the bitmap cell, for a window with no face

// ── decoding the keyboard's 1979 vocabulary ─────────────────────────────────
//
// Arrows and the editing keys do not arrive as key events with a private
// code — the keyboard driver emits them as VT100 ESCAPE BURSTS (ESC [ A for
// Up, ESC [ 3 ~ for Delete...), three or four KEY_DOWN events each stamped
// with the extended scancode. That choice was made for interop ("a future
// vim-over-serial reads these bytes unchanged" — keyboard.c), and these
// widgets are that future customer arrived early: the same decoder would
// read the same bytes off a serial line.
//
// A REAL Esc press is distinguishable by its scancode: the burst's ESC
// carries the extended key's code, the Esc key carries its own. That one
// fact is what lets a textfield have both an escape-sequence parser and a
// working Cancel key. ONE KEY, TWO DIALECTS (the keyboard_ctrl_alt_del
// precedent): the PS/2 driver stamps make-code 0x01, the xHCI HID driver
// stamps usage 0x29 — a check that knows only one of them ships a Cancel
// key that works in QEMU and dies on the P5 (found 2026-08-21, the same
// day the Ctrl+Alt chord taught the same lesson one layer down).
//
// Parser state lives in the WIDGET (one byte), not in a static — two
// focused widgets never decode concurrently, but statics in a library are
// how that assumption becomes a bug later (the SMP lesson, applied at ring 3).

#define SEQ_IDLE   0
#define SEQ_ESC    1     // saw the burst's ESC
#define SEQ_CSI    2     // saw ESC [
#define SEQ_DIGIT  10    // 10 + d after ESC [ <d>

#define SC_ESC_PS2 0x01  // the Esc KEY's PS/2 make-code — the burst never uses it
#define SC_ESC_HID 0x29  // the Esc KEY's HID usage (the P5's USB keyboard)

// Public (ui.h): apps that answer Esc ahead of widget dispatch — scribe's
// help page is customer one — need the same two-dialect answer, not a copy
// of one dialect.
bool os64_ui_key_is_esc(const os64_gui_event_t *ev)
{
	return ev->key.ascii == 0x1b &&
	       (ev->key.scancode == SC_ESC_PS2 || ev->key.scancode == SC_ESC_HID);
}

ui_key_t os64_ui_decode_key(uint8_t *seq, const os64_gui_event_t *ev, char *ch)
{
	char a = ev->key.ascii;

	if (*seq == SEQ_ESC) {
		*seq = (a == '[') ? SEQ_CSI : SEQ_IDLE;
		if (*seq == SEQ_CSI)
			return K_NONE;
		// Not a burst after all — fall through and decode this event fresh.
	} else if (*seq == SEQ_CSI) {
		*seq = SEQ_IDLE;
		switch (a) {
			case 'A': return K_UP;
			case 'B': return K_DOWN;
			case 'C': return K_RIGHT;
			case 'D': return K_LEFT;
			case 'H': return K_HOME;
			case 'F': return K_END;
		}
		if (a >= '0' && a <= '9') {
			*seq = (uint8_t)(SEQ_DIGIT + (a - '0'));
			return K_NONE;
		}
		return K_NONE;   // unknown final: swallow, never insert burst bytes
	} else if (*seq >= SEQ_DIGIT) {
		int d = *seq - SEQ_DIGIT;
		*seq = SEQ_IDLE;
		if (a == '~') {
			switch (d) {
				case 3: return K_DELETE;
				case 5: return K_PGUP;
				case 6: return K_PGDN;
				default: return K_NONE;   // Insert (2) and strangers: ignored
			}
		}
		return K_NONE;
	}

	if (a == 0x1b) {
		if (os64_ui_key_is_esc(ev))
			return K_ESC;          // the actual key, not a burst
		*seq = SEQ_ESC;
		return K_NONE;
	}
	if (a == '\n' || a == '\r') return K_ENTER;
	if (a == '\b')              return K_BACKSPACE;
	if (a == '\t')              return K_TAB;
	if (a >= 0x20 && a < 0x7f) {
		*ch = a;
		return K_CHAR;
	}
	return K_NONE;   // other control bytes are the APP's (Ctrl+S et al.)
}

// ── ui_scrollbar ────────────────────────────────────────────────────────────

// Thumb geometry along the LONG axis (y for the classic bar, x when
// `horizontal` — one function, axis chosen once, so the two orientations
// can never drift). Shared by paint and event so the pixels you grab are
// the pixels that were drawn (the wm_clamp_frame lesson, one layer up).
static int32_t scrollbar_span(const os64_ui_scrollbar_t *sb)
{
	return sb->horizontal ? sb->w.bounds.w : sb->w.bounds.h;
}

static int32_t scrollbar_origin(const os64_ui_scrollbar_t *sb)
{
	return sb->horizontal ? sb->w.bounds.x : sb->w.bounds.y;
}

static void scrollbar_thumb(const os64_ui_scrollbar_t *sb,
                            int32_t *tpos, int32_t *tlen)
{
	int32_t span = scrollbar_span(sb);
	if (sb->total <= 0 || sb->visible <= 0 || sb->visible >= sb->total) {
		*tpos = scrollbar_origin(sb);
		*tlen = span;
		return;
	}
	int64_t t = (int64_t)span * sb->visible / sb->total;
	if (t < 16)   t = 16;        // a thumb you can't hit isn't a thumb
	if (t > span) t = span;
	int64_t range = sb->total - sb->visible;
	*tlen = (int32_t)t;
	*tpos = scrollbar_origin(sb) +
	        (int32_t)((int64_t)(span - *tlen) * sb->pos / range);
}

static void scrollbar_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                            const os64_ui_theme_t *t)
{
	os64_ui_scrollbar_t *sb = (os64_ui_scrollbar_t *)w;
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->scroll_track);
	int32_t tpos, tlen;
	scrollbar_thumb(sb, &tpos, &tlen);
	os64_gui_rect_t thumb = sb->horizontal
	    ? (os64_gui_rect_t){ tpos, w->bounds.y + 2, tlen, w->bounds.h - 4 }
	    : (os64_gui_rect_t){ w->bounds.x + 2, tpos, w->bounds.w - 4, tlen };
	os64_draw_fill_round_rect(&ctx->surf, thumb, t->control_radius, t->scroll_thumb);
}

static void scrollbar_moved(os64_ui_scrollbar_t *sb, os64_ui_t *ui, int64_t pos)
{
	int64_t range = sb->total - sb->visible;
	if (range < 0)
		range = 0;
	if (pos < 0)     pos = 0;
	if (pos > range) pos = range;
	if (pos == sb->pos)
		return;
	sb->pos = pos;
	os64_ui_mark_dirty(ui, &sb->w);
	if (sb->on_scroll)
		sb->on_scroll(sb, sb->scroll_user);
}

static bool scrollbar_event(os64_ui_widget_t *w, os64_ui_t *ui,
                            const os64_gui_event_t *ev)
{
	os64_ui_scrollbar_t *sb = (os64_ui_scrollbar_t *)w;

	// The pointer's coordinate along the long axis — the only place the
	// event handler cares which orientation it is.
	int32_t m = sb->horizontal ? ev->mouse.x : ev->mouse.y;

	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
		if (sb->total <= sb->visible)
			return true;   // full thumb: nothing to move, but the click is ours
		int32_t tpos, tlen;
		scrollbar_thumb(sb, &tpos, &tlen);
		if (m >= tpos && m < tpos + tlen) {
			sb->drag_grab = m - tpos;              // ride the thumb
		} else {
			// Track click: a page jump toward the click — every scrollbar
			// since the Star has meant this.
			scrollbar_moved(sb, ui, m < tpos ? sb->pos - sb->visible
			                                 : sb->pos + sb->visible);
		}
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_MOVE: {
		if (sb->drag_grab < 0)
			return true;
		int32_t tpos, tlen;
		scrollbar_thumb(sb, &tpos, &tlen);
		int32_t travel = scrollbar_span(sb) - tlen;
		if (travel <= 0)
			return true;
		int64_t range = sb->total - sb->visible;
		int64_t at_px = m - sb->drag_grab - scrollbar_origin(sb);
		scrollbar_moved(sb, ui, at_px * range / travel);
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
		sb->drag_grab = -1;
		return true;
	default:
		return false;
	}
}

static void scrollbar_cancel(os64_ui_widget_t *w)
{
    ((os64_ui_scrollbar_t *)w)->drag_grab = -1;
}

const os64_ui_class_t os64_ui_scrollbar_class =
    { "scrollbar", scrollbar_paint, scrollbar_event, scrollbar_cancel, 0, 0, 0, 0, 0 };

void os64_ui_scrollbar(os64_ui_scrollbar_t *sb,
                       void (*on_scroll)(os64_ui_scrollbar_t *, void *),
                       void *user)
{
	*sb = (os64_ui_scrollbar_t){0};
	sb->w.cls = &os64_ui_scrollbar_class;
	sb->on_scroll = on_scroll;
	sb->scroll_user = user;
	sb->drag_grab = -1;
}

void os64_ui_scrollbar_set(os64_ui_t *ui, os64_ui_scrollbar_t *sb,
                           int64_t total, int64_t visible, int64_t pos)
{
	int64_t range = total > visible ? total - visible : 0;
	if (pos < 0)     pos = 0;
	if (pos > range) pos = range;
	if (sb->total == total && sb->visible == visible && sb->pos == pos)
		return;
	sb->total = total;
	sb->visible = visible;
	sb->pos = pos;
	os64_ui_mark_dirty(ui, &sb->w);
}

// ── where a line's letters are ──────────────────────────────────────────────
// The editors ask a line three questions in pixels: where the caret for a
// byte offset sits, which offset a pixel lands on, and where a selected span
// runs. There are TWO honest sources of answers. Under a face it is the
// row's run, the same measurement its glyphs were drawn from. In a window
// wearing no face at all it is the bitmap cell, because that is what the
// painter draws there: eight pixels a byte. The cell's answers still stop
// only at the letter edges the decoder finds, so an `é` covers two cells and
// the caret never lands between them.
//
// A REFUSED layout is neither. The window has a face and could not measure
// this line in it; the cell is a different font at a different size. A
// caller that could not get the run gets no geometry, and does nothing that
// needed one.
//
// WHAT IS LAID OUT may be less than the line. A field and a short line are
// laid out whole. A long line is laid out through a window, the span
// [from, to), with a decoration standing at each end where bytes are
// omitted: EDGE_MORE for ordinary text, EDGE_LONG for a cluster too long to
// lay out at all, and both when the line is known to go on past that
// cluster. The decorations are drawn by the editor, belong to no byte, and
// take pixels: X is measured from the row's left edge, so the span starts
// after the leading decoration. Offsets are always the line's.

enum { EDGE_NONE = 0, EDGE_MORE = 1, EDGE_LONG = 2 };   // LONG may carry MORE

typedef struct
{
	void *run;              // the span's run, or NULL: the bitmap cell is drawing
	const char *s;          // the WHOLE line
	size_t len;
	size_t from, to;        // the span laid out
	int32_t lead, trail;    // pixels of decoration before and after it
	uint8_t lead_kind, trail_kind;
} line_geom_t;

static line_geom_t geom_whole(const char *s, size_t len)
{
	return (line_geom_t){ NULL, s, len, 0, len, 0, 0, EDGE_NONE, EDGE_NONE };
}

static int64_t cell_x(size_t offset)
{
	return (int64_t)offset * OS64_FONT_GLYPH_W;
}

// Where the caret for `offset` sits. Only the span's offsets have a place:
// anything outside it is omitted, and the answer is BAD_ARGUMENT.
static os64_font_status_t geom_caret(const line_geom_t *g, size_t offset,
                                     int32_t *out_x)
{
	if (offset < g->from || offset > g->to)
		return OS64_FONT_BAD_ARGUMENT;
	int32_t x = 0;
	if (g->run) {
		os64_font_status_t status =
			os64_ui_run_caret(g->run, offset - g->from, false, &x);
		if (status != OS64_FONT_OK)
			return status;
	} else {
		x = (int32_t)cell_x(os64_ui_text_snap(g->s, g->len, offset, false) - g->from);
	}
	*out_x = g->lead + x;
	return OS64_FONT_OK;
}

// The row's whole width: decorations and span.
static int32_t geom_width(const line_geom_t *g)
{
	int32_t end = g->lead;
	geom_caret(g, g->to, &end);
	return end + g->trail;
}

// The nearest letter edge to `x`, a tie going to the later one as a run's
// does. A pixel on a decoration answers the span's end beside it, so a
// click there puts the caret against the omitted text. In the cell, the
// edge at or before the cell under `x` and the next one after it bracket
// `x`, so the answer is one of those two.
static os64_font_status_t geom_hit(const line_geom_t *g, int32_t x,
                                   size_t *out_offset)
{
	int32_t end = 0;
	os64_font_status_t status = geom_caret(g, g->to, &end);
	if (status != OS64_FONT_OK)
		return status;
	if (x < g->lead || x > end) {
		*out_offset = x < g->lead ? g->from : g->to;
		return OS64_FONT_OK;
	}
	x -= g->lead;
	if (g->run) {
		status = os64_ui_run_hit(g->run, x, out_offset);
		if (status == OS64_FONT_OK)
			*out_offset += g->from;
		return status;
	}
	size_t at = g->from + (x > 0 ? (size_t)x / OS64_FONT_GLYPH_W : 0);
	if (at > g->to)
		at = g->to;
	size_t lo = os64_ui_text_snap(g->s, g->len, at, false);
	size_t hi = lo < g->to ? os64_ui_text_step(g->s, g->len, lo, true) : g->to;
	*out_offset = (int64_t)x - cell_x(lo - g->from) < cell_x(hi - g->from) - (int64_t)x
	              ? lo : hi;
	return OS64_FONT_OK;
}

// Where the selected bytes [a, b) of the line run across the row, as X and
// width. Any bytes selected beyond an end of the span light that end's
// whole decoration: it is one indicator standing for all the bytes omitted
// there, and does not say which of them. Both ends must be letter edges.
static os64_font_status_t geom_selection(const line_geom_t *g, size_t a,
                                         size_t b, os64_gui_rect_t *out)
{
	int32_t start = 0, end = 0;
	os64_font_status_t status;
	if ((status = geom_caret(g, a < g->from ? g->from : a > g->to ? g->to : a,
	                         &start)) != OS64_FONT_OK ||
	    (status = geom_caret(g, b > g->to ? g->to : b < g->from ? g->from : b,
	                         &end)) != OS64_FONT_OK)
		return status;
	if (a < g->from)
		start = 0;
	if (b > g->to)
		end += g->trail;
	if (b < g->from)
		end = g->lead;               // all of it omitted before the span
	if (a > g->to)
		start = end - g->trail;      // all of it omitted after
	*out = (os64_gui_rect_t){ start, 0, end > start ? end - start : 0, 0 };
	return OS64_FONT_OK;
}

// ── ui_textfield ────────────────────────────────────────────────────────────

static int32_t field_inset(const os64_ui_theme_t *t)
{
    int32_t radius = t->control_radius;
    if (radius > 8) radius = 8;
    return radius > 3 ? radius + 1 : 4;
}

// The field's text is ONE LINE, laid out whole into the widget's own run —
// the same slot a label's caption lives in, which is why the constructor
// points `w.text` at the buffer: adoption stages it like any caption, and a
// paint after a font change allocates nothing. Its geometry answers the
// caret, the click and the scroll; anything but OK is a refused layout, and
// whatever needed the field's pixels does not happen.
static os64_font_status_t field_geom(os64_ui_textfield_t *tf, os64_ui_t *ui,
                                     line_geom_t *g)
{
	*g = geom_whole(tf->buf, tf->len);
	return os64_ui_run_resolve(ui, &tf->w.run, OS64_FONT_ROLE_UI,
	                           tf->buf, tf->len, &g->run);
}

// The least a scroll has to move for a caret at `cx` to show in a box
// `width` pixels wide, with a couple of pixels of air so the caret is not
// flush against the edge it scrolled to. The editors ask it while editing
// and again while a font change is being prepared.
static int64_t scroll_to_show(int64_t left, int64_t cx, int32_t width)
{
	if (cx < left)
		left = cx;
	if (cx > left + width - 2)
		left = cx - width + 2;
	return left < 0 ? 0 : left;
}

static void field_keep_caret_visible(os64_ui_textfield_t *tf, os64_ui_t *ui)
{
	int32_t width = tf->w.bounds.w - 2 * field_inset(&ui->theme);
	int32_t cx = 0;
	line_geom_t g;
	if (field_geom(tf, ui, &g) != OS64_FONT_OK ||
	    geom_caret(&g, tf->cursor, &cx) != OS64_FONT_OK)
		return;                        // keep the scroll rather than guess one
	tf->left_px = (int32_t)scroll_to_show(tf->left_px, cx, width);
}

// A font change moves every pixel the field holds: the caret's X is in the
// face being retired, and so is the scroll that keeps it in view. The old
// scroll is not a position in the new face at all, so the scroll is worked
// out afresh — the one this caret would get in a field just opened — against
// the candidate's run and the rectangle the planner staged. The first paint
// in the new face shows the caret; commit only moves the scroll into place,
// and a refusal leaves the old one standing.
static os64_font_status_t field_prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
	os64_ui_textfield_t *tf = (os64_ui_textfield_t *)w;
	os64_font_status_t status = os64_ui_stage_caption(w, ui);
	if (status != OS64_FONT_OK)
		return status;
	// An empty field stages no run, and its caret is at zero either way.
	line_geom_t g = geom_whole(tf->buf, tf->len);
	g.run = w->run_staged;
	int32_t cx = 0;
	status = geom_caret(&g, tf->cursor, &cx);
	if (status != OS64_FONT_OK)
		return status;
	os64_gui_rect_t planned = os64_ui_widget_planned_bounds(w);
	tf->left_staged = (int32_t)scroll_to_show(0, cx,
	                                          planned.w - 2 * field_inset(&ui->theme));
	return OS64_FONT_OK;
}

static void field_commit(os64_ui_widget_t *w)
{
	os64_ui_textfield_t *tf = (os64_ui_textfield_t *)w;
	os64_ui_commit_caption(w);
	tf->left_px = tf->left_staged;
}

// A field is one row of text inside a control's furniture — a button's
// arithmetic.
static void field_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
	w->natural_h = os64_ui_control_min_height(ui);
}

static void field_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                        const os64_ui_theme_t *t)
{
	os64_ui_textfield_t *tf = (os64_ui_textfield_t *)w;
	os64_ui_t *ui = os64_ui_of(w);
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
	os64_draw_fill_round_rect(&ctx->surf, w->bounds, t->control_radius, t->field_bg);
	uint32_t border = w->focused ? t->field_border_focus : t->field_border;
	os64_draw_round_rect(&ctx->surf, w->bounds, t->control_radius, border, border);

	int32_t inset = field_inset(t);
	os64_gui_rect_t inner = { w->bounds.x + inset, w->bounds.y,
	                          w->bounds.w - 2 * inset, w->bounds.h };
	int32_t row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
	int32_t ty = w->bounds.y + (w->bounds.h - row) / 2;
	int32_t origin = inner.x - tf->left_px;

	// ONE rendering for the whole paint, chosen here: the text and the
	// caret are both drawn from it, so they cannot disagree about where
	// the letters are. A refused layout draws neither.
	line_geom_t g;
	if (field_geom(tf, ui, &g) != OS64_FONT_OK)
		return;
	os64_ui_draw_run(ui, g.run, OS64_FONT_ROLE_UI, &ctx->surf, inner,
	                 origin, ty, tf->buf, tf->len, t->field_fg, t->field_bg);

    if (tf->selected && tf->anchor != tf->cursor) {
        size_t lo = tf->anchor < tf->cursor ? tf->anchor : tf->cursor;
        size_t hi = tf->anchor > tf->cursor ? tf->anchor : tf->cursor;
        os64_gui_rect_t sel, clipped;
        if (geom_selection(&g, lo, hi, &sel) == OS64_FONT_OK) {
            sel.x += origin; sel.y = ty; sel.h = row;
            if (os64_rect_intersect(sel, inner, &clipped)) {
                os64_draw_fill_rect(&ctx->surf, clipped, t->text_sel_bg);
                os64_ui_draw_run(ui, g.run, OS64_FONT_ROLE_UI, &ctx->surf, clipped,
                    origin, ty, tf->buf, tf->len, t->text_sel_fg, t->text_sel_bg);
            }
        }
    }
	int32_t cx = 0;
	if (w->focused && geom_caret(&g, tf->cursor, &cx) == OS64_FONT_OK) {
		os64_gui_rect_t caret = { origin + cx, ty, 2, row };
		os64_gui_rect_t clipped;
		if (os64_rect_intersect(caret, inner, &clipped))
			os64_draw_fill_rect(&ctx->surf, clipped, t->text_caret);
	}
}

static bool field_range(const os64_ui_textfield_t *tf, size_t *lo, size_t *hi)
{
    *lo = tf->cursor < tf->anchor ? tf->cursor : tf->anchor;
    *hi = tf->cursor > tf->anchor ? tf->cursor : tf->anchor;
    return tf->selected && *lo < *hi;
}

static bool field_delete_selection(os64_ui_textfield_t *tf)
{
    size_t lo, hi;
    if (!field_range(tf, &lo, &hi)) return false;
    os64_memmove(tf->buf + lo, tf->buf + hi, tf->len - hi + 1);
    tf->len -= hi - lo;
    tf->cursor = tf->anchor = lo;
    tf->selected = false;
    return true;
}

static bool field_event(os64_ui_widget_t *w, os64_ui_t *ui,
                        const os64_gui_event_t *ev)
{
	os64_ui_textfield_t *tf = (os64_ui_textfield_t *)w;
	const os64_ui_theme_t *t = &ui->theme;

	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
	case OS64_GUI_EVENT_MOUSE_MOVE: {
		bool down = ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN;
		if (down) os64_ui_set_focus(ui, w);
		// The pixel of the TEXT under the pointer, which the geometry turns
		// into a letter edge — never the middle of a letter.
		int32_t x = ev->mouse.x - w->bounds.x - field_inset(t) + tf->left_px;
		line_geom_t g;
		size_t at = 0;
		if (field_geom(tf, ui, &g) == OS64_FONT_OK &&
		    geom_hit(&g, x > 0 ? x : 0, &at) == OS64_FONT_OK)
			tf->cursor = at;
        if (down) tf->anchor = tf->cursor;
        tf->selected = !down && tf->anchor != tf->cursor;
        field_keep_caret_visible(tf, ui);
		os64_ui_mark_dirty(ui, w);
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
		return true;

	case OS64_GUI_EVENT_KEY_DOWN: {
        bool shift = (ev->key.modifiers & OS64_GUI_MOD_SHIFT) != 0;
        if (!tf->seq && (ev->key.modifiers & OS64_GUI_MOD_CTRL) &&
            !(ev->key.modifiers & OS64_GUI_MOD_ALT)) {
            unsigned key = (unsigned char)ev->key.ascii & 31u;
            if (key == 1) { tf->anchor = 0; tf->cursor = tf->len; tf->selected = tf->len != 0; }
            else if (key == 3) { (void)os64_ui_textfield_copy(tf); return true; }
            else if (key == 22) { (void)os64_ui_textfield_paste(ui, tf); return true; }
            else if (key == 24) { if (os64_ui_textfield_copy(tf) > 0) field_delete_selection(tf); }
            else if (ev->key.ascii != 0x1b) return true;
            if (key == 1 || key == 24) {
                tf->cursor = os64_ui_text_snap(tf->buf, tf->len, tf->cursor, true);
                field_keep_caret_visible(tf, ui); os64_ui_mark_dirty(ui, w); return true;
            }
        }
		char c = 0;
        ui_key_t key = os64_ui_decode_key(&tf->seq, ev, &c);
        bool motion = key == K_LEFT || key == K_RIGHT || key == K_HOME || key == K_END;
        size_t lo = 0, hi = 0;
        bool selection = field_range(tf, &lo, &hi);
        if (motion && shift && !tf->selected) tf->anchor = tf->cursor;
		switch (key) {
		case K_CHAR:
            field_delete_selection(tf);
			if (tf->len + 1 < tf->cap) {
				os64_memmove(tf->buf + tf->cursor + 1, tf->buf + tf->cursor,
				             tf->len - tf->cursor + 1);   // +1 rides the NUL
				tf->buf[tf->cursor++] = c;
				tf->len++;
			}
			break;
		case K_BACKSPACE:
            if (field_delete_selection(tf)) break;
			// A whole cluster: the span from the previous boundary to here,
			// found from the bytes, so no layout can refuse it.
			if (tf->cursor > 0) {
				size_t from = os64_ui_text_step(tf->buf, tf->len, tf->cursor, false);
				size_t gone = tf->cursor - from;
				os64_memmove(tf->buf + from, tf->buf + tf->cursor,
				             tf->len - tf->cursor + 1);   // +1 rides the NUL
				tf->cursor = from;
				tf->len -= gone;
			}
			break;
		case K_DELETE:
            if (field_delete_selection(tf)) break;
			if (tf->cursor < tf->len) {
				size_t to = os64_ui_text_step(tf->buf, tf->len, tf->cursor, true);
				size_t gone = to - tf->cursor;
				os64_memmove(tf->buf + tf->cursor, tf->buf + to,
				             tf->len - to + 1);
				tf->len -= gone;
			}
			break;
		case K_LEFT:
			tf->cursor = selection && !shift ? lo : os64_ui_text_step(tf->buf, tf->len, tf->cursor, false);
			break;
		case K_RIGHT:
			tf->cursor = selection && !shift ? hi : os64_ui_text_step(tf->buf, tf->len, tf->cursor, true);
			break;
		case K_HOME:  tf->cursor = 0;       break;
		case K_END:   tf->cursor = tf->len; break;
		case K_ENTER:
			if (tf->on_submit)
				tf->on_submit(tf, tf->edit_user);
			return true;
		case K_ESC:
			if (tf->on_cancel)
				tf->on_cancel(tf, tf->edit_user);
			return true;
		default:
			return true;   // consumed silently (mid-burst, strangers)
		}
        tf->selected = motion && shift && tf->anchor != tf->cursor;
        if (!tf->selected) tf->anchor = tf->cursor;
		// An edit can make one letter of the bytes either side of the caret:
		// a letter typed in front of a combining mark, or a deletion that
		// leaves a base and a mark touching. The caret goes after that
		// letter, never inside it — where a later Backspace would take the
		// base and orphan the mark. Motion already ends on an edge.
		tf->cursor = os64_ui_text_snap(tf->buf, tf->len, tf->cursor, true);
		field_keep_caret_visible(tf, ui);
		os64_ui_mark_dirty(ui, w);
		return true;
	}
	case OS64_GUI_EVENT_KEY_UP:
		return true;
	default:
		return false;
	}
}

static void field_cancel(os64_ui_widget_t *w)
{
    ((os64_ui_textfield_t *)w)->seq = 0;
}

const os64_ui_class_t os64_ui_textfield_class =
    { "textfield", field_paint, field_event, field_cancel,
      field_prepare, field_commit, os64_ui_discard_caption,
      0, field_metrics };

void os64_ui_textfield(os64_ui_textfield_t *tf, char *buf, size_t cap,
                       void (*on_submit)(os64_ui_textfield_t *, void *),
                       void (*on_cancel)(os64_ui_textfield_t *, void *),
                       void *user)
{
	*tf = (os64_ui_textfield_t){0};
	tf->w.cls = &os64_ui_textfield_class;
	tf->w.focusable = true;
	// The field's text IS a caption as far as a font change is concerned:
	// one line, NUL-kept, in storage the application owns. Pointing the
	// widget's caption at the buffer lets adoption stage it through the same
	// door a label uses, and the byte comparison on every paint catches the
	// edits libui makes to it in between.
	tf->w.text = buf;
	tf->w.auto_h = true;
	tf->buf = buf;
	tf->cap = cap;
	tf->on_submit = on_submit;
	tf->on_cancel = on_cancel;
	tf->edit_user = user;
	if (cap > 0)
		buf[0] = '\0';
}

void os64_ui_textfield_set(os64_ui_t *ui, os64_ui_textfield_t *tf,
                           const char *text)
{
	if (!text)
		text = "";
	size_t full = os64_strcopy(tf->buf, tf->cap, text);   // the SOURCE's length
	tf->len = full;
	if (tf->len >= tf->cap) {
		// Cut where a letter ends, judged against the whole text: whether
		// the last byte that fits finishes a cluster depends on the bytes
		// after it, which the copy has already dropped.
		tf->len = tf->cap ? os64_ui_text_snap(text, full, tf->cap - 1, false) : 0;
		if (tf->cap)
			tf->buf[tf->len] = '\0';
	}
	tf->cursor = tf->anchor = tf->len;
    tf->selected = false;
	tf->left_px = 0;
	field_keep_caret_visible(tf, ui);   // the caret is at the END of a long path
	os64_ui_mark_dirty(ui, &tf->w);
}

// ── the system clipboard, at the widget's edge ──────────────────────────────
// Everything below talks to /sys/clipboard the same way husk does: it is a
// FILE, opened for "w" to copy and "r" to paste (CLIPBOARD.md). No syscall,
// no special case, and text that leaves scribe this way arrives intact in
// `cat /sys/clipboard` — which is the whole reason the clipboard was built
// as a file rather than as a windowing-system API.
//
// The clipboard is never held whole: the copy streams line by line into the
// open handle, and the textview's paste streams the other way through one
// stack chunk. A field takes one line, and only what it can hold, so a 16MB
// snarf never exists twice.

// The clipboard's own doctrine says a multi-write copy is ONE snarf, sealed
// at close — so a partial write is not a partial clipboard, it is a copy that
// never publishes. That lets this be a plain loop with an honest bool.
static bool clip_write_all(int32_t h, const void *bytes, size_t n, int64_t *total)
{
	const uint8_t *p = (const uint8_t *)bytes;
	size_t sent = 0;

	while (sent < n) {
		int64_t w = os64_write(h, p + sent, n - sent);
		if (w <= 0)
			return false;
		sent += (size_t)w;
	}
	*total += (int64_t)n;
	return true;
}

int64_t os64_ui_textfield_copy(const os64_ui_textfield_t *tf)
{
    size_t lo, hi;
    if (!field_range(tf, &lo, &hi)) return 0;
    int64_t fd = os64_open(OS64_CLIPBOARD_PATH, "w");
    if (fd < 0) return fd;
    int64_t total = 0;
    bool ok = clip_write_all((int32_t)fd, tf->buf + lo, hi - lo, &total);
    int64_t closed = os64_close((int32_t)fd);
    return ok && closed >= 0 ? total : -1;
}

size_t os64_ui_textfield_paste(os64_ui_t *ui, os64_ui_textfield_t *tf)
{
    size_t lo, hi;
    size_t retained = tf->len - (field_range(tf, &lo, &hi) ? hi - lo : 0);
	size_t room = (retained + 1 < tf->cap) ? (tf->cap - 1 - retained) : 0;
	if (room == 0)
		return 0;
	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "r");
	if (h < 0)
		return 0;

	// One line's worth is all a one-line control can honestly hold, and of
	// that line, what fits — cut where one of the CLIPBOARD'S letters ends.
	// Whether the last letter that fits is whole depends on the bytes after
	// it: the rest of a two-byte `é`, or a combining mark that makes the
	// letter before it longer. No UTF-8 scalar is longer than four bytes, so
	// the line is read four bytes past the room and the cut is judged
	// against the source, not against a copy already cut short. The buffer
	// is bounded by the capacity the application gave the field.
	size_t want = room + 4;
	char *line = os64_malloc(want);
	if (!line) {
		os64_close((int32_t)h);
		return 0;
	}
	size_t got = 0;
	bool ended = false, failed = false;
	while (got < want && !ended) {
		int64_t n = os64_read((int32_t)h, line + got, want - got);
        if (n < 0) { failed = true; break; }
		if (n == 0) break;
		size_t end = got + (size_t)n;
		for (size_t i = got; i < end; i++) {
			if (line[i] == '\n' || line[i] == '\r' || !line[i]) {
				end = i;                 // a field takes line one, and says so
				ended = true;
				break;
			}
		}
		got = end;
	}
	os64_close((int32_t)h);

	// A letter that does not fit is left behind whole. Malformed bytes are
	// one-byte letters to the decoder and paste as they came: the field
	// keeps the clipboard's bytes, it does not repair them.
	size_t take = failed ? 0 : got > room ? os64_ui_text_snap(line, got, room, false) : got;
	if (take > 0) {
        field_delete_selection(tf);
		// +1 rides the NUL, exactly as the K_CHAR path does.
		os64_memmove(tf->buf + tf->cursor + take, tf->buf + tf->cursor,
		             tf->len - tf->cursor + 1);
		os64_memcpy(tf->buf + tf->cursor, line, take);
		tf->cursor += take;
		tf->len += take;
		// Pasted text can finish a letter the field already held — a
		// letter in front of a combining mark — so settle past it.
		tf->cursor = os64_ui_text_snap(tf->buf, tf->len, tf->cursor, true);
        tf->anchor = tf->cursor; tf->selected = false;
		field_keep_caret_visible(tf, ui);
		os64_ui_mark_dirty(ui, &tf->w);
	}
	os64_free(line);
	return take;
}

// ── ui_textview ─────────────────────────────────────────────────────────────

#define VIEW_INSET 2

// The pitch this view lays rows out on: the DOCUMENT face's line box, cached
// on the widget because the call that asks for a row count is handed a theme
// and a theme cannot carry a face. An unstamped view reads as the bitmap
// cell, which is what the builtin set gives anyway.
static int32_t tv_pitch(const os64_ui_textview_t *tv, const os64_ui_theme_t *t)
{
	return tv->row_h > 0 ? tv->row_h : t->font_h;
}

int32_t os64_ui_textview_rows(const os64_ui_textview_t *tv,
                              const os64_ui_theme_t *t)
{
	int32_t pitch = tv_pitch(tv, t);
	int32_t r = pitch > 0 ? (tv->w.bounds.h - 2 * VIEW_INSET) / pitch : 0;
	return r > 0 ? r : 1;
}

int32_t os64_ui_textview_width(const os64_ui_textview_t *tv)
{
	int32_t width = tv->w.bounds.w - 2 * VIEW_INSET;
	return width > 0 ? width : 1;
}

static void textview_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
	((os64_ui_textview_t *)w)->row_h =
		os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT);
}

static size_t tv_window_bytes(const os64_ui_textview_t *tv)
{
	return tv->window_bytes ? tv->window_bytes : OS64_UI_TEXTVIEW_WINDOW;
}

os64_font_status_t os64_ui_textview_line_width(os64_ui_t *ui,
                                               const os64_ui_textview_t *tv,
                                               const char *s, size_t len,
                                               int64_t *out, bool *whole)
{
	*whole = len <= tv_window_bytes(tv);
	if (!*whole)
		return OS64_FONT_OK;
	int32_t width = 0;
	os64_font_status_t status =
		os64_ui_text_measure(ui, OS64_FONT_ROLE_DOCUMENT, s, len, &width);
	if (status == OS64_FONT_OK)
		*out = width;
	return status;
}

// Defined with the rest of the model accessors below; the run cache needs
// them and sits next to the geometry it serves.
static const char *tv_line(const os64_ui_textview_t *tv, size_t i, size_t *len);
static size_t tv_count(const os64_ui_textview_t *tv);

// ── the visible lines' runs ─────────────────────────────────────────────────
// One per row the view can show, plus one for the line the caret is on —
// which is usually the same line, and is kept separately because motion
// happens before the scroll that brings it back into view.

static void tv_free_runs(void ***runs, size_t *count)
{
	for (size_t i = 0; i < *count; ++i)
		os64_ui_run_release((*runs)[i]);
	os64_free(*runs);
	*runs = NULL;
	*count = 0;
}

// More row slots, keeping the runs the existing ones hold. Refusal keeps the
// array as it was.
static void tv_grow_slots(os64_ui_textview_t *tv, size_t want)
{
	void **runs = os64_malloc(want * sizeof(*runs));
	if (!runs)
		return;
	for (size_t i = 0; i < want; ++i)
		runs[i] = i < tv->row_run_count ? tv->row_runs[i] : NULL;
	os64_free(tv->row_runs);
	tv->row_runs = runs;
	tv->row_run_count = want;
}

// The run for a line, in the slot that belongs to it, laid out only if what
// is there says something else. TWO DIFFERENT KINDS OF NULL, and a caller
// must not treat them alike: OK with no run means the window wears no face
// and the bitmap cell is drawing; any other status means a layout was
// refused, the slot keeps what it had, and whatever needed the run's pixels
// does not happen.
static os64_font_status_t tv_run(os64_ui_t *ui, void **slot,
                                 const char *s, size_t len, void **out)
{
	return os64_ui_run_resolve(ui, slot, OS64_FONT_ROLE_DOCUMENT, s, len, out);
}

// ── a long line's window ────────────────────────────────────────────────────
// What a row lays out. Everything here scans a bounded number of bytes,
// however long the line and however long any one cluster in it: a cluster
// is recognized as too long to lay out after `budget` bytes of it, not at
// its end. The one walk that is not bounded finds the edge before a kept
// window origin that an edit has left inside a cluster — a mark typed at
// the window's start joins the cluster before it — and it costs the length
// of that one cluster.

typedef struct
{
	size_t from, to;             // the span laid out
	uint8_t lead, trail;         // EDGE_* at each end
} tv_span_t;

// Whole clusters from the edge `from`, at most `budget` bytes of them,
// stopping before any cluster that is itself longer than the budget.
//
// PAST A CLUSTER TOO LONG TO LAY OUT, the line may go on or may not, and
// finding its far end is a walk the length of the cluster. The line's own
// end answers most of it for a bounded price: if the line's last cluster is
// short, it is not the long one, so the line goes on past the long one —
// and likewise before it, if the line's first cluster is short. A long
// cluster at the line's end leaves the answer unknown, and unknown draws
// no "more".
static tv_span_t tv_span_from(const char *s, size_t len, size_t from,
                              size_t budget)
{
	tv_span_t sp = { from, from, EDGE_NONE, EDGE_NONE };
	size_t edge;
	if (from > 0) {
		sp.lead = ui_text_cluster_before(s, len, from, budget, &edge)
		          ? EDGE_MORE : EDGE_LONG;
		if (sp.lead == EDGE_LONG && ui_text_cluster_after(s, len, 0, budget, &edge))
			sp.lead |= EDGE_MORE;
	}
	while (sp.to < len) {
		if (!ui_text_cluster_after(s, len, sp.to, budget, &edge)) {
			sp.trail = EDGE_LONG;
			if (ui_text_cluster_before(s, len, len, budget, &edge))
				sp.trail |= EDGE_MORE;
			return sp;
		}
		if (edge - from > budget)
			break;
		sp.to = edge;
	}
	if (sp.to < len)
		sp.trail = EDGE_MORE;
	return sp;
}

// Where a window around the caret starts: half a budget of whole clusters
// back from it — more near the line's end, where the other half has nothing
// to show — or less where a cluster too long to lay out, or the line's
// start, is nearer.
static size_t tv_origin_around(const char *s, size_t len, size_t caret,
                               size_t budget)
{
	size_t back = budget / 2;
	if (len - caret < budget / 2)
		back = budget - (len - caret);
	size_t from = caret, start;
	while (from > 0 && ui_text_cluster_before(s, len, from, budget, &start) &&
	       caret - start <= back)
		from = start;
	return from;
}

// The span row `li` shows, with `budget` bytes to spend. A line that fits
// is shown whole. The caret's line keeps the window it has while the caret
// is inside it and at least an eighth of a window from an end with
// ordinary text omitted beyond, and is otherwise centred on the caret, so
// reaching an edge moves the window and the caret stays where it is in the
// document. (A cluster too long to lay out is never slid into view; the
// caret crosses it in one step, and the window follows.) Every other long
// line shows its start. The answer depends only on the bytes, the caret,
// the kept origin and the budget, so a paint and the click after it agree.
static tv_span_t tv_window(const os64_ui_textview_t *tv, size_t li,
                           size_t budget)
{
	size_t len;
	const char *s = tv_line(tv, li, &len);
	if (len <= budget)
		return (tv_span_t){ 0, len, EDGE_NONE, EDGE_NONE };
	if (li != tv->cur_line)
		return tv_span_from(s, len, 0, budget);
	size_t caret = tv->cur_col <= len ? tv->cur_col : len;
	if (tv->win_line == li && tv->win_from <= caret) {
		tv_span_t sp = tv_span_from(s, len,
		                            os64_ui_text_snap(s, len, tv->win_from, false),
		                            budget);
		size_t margin = budget / 8;
		bool near_start = sp.lead == EDGE_MORE && caret < sp.from + margin;
		bool near_end = sp.trail == EDGE_MORE && caret + margin > sp.to;
		if (caret >= sp.from && caret <= sp.to && !near_start && !near_end)
			return sp;
	}
	return tv_span_from(s, len, tv_origin_around(s, len, caret, budget), budget);
}

// The budget this view lays rows out with: what a LIMIT left behind, or the
// application's window when it has set a different one.
static size_t tv_budget(os64_ui_textview_t *tv)
{
	size_t want = tv_window_bytes(tv);
	if (!tv->win_budget || tv->win_budget_of != want) {
		tv->win_budget = want;
		tv->win_budget_of = want;
	}
	return tv->win_budget;
}

// Keep the caret line's window where tv_window puts it, so the next answer
// starts from there. Called wherever the caret may have moved.
static void tv_settle_window(os64_ui_textview_t *tv)
{
	tv->win_line = tv->cur_line;
	tv->win_from = tv_window(tv, tv->cur_line, tv_budget(tv)).from;
}

// A decoration is a row's height square for "more", two for a cluster:
// enough to be seen, not so much that it crowds the text it stands beside.
static int32_t edge_width(uint8_t kind, int32_t pitch)
{
	if (pitch < OS64_FONT_GLYPH_W)
		pitch = OS64_FONT_GLYPH_W;
	return (kind & EDGE_MORE ? pitch : 0) + (kind & EDGE_LONG ? 2 * pitch : 0);
}

// The smallest window a LIMIT answer halves down to. There the halving
// stops and the answer stands, whatever it is; a run of that few bytes is
// nowhere near the text engine's limits.
#define TV_WINDOW_FLOOR 64u

// The geometry of row `li`, its span laid out into `slot`, with `*budget`
// bytes to spend. A window the run calls too big (LIMIT) is halved until it
// is not — rather than leaving the line with nothing to show or edit — and
// `*budget` comes back at what it took, so every later row and every later
// lookup resolves the same spans. Any other refusal is the answer.
static os64_font_status_t tv_row_geom(os64_ui_textview_t *tv, os64_ui_t *ui,
                                      size_t li, void **slot, int32_t pitch,
                                      size_t *budget, line_geom_t *g)
{
	size_t len;
	const char *s = tv_line(tv, li, &len);
	for (;;) {
		tv_span_t sp = tv_window(tv, li, *budget);
		*g = (line_geom_t){ NULL, s, len, sp.from, sp.to,
		                    edge_width(sp.lead, pitch), edge_width(sp.trail, pitch),
		                    sp.lead, sp.trail };
		os64_font_status_t status =
			tv_run(ui, slot, s + sp.from, sp.to - sp.from, &g->run);
		if (status != OS64_FONT_LIMIT || *budget / 2 < TV_WINDOW_FLOOR)
			return status;
		*budget /= 2;
	}
}

// A row laid out with the LIVE budget, which a LIMIT here reduces for good:
// the alternative is a later lookup resolving a wider span than the run this
// one retained, which is the geometry the caret and the scroll came from.
static os64_font_status_t tv_row_live(os64_ui_textview_t *tv, os64_ui_t *ui,
                                      size_t li, void **slot, int32_t pitch,
                                      line_geom_t *g)
{
	size_t budget = tv_budget(tv);
	os64_font_status_t status = tv_row_geom(tv, ui, li, slot, pitch, &budget, g);
	tv->win_budget = budget;
	return status;
}

// A line's geometry, its span laid out into the caret's slot: the line
// motion is about to put the caret on, or the one it is on.
static os64_font_status_t tv_line_geom(os64_ui_textview_t *tv, os64_ui_t *ui,
                                       size_t li, line_geom_t *g)
{
	return tv_row_live(tv, ui, li, &tv->w.run, tv_pitch(tv, &ui->theme), g);
}

os64_font_status_t os64_ui_textview_row_width(os64_ui_t *ui,
                                              os64_ui_textview_t *tv,
                                              size_t line, int64_t *out)
{
	line_geom_t g;
	os64_font_status_t status = tv_line_geom(tv, ui, line, &g);
	if (status == OS64_FONT_OK)
		*out = geom_width(&g);
	return status;
}

os64_font_status_t os64_ui_textview_shown_width(os64_ui_t *ui,
                                                os64_ui_textview_t *tv,
                                                int64_t *out)
{
	// The rows the paint walks, in the slots it walks them with.
	int32_t rows = os64_ui_textview_rows(tv, &ui->theme);
	int32_t pitch = tv_pitch(tv, &ui->theme);
	size_t count = tv_count(tv);
	size_t shown = tv->top < count ? count - tv->top : 0;
	if (shown > (size_t)rows)
		shown = (size_t)rows;
	if (tv->row_run_count < shown)
		tv_grow_slots(tv, shown);

	int64_t widest = 0;
	for (size_t r = 0; r < shown; ++r) {
		void *spare = NULL;
		void **slot = r < tv->row_run_count ? &tv->row_runs[r] : &spare;
		line_geom_t g;
		os64_font_status_t status = tv_row_live(tv, ui, tv->top + r, slot, pitch, &g);
		if (status == OS64_FONT_OK && geom_width(&g) > widest)
			widest = geom_width(&g);
		os64_ui_run_release(spare);
		if (status != OS64_FONT_OK)
			return status;
	}
	*out = widest;
	return OS64_FONT_OK;
}

// What one preparation pass stages: the caret's run, the visible rows' runs,
// and the view they were measured in.
typedef struct
{
	void *caret;
	void **runs;
	size_t count;
	size_t top;
	int64_t left, goal;
} tv_pass_t;

static void tv_pass_release(tv_pass_t *p)
{
	os64_ui_run_release(p->caret);
	for (size_t i = 0; i < p->count; ++i)
		os64_ui_run_release(p->runs[i]);
	os64_free(p->runs);
	*p = (tv_pass_t){0};
}

// One pass: lay the caret's line and every visible row out with `*budget`,
// and work out the view they imply. A LIMIT lowers `*budget`, and the caller
// runs the pass again — the rows laid out before it dropped hold spans of a
// window this view no longer uses.
static os64_font_status_t tv_pass(os64_ui_textview_t *tv, os64_ui_t *ui,
                                  int32_t pitch, size_t rows, int32_t width,
                                  size_t *budget, tv_pass_t *out)
{
	*out = (tv_pass_t){0};
	size_t count = tv_count(tv);

	// The caret's line FIRST: where the caret is decides where the view has
	// to be, and the view decides which lines to prepare. Its window, and
	// every row's, is chosen in bytes, so a font change keeps it.
	line_geom_t g;
	os64_font_status_t status =
		tv_row_geom(tv, ui, tv->cur_line, &out->caret, pitch, budget, &g);
	if (status != OS64_FONT_OK)
		return status;

	// THE VIEW THIS FACE WILL SHOW. The vertical half is pure arithmetic on
	// the candidate's pitch. The horizontal half is worked out afresh from
	// the caret: the old scroll is a number in the retired face's pixels and
	// no position in this one, so the view is the one this caret would get
	// in a document just opened. The remembered Up/Down X is re-derived from
	// the caret too, so the next vertical run holds the lane the caret is
	// actually in.
	size_t top = tv->top;
	if (tv->cur_line < top)
		top = tv->cur_line;
	if (tv->cur_line >= top + rows)
		top = tv->cur_line - rows + 1;
	int32_t cx = 0;
	status = geom_caret(&g, tv->cur_col, &cx);
	if (status != OS64_FONT_OK)
		return status;      // committing would keep a scroll in retired pixels
	out->top = top;
	out->left = scroll_to_show(0, cx, width);
	out->goal = cx;

	size_t visible = rows;
	if (top < count && visible > count - top)
		visible = count - top;
	else if (top >= count)
		visible = 0;
	if (!visible)
		return OS64_FONT_OK;

	out->runs = os64_malloc(visible * sizeof(*out->runs));
	if (!out->runs)
		return OS64_FONT_NO_MEMORY;
	for (size_t i = 0; i < visible; ++i)
		out->runs[i] = NULL;
	out->count = visible;
	for (size_t i = 0; i < visible; ++i) {
		line_geom_t rg;
		status = tv_row_geom(tv, ui, top + i, &out->runs[i], pitch, budget, &rg);
		if (status != OS64_FONT_OK)
			return status;
	}
	return OS64_FONT_OK;
}

static os64_font_status_t textview_prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	int32_t pitch = os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT);
	os64_gui_rect_t planned = os64_ui_widget_planned_bounds(w);
	int32_t fits = pitch > 0 ? (planned.h - 2 * VIEW_INSET) / pitch : 0;
	size_t rows = fits > 0 ? (size_t)fits : 1;
	int32_t width = planned.w - 2 * VIEW_INSET;

	// A WIDER WINDOW IS TRIED AGAIN HERE. Preparation starts from the
	// application's window whatever a LIMIT left behind, because this is
	// where a refusal still means the old face, the old spans and the old
	// view stay exactly as they are. (An application that sets
	// `window_bytes` asks for a different window, and gets it afresh.) A
	// pass that ends on a smaller budget than it began with is thrown away
	// and run again, so every staged run holds a span of the SAME window —
	// the one commit will lay the caret, the scroll and the decorations
	// against.
	tv_pass_t pass = {0};
	size_t budget = tv_window_bytes(tv);
	for (;;) {
		size_t started = budget;
		os64_font_status_t status = tv_pass(tv, ui, pitch, rows, width, &budget, &pass);
		if (status != OS64_FONT_OK) {
			tv_pass_release(&pass);
			return status;
		}
		if (budget == started)
			break;
		tv_pass_release(&pass);
	}

	tv_free_runs(&tv->row_runs_staged, &tv->row_runs_staged_count);
	tv->row_runs_staged = pass.runs;
	tv->row_runs_staged_count = pass.count;
	os64_ui_run_release(w->run_staged);
	w->run_staged = pass.caret;
	tv->top_staged = pass.top;
	tv->left_staged = pass.left;
	tv->goal_staged = pass.goal;
	tv->win_budget_staged = budget;
	return OS64_FONT_OK;
}

// Everything here was decided in preparation; commit only moves it into
// place, so it cannot allocate and cannot fail. It fires no callback either
// — the application's own commit runs next and brings its scrollbars along.
static void textview_commit(os64_ui_widget_t *w)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	tv_free_runs(&tv->row_runs, &tv->row_run_count);
	tv->row_runs = tv->row_runs_staged;
	tv->row_run_count = tv->row_runs_staged_count;
	tv->row_runs_staged = NULL;
	tv->row_runs_staged_count = 0;
	os64_ui_commit_caption(w);          // the caret line's run
	tv->top = tv->top_staged;
	tv->left_px = tv->left_staged;
	tv->goal_x = tv->goal_staged;
	// The budget those runs were laid out with, so every lookup after this
	// resolves the spans they hold rather than reaching for a wider window.
	tv->win_budget = tv->win_budget_staged;
	tv->win_budget_of = tv_window_bytes(tv);
}

static void textview_discard(os64_ui_widget_t *w)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	tv_free_runs(&tv->row_runs_staged, &tv->row_runs_staged_count);
	os64_ui_discard_caption(w);
}

static void textview_destroy(os64_ui_widget_t *w)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	tv_free_runs(&tv->row_runs, &tv->row_run_count);
	tv_free_runs(&tv->row_runs_staged, &tv->row_runs_staged_count);
}

static const char *tv_line(const os64_ui_textview_t *tv, size_t i, size_t *len)
{
	return tv->buf->line(tv->buf->user, i, len);
}

static size_t tv_count(const os64_ui_textview_t *tv)
{
	size_t n = tv->buf->line_count(tv->buf->user);
	return n > 0 ? n : 1;
}

static bool tv_editable(const os64_ui_textview_t *tv)
{
	// A read-only model simply leaves the editing half of the vtable NULL —
	// insert stands proxy for the set (SCRIBE.md: "all NULL" is the contract).
	return tv->buf->insert != NULL;
}

// Selection, normalized to (start <= end). False when there is nothing lit.
static bool tv_sel_range(const os64_ui_textview_t *tv,
                         size_t *sl, size_t *sc, size_t *el, size_t *ec)
{
	if (!tv->sel)
		return false;
	if (tv->sel_line < tv->cur_line ||
	    (tv->sel_line == tv->cur_line && tv->sel_col < tv->cur_col)) {
		*sl = tv->sel_line; *sc = tv->sel_col;
		*el = tv->cur_line; *ec = tv->cur_col;
	} else {
		*sl = tv->cur_line; *sc = tv->cur_col;
		*el = tv->sel_line; *ec = tv->sel_col;
	}
	return !(*sl == *el && *sc == *ec);
}


static void tv_fire_view(os64_ui_textview_t *tv)
{
	if (tv->on_view)
		tv->on_view(tv, tv->view_user);
}

// Scroll the viewport (never the cursor) until the cursor is inside it.
static void tv_ensure_visible(os64_ui_t *ui, os64_ui_textview_t *tv)
{
	const os64_ui_theme_t *t = &ui->theme;
	int32_t rows = os64_ui_textview_rows(tv, t);
	int32_t width = os64_ui_textview_width(tv);
	size_t old_top = tv->top, old_win = tv->win_from;
	int64_t old_left = tv->left_px;

	if (tv->cur_line < tv->top)
		tv->top = tv->cur_line;
	if (tv->cur_line >= tv->top + (size_t)rows)
		tv->top = tv->cur_line - (size_t)rows + 1;

	// A long line's window moves first, if the caret has left it or come
	// near an end of it: where the caret sits in the row depends on where
	// the row starts.
	tv_settle_window(tv);

	// A caret that cannot be located leaves the horizontal scroll alone
	// rather than jumping somewhere arbitrary; the vertical half above is
	// pure arithmetic and always right.
	int32_t cx = 0;
	line_geom_t g;
	if (tv_line_geom(tv, ui, tv->cur_line, &g) == OS64_FONT_OK &&
	    geom_caret(&g, tv->cur_col, &cx) == OS64_FONT_OK)
		tv->left_px = scroll_to_show(tv->left_px, cx, width);

	if (tv->top != old_top || tv->left_px != old_left || tv->win_from != old_win)
		tv_fire_view(tv);
	os64_ui_mark_dirty(ui, &tv->w);
}

static void fill_clipped(os64_gui_surface_t *surf, os64_gui_rect_t r,
                         os64_gui_rect_t clip, uint32_t colour)
{
	os64_gui_rect_t painted;
	if (os64_rect_intersect(r, clip, &painted))
		os64_draw_fill_rect(surf, painted, colour);
}

// A decoration, in the window's chrome colours because it is not text:
// omitted bytes are a small arrowhead pointing at where they are, and a
// cluster too long to lay out is a hollow box standing in for it. When
// both stand at one end, the box is beside the text it follows or leads
// and the arrow is outside it, because that is the order of the bytes.
static void tv_draw_edge(os64_gui_surface_t *surf, os64_gui_rect_t clip,
                         os64_gui_rect_t box, uint8_t kind, bool before,
                         uint32_t bg, uint32_t fg)
{
	fill_clipped(surf, box, clip, bg);
	int32_t cy = box.y + box.h / 2;
	if (kind & EDGE_MORE) {
		int32_t w = edge_width(EDGE_MORE, box.h);
		os64_gui_rect_t cell = { before ? box.x : box.x + box.w - w, box.y, w, box.h };
		int32_t size = box.h / 4 > 2 ? box.h / 4 : 2;
		int32_t x0 = cell.x + (cell.w - size) / 2;
		for (int32_t i = 0; i < size; ++i) {
			int32_t reach = before ? i + 1 : size - i;
			fill_clipped(surf, (os64_gui_rect_t){ x0 + i, cy - reach, 1, 2 * reach },
			             clip, fg);
		}
		box.w -= w;
		if (before)
			box.x += w;
	}
	if (kind & EDGE_LONG) {
		os64_gui_rect_t in = { box.x + 3, box.y + 3, box.w - 6, box.h - 6 };
		fill_clipped(surf, (os64_gui_rect_t){ in.x, in.y, in.w, 1 }, clip, fg);
		fill_clipped(surf, (os64_gui_rect_t){ in.x, in.y + in.h - 1, in.w, 1 }, clip, fg);
		fill_clipped(surf, (os64_gui_rect_t){ in.x, in.y, 1, in.h }, clip, fg);
		fill_clipped(surf, (os64_gui_rect_t){ in.x + in.w - 1, in.y, 1, in.h }, clip, fg);
		for (int32_t x = in.x + 3; x + 1 < in.x + in.w - 2; x += 4)
			fill_clipped(surf, (os64_gui_rect_t){ x, cy, 2, 1 }, clip, fg);
	}
}

// One row as its geometry describes it: the leading decoration, the span,
// the trailing decoration. Everything is drawn from `g`'s one rendering and
// nothing is looked up again.
static void tv_draw_row(os64_ui_t *ui, os64_gui_surface_t *surf,
                        const line_geom_t *g, os64_gui_rect_t clip,
                        int32_t origin, int32_t y, int32_t pitch,
                        uint32_t fg, uint32_t bg, uint32_t edge_bg, uint32_t edge_fg)
{
	if (g->lead)
		tv_draw_edge(surf, clip, (os64_gui_rect_t){ origin, y, g->lead, pitch },
		             g->lead_kind, true, edge_bg, edge_fg);
	os64_ui_draw_run(ui, g->run, OS64_FONT_ROLE_DOCUMENT, surf, clip,
	                 origin + g->lead, y, g->s + g->from, g->to - g->from, fg, bg);
	if (g->trail) {
		int32_t end = g->lead;
		geom_caret(g, g->to, &end);
		tv_draw_edge(surf, clip, (os64_gui_rect_t){ origin + end, y, g->trail, pitch },
		             g->trail_kind, false, edge_bg, edge_fg);
	}
}

static void textview_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                           const os64_ui_theme_t *t)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	os64_ui_t *ui = os64_ui_of(w);
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->text_bg);

	int32_t rows = os64_ui_textview_rows(tv, t);
	int32_t pitch = tv_pitch(tv, t);
	size_t count = tv_count(tv);
	size_t sl = 0, sc = 0, el = 0, ec = 0;
	bool have_sel = tv_sel_range(tv, &sl, &sc, &el, &ec);

	// The paper the text sits on, and the origin the line's own pixel zero
	// maps to once the view has scrolled.
	os64_gui_rect_t area = { w->bounds.x + VIEW_INSET, w->bounds.y + VIEW_INSET,
	                         w->bounds.w - 2 * VIEW_INSET,
	                         w->bounds.h - 2 * VIEW_INSET };
	int32_t origin = area.x - (int32_t)tv->left_px;

	// A slot for every row, so an unchanged line repaints from the run it
	// kept. Adoption stages one per line the view could show; a document
	// that has grown since, or a view that has never adopted a face, gets
	// its slots here. A refused growth still paints — those rows lay out a
	// run of their own and let it go.
	size_t shown = tv->top < count ? count - tv->top : 0;
	if (shown > (size_t)rows)
		shown = (size_t)rows;
	if (tv->row_run_count < shown)
		tv_grow_slots(tv, shown);

	for (int32_t r = 0; r < rows; r++) {
		size_t li = tv->top + (size_t)r;
		if (li >= count)
			break;
		int32_t y = area.y + r * pitch;
		os64_gui_rect_t row = { area.x, y, area.w, pitch };
		os64_gui_rect_t clip;
		if (!os64_rect_intersect(row, area, &clip))
			continue;

		void *spare = NULL;
		void **slot = (size_t)r < tv->row_run_count ? &tv->row_runs[r] : &spare;

		// ONE RENDERING ANSWERS ALL THREE QUESTIONS, chosen once per row.
		// The glyphs, the highlight and the caret are the same measurement
		// of the same bytes, so they cannot describe different text — which
		// is what happens the moment a selection is computed one way and
		// painted another. The draws below are of this choice and look
		// nothing up again: the row's run, or with no face the bitmap cell.
		// A long line's row is its window, and the geometry says where its
		// decorations stand. A refused layout draws nothing and places
		// nothing.
		line_geom_t g;
		if (tv_row_live(tv, ui, li, slot, pitch, &g) != OS64_FONT_OK) {
			os64_ui_run_release(spare);
			continue;
		}

		// Selected bytes on this line, as a byte range of it.
		size_t from = 0, to = 0;
		bool lit = false;
		if (have_sel && li >= sl && li <= el) {
			from = li == sl ? sc : 0;
			to = li == el ? ec : g.len;
			lit = to > from;
		}

		// TWO PASSES OVER ONE RENDERING. The whole row in its ordinary
		// colours, then the row again in the selection's colours CLIPPED
		// TO THE SELECTED SPAN — the draw fills its own box with the
		// background it is given, so the second pass lays down the
		// highlight and the lit glyphs together, exactly as wide as the
		// selection and no wider. The same rendering both times, so the
		// second pass costs no layout and its glyphs land on the first
		// pass's pixels. A decoration standing for selected bytes is lit
		// with them.
		tv_draw_row(ui, &ctx->surf, &g, clip, origin, y, pitch,
		            t->text_fg, t->text_bg, t->scroll_track, t->scroll_thumb);
		if (lit) {
			os64_gui_rect_t sel;
			if (geom_selection(&g, from, to, &sel) == OS64_FONT_OK) {
				// X is relative to the row; the ROW is what this view lays
				// out on (FONT_PROVIDER.md: pitch comes from the primary
				// face and does not grow for a fallback).
				os64_gui_rect_t lit_rect = { origin + sel.x, y, sel.w, pitch };
				os64_gui_rect_t sel_clip;
				if (os64_rect_intersect(lit_rect, clip, &sel_clip))
					tv_draw_row(ui, &ctx->surf, &g, sel_clip, origin, y, pitch,
					            t->text_sel_fg, t->text_sel_bg,
					            t->text_sel_bg, t->text_sel_fg);
			}
		}

		if (w->focused && li == tv->cur_line) {
			int32_t cx = 0;
			if (geom_caret(&g, tv->cur_col, &cx) == OS64_FONT_OK) {
				os64_gui_rect_t caret = { origin + cx, y, 2, pitch };
				os64_gui_rect_t painted;
				if (os64_rect_intersect(caret, clip, &painted))
					os64_draw_fill_rect(&ctx->surf, painted, t->text_caret);
			}
		}
		os64_ui_run_release(spare);
	}
}

// Delete the selection through the vtable's choke points. The multi-line
// case: behead the last line, curtail the first, drop the WHOLE lines
// between (one erase_lines — the O(n) path), then join the survivors.
static bool tv_delete_selection(os64_ui_textview_t *tv)
{
	size_t sl, sc, el, ec;
	if (!tv_sel_range(tv, &sl, &sc, &el, &ec))
		return false;
	const os64_ui_textbuf_t *b = tv->buf;

	if (sl == el) {
		b->erase(b->user, sl, sc, ec - sc);
	} else {
		size_t len;
		tv_line(tv, sl, &len);
		b->erase(b->user, sl, sc, len - sc);
		b->erase(b->user, el, 0, ec);
		if (el > sl + 1) {
			if (b->erase_lines) {
				b->erase_lines(b->user, sl + 1, el - sl - 1);
			} else {
				// No bulk op: repeated joins onto the (now empty-tailed)
				// first line. Correct, O(selection) — fine for buffers that
				// didn't bother implementing the fast path.
				for (size_t i = sl + 1; i < el; i++)
					b->join(b->user, sl);
			}
		}
		b->join(b->user, sl);
	}
	tv->cur_line = sl;
	tv->cur_col = sc;
	tv->sel = false;
	return true;
}

// Cursor motion bookkeeping: Shift extends (planting the anchor on the
// first extension), unshifted motion collapses. Every motion path calls
// this FIRST, then moves the cursor.
static void tv_motion_prologue(os64_ui_textview_t *tv, bool shift)
{
	if (shift) {
		if (!tv->sel) {
			tv->sel = true;
			tv->sel_line = tv->cur_line;
			tv->sel_col = tv->cur_col;
		}
	} else {
		tv->sel = false;
	}
}

// Remember where the caret is, in pixels, for a later Up or Down. A caret
// that cannot be measured keeps the goal it had: the goal is a preference,
// not a position, so a stale one costs a lane and never a byte.
static void tv_remember_goal(os64_ui_textview_t *tv, os64_ui_t *ui)
{
	line_geom_t g;
	int32_t cx = 0;
	if (tv_line_geom(tv, ui, tv->cur_line, &g) == OS64_FONT_OK &&
	    geom_caret(&g, tv->cur_col, &cx) == OS64_FONT_OK)
		tv->goal_x = cx;
}

// Where the remembered X lands on line `li`: the half of Up and Down that
// needs pixels, asked BEFORE anything moves. A line whose run cannot be laid
// out refuses, and the caller moves nothing — not the caret, the selection
// or the scroll.
static os64_font_status_t tv_goal_offset(os64_ui_textview_t *tv, os64_ui_t *ui,
                                         size_t li, size_t *out)
{
	line_geom_t g;
	os64_font_status_t status = tv_line_geom(tv, ui, li, &g);
	if (status != OS64_FONT_OK)
		return status;
	return geom_hit(&g, (int32_t)tv->goal_x, out);
}

// One cluster forward or back along the caret's line, read from its bytes.
static void tv_step_caret(os64_ui_textview_t *tv, bool forward)
{
	size_t len;
	const char *ln = tv_line(tv, tv->cur_line, &len);
	tv->cur_col = os64_ui_text_step(ln, len, tv->cur_col, forward);
}

// After an edit, bring the caret out of any cluster the edit has made
// around it. Bytes meet across the caret — a letter typed in front of a
// combining mark, two lines joined, a cluster removed from between them —
// and can become one letter; the caret goes after it, never inside it.
static void tv_settle_caret(os64_ui_textview_t *tv)
{
	size_t len;
	const char *ln = tv_line(tv, tv->cur_line, &len);
	tv->cur_col = os64_ui_text_snap(ln, len, tv->cur_col, true);
}

// Put the caret where the pointer is. False, with nothing moved, when the
// line under the pointer cannot be laid out: without its geometry there is
// no knowing which letter was pointed at.
static bool tv_place_from_point(os64_ui_textview_t *tv, os64_ui_t *ui,
                                int32_t mx, int32_t my)
{
	const os64_ui_theme_t *t = &ui->theme;
	size_t count = tv_count(tv);

	int32_t pitch = tv_pitch(tv, t);
	int32_t row = pitch > 0 ? (my - tv->w.bounds.y - VIEW_INSET) / pitch : 0;
	int64_t li = (int64_t)tv->top + row;      // row may be negative: a drag
	if (li < 0)                                // above the view auto-scrolls up
		li = 0;
	if (li >= (int64_t)count)
		li = (int64_t)count - 1;

	// The pixel of the LINE the pointer is over, which is where the pointer
	// is minus where the line starts on screen.
	int64_t x = (int64_t)(mx - tv->w.bounds.x - VIEW_INSET) + tv->left_px;
	if (x < 0)
		x = 0;

	line_geom_t g;
	size_t offset = 0;
	int32_t cx = 0;
	if (tv_line_geom(tv, ui, (size_t)li, &g) != OS64_FONT_OK ||
	    geom_hit(&g, (int32_t)x, &offset) != OS64_FONT_OK ||
	    geom_caret(&g, offset, &cx) != OS64_FONT_OK)
		return false;
	tv->cur_line = (size_t)li;
	tv->cur_col = offset;
	tv->goal_x = cx;
	return true;
}

static bool textview_event(os64_ui_widget_t *w, os64_ui_t *ui,
                           const os64_gui_event_t *ev)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	const os64_ui_theme_t *t = &ui->theme;
	size_t count = tv_count(tv);

	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
		os64_ui_set_focus(ui, w);
		if (tv_place_from_point(tv, ui, ev->mouse.x, ev->mouse.y)) {
			tv->sel = false;
			tv->sel_line = tv->cur_line;
			tv->sel_col = tv->cur_col;
			// A click on a long line's decoration puts the caret against
			// the omitted text, and its window moves to show some.
			tv_ensure_visible(ui, tv);
		} else if (!tv->sel) {
			// Refused: the caret stays, and a drag that follows starts
			// from it rather than from wherever an old anchor was left.
			tv->sel_line = tv->cur_line;
			tv->sel_col = tv->cur_col;
		}
		os64_ui_mark_dirty(ui, w);
		return true;

	case OS64_GUI_EVENT_MOUSE_MOVE: {
		// We only see moves while grabbed (dispatch's rule) — a drag.
		size_t ol = tv->cur_line, oc = tv->cur_col;
		if (tv_place_from_point(tv, ui, ev->mouse.x, ev->mouse.y) &&
		    (tv->cur_line != ol || tv->cur_col != oc)) {
			tv->sel = true;
			tv_ensure_visible(ui, tv);
		}
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
		return true;

	case OS64_GUI_EVENT_KEY_DOWN: {
		bool shift = (ev->key.modifiers & OS64_GUI_MOD_SHIFT) != 0;
		bool can_edit = tv_editable(tv);
		bool changed = false;
		char c = 0;
		size_t len;
		const char *ln;

		ui_key_t k = os64_ui_decode_key(&tv->seq, ev, &c);
		switch (k) {
		case K_CHAR:
		case K_TAB: {
			if (!can_edit)
				return true;
			if (c == 0)
				c = '\t';
			if (tv->sel)
				changed = tv_delete_selection(tv);
			// The caret moves only past bytes that are really there: a
			// refused insert leaves it where the text still is.
			if (tv->buf->insert(tv->buf->user, tv->cur_line, tv->cur_col, &c, 1)) {
				tv->cur_col++;
				changed = true;
			}
			break;
		}
		case K_ENTER:
			if (!can_edit)
				return true;
			if (tv->sel)
				changed = tv_delete_selection(tv);
			if (tv->buf->split(tv->buf->user, tv->cur_line, tv->cur_col)) {
				tv->cur_line++;
				tv->cur_col = 0;
				changed = true;
			}
			break;
		// DELETION IS BY CLUSTER: the span between the caret and the
		// boundary beside it, found from the line's bytes, so no layout can
		// refuse it and `é` goes whole — both of its bytes, or neither.
		// The line joins at either end are the model's own verb.
		case K_BACKSPACE:
			if (!can_edit)
				return true;
			if (tv->sel) {
				changed = tv_delete_selection(tv);
			} else if (tv->cur_col > 0) {
				ln = tv_line(tv, tv->cur_line, &len);
				size_t from = os64_ui_text_step(ln, len, tv->cur_col, false);
				if (tv->buf->erase(tv->buf->user, tv->cur_line, from,
				                   tv->cur_col - from)) {
					tv->cur_col = from;
					changed = true;
				}
			} else if (tv->cur_line > 0) {
				tv_line(tv, tv->cur_line - 1, &len);
				if (tv->buf->join(tv->buf->user, tv->cur_line - 1)) {
					tv->cur_line--;
					tv->cur_col = len;
					changed = true;
				}
			}
			break;
		case K_DELETE:
			if (!can_edit)
				return true;
			if (tv->sel) {
				changed = tv_delete_selection(tv);
			} else {
				ln = tv_line(tv, tv->cur_line, &len);
				if (tv->cur_col < len) {
					size_t to = os64_ui_text_step(ln, len, tv->cur_col, true);
					changed = tv->buf->erase(tv->buf->user, tv->cur_line,
					                         tv->cur_col, to - tv->cur_col);
				} else if (tv->cur_line + 1 < count) {
					changed = tv->buf->join(tv->buf->user, tv->cur_line);
				}
			}
			break;

		case K_LEFT:
			tv_motion_prologue(tv, shift);
			// Stepping is by CLUSTER BOUNDARY, not by byte, so over an
			// accented letter the caret moves past the whole letter rather
			// than into the middle of it.
			if (tv->cur_col > 0) {
				tv_step_caret(tv, false);
			} else if (tv->cur_line > 0) {
				tv->cur_line--;
				tv_line(tv, tv->cur_line, &len);
				tv->cur_col = len;
			}
			tv_remember_goal(tv, ui);
			break;
		case K_RIGHT:
			tv_motion_prologue(tv, shift);
			ln = tv_line(tv, tv->cur_line, &len);
			if (tv->cur_col < len) {
				tv_step_caret(tv, true);
			} else if (tv->cur_line + 1 < count) {
				tv->cur_line++;
				tv->cur_col = 0;
			}
			tv_remember_goal(tv, ui);
			break;
		case K_HOME:
			tv_motion_prologue(tv, shift);
			tv->cur_col = 0;
			tv->goal_x = 0;
			break;
		case K_END:
			tv_motion_prologue(tv, shift);
			ln = tv_line(tv, tv->cur_line, &len);
			tv->cur_col = len;
			tv_remember_goal(tv, ui);
			break;
		case K_UP:
		case K_DOWN: {
			// The remembered goal X: runs of Up/Down hold their lane
			// through short lines — every editor since vi has kept this
			// promise, and its absence is instantly felt. It is a PIXEL
			// now, because under a proportional face the column the caret
			// "was in" is not a distance anything else agrees on. Where it
			// lands is worked out first; a refusal moves nothing.
			size_t li = tv->cur_line, col;
			if (k == K_UP && li > 0)
				li--;
			else if (k == K_DOWN && li + 1 < count)
				li++;
			if (tv_goal_offset(tv, ui, li, &col) != OS64_FONT_OK)
				return true;
			tv_motion_prologue(tv, shift);
			tv->cur_line = li;
			tv->cur_col = col;
			break;
		}
		case K_PGUP:
		case K_PGDN: {
			size_t rows = (size_t)os64_ui_textview_rows(tv, t);
			size_t li, top, col;
			if (k == K_PGUP) {
				li = tv->cur_line > rows ? tv->cur_line - rows : 0;
				top = tv->top > rows ? tv->top - rows : 0;
			} else {
				li = tv->cur_line + rows < count ? tv->cur_line + rows : count - 1;
				top = tv->top + rows;
				if (top + rows > count)
					top = count > rows ? count - rows : 0;
			}
			if (tv_goal_offset(tv, ui, li, &col) != OS64_FONT_OK)
				return true;
			tv_motion_prologue(tv, shift);
			tv->cur_line = li;
			tv->cur_col = col;
			tv->top = top;
			tv_fire_view(tv);
			break;
		}
		case K_ESC:
			tv->sel = false;
			break;
		default:
			return true;   // mid-burst / strangers: consumed silently
		}

		if (changed) {
			tv_settle_caret(tv);
			if (tv->on_change)
				tv->on_change(tv, tv->view_user);
		}
		tv_ensure_visible(ui, tv);
		return true;
	}
	case OS64_GUI_EVENT_KEY_UP:
		return true;
	default:
		return false;
	}
}

static void textview_cancel(os64_ui_widget_t *w)
{
    ((os64_ui_textview_t *)w)->seq = 0;
}

const os64_ui_class_t os64_ui_textview_class =
    { "textview", textview_paint, textview_event, textview_cancel,
      textview_prepare, textview_commit, textview_discard, textview_destroy,
      textview_metrics };

void os64_ui_textview(os64_ui_textview_t *tv, const os64_ui_textbuf_t *buf,
                      void (*on_change)(os64_ui_textview_t *, void *),
                      void (*on_view)(os64_ui_textview_t *, void *),
                      void *user)
{
	*tv = (os64_ui_textview_t){0};
	tv->w.cls = &os64_ui_textview_class;
	tv->w.focusable = true;
	tv->w.accepts_tab = buf && buf->insert;
	tv->buf = buf;
	tv->on_change = on_change;
	tv->on_view = on_view;
	tv->view_user = user;
}

void os64_ui_textview_scroll_left(os64_ui_t *ui, os64_ui_textview_t *tv,
                                  int64_t left_px)
{
	if (left_px < 0)
		left_px = 0;
	if (left_px == tv->left_px)
		return;
	tv->left_px = left_px;
	tv_fire_view(tv);
	os64_ui_mark_dirty(ui, &tv->w);
}

void os64_ui_textview_scroll_to(os64_ui_t *ui, os64_ui_textview_t *tv,
                                size_t top)
{
	int32_t rows = os64_ui_textview_rows(tv, &ui->theme);
	size_t count = tv_count(tv);
	size_t max_top = count > (size_t)rows ? count - (size_t)rows : 0;
	if (top > max_top)
		top = max_top;
	if (top == tv->top)
		return;
	tv->top = top;
	tv_fire_view(tv);
	os64_ui_mark_dirty(ui, &tv->w);
}

void os64_ui_textview_goto(os64_ui_t *ui, os64_ui_textview_t *tv,
                           size_t line, size_t col, bool select)
{
	size_t count = tv_count(tv);
	if (line >= count)
		line = count - 1;
	size_t len;
	const char *ln = tv_line(tv, line, &len);
	col = os64_ui_text_snap(ln, len, col, false);

	if (!select)
		tv->sel = false;
	else if (!tv->sel) {
		tv->sel = true;
		tv->sel_line = tv->cur_line;
		tv->sel_col = tv->cur_col;
	}
	tv->cur_line = line;
	tv->cur_col = col;
	tv_remember_goal(tv, ui);
	tv_ensure_visible(ui, tv);
}

void os64_ui_textview_select(os64_ui_t *ui, os64_ui_textview_t *tv,
                             size_t sl, size_t sc, size_t el, size_t ec)
{
	// A range from elsewhere — a search match is a byte count — widens to
	// whole clusters, so a selection never starts or ends inside a letter.
	size_t count = tv_count(tv), len;
	bool forward = sl < el || (sl == el && sc <= ec);
	if (sl < count) {
		const char *ln = tv_line(tv, sl, &len);
		sc = os64_ui_text_snap(ln, len, sc, !forward);
	}
	if (el < count) {
		const char *ln = tv_line(tv, el, &len);
		ec = os64_ui_text_snap(ln, len, ec, forward);
	}
	os64_ui_textview_goto(ui, tv, sl, sc, false);
	tv->sel = true;
	tv->sel_line = sl;
	tv->sel_col = sc;
	os64_ui_textview_goto(ui, tv, el, ec, true);
}

// ── copy / cut / paste (ui.h carries the doctrine) ──────────────────────────

int64_t os64_ui_textview_copy(const os64_ui_textview_t *tv)
{
	size_t sl, sc, el, ec;
	if (!tv_sel_range(tv, &sl, &sc, &el, &ec))
		return 0;

	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "w");
	if (h < 0)
		return h;

	// The newline between lines is MANUFACTURED here, and that is the honest
	// shape: the model holds lines, not terminators (scribe_buf.h), so the
	// separator is the copy's business — one between lines, none after the
	// last, which is what makes a within-one-line copy paste back inline.
	int64_t total = 0;
	bool ok = true;

	for (size_t li = sl; li <= el && ok; li++) {
		size_t len;
		const char *ln = tv_line(tv, li, &len);
		size_t from = (li == sl) ? sc : 0;
		size_t to   = (li == el) ? ec : len;

		if (from > len) from = len;
		if (to > len)   to = len;
		if (to > from)
			ok = clip_write_all((int32_t)h, ln + from, to - from, &total);
		if (ok && li < el)
			ok = clip_write_all((int32_t)h, "\n", 1, &total);
	}

	// Close is the SEAL — or, if a write was refused, the discard that leaves
	// the previous snarf standing. Either way it is the same call.
	os64_close((int32_t)h);
	return ok ? total : -1;
}

bool os64_ui_textview_cut(os64_ui_t *ui, os64_ui_textview_t *tv)
{
	if (!tv_editable(tv))
		return false;

	// Copy FIRST and believe its answer. A cut that deleted the text and then
	// discovered the clipboard wouldn't take it would be the only operation in
	// this editor that can lose work — so it doesn't exist.
	if (os64_ui_textview_copy(tv) <= 0)
		return false;

	if (!tv_delete_selection(tv))
		return false;
	tv_settle_caret(tv);
	if (tv->on_change)
		tv->on_change(tv, tv->view_user);
	tv_ensure_visible(ui, tv);
	return true;
}

bool os64_ui_textview_paste(os64_ui_t *ui, os64_ui_textview_t *tv)
{
	if (!tv_editable(tv))
		return false;

	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "r");
	if (h < 0)
		return false;

	// Ask the length before touching the document: an empty clipboard must
	// not eat a selection. (seek to the end IS the question — os64's seek
	// returns the new position.) The handle holds one entry for its whole
	// life, so this length and the bytes below cannot disagree.
	int64_t length = os64_seek((int32_t)h, 0, OS64_SEEK_END);
	if (length <= 0 || os64_seek((int32_t)h, 0, OS64_SEEK_SET) < 0) {
		os64_close((int32_t)h);
		return false;
	}

	bool changed = tv_delete_selection(tv);
	bool refused = false;   // the model said no: stop rather than paste around it
	char chunk[512];
	int64_t n;

	while (!refused && (n = os64_read((int32_t)h, chunk, sizeof(chunk))) > 0) {
		size_t i = 0;
		while (!refused && i < (size_t)n) {
			// ONE insert per RUN of ordinary bytes, never one per byte: the
			// line's tail moves once instead of once per character, which is
			// the difference between a paste and a coffee break.
			size_t run = i;
			while (run < (size_t)n && chunk[run] != '\n' && chunk[run] != '\r')
				run++;
			if (run > i) {
				if (!tv->buf->insert(tv->buf->user, tv->cur_line, tv->cur_col,
				                     chunk + i, run - i)) {
					refused = true;
					break;
				}
				tv->cur_col += run - i;
				changed = true;
			}
			i = run;

			// CR is dropped, alone or as half of a CRLF: a buffer holds lines,
			// and a carriage return inside one is a fossil of a file format,
			// not a character anybody meant to type. (A lone CR therefore
			// joins its neighbours — old-Mac line endings are not a format
			// os64 has ever claimed to read.)
			while (i < (size_t)n && chunk[i] == '\r')
				i++;
			if (i < (size_t)n && chunk[i] == '\n') {
				if (!tv->buf->split(tv->buf->user, tv->cur_line, tv->cur_col)) {
					refused = true;
					break;
				}
				tv->cur_line++;
				tv->cur_col = 0;
				changed = true;
				i++;
			}
		}
	}

	os64_close((int32_t)h);

	if (changed) {
		tv_settle_caret(tv);
		if (tv->on_change)
			tv->on_change(tv, tv->view_user);
	}
	tv_ensure_visible(ui, tv);
	return changed;
}
