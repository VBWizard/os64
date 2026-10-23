#include "os64/ui_window.h"

static bool placement_event(os64_ui_t *ui, const os64_gui_event_t *e)
{
    return os64_window_placement_event(&ui->placement,e);
}
os64_ui_status_code_t os64_ui_window_open(os64_ui_t *ui, os64_draw_ctx_t *ctx,
    int64_t *window, const os64_ui_window_options_t *options)
{
    if (window) *window=0;
    if (!ui || !ctx || !window || !options || !options->title) return OS64_UI_INVALID;
    os64_window_placement_t placement;
    os64_window_geometry_t defaults={options->x,options->y,options->content_width,options->content_height};
    if (!os64_window_placement_init(&placement,options->remember,defaults)) return OS64_UI_INVALID;
    if (placement.mode && (options->flags & (OS64_GUI_WINDOW_DESKTOP|OS64_GUI_CREATE_OWNER_MASK)))
        return OS64_UI_INVALID;
    os64_window_geometry_t g=os64_window_placement_prepare(&placement);
    *ctx=(os64_draw_ctx_t){0};
    *window=os64_gui_window_create_content(options->title,g.x,g.y,g.width,g.height,
        options->flags|OS64_GUI_CREATE_FIT_SCREEN);
    if (*window<=0) return OS64_UI_ERROR;
    // Surface acquisition can fail before draw_ctx_init binds the handle.
    // Keep the retained UI/draw storage associated with a refused cleanup.
    ctx->win=*window;
    int64_t rc=os64_draw_ctx_init(ctx,*window);
    os64_ui_init(ui,ctx);
    ui->placement=placement;
    ui->placement_event=placement.mode ? placement_event : NULL;
    if (rc) {
        if (os64_gui_window_destroy(*window)==0) *window=0;
        return OS64_UI_ERROR;
    }
    return OS64_UI_OK;
}
