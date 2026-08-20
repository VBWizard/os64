// ui.c — libui, the retained-lite widget toolkit (L2). See os64/ui.h for the
// model, the rulings it implements, and the vocabulary; this file is the
// mechanism. Born 2026-08-19, the night after VT8 took the console window
// away and left the desktop with nothing to click.

#include "os64/ui.h"
#include "os64/fmt.h"
#include "os64/io.h"

// ── tiny local string helpers ───────────────────────────────────────────────
// Self-contained on purpose (the klog_format precedent): three helpers do
// not justify a header dependency, and the theme parser wants exactly these
// shapes.

static size_t ui_strlen(const char *s)
{
	size_t n = 0;
	while (s && s[n])
		n++;
	return n;
}

static bool ui_span_equals(const char *span, size_t len, const char *lit)
{
	size_t n = 0;
	while (n < len && lit[n] != '\0' && span[n] == lit[n])
		n++;
	return n == len && lit[n] == '\0';
}

// ── the theme ───────────────────────────────────────────────────────────────

static void theme_defaults(os64_ui_theme_t *t)
{
	// The house look: the grays every window already wears, the focused-
	// titlebar blue for the one element that invites a press. EVERY value
	// here is overridable from /home/theme.conf — that is the entire point
	// of this table existing.
	t->panel_bg            = OS64_GUI_COLOR_LIGHT_GRAY;
	t->panel_border        = OS64_GUI_COLOR_DARK_GRAY;
	t->label_fg            = OS64_GUI_COLOR_BLACK;
	t->button_face         = 0xff2a62b8u;   // the titlebar-focused blue
	t->button_face_pressed = 0xff1c4380u;   // same hue, pressed down a stop
	t->button_border       = OS64_GUI_COLOR_DARK_GRAY;
	t->button_fg           = OS64_GUI_COLOR_WHITE;

	// The text family: gkeys' warm paper, ink on it, and the house blue
	// inverted for selection — the look every editor since Bravo settled on.
	t->text_bg             = 0xfff4f2eau;   // warm paper white
	t->text_fg             = OS64_GUI_COLOR_BLACK;
	t->text_sel_bg         = 0xff2a62b8u;   // the house blue
	t->text_sel_fg         = OS64_GUI_COLOR_WHITE;
	t->text_caret          = OS64_GUI_COLOR_BLACK;
	t->field_bg            = OS64_GUI_COLOR_WHITE;
	t->field_fg            = OS64_GUI_COLOR_BLACK;
	t->field_border        = OS64_GUI_COLOR_DARK_GRAY;
	t->field_border_focus  = 0xff2a62b8u;   // "your keys land here"
	t->scroll_track        = 0xffd8d6ceu;
	t->scroll_thumb        = 0xff8a8880u;

	t->pad      = 8;
	t->gap      = 8;
	t->button_h = 24;
	t->scroll_w = 14;

	t->font_w = 8;    // the embedded PSF1 face (font_psf1.h)
	t->font_h = 16;
}

// The key table: theme.conf names → theme fields. Adding a themable value =
// one struct field + one row here; scattering a constant anywhere else is
// the review offense the ui.h header warns about.
typedef enum { THEME_COLOR, THEME_METRIC } theme_kind_t;
typedef struct
{
	const char  *key;
	theme_kind_t kind;
	size_t       offset;
} theme_key_t;

#define THEME_ROW(name, kind, field) \
	{ name, kind, __builtin_offsetof(os64_ui_theme_t, field) }

