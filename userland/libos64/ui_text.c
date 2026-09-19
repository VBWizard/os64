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
// paint after a font change allocates nothing. The run answers the caret,
// the click and the scroll; nothing here counts cells.
static void *field_run(os64_ui_textfield_t *tf, os64_ui_t *ui)
{
	if (os64_ui_run_matches(tf->w.run, tf->buf, tf->len))
		return tf->w.run;
	void *run = NULL;
	if (os64_ui_run_layout(ui, OS64_FONT_ROLE_UI, tf->buf, tf->len, &run) != OS64_FONT_OK)
		return NULL;
	if (run) {
		os64_ui_run_release(tf->w.run);
		tf->w.run = run;
	}
	return run;
}

static void field_keep_caret_visible(os64_ui_textfield_t *tf, os64_ui_t *ui)
{
	int32_t width = tf->w.bounds.w - 2 * field_inset(&ui->theme);
	int32_t cx = 0;
	void *run = field_run(tf, ui);
	if (!run || os64_ui_run_caret(run, tf->cursor, false, &cx) != OS64_FONT_OK)
		return;                        // keep the scroll rather than guess one
	if (cx < tf->left_px)
		tf->left_px = cx;
	if (cx > tf->left_px + width - 2)
		tf->left_px = cx - width + 2;
	if (tf->left_px < 0)
		tf->left_px = 0;
}

// One cluster either way, so Backspace over an accented letter removes the
// letter and not half of its encoding. Falls back to a byte only when the
// window wears no face, where every byte IS a cluster.
static size_t field_step(os64_ui_textfield_t *tf, os64_ui_t *ui, bool forward)
{
	void *run = field_run(tf, ui);
	size_t to = tf->cursor;
	if (run && os64_ui_run_step(run, tf->cursor, forward, &to) == OS64_FONT_OK)
		return to;
	if (forward)
		return tf->cursor < tf->len ? tf->cursor + 1 : tf->len;
	return tf->cursor > 0 ? tf->cursor - 1 : 0;
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
	os64_ui_draw_text(ui, &tf->w.run, OS64_FONT_ROLE_UI, &ctx->surf, inner,
	                  origin, ty, tf->buf, tf->len, t->field_fg, t->field_bg);

	int32_t cx = 0;
	void *run = w->focused ? field_run(tf, ui) : NULL;
	if (run && os64_ui_run_caret(run, tf->cursor, false, &cx) == OS64_FONT_OK) {
		os64_gui_rect_t caret = { origin + cx, ty, 2, row };
		os64_gui_rect_t clipped;
		if (os64_rect_intersect(caret, inner, &clipped))
			os64_draw_fill_rect(&ctx->surf, clipped, t->text_caret);
	}
}

