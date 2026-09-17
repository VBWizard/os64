// ui.c — libui, the retained-lite widget toolkit (L2). See os64/ui.h for the
// model, the rulings it implements, and the vocabulary; this file is the
// mechanism. Born 2026-08-19, the night after VT8 took the console window
// away and left the desktop with nothing to click.

#include "os64/ui.h"
#include "os64/fmt.h"
#include "os64/io.h"

static size_t ui_strlen(const char *s)
{
	size_t n = 0;
	while (s && s[n])
		n++;
	return n;
}

// ── tree + dirty ────────────────────────────────────────────────────────────

// Thread a subtree onto its UI, so every widget in it can reach the window's
// font binding — layout runs before the first paint and already measures.
static void adopt_tree(os64_ui_widget_t *w, os64_ui_t *ui)
{
	if (w == (os64_ui_widget_t *)0)
		return;
	w->ui = ui;
	if (w->cls && w->cls->metrics)
		w->cls->metrics(w, ui);
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		adopt_tree(c, ui);
}

void os64_ui_add_child(os64_ui_widget_t *parent, os64_ui_widget_t *child)
{
	child->parent = parent;
	child->next_sibling = (os64_ui_widget_t *)0;
	if (parent->ui)
		adopt_tree(child, parent->ui);
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
	os64_ui_theme_defaults(&ui->theme);
	ui->appearance_generation = 0;
	ui->follow_session = true;
	os64_ui_theme_current(&ui->theme, &ui->appearance_generation);
	ui->root = ui->grab = ui->focus = ui->hover = (os64_ui_widget_t *)0;
	ui->grab_button = 0;
	ui->window_blurred = false;
	ui->any_dirty = false;
	ui->dirty = (os64_gui_rect_t){0, 0, 0, 0};
	ui->on_resize = (void (*)(os64_ui_t *))0;
	ui->on_close = (void (*)(os64_ui_t *))0;
	ui->quit = false;
	ui->font = (void *)0;
}

void os64_ui_set_root(os64_ui_t *ui, os64_ui_widget_t *root)
{
	os64_ui_cancel_interaction(ui);
	ui->root = root;
	adopt_tree(root, ui);
	os64_ui_mark_dirty(ui, root);
}

bool os64_ui_widget_enabled(const os64_ui_widget_t *w)
{
	if (!w) return false;
	for (; w; w = w->parent)
		if (w->hidden || w->disabled) return false;
	return true;
}

static bool belongs_to(os64_ui_t *ui, os64_ui_widget_t *w)
{
	if (!w) return false;
	while (w->parent) w = w->parent;
	return w == ui->root;
}

static void cancel_widget(os64_ui_t *ui, os64_ui_widget_t *w)
{
	if (!w) return;
	w->pressed = false;
	w->activation_key = 0;
	if (w->cls->cancel) w->cls->cancel(w);
	os64_ui_mark_dirty(ui, w);
}

void os64_ui_set_focus(os64_ui_t *ui, os64_ui_widget_t *w)
{
	if (w && (!w->focusable || !os64_ui_widget_enabled(w) || !belongs_to(ui, w)))
		return;
	if (ui->focus == w)
		return;
	// Both ends repaint: the old widget's caret has to DISAPPEAR, which no
	// amount of painting the new one can accomplish.
	if (ui->focus) {
		if (ui->grab == ui->focus) ui->grab = NULL;
		cancel_widget(ui, ui->focus);
		ui->focus->focused = false;
		os64_ui_mark_dirty(ui, ui->focus);
	}
	ui->focus = w;
	if (w) {
		w->focused = !ui->window_blurred;
		os64_ui_mark_dirty(ui, w);
	}
}

void os64_ui_clear_hover(os64_ui_t *ui)
{
	if (ui->hover) {
		ui->hover->hovered = false;
		os64_ui_mark_dirty(ui, ui->hover);
		ui->hover = NULL;
	}
}

