// ui_text.c — the text family: scrollbar, textfield, textview. libui's
// second wave, pulled into being by scribe(1) exactly as the app-driven rule
// prescribes (ui.h: "the first app that wants a textbox is what brings the
// textbox into being"). Same doctrine as ui.c throughout: logic in event,
// look in paint, look reads only the theme, the app owns every struct.
//
// THE CONTAINER PATTERN (ui.h documents it): these widgets need state the
// base struct doesn't carry, so each embeds os64_ui_widget_t as its FIRST
// member and casts back. libui threads the base; the app owns the whole.

#include "os64/ui.h"
#include "os64/str.h"

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
// carries the extended key's code, the Esc key carries 0x01. That one fact
// is what lets a textfield have both an escape-sequence parser and a
// working Cancel key.
//
// Parser state lives in the WIDGET (one byte), not in a static — two
// focused widgets never decode concurrently, but statics in a library are
// how that assumption becomes a bug later (the SMP lesson, applied at ring 3).

typedef enum
{
	K_NONE,        // consumed mid-burst, or ignorable
	K_CHAR,        // printable; the byte is in *ch
	K_ENTER, K_BACKSPACE, K_TAB, K_ESC,
	K_UP, K_DOWN, K_LEFT, K_RIGHT,
	K_HOME, K_END, K_PGUP, K_PGDN, K_DELETE,
} ui_key_t;

#define SEQ_IDLE   0
#define SEQ_ESC    1     // saw the burst's ESC
#define SEQ_CSI    2     // saw ESC [
#define SEQ_DIGIT  10    // 10 + d after ESC [ <d>

#define SC_ESC_KEY 0x01  // the Esc KEY's scancode — the burst never uses it

static ui_key_t decode_key(uint8_t *seq, const os64_gui_event_t *ev, char *ch)
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
		if (ev->key.scancode == SC_ESC_KEY)
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

// ── tab-aware column geometry ───────────────────────────────────────────────
// The buffer stores BYTES; the screen shows VISUAL columns, and the two
// diverge at every '\t' (next multiple-of-8 stop — the teletype's number,
// and /proc's files are tab-separated, which is why a log-reading editor
// cannot fake this with one glyph).

#define TAB_STOP 8

static int64_t vcol_step(int64_t v, char c)
{
	return (c == '\t') ? ((v / TAB_STOP) + 1) * TAB_STOP : v + 1;
}

static int64_t vcol_of(const char *s, size_t len, size_t col)
{
	int64_t v = 0;
	if (col > len)
		col = len;
	for (size_t i = 0; i < col; i++)
		v = vcol_step(v, s[i]);
	return v;
}

// The byte whose cell covers `vcol` (or the line end if vcol is past it).
static size_t byte_of(const char *s, size_t len, int64_t vcol)
{
	int64_t v = 0;
	for (size_t i = 0; i < len; i++) {
		int64_t next = vcol_step(v, s[i]);
		if (vcol < next)
			return i;
		v = next;
	}
	return len;
}

// ── ui_scrollbar ────────────────────────────────────────────────────────────

// Thumb geometry, shared by paint and event so the pixels you grab are the
// pixels that were drawn (the wm_clamp_frame lesson, one layer up).
static void scrollbar_thumb(const os64_ui_scrollbar_t *sb,
                            int32_t *ty, int32_t *th)
{
	int32_t h = sb->w.bounds.h;
	if (sb->total <= 0 || sb->visible <= 0 || sb->visible >= sb->total) {
		*ty = sb->w.bounds.y;
		*th = h;
		return;
	}
	int64_t t = (int64_t)h * sb->visible / sb->total;
	if (t < 16) t = 16;          // a thumb you can't hit isn't a thumb
	if (t > h)  t = h;
	int64_t range = sb->total - sb->visible;
	*th = (int32_t)t;
	*ty = sb->w.bounds.y + (int32_t)((int64_t)(h - *th) * sb->pos / range);
}

