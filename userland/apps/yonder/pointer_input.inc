// Native page samples enter here outside libui. Each sample completes its
// pointer/compatibility packet before the next native window event is read.
static void pointer_widget_canceled(os64_ui_t *ui, os64_ui_widget_t *widget)
{
    (void)ui; (void)widget;
    if (s_pointer_ignore_cancel) return;
    yonder_scripts_pointer_cancel(g.page.scripts);
    s_pointer_cancel_owed = true;
}

static void pointer_native_release(void)
{
    s_pointer_ignore_cancel = true;
    os64_ui_cancel_gestures(&g.ui);
    s_pointer_ignore_cancel = false;
}

typedef struct {
    os64_gui_event_t native;
    uint64_t serial;
    bool replayed;
    const os64_html_node_t *frame; // held until the task's navigation/default is chosen
} PointerDefault;

static bool pointer_default(void *opaque, const os64_html_node_t *node,
                              const os64_dom_event_t *event, bool prevented)
{
    PointerDefault *p = opaque;
    if (p->serial != g.page_serial || !scripts_live()) return false;
    // Standard click activation belongs to libdom. Frame placeholders are
    // Yonder's own source-opening affordance, deferred until the task ends.
    // Native replay supplies focus, editor selection and widget pressed
    // state, with activation callbacks
    // suppressed so checkboxes and forms cannot act twice.
    if (os64_streq(event->type, "click")) {
        if (!prevented && node && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML &&
            (node->tag == OS64_HTML_TAG_FRAME || node->tag == OS64_HTML_TAG_IFRAME)) {
            os64_html_hold(page_doc(&g.page), node);
            p->frame = node;
        }
        return true;
    }
    if (os64_streq(event->type, "auxclick")) return true;
    if (prevented) {
        if (os64_streq(event->type, "mouseup")) pointer_native_release();
        return true;
    }
    if (event->button > 0 && !os64_streq(event->type, "mousemove")) return true;
    const os64_html_node_t *root = node;
    while (root && root->parent) root = root->parent;
    if (node && root != page_doc(&g.page)->document) return true;
    if (os64_streq(event->type, "mousedown")) {
        // A listener may remove the hit or capture a chord to another node.
        // Do not focus an unrelated native control under the physical cursor.
        if (node != element_at(p->native.mouse.x, p->native.mouse.y, NULL) ||
            (node && os64_html_attr(node, "disabled"))) return true;
    }
    s_pointer_replay = true;
    ui_dispatch(&p->native);
    s_pointer_replay = false;
    p->replayed = true;
    return p->serial == g.page_serial;
}

static os64_dom_event_t pointer_coordinates(int32_t x, int32_t y, unsigned modifiers)
{
    double scale = 1000.0 / (g.zoom ? g.zoom : 1000);
    os64_gui_rect_t content = {0};
    (void)os64_gui_window_get_content_rect(g.win, &content);
    return (os64_dom_event_t){
        .client_x = ((double)x - g.view.bounds.x) * scale,
        .client_y = ((double)y - g.view.bounds.y) * scale,
        .page_x = ((double)x - g.view.bounds.x + g.sx) * scale,
        .page_y = ((double)y - g.view.bounds.y + g.sy) * scale,
        .screen_x = (double)content.x + x, .screen_y = (double)content.y + y,
        .page_coordinates = true, .button = -1, .buttons = s_pointer_buttons,
        .shift = (modifiers & OS64_GUI_MOD_SHIFT) != 0,
        .ctrl = (modifiers & OS64_GUI_MOD_CTRL) != 0,
        .alt = (modifiers & OS64_GUI_MOD_ALT) != 0};
}

static void pointer_packet(const os64_dom_pointer_sample_t *sample, PointerDefault *action)
{
    yonder_scripts_t *host = g.page.scripts;
    const os64_html_document_t *doc = page_doc(&g.page);
    if (!g.modern || !scripts_live() || !doc) return;
    if (sample->hit) os64_html_hold(doc, sample->hit);
    forms_flush();
    os64_js_outcome_t out;
    bool ran;
    int64_t began = task_begin(host);
    yonder_scripts_pointer(host, sample, action ? pointer_default : NULL, action, &ran, &out);
    task_said(host, ran, "pointer", &out, began);
    if (!scripts_live()) {
        if (s_pointer_buttons) s_pointer_draining = true;
        pointer_native_release();
    }
    // Keep native editors consistent even if a click changes no DOM version.
    forms_sync_from_model(false);
    if (sample->hit) os64_html_release(doc, sample->hit);
    const os64_html_node_t *frame = action ? action->frame : NULL;
    if (action) action->frame = NULL;
    if (frame && !scripts_live()) {
        os64_html_release(doc, frame);
        frame = NULL;
    }
    if (script_rebuild()) asks_perform_with_fallback(host, doc, true, frame);
    else if (frame) os64_html_release(doc, frame);
}

static void pointer_cancel_run(void)
{
    if (!s_pointer_cancel_owed) return;
    s_pointer_cancel_owed = false;
    if (!g.modern || !scripts_live()) return;
    os64_dom_pointer_sample_t sample = {.action = OS64_DOM_POINTER_CANCEL};
    pointer_packet(&sample, NULL);
    pointer_native_release();
}

static uint64_t pointer_next(void)
{
    return g.modern && !g.covered && scripts_live() &&
        (s_pointer_cancel_owed || (s_pointer_known && s_pointer_rehit))
            ? s_pointer_rested : YONDER_NEVER;
}