void os64_ui_cancel_gestures(os64_ui_t *ui)
{
	cancel_widget(ui, ui->grab);
	if (ui->focus != ui->grab) cancel_widget(ui, ui->focus);
	ui->grab = NULL;
	ui->grab_button = 0;
	os64_ui_clear_hover(ui);
}

void os64_ui_cancel_interaction(os64_ui_t *ui)
{
	os64_ui_set_focus(ui, NULL);
	os64_ui_cancel_gestures(ui);
}

static void reconcile(os64_ui_t *ui)
{
	if (ui->grab && (!os64_ui_widget_enabled(ui->grab) || !belongs_to(ui, ui->grab))) {
		cancel_widget(ui, ui->grab);
		ui->grab = NULL;
	}
	if (ui->focus && (!os64_ui_widget_enabled(ui->focus) ||
	                  !ui->focus->focusable || !belongs_to(ui, ui->focus)))
		os64_ui_set_focus(ui, NULL);
	if (ui->hover && (!os64_ui_widget_enabled(ui->hover) || !belongs_to(ui, ui->hover)))
		os64_ui_clear_hover(ui);
}

void os64_ui_set_enabled(os64_ui_t *ui, os64_ui_widget_t *w, bool enabled)
{
	w->disabled = !enabled;
	reconcile(ui);
	os64_ui_mark_dirty(ui, w);
}

void os64_ui_set_hidden(os64_ui_t *ui, os64_ui_widget_t *w, bool hidden)
{
	w->hidden = hidden;
	reconcile(ui);
	// The parent repaints the vacated area when a child disappears.
	os64_ui_mark_dirty(ui, w->parent ? w->parent : w);
}

static os64_ui_widget_t *tree_next(os64_ui_widget_t *w)
{
	if (w->first_child) return w->first_child;
	while (w && !w->next_sibling) w = w->parent;
	return w ? w->next_sibling : NULL;
}

bool os64_ui_focus_next(os64_ui_t *ui, bool reverse, bool wrap)
{
	reconcile(ui);
	os64_ui_widget_t *first = NULL, *last = NULL, *previous = NULL, *next = NULL;
	bool seen = ui->focus == NULL;
	for (os64_ui_widget_t *w = ui->root; w; w = tree_next(w)) {
		if (!w->focusable || !os64_ui_widget_enabled(w)) continue;
		if (!first) first = w;
		if (w == ui->focus) { previous = last; seen = true; }
		else if (seen && !next) next = w;
		last = w;
	}
	os64_ui_widget_t *target = reverse ? previous : next;
	if (!ui->focus || (wrap && !target)) target = reverse ? last : first;
	os64_ui_set_focus(ui, target);
	return target != NULL;
}

// ── hit testing ─────────────────────────────────────────────────────────────