static void scrollbar_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                            const os64_ui_theme_t *t)
{
	os64_ui_scrollbar_t *sb = (os64_ui_scrollbar_t *)w;
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->scroll_track);
	int32_t ty, th;
	scrollbar_thumb(sb, &ty, &th);
	os64_gui_rect_t thumb = { w->bounds.x + 2, ty, w->bounds.w - 4, th };
	os64_draw_fill_rect(&ctx->surf, thumb, t->scroll_thumb);
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

	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
		if (sb->total <= sb->visible)
			return true;   // full thumb: nothing to move, but the click is ours
		int32_t ty, th;
		scrollbar_thumb(sb, &ty, &th);
		if (ev->mouse.y >= ty && ev->mouse.y < ty + th) {
			sb->drag_grab = ev->mouse.y - ty;      // ride the thumb
		} else {
			// Track click: a page jump toward the click — every scrollbar
			// since the Star has meant this.
			scrollbar_moved(sb, ui, ev->mouse.y < ty ? sb->pos - sb->visible
			                                         : sb->pos + sb->visible);
		}
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_MOVE: {
		if (sb->drag_grab < 0)
			return true;
		int32_t ty, th;
		scrollbar_thumb(sb, &ty, &th);
		int32_t span = w->bounds.h - th;
		if (span <= 0)
			return true;
		int64_t range = sb->total - sb->visible;
		int64_t top_px = ev->mouse.y - sb->drag_grab - w->bounds.y;
		scrollbar_moved(sb, ui, top_px * range / span);
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
		sb->drag_grab = -1;
		return true;
	default:
		return false;
	}
}

const os64_ui_class_t os64_ui_scrollbar_class =
	{ "scrollbar", scrollbar_paint, scrollbar_event };

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

#define FIELD_INSET 4   // px between border and glyphs

static int32_t field_cols(const os64_ui_textfield_t *tf, const os64_ui_theme_t *t)
{
	int32_t c = (tf->w.bounds.w - 2 * FIELD_INSET) / t->font_w;
	return c > 0 ? c : 1;
}

static void field_keep_caret_visible(os64_ui_textfield_t *tf,
                                     const os64_ui_theme_t *t)
{
	size_t cols = (size_t)field_cols(tf, t);
	if (tf->cursor < tf->first)
		tf->first = tf->cursor;
	if (tf->cursor >= tf->first + cols)
		tf->first = tf->cursor - cols + 1;
}

static void field_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                        const os64_ui_theme_t *t)
{
	os64_ui_textfield_t *tf = (os64_ui_textfield_t *)w;
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->field_bg);
	os64_draw_rect(&ctx->surf, w->bounds,
	               w->focused ? t->field_border_focus : t->field_border);

	int32_t cols = field_cols(tf, t);
	size_t n = tf->len > tf->first ? tf->len - tf->first : 0;
	if (n > (size_t)cols)
		n = (size_t)cols;
	int32_t ty = w->bounds.y + (w->bounds.h - t->font_h) / 2;
	os64_draw_text(&ctx->surf, w->bounds.x + FIELD_INSET, ty,
	               tf->buf + tf->first, n, t->field_fg, t->field_bg);

	if (w->focused && tf->cursor >= tf->first &&
	    tf->cursor <= tf->first + (size_t)cols) {
		int32_t cx = w->bounds.x + FIELD_INSET +
		             (int32_t)(tf->cursor - tf->first) * t->font_w;
		os64_gui_rect_t caret = { cx, ty, 2, t->font_h };
		os64_draw_fill_rect(&ctx->surf, caret, t->text_caret);
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
		int32_t cell = (ev->mouse.x - w->bounds.x - FIELD_INSET) / t->font_w;
		size_t cur = tf->first + (size_t)(cell > 0 ? cell : 0);
		tf->cursor = cur > tf->len ? tf->len : cur;
		os64_ui_mark_dirty(ui, w);
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_MOVE:
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
		return true;   // no drag-selection in a one-line field, v1

	case OS64_GUI_EVENT_KEY_DOWN: {
		char c = 0;
		switch (decode_key(&tf->seq, ev, &c)) {
		case K_CHAR:
			if (tf->len + 1 < tf->cap) {
				os64_memmove(tf->buf + tf->cursor + 1, tf->buf + tf->cursor,
				             tf->len - tf->cursor + 1);   // +1 rides the NUL
				tf->buf[tf->cursor++] = c;
				tf->len++;
			}
			break;
		case K_BACKSPACE:
			if (tf->cursor > 0) {
				os64_memmove(tf->buf + tf->cursor - 1, tf->buf + tf->cursor,
				             tf->len - tf->cursor + 1);
				tf->cursor--;
				tf->len--;
			}
			break;
		case K_DELETE:
			if (tf->cursor < tf->len) {
				os64_memmove(tf->buf + tf->cursor, tf->buf + tf->cursor + 1,
				             tf->len - tf->cursor);
				tf->len--;
			}
			break;
		case K_LEFT:  if (tf->cursor > 0)       tf->cursor--; break;
		case K_RIGHT: if (tf->cursor < tf->len) tf->cursor++; break;
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
		field_keep_caret_visible(tf, t);
		os64_ui_mark_dirty(ui, w);
		return true;
	}
	case OS64_GUI_EVENT_KEY_UP:
		return true;
	default:
		return false;
	}
}