static const theme_key_t kThemeKeys[] = {
	THEME_ROW("panel.bg",            THEME_COLOR,  panel_bg),
	THEME_ROW("panel.border",        THEME_COLOR,  panel_border),
	THEME_ROW("label.fg",            THEME_COLOR,  label_fg),
	THEME_ROW("button.face",         THEME_COLOR,  button_face),
	THEME_ROW("button.face.pressed", THEME_COLOR,  button_face_pressed),
	THEME_ROW("button.border",       THEME_COLOR,  button_border),
	THEME_ROW("button.fg",           THEME_COLOR,  button_fg),
	THEME_ROW("text.bg",             THEME_COLOR,  text_bg),
	THEME_ROW("text.fg",             THEME_COLOR,  text_fg),
	THEME_ROW("text.sel.bg",         THEME_COLOR,  text_sel_bg),
	THEME_ROW("text.sel.fg",         THEME_COLOR,  text_sel_fg),
	THEME_ROW("text.caret",          THEME_COLOR,  text_caret),
	THEME_ROW("field.bg",            THEME_COLOR,  field_bg),
	THEME_ROW("field.fg",            THEME_COLOR,  field_fg),
	THEME_ROW("field.border",        THEME_COLOR,  field_border),
	THEME_ROW("field.border.focus",  THEME_COLOR,  field_border_focus),
	THEME_ROW("scroll.track",        THEME_COLOR,  scroll_track),
	THEME_ROW("scroll.thumb",        THEME_COLOR,  scroll_thumb),
	THEME_ROW("pad",                 THEME_METRIC, pad),
	THEME_ROW("gap",                 THEME_METRIC, gap),
	THEME_ROW("button.h",            THEME_METRIC, button_h),
	THEME_ROW("scroll.w",            THEME_METRIC, scroll_w),
	THEME_ROW("font.w",              THEME_METRIC, font_w),
	THEME_ROW("font.h",              THEME_METRIC, font_h),
};
#define THEME_KEY_COUNT (sizeof(kThemeKeys) / sizeof(kThemeKeys[0]))

// Parse a 6-digit RGB hex span ("2a62b8") into opaque XRGB. Returns false on
// anything else — a color the user wrote wrong should be refused loudly, not
// half-parsed into a surprise.
static bool parse_color(const char *s, size_t len, uint32_t *out)
{
	if (len != 6)
		return false;
	uint32_t v = 0;
	for (size_t i = 0; i < 6; i++) {
		char c = s[i];
		uint32_t d;
		if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
		else return false;
		v = (v << 4) | d;
	}
	*out = 0xff000000u | v;   // the X byte stays opaque until alpha exists
	return true;
}

static bool parse_metric(const char *s, size_t len, int32_t *out)
{
	if (len == 0 || len > 5)
		return false;
	int32_t v = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (s[i] - '0');
	}
	*out = v;
	return true;
}

// /home/theme.conf — logd.conf's grammar verbatim: `key = value`, whitespace
// anywhere, '#' to end of line, last occurrence wins. Same reader shape too
// (whole file into a static buffer, complain if it overflows), because
// os64's config files should feel like one family.
#define THEME_CONF_PATH "/home/theme.conf"
#define THEME_CONF_MAX  8192

static void theme_load_conf(os64_ui_theme_t *t)
{
	int64_t fd = os64_open(THEME_CONF_PATH, "r");
	if (fd < 0)
		return;   // no file = the defaults, silently; absence is not an error

	static char buf[THEME_CONF_MAX];
	size_t got = 0;
	for (;;) {
		int64_t n = os64_read((int32_t)fd, buf + got, sizeof(buf) - 1 - got);
		if (n <= 0)
			break;
		got += (size_t)n;
		if (got >= sizeof(buf) - 1) {
			os64_printf("libui: %s larger than the reader's %u bytes — tail unread\n",
			            THEME_CONF_PATH, (unsigned)THEME_CONF_MAX);
			break;
		}
	}
	os64_close((int32_t)fd);
	if (got == 0)
		return;
	buf[got] = '\0';

	size_t i = 0;
	while (buf[i] != '\0') {
		size_t start = i;
		while (buf[i] != '\0' && buf[i] != '\n')
			i++;
		size_t end = i;
		if (buf[i] == '\n')
			i++;
		for (size_t c = start; c < end; c++)
			if (buf[c] == '#') { end = c; break; }

		// key
		size_t k = start;
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t ks = k;
		while (k < end && buf[k] != ' ' && buf[k] != '\t' && buf[k] != '=')
			k++;
		size_t ke = k;
		if (ks == ke)
			continue;   // blank or comment-only line

		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		if (k >= end || buf[k] != '=') {
			os64_printf("libui: %s: expected 'key = value' — line ignored\n",
			            THEME_CONF_PATH);
			continue;
		}
		k++;
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t vs = k, ve = end;
		while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t' || buf[ve - 1] == '\r'))
			ve--;

		const theme_key_t *row = (const theme_key_t *)0;
		for (size_t r = 0; r < THEME_KEY_COUNT; r++) {
			if (ui_span_equals(&buf[ks], ke - ks, kThemeKeys[r].key)) {
				row = &kThemeKeys[r];
				break;
			}
		}
		if (row == (const theme_key_t *)0) {
			buf[ke] = '\0';
			os64_printf("libui: %s: unknown setting, ignored: %s\n",
			            THEME_CONF_PATH, &buf[ks]);
			continue;
		}

		bool ok;
		if (row->kind == THEME_COLOR)
			ok = parse_color(&buf[vs], ve - vs,
			                 (uint32_t *)((char *)t + row->offset));
		else
			ok = parse_metric(&buf[vs], ve - vs,
			                  (int32_t *)((char *)t + row->offset));
		if (!ok) {
			buf[ke] = '\0';
			os64_printf("libui: %s: bad value for %s (colors: 6 hex digits; "
			            "metrics: decimal) — keeping previous\n",
			            THEME_CONF_PATH, &buf[ks]);
		}
	}
}