static bool rect_contains(os64_gui_rect_t r, int32_t x, int32_t y)
{
	return x >= r.x && y >= r.y && (int64_t)x < (int64_t)r.x + r.w &&
	       (int64_t)y < (int64_t)r.y + r.h;
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

static void update_hover(os64_ui_t *ui, int32_t x, int32_t y)
{
	os64_ui_widget_t *w = hit_test(ui->root, x, y);
	if (w && (!w->cls->event || !os64_ui_widget_enabled(w) ||
	          (ui->grab && ui->grab != w))) w = NULL;
	if (w == ui->hover) return;
	os64_ui_clear_hover(ui);
	ui->hover = w;
	if (w) {
		w->hovered = true;
		os64_ui_mark_dirty(ui, w);
	}
}

bool os64_ui_dispatch(os64_ui_t *ui, const os64_gui_event_t *ev)
{
	if (ev->type == OS64_GUI_EVENT_APPEARANCE) {
		if (ui->follow_session && os64_ui_theme_session(&ui->theme,
		        &ui->appearance_generation, os64_gui_appearance_generation(ev)))
			os64_ui_mark_dirty(ui, ui->root);
		return true;
	}
	reconcile(ui);
	// A close request needs no widget tree to land on — handled first so the
	// root guard cannot swallow it.
	if (ev->type == OS64_GUI_EVENT_WINDOW_CLOSE) {
		if (ui->on_close)
			ui->on_close(ui);
		else
			ui->quit = true;
		return true;
	}
	if (ui->root == (os64_ui_widget_t *)0)
		return false;

	switch (ev->type) {
	case OS64_GUI_EVENT_POINTER_STATE:
		if (ev->pointer.inside) update_hover(ui, ev->pointer.x, ev->pointer.y);
		else os64_ui_clear_hover(ui);
		return true;
	case OS64_GUI_EVENT_WINDOW_COVERED:
		cancel_widget(ui, ui->grab);
		ui->grab = NULL;
		cancel_widget(ui, ui->focus);
		return true;
	case OS64_GUI_EVENT_WINDOW_FOCUS:
		ui->window_blurred = !ev->focus.gained;
		if (ui->window_blurred) {
			cancel_widget(ui, ui->grab);
			ui->grab = NULL;
			cancel_widget(ui, ui->focus);
		}
		// Pointer presence is independent of keyboard focus. Its snapshot
		// clears hover when another window actually covers this control.
		if (ui->focus) {
			ui->focus->focused = !ui->window_blurred;
			os64_ui_mark_dirty(ui, ui->focus);
		}
		return true;
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
		if (ev->mouse.button != OS64_GUI_MOUSE_LEFT || ui->grab) return false;
		update_hover(ui, ev->mouse.x, ev->mouse.y);
		os64_ui_widget_t *w = hit_test(ui->root, ev->mouse.x, ev->mouse.y);
		if (w && os64_ui_widget_enabled(w) && w->cls->event && w->cls->event(w, ui, ev)) {
			// Callbacks may disable or hide their control during the press.
			if (os64_ui_widget_enabled(w) && belongs_to(ui, w)) {
				ui->grab = w;
				ui->grab_button = ev->mouse.button;
			}
			return true;
		}
		return false;
	}
	case OS64_GUI_EVENT_MOUSE_MOVE:
	case OS64_GUI_EVENT_MOUSE_BUTTON_UP: {
		update_hover(ui, ev->mouse.x, ev->mouse.y);
		// A grab owns the pointer until release — the ancient button
		// contract (drag off to cancel) needs moves and the release even
		// when the cursor has left the widget's bounds.
		os64_ui_widget_t *w = ui->grab;
		if (w == (os64_ui_widget_t *)0)
			return false;
		if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP &&
		    ev->mouse.button != ui->grab_button) return false;
		// Release ownership before callbacks can change focus or enabled state.
		if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP) ui->grab = NULL;
		bool consumed = w->cls->event ? w->cls->event(w, ui, ev) : false;
		return consumed;
	}
	case OS64_GUI_EVENT_KEY_DOWN:
	case OS64_GUI_EVENT_KEY_UP: {
		if (ui->window_blurred) return false;
		os64_ui_widget_t *w = ui->focus;
		if (ev->key.ascii == '\t' && !(ev->key.modifiers & OS64_GUI_MOD_ALT) &&
		    (!w || !w->accepts_tab || (ev->key.modifiers & OS64_GUI_MOD_CTRL))) {
			if (ev->type == OS64_GUI_EVENT_KEY_DOWN)
				os64_ui_focus_next(ui, ev->key.modifiers & OS64_GUI_MOD_SHIFT, true);
			return true;
		}
		return (w && w->cls->event) ? w->cls->event(w, ui, ev) : false;
	}
	case OS64_GUI_EVENT_WINDOW_RESIZE: {
		// Cancel presses/drags across changed bounds but keep the typing target.
		os64_ui_cancel_gestures(ui);
		// After cancelling gestures, refresh geometry before app layout:
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
		if (!ui->root)
			return true;
		ui->root->bounds = (os64_gui_rect_t){0, 0,
		                                     (int32_t)ui->ctx->surf.width,
		                                     (int32_t)ui->ctx->surf.height};
		if (ui->on_resize)
			ui->on_resize(ui);
		reconcile(ui);
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
	w->ui = ui;   // a widget attached by hand still reaches the fonts
	os64_gui_rect_t clip;
	if (!os64_rect_intersect(w->bounds, ui->dirty, &clip))
		return;
	// Parent before children (children overpaint), whole widget even when
	// only partly dirty: the canvas beyond the dirty rect just gets the same
	// bytes it already shows (repaint-from-state), and only `dirty` is
	// published. Widgets paint INSIDE their bounds by contract — the
	// primitives clip to the CANVAS, so a widget that strays stomps a
	// sibling, which is LIBDRAW.md's named fingerprint for this layer.
	if (w->cls->paint) {
		os64_ui_theme_t t = ui->theme;
		if (!os64_ui_widget_enabled(w)) {
			t.panel_bg = t.button_face = t.button_face_pressed = t.button_face_hover =
			    t.text_bg = t.field_bg = t.scroll_track = t.disabled_bg;
			t.label_fg = t.button_fg = t.text_fg = t.field_fg = t.scroll_thumb =
			    t.button_border = t.field_border = t.button_highlight = t.button_shadow =
			    t.hover_border = t.focus_ring = t.text_caret = t.disabled_fg;
			t.text_sel_bg = t.disabled_fg;
			t.text_sel_fg = t.disabled_bg;
		}
		w->cls->paint(w, ui->ctx, &t);
	}
	for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling)
		paint_recursive(c, ui);
}