const os64_ui_class_t os64_ui_textfield_class =
	{ "textfield", field_paint, field_event };

void os64_ui_textfield(os64_ui_textfield_t *tf, char *buf, size_t cap,
                       void (*on_submit)(os64_ui_textfield_t *, void *),
                       void (*on_cancel)(os64_ui_textfield_t *, void *),
                       void *user)
{
	*tf = (os64_ui_textfield_t){0};
	tf->w.cls = &os64_ui_textfield_class;
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
	tf->first = 0;
	os64_ui_mark_dirty(ui, &tf->w);
}

// ── ui_textview ─────────────────────────────────────────────────────────────

#define VIEW_INSET 2

int32_t os64_ui_textview_rows(const os64_ui_textview_t *tv,
                              const os64_ui_theme_t *t)
{
	int32_t r = (tv->w.bounds.h - 2 * VIEW_INSET) / t->font_h;
	return r > 0 ? r : 1;
}

int32_t os64_ui_textview_cols(const os64_ui_textview_t *tv,
                              const os64_ui_theme_t *t)
{
	int32_t c = (tv->w.bounds.w - 2 * VIEW_INSET) / t->font_w;
	return c > 0 ? c : 1;
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

static bool tv_byte_selected(size_t li, size_t b,
                             size_t sl, size_t sc, size_t el, size_t ec)
{
	if (li < sl || li > el) return false;
	if (li == sl && b < sc) return false;
	if (li == el && b >= ec) return false;
	return true;
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
	int32_t cols = os64_ui_textview_cols(tv, t);
	size_t old_top = tv->top;
	int64_t old_left = tv->left;

	if (tv->cur_line < tv->top)
		tv->top = tv->cur_line;
	if (tv->cur_line >= tv->top + (size_t)rows)
		tv->top = tv->cur_line - (size_t)rows + 1;

	size_t len;
	const char *ln = tv_line(tv, tv->cur_line, &len);
	int64_t cv = vcol_of(ln, len, tv->cur_col);
	if (cv < tv->left)
		tv->left = cv;
	if (cv >= tv->left + cols)
		tv->left = cv - cols + 1;
	if (tv->left < 0)
		tv->left = 0;

	if (tv->top != old_top || tv->left != old_left)
		tv_fire_view(tv);
	os64_ui_mark_dirty(ui, &tv->w);
}

static void textview_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                           const os64_ui_theme_t *t)
{
	os64_ui_textview_t *tv = (os64_ui_textview_t *)w;
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->text_bg);

	int32_t rows = os64_ui_textview_rows(tv, t);
	int32_t cols = os64_ui_textview_cols(tv, t);
	size_t count = tv_count(tv);
	size_t sl = 0, sc = 0, el = 0, ec = 0;
	bool have_sel = tv_sel_range(tv, &sl, &sc, &el, &ec);

	for (int32_t r = 0; r < rows; r++) {
		size_t li = tv->top + (size_t)r;
		if (li >= count)
			break;
		size_t len;
		const char *ln = tv_line(tv, li, &len);
		int32_t y = w->bounds.y + VIEW_INSET + r * t->font_h;

		int64_t v = 0;
		for (size_t b = 0; b < len && v < tv->left + cols; b++) {
			char c = ln[b];
			int64_t vnext = vcol_step(v, c);
			bool selq = have_sel && tv_byte_selected(li, b, sl, sc, el, ec);
			uint32_t fg = selq ? t->text_sel_fg : t->text_fg;
			uint32_t bg = selq ? t->text_sel_bg : t->text_bg;

			if (c == '\t') {
				// A tab is background all the way to its stop — lit
				// background when selected, so a selection over a tab is
				// visibly a selection over a tab.
				int64_t from = v < tv->left ? tv->left : v;
				int64_t to = vnext > tv->left + cols ? tv->left + cols : vnext;
				if (selq && to > from) {
					os64_gui_rect_t span = {
						w->bounds.x + VIEW_INSET + (int32_t)(from - tv->left) * t->font_w,
						y, (int32_t)(to - from) * t->font_w, t->font_h };
					os64_draw_fill_rect(&ctx->surf, span, bg);
				}
			} else if (v >= tv->left) {
				int32_t x = w->bounds.x + VIEW_INSET +
				            (int32_t)(v - tv->left) * t->font_w;
				os64_draw_text(&ctx->surf, x, y, &c, 1, fg, bg);
			}
			v = vnext;
		}

		if (w->focused && li == tv->cur_line) {
			int64_t cv = vcol_of(ln, len, tv->cur_col);
			if (cv >= tv->left && cv <= tv->left + cols) {
				int32_t x = w->bounds.x + VIEW_INSET +
				            (int32_t)(cv - tv->left) * t->font_w;
				os64_gui_rect_t caret = { x, y, 2, t->font_h };
				os64_draw_fill_rect(&ctx->surf, caret, t->text_caret);
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

static void tv_place_from_point(os64_ui_textview_t *tv, os64_ui_t *ui,
                                int32_t mx, int32_t my)
{
	const os64_ui_theme_t *t = &ui->theme;
	size_t count = tv_count(tv);

	int32_t row = (my - tv->w.bounds.y - VIEW_INSET) / t->font_h;
	int64_t li = (int64_t)tv->top + row;      // row may be negative: a drag
	if (li < 0)                                // above the view auto-scrolls up
		li = 0;
	if (li >= (int64_t)count)
		li = (int64_t)count - 1;

	int32_t cell = (mx - tv->w.bounds.x - VIEW_INSET) / t->font_w;
	int64_t vcol = tv->left + (cell > 0 ? cell : 0);

	size_t len;
	const char *ln = tv_line(tv, (size_t)li, &len);
	tv->cur_line = (size_t)li;
	tv->cur_col = byte_of(ln, len, vcol);
	tv->goal_vcol = vcol_of(ln, len, tv->cur_col);
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

		ui_key_t k = decode_key(&tv->seq, ev, &c);
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
			if (tv->cur_col > 0) {
				tv->cur_col--;
			} else if (tv->cur_line > 0) {
				tv->cur_line--;
				tv_line(tv, tv->cur_line, &len);
				tv->cur_col = len;
			}
			ln = tv_line(tv, tv->cur_line, &len);
			tv->goal_vcol = vcol_of(ln, len, tv->cur_col);
			break;
		case K_RIGHT:
			tv_motion_prologue(tv, shift);
			ln = tv_line(tv, tv->cur_line, &len);
			if (tv->cur_col < len) {
				tv->cur_col++;
			} else if (tv->cur_line + 1 < count) {
				tv->cur_line++;
				tv->cur_col = 0;
			}
			ln = tv_line(tv, tv->cur_line, &len);
			tv->goal_vcol = vcol_of(ln, len, tv->cur_col);
			break;
		case K_HOME:
			tv_motion_prologue(tv, shift);
			tv->cur_col = 0;
			tv->goal_vcol = 0;
			break;
		case K_END:
			tv_motion_prologue(tv, shift);
			ln = tv_line(tv, tv->cur_line, &len);
			tv->cur_col = len;
			tv->goal_vcol = vcol_of(ln, len, len);
			break;
		case K_UP:
		case K_DOWN:
			// The remembered goal column: runs of Up/Down hold their lane
			// through short lines — every editor since vi has kept this
			// promise, and its absence is instantly felt.
			tv_motion_prologue(tv, shift);
			if (k == K_UP && tv->cur_line > 0)
				tv->cur_line--;
			else if (k == K_DOWN && tv->cur_line + 1 < count)
				tv->cur_line++;
			ln = tv_line(tv, tv->cur_line, &len);
			tv->cur_col = byte_of(ln, len, tv->goal_vcol);
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
			ln = tv_line(tv, tv->cur_line, &len);
			tv->cur_col = byte_of(ln, len, tv->goal_vcol);
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

const os64_ui_class_t os64_ui_textview_class =
	{ "textview", textview_paint, textview_event };

void os64_ui_textview(os64_ui_textview_t *tv, const os64_ui_textbuf_t *buf,
                      void (*on_change)(os64_ui_textview_t *, void *),
                      void (*on_view)(os64_ui_textview_t *, void *),
                      void *user)
{
	*tv = (os64_ui_textview_t){0};
	tv->w.cls = &os64_ui_textview_class;
	tv->buf = buf;
	tv->on_change = on_change;
	tv->on_view = on_view;
	tv->view_user = user;
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
	tv->goal_vcol = vcol_of(ln, len, col);
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