void os64_ui_theme_init(os64_ui_theme_t *t)
{
	theme_defaults(t);
	theme_load_conf(t);
}

// ── tree + dirty ────────────────────────────────────────────────────────────

void os64_ui_add_child(os64_ui_widget_t *parent, os64_ui_widget_t *child)
{
	child->parent = parent;
	child->next_sibling = (os64_ui_widget_t *)0;
	if (parent->first_child == (os64_ui_widget_t *)0) {
		parent->first_child = child;
		return;
	}
	os64_ui_widget_t *w = parent->first_child;
	while (w->next_sibling)
		w = w->next_sibling;
	w->next_sibling = child;
}

void os64_ui_mark_dirty(os64_ui_t *ui, os64_ui_widget_t *w)
{
	if (w == (os64_ui_widget_t *)0)
		return;
	ui->dirty = ui->any_dirty ? os64_rect_union(ui->dirty, w->bounds)
	                          : w->bounds;
	ui->any_dirty = true;
}

void os64_ui_init(os64_ui_t *ui, os64_draw_ctx_t *ctx)
{
	ui->ctx = ctx;
	os64_ui_theme_init(&ui->theme);
	ui->root = ui->grab = ui->focus = (os64_ui_widget_t *)0;
	ui->any_dirty = false;
	ui->dirty = (os64_gui_rect_t){0, 0, 0, 0};
	ui->on_resize = (void (*)(os64_ui_t *))0;
}

void os64_ui_set_root(os64_ui_t *ui, os64_ui_widget_t *root)
{
	ui->root = root;
	os64_ui_mark_dirty(ui, root);
}

void os64_ui_set_focus(os64_ui_t *ui, os64_ui_widget_t *w)
{
	if (ui->focus == w)
		return;
	// Both ends repaint: the old widget's caret has to DISAPPEAR, which no
	// amount of painting the new one can accomplish.
	if (ui->focus) {
		ui->focus->focused = false;
		os64_ui_mark_dirty(ui, ui->focus);
	}
	ui->focus = w;
	if (w) {
		w->focused = true;
		os64_ui_mark_dirty(ui, w);
	}
}

// ── hit testing ─────────────────────────────────────────────────────────────

static bool rect_contains(os64_gui_rect_t r, int32_t x, int32_t y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// Deepest visible widget under the point. Children are checked LAST-first so
// a hit resolves to whatever painted on top (list order is paint order).
static os64_ui_widget_t *hit_test(os64_ui_widget_t *w, int32_t x, int32_t y)
{
	if (w == (os64_ui_widget_t *)0 || w->hidden || !rect_contains(w->bounds, x, y))
		return (os64_ui_widget_t *)0;

	os64_ui_widget_t *best = (os64_ui_widget_t *)0;
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling) {
		os64_ui_widget_t *hit = hit_test(c, x, y);
		if (hit)
			best = hit;   // later sibling wins — it painted later
	}
	return best ? best : w;
}

// ── dispatch ────────────────────────────────────────────────────────────────