bool os64_ui_render(os64_ui_t *ui, os64_gui_rect_t *damage)
{
	reconcile(ui);
	if (!ui->any_dirty || ui->root == (os64_ui_widget_t *)0)
		return false;
	paint_recursive(ui->root, ui);
	if (damage)
		*damage = ui->dirty;
	ui->any_dirty = false;
	ui->dirty = (os64_gui_rect_t){0, 0, 0, 0};
	return true;
}

void os64_ui_paint(os64_ui_t *ui)
{
	os64_gui_rect_t damage;
	if (os64_ui_render(ui, &damage))
		os64_draw_publish(ui->ctx, &damage);
}

// ── the packaged loop ───────────────────────────────────────────────────────

void os64_ui_run(os64_ui_t *ui, int64_t win, volatile bool *running)
{
	os64_ui_paint(ui);   // first frame: whatever set_root marked

	while ((running == (volatile bool *)0 || *running) && !ui->quit) {
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

int32_t os64_ui_control_min_height(os64_ui_t *ui)
{
	int32_t row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
	int32_t want = row + 2 * (ui ? ui->theme.pad : 0);
	int32_t floor = ui ? ui->theme.button_h : 0;
	return want > floor ? want : floor;
}

// A label is one row of text, a button is a control: same face, different
// furniture around it.
static void label_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
	w->bounds.h = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
}

static void button_metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{
	w->bounds.h = os64_ui_control_min_height(ui);
}

static void label_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                        const os64_ui_theme_t *t)
{
	// Labels sit on their parent panel's face — repaint that patch first so
	// a text change never shows the old glyphs underneath.
	os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
	os64_ui_t *ui = os64_ui_of(w);
	int32_t ty = w->bounds.y + (w->bounds.h - os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI)) / 2;
	os64_ui_draw_text(ui, OS64_FONT_ROLE_UI, &ctx->surf, w->bounds,
	                  w->bounds.x, ty < w->bounds.y ? w->bounds.y : ty,
	                  w->text ? w->text : "", ui_strlen(w->text),
	                  t->label_fg, t->panel_bg);
}

