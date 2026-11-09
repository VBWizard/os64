#include "os64/ui_icon.h"
#include "os64/str.h"
#include "ui_internal.h"

static void paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    os64_ui_icon_button_t *b = (os64_ui_icon_button_t *)w;
    if (!b->image.pixels) {
        os64_ui_button_class.paint(w, ctx, t);
        return;
    }
    os64_ui_button_face(w, ctx, t);
    os64_ui_t *ui = os64_ui_of(w);
    os64_font_role_t role = ui_role(w, OS64_FONT_ROLE_UI);
    int32_t row = os64_ui_font_row_height(ui, role);
    os64_gui_rect_t inside = {w->bounds.x + 4, w->bounds.y + 4,
                             w->bounds.w - 8, w->bounds.h - 8};
    os64_gui_rect_t slot = inside;
    slot.h -= row + 2;
    os64_ui_icon_draw(&ctx->surf, slot, &b->image, !os64_ui_widget_enabled(w));
    const char *text = w->text ? w->text : "";
    int32_t width = 0;
    if (os64_ui_run_width(ui, &w->run, role, text,
                          os64_strlen(text), &width) != OS64_FONT_OK) return;
    uint32_t face = w->pressed ? t->button_face_pressed :
                    w->hovered ? t->button_face_hover : t->button_face;
    os64_ui_draw_run(ui, w->run, role, &ctx->surf, inside,
        inside.x + (inside.w - width) / 2, inside.y + inside.h - row,
        text, os64_strlen(text), t->button_fg, face);
}
static bool event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{ return os64_ui_button_class.event(w, ui, ev); }
static void metrics(os64_ui_widget_t *w, os64_ui_t *ui)
{ w->natural_h = 24 + os64_ui_font_row_height(ui, ui_role(w, OS64_FONT_ROLE_UI)) + 10; }
static void destroy(os64_ui_widget_t *w)
{ os64_ui_icon_button_clear((os64_ui_icon_button_t *)w); }
/* Icon leases are independent of the window font. */
static void reface(os64_ui_widget_t *w) { (void)w; }
static const os64_ui_class_t icon_button_class = {
    .name="icon-button", .paint=paint, .event=event,
    .prepare=os64_ui_stage_caption, .commit=os64_ui_commit_caption,
    .discard=os64_ui_discard_caption, .destroy=destroy, .metrics=metrics, .reface=reface
};
void os64_ui_icon_button(os64_ui_icon_button_t *b, const char *text,
    void (*click)(os64_ui_widget_t *, void *), void *user)
{
    *b = (os64_ui_icon_button_t){0};
    os64_ui_button(&b->w, text, click, user);
    b->w.cls = &icon_button_class;
}
os64_ui_icon_status_t os64_ui_icon_button_image(os64_ui_icon_button_t *b,
    const os64_ui_icon_provider_t *provider, const char *key)
{
    if (!b) return OS64_UI_ICON_INVALID;
    os64_ui_icon_lease_t image;
    os64_ui_icon_status_t rc = os64_ui_icon_acquire(provider, key, 24, &image);
    if (rc != OS64_UI_ICON_OK) return rc;
    os64_ui_icon_button_clear(b);
    b->provider = provider; b->image = image;
    if (b->w.ui) os64_ui_mark_dirty(b->w.ui, &b->w);
    return OS64_UI_ICON_OK;
}
void os64_ui_icon_button_clear(os64_ui_icon_button_t *b)
{
    if (!b) return;
    os64_ui_icon_release(b->provider, &b->image); b->provider = NULL;
    if (b->w.ui) os64_ui_mark_dirty(b->w.ui, &b->w);
}