bool os64_ui_dispatch(os64_ui_t *ui, const os64_gui_event_t *ev)
{
	if (ui->root == (os64_ui_widget_t *)0)
		return false;

	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
		os64_ui_widget_t *w = hit_test(ui->root, ev->mouse.x, ev->mouse.y);
		if (w && w->cls->event && w->cls->event(w, ui, ev)) {
			ui->grab = w;
			return true;
		}
		return false;
	}
	case OS64_GUI_EVENT_MOUSE_MOVE:
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
		// A grab owns the pointer until release — the ancient button
		// contract (drag off to cancel) needs moves and the release even
		// when the cursor has left the widget's bounds.
		os64_ui_widget_t *w = ui->grab;
		if (w == (os64_ui_widget_t *)0)
			return false;
		bool consumed = w->cls->event ? w->cls->event(w, ui, ev) : false;
		if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP)
			ui->grab = (os64_ui_widget_t *)0;
		return consumed;
	}
	case OS64_GUI_EVENT_KEY_DOWN:
	case OS64_GUI_EVENT_KEY_UP: {
		os64_ui_widget_t *w = ui->focus;
		return (w && w->cls->event) ? w->cls->event(w, ui, ev) : false;
	}
	case OS64_GUI_EVENT_WINDOW_RESIZE: {
		// The window grew or shrank. libui does the three things every app
		// would otherwise have to remember, in the order they have to happen:
		//
		//   1. refresh the draw context, or every primitive keeps clipping to
		//      the old bounds and the new strip stays blank;
		//   2. stretch the root to the whole content area, because "the root
		//      IS the window" is the model every one of these trees assumes;
		//   3. hand the app its re-layout callback, since the layout call was
		//      the APP's (os64_ui_stack_vertical is a function you call, not a
		//      mode you set) and libui has no way to guess which one.
		//
		// Then mark the ENTIRE surface dirty rather than unioning bounds: on a
		// shrink the vacated area belongs to nobody's widget, and on a grow the
		// newly exposed strip holds the window's background — neither is
		// reachable from any widget rect, so a bounds-union repaint would
		// leave visible litter exactly where the eye is already looking.
		os64_draw_ctx_refresh(ui->ctx);
		// Rootless is legal (os64_ui_init leaves root NULL, and the sibling
		// cases above all tolerate it) — refresh the context and leave; there
		// is no tree to stretch and nothing of ours to repaint.
		if (!ui->root)
			return true;
		ui->root->bounds = (os64_gui_rect_t){0, 0,
		                                     (int32_t)ui->ctx->surf.width,
		                                     (int32_t)ui->ctx->surf.height};
		if (ui->on_resize)
			ui->on_resize(ui);
		ui->dirty = ui->root->bounds;
		ui->any_dirty = true;
		return true;
	}
	default:
		return false;
	}
}

// ── paint ───────────────────────────────────────────────────────────────────

static void paint_recursive(os64_ui_widget_t *w, os64_ui_t *ui)
{
	if (w == (os64_ui_widget_t *)0 || w->hidden)
		return;
	os64_gui_rect_t clip;
	if (!os64_rect_intersect(w->bounds, ui->dirty, &clip))
		return;
	// Parent before children (children overpaint), whole widget even when
	// only partly dirty: the canvas beyond the dirty rect just gets the same
	// bytes it already shows (repaint-from-state), and only `dirty` is
	// published. Widgets paint INSIDE their bounds by contract — the
	// primitives clip to the CANVAS, so a widget that strays stomps a
	// sibling, which is LIBDRAW.md's named fingerprint for this layer.
	if (w->cls->paint)
		w->cls->paint(w, ui->ctx, &ui->theme);
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		paint_recursive(c, ui);
}

void os64_ui_paint(os64_ui_t *ui)
{
	if (!ui->any_dirty || ui->root == (os64_ui_widget_t *)0)
		return;
	paint_recursive(ui->root, ui);
	os64_draw_publish(ui->ctx, &ui->dirty);
	ui->any_dirty = false;
	ui->dirty = (os64_gui_rect_t){0, 0, 0, 0};
}

// ── the packaged loop ───────────────────────────────────────────────────────

void os64_ui_run(os64_ui_t *ui, int64_t win, volatile bool *running)
{
	os64_ui_paint(ui);   // first frame: whatever set_root marked

	while (running == (volatile bool *)0 || *running) {
		os64_gui_event_t ev;
		int64_t rc = os64_gui_event_wait(win, &ev);
		if (rc != 1)
			break;   // window died or we are being terminated — leave
		os64_ui_dispatch(ui, &ev);
		// Drain whatever queued behind the one we slept on, so a burst
		// (typematic, mouse motion) becomes one repaint, not many.
		while (os64_gui_event_poll(win, &ev) == 1)
			os64_ui_dispatch(ui, &ev);
		os64_ui_paint(ui);
	}
}

// ── layout ──────────────────────────────────────────────────────────────────