static void button_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                         const os64_ui_theme_t *t)
{
	uint32_t face = w->pressed ? t->button_face_pressed :
	                w->hovered ? t->button_face_hover : t->button_face;
	int32_t radius = t->control_radius;
	if (radius < 0) radius = 0;
	if (radius > 8) radius = 8;
	if (radius > w->bounds.w / 2) radius = w->bounds.w / 2;
	if (radius > w->bounds.h / 2) radius = w->bounds.h / 2;
	if (radius) os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
	os64_draw_fill_round_rect(&ctx->surf, w->bounds, radius, face);
	uint32_t border = w->focused ? t->focus_ring :
		w->hovered ? t->hover_border : t->button_border;
	os64_draw_round_rect(&ctx->surf, w->bounds, radius, border, border);
	// Bound relief to the control, including when it is smaller than its radius.
	int32_t bevel = t->button_bevel;
	if (bevel > 4) bevel = 4;
	if (bevel > w->bounds.w / 2) bevel = w->bounds.w / 2;
	if (bevel > w->bounds.h / 2) bevel = w->bounds.h / 2;
	uint32_t light = w->pressed ? t->button_shadow : t->button_highlight;
	uint32_t dark = w->pressed ? t->button_highlight : t->button_shadow;
	for (int32_t i = 1; i < bevel; ++i) {
		os64_gui_rect_t edge = {w->bounds.x + i, w->bounds.y + i,
		                        w->bounds.w - 2 * i, w->bounds.h - 2 * i};
		os64_draw_round_rect(&ctx->surf, edge, radius > i ? radius - i : 0, light, dark);
	}
	os64_ui_t *ui = os64_ui_of(w);
	size_t len = ui_strlen(w->text);
	int32_t tw = os64_ui_text_width(ui, OS64_FONT_ROLE_UI, w->text, len);
	int32_t tx = w->bounds.x + (w->bounds.w - tw) / 2;
	int32_t ty = w->bounds.y +
	             (w->bounds.h - os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI)) / 2;
	if (tx < w->bounds.x + t->pad)
		tx = w->bounds.x + t->pad;
	if (ty < w->bounds.y)
		ty = w->bounds.y;
	os64_gui_rect_t text_clip = w->bounds;
	if (radius > 0) { text_clip.x += radius; text_clip.w -= 2 * radius; }
	os64_ui_draw_text(ui, OS64_FONT_ROLE_UI, &ctx->surf, text_clip, tx, ty,
	                  w->text ? w->text : "", len, t->button_fg, face);
}

static bool button_event(os64_ui_widget_t *w, os64_ui_t *ui,
                         const os64_gui_event_t *ev)
{
	switch (ev->type) {
	case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN:
		if (ev->mouse.button != OS64_GUI_MOUSE_LEFT) return false;
		os64_ui_set_focus(ui, w);
		w->activation_key = 0;
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
	case OS64_GUI_EVENT_KEY_DOWN: {
		if (ui->grab || (ev->key.modifiers & (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT)) ||
		    (ev->key.ascii != ' ' && ev->key.ascii != '\n' && ev->key.ascii != '\r'))
			return false;
		if (!w->activation_key) {
			// Keep the physical key identity: a changed modifier on release
			// must not strand a pressed visual or activate a different key.
			w->activation_key = 0x100u | ev->key.scancode |
			                    ((ev->key.modifiers & OS64_GUI_MOD_HID) ? 0x200u : 0);
			w->pressed = true;
			os64_ui_mark_dirty(ui, w);
		}
		return true;
	}
	case OS64_GUI_EVENT_KEY_UP: {
		uint16_t key = 0x100u | ev->key.scancode |
		               ((ev->key.modifiers & OS64_GUI_MOD_HID) ? 0x200u : 0);
		if (key != w->activation_key) return false;
		w->activation_key = 0;
		w->pressed = false;
		os64_ui_mark_dirty(ui, w);
		if (w->on_click) w->on_click(w, w->user);
		return true;
	}
	default:
		return false;
	}
}

const os64_ui_class_t os64_ui_panel_class  = { "panel",  panel_paint,  0, 0, 0 };
const os64_ui_class_t os64_ui_label_class  = { "label",  label_paint,  0, 0, label_metrics };
const os64_ui_class_t os64_ui_button_class = { "button", button_paint, button_event, 0, button_metrics };

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
	w->focusable = true;
	w->text = text;
	w->on_click = on_click;
	w->user = user;
}