static bool field_event(os64_ui_widget_t *w, os64_ui_t *ui,
                        const os64_gui_event_t *ev)
{
	os64_ui_textfield_t *tf = (os64_ui_textfield_t *)w;
	const os64_ui_theme_t *t = &ui->theme;

	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
		os64_ui_set_focus(ui, w);
		// The pixel of the TEXT under the pointer, which the run turns into
		// a legal boundary — never the middle of a letter.
		int32_t x = ev->mouse.x - w->bounds.x - field_inset(t) + tf->left_px;
		void *run = field_run(tf, ui);
		size_t at = 0;
		if (run && os64_ui_run_hit(run, x > 0 ? x : 0, &at) == OS64_FONT_OK)
			tf->cursor = at;
		os64_ui_mark_dirty(ui, w);
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_MOVE:
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
		return true;   // no drag-selection in a one-line field, v1

	case OS64_GUI_EVENT_KEY_DOWN: {
		char c = 0;
		switch (os64_ui_decode_key(&tf->seq, ev, &c)) {
		case K_CHAR:
			if (tf->len + 1 < tf->cap) {
				os64_memmove(tf->buf + tf->cursor + 1, tf->buf + tf->cursor,
				             tf->len - tf->cursor + 1);   // +1 rides the NUL
				tf->buf[tf->cursor++] = c;
				tf->len++;
			}
			break;
		case K_BACKSPACE:
			// A whole cluster: the span from the previous boundary to here.
			if (tf->cursor > 0) {
				size_t from = field_step(tf, ui, false);
				size_t gone = tf->cursor - from;
				os64_memmove(tf->buf + from, tf->buf + tf->cursor,
				             tf->len - tf->cursor + 1);   // +1 rides the NUL
				tf->cursor = from;
				tf->len -= gone;
			}
			break;
		case K_DELETE:
			if (tf->cursor < tf->len) {
				size_t to = field_step(tf, ui, true);
				size_t gone = to - tf->cursor;
				os64_memmove(tf->buf + tf->cursor, tf->buf + to,
				             tf->len - to + 1);
				tf->len -= gone;
			}
			break;
		case K_LEFT:  tf->cursor = field_step(tf, ui, false); break;
		case K_RIGHT: tf->cursor = field_step(tf, ui, true);  break;
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
      os64_ui_stage_caption, os64_ui_commit_caption, os64_ui_discard_caption,
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
	tf->len = os64_strcopy(tf->buf, tf->cap, text ? text : "");
	if (tf->len >= tf->cap)
		tf->len = tf->cap - 1;   // strcopy reports the untruncated length
	tf->cursor = tf->len;
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
// libui allocates nothing here, same as everywhere else in this library: the
// copy streams line by line into the open handle, and the paste streams the
// other way through one stack chunk. A 16MB snarf never exists twice.

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

size_t os64_ui_textfield_paste(os64_ui_t *ui, os64_ui_textfield_t *tf)
{
	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "r");
	if (h < 0)
		return 0;

	// One line's worth is all a one-line control can honestly hold, so read
	// only as much as could possibly fit and stop at the first newline.
	char chunk[256];
	size_t added = 0;

	for (;;) {
		size_t room = (tf->len + 1 < tf->cap) ? (tf->cap - 1 - tf->len) : 0;
		if (room == 0)
			break;
		size_t want = room < sizeof(chunk) ? room : sizeof(chunk);

		int64_t n = os64_read((int32_t)h, chunk, want);
		if (n <= 0)
			break;

		size_t run = 0;
		while (run < (size_t)n && chunk[run] != '\n' && chunk[run] != '\r')
			run++;

		if (run > 0) {
			// +1 rides the NUL, exactly as the K_CHAR path does.
			os64_memmove(tf->buf + tf->cursor + run, tf->buf + tf->cursor,
			             tf->len - tf->cursor + 1);
			for (size_t i = 0; i < run; i++)
				tf->buf[tf->cursor + i] = chunk[i];
			tf->cursor += run;
			tf->len += run;
			added += run;
		}
		if (run < (size_t)n)
			break;   // hit the line ending — a field takes line one, and says so
	}

	os64_close((int32_t)h);
	if (added > 0) {
		field_keep_caret_visible(tf, ui);
		os64_ui_mark_dirty(ui, &tf->w);
	}
	return added;
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

os64_font_status_t os64_ui_textview_line_width(os64_ui_t *ui,
                                               const char *s, size_t len,
                                               int64_t *out)
{
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

// The run for a line, in the slot that belongs to it, laid out only if what
// is there says something else. NULL means this view is wearing no face and
// the caller should fall back; a failure is reported as NULL too, and the
// binding remembers why.
static void *tv_run(os64_ui_t *ui, void **slot, const char *s, size_t len)
{
	if (!slot)
		return NULL;
	if (os64_ui_run_matches(*slot, s, len))
		return *slot;
	void *run = NULL;
	if (os64_ui_run_layout(ui, OS64_FONT_ROLE_DOCUMENT, s, len, &run) != OS64_FONT_OK)
		return NULL;
	if (run) {
		os64_ui_run_release(*slot);
		*slot = run;
	}
	return run;
}

// The caret's line, which motion needs whether or not it is on screen.
static void *tv_caret_run(os64_ui_textview_t *tv, os64_ui_t *ui)
{
	size_t len;
	const char *ln = tv_line(tv, tv->cur_line, &len);
	return tv_run(ui, &tv->w.run, ln, len);
}

static os64_font_status_t textview_prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	int32_t pitch = os64_ui_font_row_height(ui, OS64_FONT_ROLE_DOCUMENT);
	os64_gui_rect_t planned = os64_ui_widget_planned_bounds(w);
	int32_t fits = pitch > 0 ? (planned.h - 2 * VIEW_INSET) / pitch : 0;
	size_t rows = fits > 0 ? (size_t)fits : 1;
	int32_t width = planned.w - 2 * VIEW_INSET;
	size_t count = tv_count(tv);

	// The caret's line FIRST: where the caret is decides where the view has
	// to be, and the view decides which lines to prepare.
	size_t len;
	const char *ln = tv_line(tv, tv->cur_line, &len);
	void *caret = NULL;
	os64_font_status_t status =
		os64_ui_run_layout(ui, OS64_FONT_ROLE_DOCUMENT, ln, len, &caret);
	if (status != OS64_FONT_OK)
		return status;

	// THE VIEW THIS FACE WILL SHOW. The vertical half is pure arithmetic on
	// the candidate's pitch. The horizontal half starts from the old scroll
	// — a number in the retired face's pixels, kept if it happens to still
	// show the caret — and moves only as far as the caret requires. The
	// remembered Up/Down X is re-derived from the caret too, so the next
	// vertical run holds the lane the caret is actually in.
	size_t top = tv->top;
	if (tv->cur_line < top)
		top = tv->cur_line;
	if (tv->cur_line >= top + rows)
		top = tv->cur_line - rows + 1;
	int64_t left = tv->left_px, goal = tv->goal_x;
	int32_t cx = 0;
	if (caret && os64_ui_run_caret(caret, tv->cur_col, false, &cx) == OS64_FONT_OK) {
		if (cx < left)
			left = cx;
		if (cx > left + width - 2)
			left = cx - width + 2;
		if (left < 0)
			left = 0;
		goal = cx;
	}

	size_t visible = rows;
	if (top < count && visible > count - top)
		visible = count - top;
	else if (top >= count)
		visible = 0;

	tv_free_runs(&tv->row_runs_staged, &tv->row_runs_staged_count);
	if (visible) {
		void **runs = os64_malloc(visible * sizeof(*runs));
		if (!runs) {
			os64_ui_run_release(caret);
			return OS64_FONT_NO_MEMORY;
		}
		for (size_t i = 0; i < visible; ++i)
			runs[i] = NULL;
		for (size_t i = 0; i < visible; ++i) {
			const char *row = tv_line(tv, top + i, &len);
			status = os64_ui_run_layout(ui, OS64_FONT_ROLE_DOCUMENT, row, len, &runs[i]);
			if (status != OS64_FONT_OK) {
				for (size_t j = 0; j < i; ++j)
					os64_ui_run_release(runs[j]);
				os64_free(runs);
				os64_ui_run_release(caret);
				return status;
			}
		}
		tv->row_runs_staged = runs;
		tv->row_runs_staged_count = visible;
	}

	os64_ui_run_release(w->run_staged);
	w->run_staged = caret;
	tv->top_staged = top;
	tv->left_staged = left;
	tv->goal_staged = goal;
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
	size_t old_top = tv->top;
	int64_t old_left = tv->left_px;

	if (tv->cur_line < tv->top)
		tv->top = tv->cur_line;
	if (tv->cur_line >= tv->top + (size_t)rows)
		tv->top = tv->cur_line - (size_t)rows + 1;

	// A caret that cannot be located leaves the horizontal scroll alone
	// rather than jumping somewhere arbitrary; the vertical half above is
	// pure arithmetic and always right.
	int32_t cx = 0;
	void *run = tv_caret_run(tv, ui);
	if (run && os64_ui_run_caret(run, tv->cur_col, false, &cx) == OS64_FONT_OK) {
		// A couple of pixels of air, so the caret is not flush against the
		// edge it just scrolled to.
		if (cx < tv->left_px)
			tv->left_px = cx;
		if (cx > tv->left_px + width - 2)
			tv->left_px = cx - width + 2;
		if (tv->left_px < 0)
			tv->left_px = 0;
	}

	if (tv->top != old_top || tv->left_px != old_left)
		tv_fire_view(tv);
	os64_ui_mark_dirty(ui, &tv->w);
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

	for (int32_t r = 0; r < rows; r++) {
		size_t li = tv->top + (size_t)r;
		if (li >= count)
			break;
		size_t len;
		const char *ln = tv_line(tv, li, &len);
		int32_t y = area.y + r * pitch;
		os64_gui_rect_t row = { area.x, y, area.w, pitch };
		os64_gui_rect_t clip;
		if (!os64_rect_intersect(row, area, &clip))
			continue;

		void **slot = (size_t)r < tv->row_run_count ? &tv->row_runs[r] : NULL;

		// ONE RUN ANSWERS ALL THREE QUESTIONS. The glyphs, the highlight
		// and the caret are the same measurement of the same bytes, so
		// they cannot describe different text — which is what happens the
		// moment a selection is computed one way and painted another.
		void *run = tv_run(ui, slot ? slot : &tv->w.run, ln, len);

		// Selected bytes on this line, as a byte range of it.
		size_t from = 0, to = 0;
		bool lit = false;
		if (have_sel && li >= sl && li <= el) {
			from = li == sl ? sc : 0;
			to = li == el ? ec : len;
			lit = to > from;
		}

		// TWO PASSES OVER ONE RUN. The whole line in its ordinary colours,
		// then the line again in the selection's colours CLIPPED TO THE
		// SELECTED SPAN — the draw fills its own box with the background it
		// is given, so the second pass lays down the highlight and the lit
		// glyphs together, exactly as wide as the selection and no wider.
		// Same run both times, so the second pass costs no layout and its
		// glyphs land on the first pass's pixels.
		os64_ui_draw_text(ui, slot, OS64_FONT_ROLE_DOCUMENT, &ctx->surf, clip,
		                  origin, y, ln, len, t->text_fg, t->text_bg);
		if (run && lit) {
			os64_gui_rect_t sel;
			if (os64_ui_run_selection(run, from, to, &sel) == OS64_FONT_OK) {
				// The run reports X relative to the line; the ROW is what
				// this view lays out on (FONT_PROVIDER.md: pitch comes from
				// the primary face and does not grow for a fallback).
				os64_gui_rect_t lit_rect = { origin + sel.x, y, sel.w, pitch };
				os64_gui_rect_t sel_clip;
				if (os64_rect_intersect(lit_rect, clip, &sel_clip))
					os64_ui_draw_text(ui, slot, OS64_FONT_ROLE_DOCUMENT, &ctx->surf,
					                  sel_clip, origin, y, ln, len,
					                  t->text_sel_fg, t->text_sel_bg);
			}
		}

		if (w->focused && li == tv->cur_line && run) {
			int32_t cx = 0;
			if (os64_ui_run_caret(run, tv->cur_col, false, &cx) == OS64_FONT_OK) {
				os64_gui_rect_t caret = { origin + cx, y, 2, pitch };
				os64_gui_rect_t painted;
				if (os64_rect_intersect(caret, clip, &painted))
					os64_draw_fill_rect(&ctx->surf, painted, t->text_caret);
			}
		}
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

// Remember where the caret is, in pixels, for a later Up or Down.
static void tv_remember_goal(os64_ui_textview_t *tv, os64_ui_t *ui)
{
	void *run = tv_caret_run(tv, ui);
	int32_t cx = 0;
	if (run && os64_ui_run_caret(run, tv->cur_col, false, &cx) == OS64_FONT_OK)
		tv->goal_x = cx;
}

// Put the caret at the remembered X on whatever line it is now on.
static void tv_seek_goal(os64_ui_textview_t *tv, os64_ui_t *ui)
{
	void *run = tv_caret_run(tv, ui);
	size_t offset = 0;
	if (run && os64_ui_run_hit(run, (int32_t)tv->goal_x, &offset) == OS64_FONT_OK) {
		tv->cur_col = offset;
		return;
	}
	size_t len;
	tv_line(tv, tv->cur_line, &len);
	if (tv->cur_col > len)
		tv->cur_col = len;
}

// One cluster forward or back, staying on legal boundaries.
static void tv_step_caret(os64_ui_textview_t *tv, os64_ui_t *ui, bool forward)
{
	void *run = tv_caret_run(tv, ui);
	size_t offset = tv->cur_col;
	if (run && os64_ui_run_step(run, tv->cur_col, forward, &offset) == OS64_FONT_OK) {
		tv->cur_col = offset;
		return;
	}
	size_t len;
	tv_line(tv, tv->cur_line, &len);
	if (forward && tv->cur_col < len)
		tv->cur_col++;
	else if (!forward && tv->cur_col > 0)
		tv->cur_col--;
}

static void tv_place_from_point(os64_ui_textview_t *tv, os64_ui_t *ui,
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

	tv->cur_line = (size_t)li;
	void *run = tv_caret_run(tv, ui);
	size_t offset = tv->cur_col;
	if (run && os64_ui_run_hit(run, (int32_t)x, &offset) == OS64_FONT_OK) {
		tv->cur_col = offset;
		int32_t cx = 0;
		if (os64_ui_run_caret(run, tv->cur_col, false, &cx) == OS64_FONT_OK)
			tv->goal_x = cx;
	} else {
		// No face, or a line that could not be laid out: put the caret at
		// the start rather than somewhere invented.
		size_t len;
		tv_line(tv, tv->cur_line, &len);
		tv->cur_col = 0;
		tv->goal_x = 0;
	}
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
		tv_place_from_point(tv, ui, ev->mouse.x, ev->mouse.y);
		tv->sel = false;
		tv->sel_line = tv->cur_line;
		tv->sel_col = tv->cur_col;
		os64_ui_mark_dirty(ui, w);
		return true;

	case OS64_GUI_EVENT_MOUSE_MOVE: {
		// We only see moves while grabbed (dispatch's rule) — a drag.
		size_t ol = tv->cur_line, oc = tv->cur_col;
		tv_place_from_point(tv, ui, ev->mouse.x, ev->mouse.y);
		if (tv->cur_line != ol || tv->cur_col != oc) {
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
				tv_delete_selection(tv);
			tv->buf->insert(tv->buf->user, tv->cur_line, tv->cur_col, &c, 1);
			tv->cur_col++;
			changed = true;
			break;
		}
		case K_ENTER:
			if (!can_edit)
				return true;
			if (tv->sel)
				tv_delete_selection(tv);
			tv->buf->split(tv->buf->user, tv->cur_line, tv->cur_col);
			tv->cur_line++;
			tv->cur_col = 0;
			changed = true;
			break;
		case K_BACKSPACE:
			if (!can_edit)
				return true;
			if (tv->sel) {
				changed = tv_delete_selection(tv);
			} else if (tv->cur_col > 0) {
				tv->buf->erase(tv->buf->user, tv->cur_line, tv->cur_col - 1, 1);
				tv->cur_col--;
				changed = true;
			} else if (tv->cur_line > 0) {
				tv_line(tv, tv->cur_line - 1, &len);
				tv->buf->join(tv->buf->user, tv->cur_line - 1);
				tv->cur_line--;
				tv->cur_col = len;
				changed = true;
			}
			break;
		case K_DELETE:
			if (!can_edit)
				return true;
			if (tv->sel) {
				changed = tv_delete_selection(tv);
			} else {
				ln = tv_line(tv, tv->cur_line, &len);
				(void)ln;
				if (tv->cur_col < len) {
					tv->buf->erase(tv->buf->user, tv->cur_line, tv->cur_col, 1);
					changed = true;
				} else if (tv->cur_line + 1 < count) {
					tv->buf->join(tv->buf->user, tv->cur_line);
					changed = true;
				}
			}
			break;

		case K_LEFT:
			tv_motion_prologue(tv, shift);
			// Stepping is by CLUSTER BOUNDARY, not by byte. F2 publishes
			// a caret for each legal boundary and snaps anything between
			// them, so over a composed accent the caret moves past the
			// whole letter rather than into the middle of it.
			if (tv->cur_col > 0) {
				tv_step_caret(tv, ui, false);
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
				tv_step_caret(tv, ui, true);
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
		case K_DOWN:
			// The remembered goal X: runs of Up/Down hold their lane
			// through short lines — every editor since vi has kept this
			// promise, and its absence is instantly felt. It is a PIXEL
			// now, because under a proportional face the column the caret
			// "was in" is not a distance anything else agrees on.
			tv_motion_prologue(tv, shift);
			if (k == K_UP && tv->cur_line > 0)
				tv->cur_line--;
			else if (k == K_DOWN && tv->cur_line + 1 < count)
				tv->cur_line++;
			tv_seek_goal(tv, ui);
			break;
		case K_PGUP:
		case K_PGDN: {
			int32_t rows = os64_ui_textview_rows(tv, t);
			bool up = (k == K_PGUP);
			tv_motion_prologue(tv, shift);
			if (up) {
				tv->cur_line = tv->cur_line > (size_t)rows
				                   ? tv->cur_line - (size_t)rows : 0;
				tv->top = tv->top > (size_t)rows ? tv->top - (size_t)rows : 0;
			} else {
				tv->cur_line += (size_t)rows;
				if (tv->cur_line >= count)
					tv->cur_line = count - 1;
				tv->top += (size_t)rows;
				if (tv->top + (size_t)rows > count)
					tv->top = count > (size_t)rows ? count - (size_t)rows : 0;
			}
			tv_seek_goal(tv, ui);
			tv_fire_view(tv);
			break;
		}
		case K_ESC:
			tv->sel = false;
			break;
		default:
			return true;   // mid-burst / strangers: consumed silently
		}

		if (changed && tv->on_change)
			tv->on_change(tv, tv->view_user);
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
	tv_line(tv, line, &len);
	if (col > len)
		col = len;

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
	char chunk[512];
	int64_t n;

	while ((n = os64_read((int32_t)h, chunk, sizeof(chunk))) > 0) {
		size_t i = 0;
		while (i < (size_t)n) {
			// ONE insert per RUN of ordinary bytes, never one per byte: the
			// line's tail moves once instead of once per character, which is
			// the difference between a paste and a coffee break.
			size_t run = i;
			while (run < (size_t)n && chunk[run] != '\n' && chunk[run] != '\r')
				run++;
			if (run > i) {
				tv->buf->insert(tv->buf->user, tv->cur_line, tv->cur_col,
				                chunk + i, run - i);
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
				tv->buf->split(tv->buf->user, tv->cur_line, tv->cur_col);
				tv->cur_line++;
				tv->cur_col = 0;
				changed = true;
				i++;
			}
		}
	}

	os64_close((int32_t)h);

	if (changed && tv->on_change)
		tv->on_change(tv, tv->view_user);
	tv_ensure_visible(ui, tv);
	return changed;
}
