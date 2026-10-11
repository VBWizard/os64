#include "os64/ui_status.h"
#include "ui_internal.h"
#include "ui_message.h"
#include "os64/mem.h"
#include "os64/str.h"

static os64_ui_status_slot_t *slot_at(os64_ui_status_t *s, size_t i)
{
    return i == 0 ? &s->base : i == 1 ? &s->help : &s->notice;
}
static size_t length(const char *text) { return text ? os64_strlen(text) : 0; }
static void dirty(os64_ui_status_t *s)
{
    if (s->w.ui) os64_ui_mark_dirty(s->w.ui, &s->w);
}
static os64_ui_status_code_t set(os64_ui_status_t *s, os64_ui_status_slot_t *slot,
    const char *text, uint64_t *token)
{
    if (!s || s->w.cls != &os64_ui_status_class || !text) return OS64_UI_INVALID;
    if (token) {
        *token = 0;
        if (s->generation == UINT64_MAX) return OS64_UI_LIMIT;
    }
    size_t n;
    os64_ui_status_code_t rc = ui_message_length(text, OS64_UI_STATUS_TEXT_MAX, &n);
    if (rc != OS64_UI_OK) return rc;
    char *copy = os64_malloc(n + 1);
    if (!copy) return OS64_UI_NO_MEMORY;
    os64_memcpy(copy, text, n + 1);
    void *run = NULL;
    os64_font_status_t fs = os64_ui_run_layout(s->w.ui, OS64_FONT_ROLE_UI, copy, n, &run);
    if (fs != OS64_FONT_OK) {
        os64_free(copy);
        return ui_message_font_result(fs);
    }
    os64_ui_run_release(slot->run);
    os64_ui_run_release(slot->staged);
    os64_free(slot->text);
    slot->text = copy; slot->run = run; slot->staged = NULL;
    if (token) slot->token = *token = ++s->generation;
    dirty(s);
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_status_base(os64_ui_status_t *s, const char *text)
{ return s ? set(s, &s->base, text, NULL) : OS64_UI_INVALID; }
os64_ui_status_code_t os64_ui_status_help(os64_ui_status_t *s, const char *text, uint64_t *token)
{ return s && token ? set(s, &s->help, text, token) : OS64_UI_INVALID; }
os64_ui_status_code_t os64_ui_status_notice(os64_ui_status_t *s, const char *text, uint64_t *token)
{ return s && token ? set(s, &s->notice, text, token) : OS64_UI_INVALID; }
static os64_ui_status_code_t clear(os64_ui_status_t *s, os64_ui_status_slot_t *slot, uint64_t token)
{
    if (!s || s->w.cls != &os64_ui_status_class) return OS64_UI_INVALID;
    if (!token || token != slot->token) return OS64_UI_STALE;
    os64_ui_run_release(slot->run); os64_ui_run_release(slot->staged);
    os64_free(slot->text);
    *slot = (os64_ui_status_slot_t){0};
    dirty(s);
    return OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_status_clear_help(os64_ui_status_t *s, uint64_t token)
{ return s ? clear(s, &s->help, token) : OS64_UI_INVALID; }
os64_ui_status_code_t os64_ui_status_clear_notice(os64_ui_status_t *s, uint64_t token)
{ return s ? clear(s, &s->notice, token) : OS64_UI_INVALID; }

static void discard(os64_ui_widget_t *w)
{
    os64_ui_status_t *s = (void *)w;
    for (size_t i = 0; i < 3; ++i) {
        os64_ui_status_slot_t *slot = slot_at(s, i);
        os64_ui_run_release(slot->staged); slot->staged = NULL;
    }
}
static os64_font_status_t prepare(os64_ui_widget_t *w, os64_ui_t *ui)
{
    os64_ui_status_t *s = (void *)w;
    discard(w);
    for (size_t i = 0; i < 3; ++i) {
        os64_ui_status_slot_t *slot = slot_at(s, i);
        if (!slot->text) continue;
        os64_font_status_t rc = os64_ui_run_layout(ui, OS64_FONT_ROLE_UI,
            slot->text, length(slot->text), &slot->staged);
        if (rc != OS64_FONT_OK) { discard(w); return rc; }
    }
    return OS64_FONT_OK;
}
static void commit(os64_ui_widget_t *w)
{
    os64_ui_status_t *s = (void *)w;
    for (size_t i = 0; i < 3; ++i) {
        os64_ui_status_slot_t *slot = slot_at(s, i);
        os64_ui_run_release(slot->run); slot->run = slot->staged; slot->staged = NULL;
    }
}
static void reface(os64_ui_widget_t *w)
{
    os64_ui_status_t *s = (void *)w;
    discard(w);
    for (size_t i = 0; i < 3; ++i) {
        os64_ui_status_slot_t *slot = slot_at(s, i);
        os64_ui_run_release(slot->run); slot->run = NULL;
    }
}
void os64_ui_status_destroy(os64_ui_status_t *s)
{
    if (!s) return;
    reface(&s->w);
    for (size_t i = 0; i < 3; ++i) {
        os64_ui_status_slot_t *slot = slot_at(s, i);
        os64_free(slot->text); *slot = (os64_ui_status_slot_t){0};
    }
}
static void destroy(os64_ui_widget_t *w) { os64_ui_status_destroy((void *)w); }
static void metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{ w->natural_h = ui_control_min_height(ui, OS64_FONT_ROLE_UI); }

static void draw_slot(os64_ui_status_t *s, os64_ui_status_slot_t *slot,
    os64_draw_ctx_t *ctx, const os64_ui_theme_t *theme, os64_gui_rect_t clip, int32_t y)
{
    if (!slot->text || clip.w <= 0) return;
    os64_ui_draw_text(s->w.ui, &slot->run, OS64_FONT_ROLE_UI, &ctx->surf,
        clip, clip.x, y, slot->text, length(slot->text), theme->label_fg, theme->panel_bg);
}
static void paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    os64_ui_status_t *s = (void *)w;
    os64_draw_fill_rect(&ctx->surf, w->bounds, t->panel_bg);
    os64_draw_rect(&ctx->surf, w->bounds, t->panel_border);
    os64_gui_rect_t inner = w->bounds;
    inner.x += t->pad; inner.w -= 2 * t->pad;
    if (inner.w <= 0 || inner.h <= 0) return;
    int32_t y = inner.y + (inner.h - os64_ui_font_row_height(w->ui, OS64_FONT_ROLE_UI)) / 2;
    os64_ui_status_slot_t *secondary = s->help.token ? &s->help : &s->base;
    if (!s->notice.token) { draw_slot(s, secondary, ctx, t, inner, y); return; }
    int32_t width;
    if (os64_ui_run_width(w->ui, &s->notice.run, OS64_FONT_ROLE_UI,
        s->notice.text, length(s->notice.text), &width) != OS64_FONT_OK) return;
    os64_gui_rect_t notice = inner;
    if (width < notice.w) notice.w = width;
    draw_slot(s, &s->notice, ctx, t, notice, y);
    /* Persistent information owns the left edge and its measured width. Help
     * uses the remaining clip; a narrow layout cannot displace the notice. */
    if (width >= inner.w || t->gap >= inner.w - width) return;
    inner.x += width + t->gap; inner.w -= width + t->gap;
    draw_slot(s, secondary, ctx, t, inner, y);
}
const os64_ui_class_t os64_ui_status_class = {
    "status", paint, NULL, NULL, prepare, commit, discard, destroy, metrics, reface
};
void os64_ui_status_init(os64_ui_status_t *s)
{
    *s = (os64_ui_status_t){0};
    s->w.cls = &os64_ui_status_class; s->w.auto_h = true;
}