static bool pointer_turn(void)
{
    if (pointer_next() > yonder_now_ms()) return false;
    if (s_pointer_cancel_owed) { pointer_cancel_run(); return true; }
    uint64_t revision = s_pointer_revision;
    s_pointer_rested = yonder_now_ms() + 16;
    s_pointer_rehit = false;
    os64_dom_pointer_sample_t sample = {.action = OS64_DOM_POINTER_BOUNDARY,
        .event = pointer_coordinates(g.pointer_x, g.pointer_y, s_pointer_modifiers),
        .hit = element_at(g.pointer_x, g.pointer_y, NULL)};
    pointer_packet(&sample, NULL);
    if (revision != s_pointer_revision) s_pointer_rehit = true;
    return true;
}

static bool pointer_route(const os64_gui_event_t *ev)
{
    bool motion = ev->type == OS64_GUI_EVENT_MOUSE_MOVE;
    bool presence = ev->type == OS64_GUI_EVENT_POINTER_STATE;
    bool down = ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN;
    bool up = ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP;
    if (!motion && !presence && !down && !up) return false;
    if (s_pointer_draining) {
        // This belongs to the window, not the departing page's controller.
        // A passive presence snapshot cannot finish a discarded gesture.
        if (!presence) {
            s_pointer_buttons = ev->mouse.buttons & 7;
            if (!s_pointer_buttons) s_pointer_draining = false;
        }
        pointer_native_release();
        return true;
    }
    if (!g.modern || !scripts_live()) return false;
    int32_t x = presence ? (ev->pointer.inside ? ev->pointer.x : -1) : ev->mouse.x;
    int32_t y = presence ? (ev->pointer.inside ? ev->pointer.y : -1) : ev->mouse.y;
    os64_gui_rect_t v = g.view.bounds;
    bool inside = x >= v.x && y >= v.y && (int64_t)x < (int64_t)v.x + v.w && (int64_t)y < (int64_t)v.y + v.h;
    bool active = yonder_scripts_pointer_active(g.page.scripts);
    // Chrome owns a gesture it began. Presence still reconciles the page's
    // boundary when the cursor leaves the viewport.
    if (!active && !inside && !motion && !presence) return false;
    if (!active && g.ui.grab && g.ui.grab != &g.view) {
        bool form = false;
        for (int32_t i = 0; i < g.nfw; i++) if (g.fw[i]->w == g.ui.grab) form = true;
        if (!form) return false;
    }
    if (!script_rebuild()) {
        yonder_scripts_pointer_cancel(g.page.scripts);
        s_pointer_cancel_owed = true;
        return true;
    }
    uint64_t revision = s_pointer_revision, serial = g.page_serial;
    s_pointer_known = true;
    s_pointer_rehit = false;
    s_pointer_rested = yonder_now_ms() + 16;
    g.pointer_x = x; g.pointer_y = y;
    if (!presence) { s_pointer_buttons = ev->mouse.buttons & 7; s_pointer_modifiers = ev->mouse.modifiers; }
    os64_dom_pointer_sample_t sample = {
        .action = presence ? OS64_DOM_POINTER_BOUNDARY : down ? OS64_DOM_POINTER_DOWN : up ? OS64_DOM_POINTER_UP : OS64_DOM_POINTER_MOVE,
        .event = pointer_coordinates(x, y, s_pointer_modifiers),
        .hit = element_at(x, y, NULL)};
    if (down || up) sample.event.button = ev->mouse.button == OS64_GUI_MOUSE_RIGHT ? 2 :
                                         ev->mouse.button == OS64_GUI_MOUSE_MIDDLE ? 1 : 0;
    // Snapshot menu addresses before script can remove or change the link.
    os64_ui_menu_model_t *menu = NULL;
    if (down && ev->mouse.button == OS64_GUI_MOUSE_RIGHT && inside && g.menus &&
        g.context_menus[CONTEXT_MENUS - 1]) {
        const os64_html_node_t *link = NULL, *boxed = NULL;
        (void)element_hit(x, y, &link, &boxed);
        bool has_link = context_link_address(link, g.context_link, sizeof(g.context_link));
        bool has_picture = context_picture_address(boxed, g.context_picture, sizeof(g.context_picture));
        if (!has_link) g.context_link[0] = '\0';
        if (!has_picture) g.context_picture[0] = '\0';
        if (has_link || has_picture) menu = g.context_menus[has_link && has_picture ? CONTEXT_BOTH :
                                                          has_link ? CONTEXT_LINK : CONTEXT_PICTURE];
    }
    PointerDefault action = {.native = *ev, .serial = serial};
    pointer_packet(&sample, presence ? NULL : &action);
    if (serial != g.page_serial) return true;
    if (up && ev->mouse.button == OS64_GUI_MOUSE_LEFT && !action.replayed) pointer_native_release();
    if (revision != s_pointer_revision) s_pointer_rehit = true;
    if (menu) {
        yonder_scripts_pointer_cancel(g.page.scripts);
        s_pointer_cancel_owed = true;
        pointer_cancel_run();
        (void)os64_ui_actions_refresh(g.action_view);
        os64_ui_status_code_t rc = os64_ui_menu_open(g.menus, menu, g.action_view,
                                                   (os64_gui_rect_t){x,y,0,0}, OS64_UI_MENU_CONTEXT);
        if (rc != OS64_UI_OK && rc != OS64_UI_BUSY) status_rest("The menu would not open.");
    }
    // Keep native hover decoration even when no compatibility motion is due.
    if (presence) ui_dispatch(ev);
    return inside || active || presence;
}