void os64_ui_stack_vertical(os64_ui_t *ui, os64_ui_widget_t *parent)
{
	const os64_ui_theme_t *t = &ui->theme;
	int32_t x = parent->bounds.x + t->pad;
	int32_t y = parent->bounds.y + t->pad;
	int32_t w = parent->bounds.w - 2 * t->pad;

	for (os64_ui_widget_t *c = parent->first_child; c; c = c->next_sibling) {
		if (c->hidden)
			continue;
		int32_t h = c->bounds.h > 0 ? c->bounds.h : t->button_h;
		c->bounds = (os64_gui_rect_t){x, y, w, h};
		y += h + t->gap;
	}
	os64_ui_mark_dirty(ui, parent);
}

// ── the stock widgets ───────────────────────────────────────────────────────
// Logic in event, look in paint, look reads only the theme. Swap a paint
// pointer, keep the logic: that is the engine seam, working.

static void panel_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                        const os64_ui_theme_t *t)
{
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
	os64_draw_rect(&ctx->surf, w->bounds, t->panel_border);
}

static void label_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                        const os64_ui_theme_t *t)
{
	// Labels sit on their parent panel's face — repaint that patch first so
	// a text change never shows the old glyphs underneath.
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
	int32_t ty = w->bounds.y + (w->bounds.h - t->font_h) / 2;
	os64_draw_text(&ctx->surf, w->bounds.x, ty < w->bounds.y ? w->bounds.y : ty,
	               w->text ? w->text : "", ui_strlen(w->text),
	               t->label_fg, t->panel_bg);
}

static void button_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                         const os64_ui_theme_t *t)
{
	uint32_t face = w->pressed ? t->button_face_pressed : t->button_face;
	os64_draw_fill_rect(&ctx->surf, w->bounds, face);
	os64_draw_rect(&ctx->surf, w->bounds, t->button_border);
	size_t len = ui_strlen(w->text);
	int32_t tw = (int32_t)len * t->font_w;
	int32_t tx = w->bounds.x + (w->bounds.w - tw) / 2;
	int32_t ty = w->bounds.y + (w->bounds.h - t->font_h) / 2;
	if (tx < w->bounds.x + t->pad)
		tx = w->bounds.x + t->pad;
	if (ty < w->bounds.y)
		ty = w->bounds.y;
	os64_draw_text(&ctx->surf, tx, ty, w->text ? w->text : "", len,
	               t->button_fg, face);
}

static bool button_event(os64_ui_widget_t *w, os64_ui_t *ui,
                         const os64_gui_event_t *ev)
{
	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
		w->pressed = true;
		os64_ui_mark_dirty(ui, w);
		return true;
	case OS64_GUI_EVENT_MOUSE_MOVE: {
		// Held button, cursor wandering: pressed tracks whether release
		// here would still count. The visual IS the contract.
		bool inside = rect_contains(w->bounds, ev->mouse.x, ev->mouse.y);
		if (inside != w->pressed) {
			w->pressed = inside;
			os64_ui_mark_dirty(ui, w);
		}
		return true;
	}
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
		bool fire = w->pressed &&
		            rect_contains(w->bounds, ev->mouse.x, ev->mouse.y);
		w->pressed = false;
		os64_ui_mark_dirty(ui, w);
		if (fire && w->on_click)
			w->on_click(w, w->user);
		return true;
	}
	default:
		return false;
	}
}

const os64_ui_class_t os64_ui_panel_class  = { "panel",  panel_paint,  0 };
const os64_ui_class_t os64_ui_label_class  = { "label",  label_paint,  0 };
const os64_ui_class_t os64_ui_button_class = { "button", button_paint, button_event };

static void widget_zero(os64_ui_widget_t *w)
{
	*w = (os64_ui_widget_t){0};
}

void os64_ui_panel(os64_ui_widget_t *w)
{
	widget_zero(w);
	w->cls = &os64_ui_panel_class;
}

void os64_ui_label(os64_ui_widget_t *w, const char *text)
{
	widget_zero(w);
	w->cls = &os64_ui_label_class;
	w->text = text;
}

void os64_ui_button(os64_ui_widget_t *w, const char *text,
                    void (*on_click)(os64_ui_widget_t *, void *), void *user)
{
	widget_zero(w);
	w->cls = &os64_ui_button_class;
	w->text = text;
	w->on_click = on_click;
	w->user = user;
}
