#ifndef OS64_UI_MENU_INTERNAL_H
#define OS64_UI_MENU_INTERNAL_H
#include "os64/ui_menu.h"
#define MENU_PAD 3
#define MENU_GAP 6
#define MENU_SEP 7
#define MENU_NONE SIZE_MAX
struct os64_ui_menu_model {
    os64_ui_actions_t *actions;
    size_t refs,count,graph,depth;
    os64_ui_menu_entry_t entries[OS64_UI_MENU_ENTRIES];
    char labels[OS64_UI_MENU_ENTRIES][128];
};
typedef struct {
    char label[128],shortcut[40];
    void *run,*legend;
    os64_ui_icon_lease_t icon;
    os64_ui_action_spec_t spec;
} menu_row;
/* A dropdown's list, the session's own copy (os64_ui_menu_open_list): the
 * labels packed in one block, the enabled flags, the row that is the value,
 * and who is told of a pick (NULL once detached). The anchor widget and the
 * bounds it had at the call are what the boundary watches. */
typedef struct {
    size_t count,value;
    int32_t min_width;
    const char **labels;
    bool *enabled;
    void (*pick)(void *,size_t);
    void *user;
    os64_ui_widget_t *widget;
    os64_gui_rect_t widget_bounds;
} menu_list;
/* A popup carries one of two contents, and every site that reads its rows
 * asks the content questions below (menu_count, menu_separator,
 * menu_selectable, menu_label, menu_mnemonic) rather than the model. A
 * menu's rows are all laid out at creation in `rows`; a list's are STAGED
 * by viewport in `stage`, runs only for the rows it can show, laid out as
 * they scroll in. */
struct os64_ui_popup {
    os64_ui_widget_t w;
    os64_ui_menu_model_t *model;
    os64_ui_action_view_t *view;
    const menu_list *list;
    const os64_ui_icon_provider_t *provider;
    size_t selected,top,bottom;
    int32_t row_h,icon_size,check_w,label_x,label_w,legend_x,legend_w,arrow_x;
    bool icons,scroll;
    menu_row rows[OS64_UI_MENU_ENTRIES];
    menu_row *stage;
    size_t stage_cap,stage_top,stage_count;
};
typedef struct {
    os64_ui_menu_session_t *session;
    os64_ui_popup_t *widget;
    os64_draw_ctx_t draw;
    int64_t handle;
    size_t index;
} menu_panel;
struct os64_ui_menu_session {
    os64_ui_dispatcher_t *dispatcher;
    os64_ui_t *ui;
    int64_t owner;
    os64_ui_menubar_t *bar;
    os64_ui_status_t *status;
    const os64_ui_icon_provider_t *provider;
    os64_ui_action_view_t *view,*bar_view;
    os64_ui_menu_model_t *bar_snapshot_model;
    uint64_t bar_enabled;
    bool bar_snapshot;
    os64_ui_menu_model_t *pending;
    /* The list carried in place of a menu, NULL when a menu is; while
     * `list_pending` it waits for the quarantine to drain, as `pending`
     * does for a model. `close_retry`: a detached list's close was refused,
     * and the boundary asks again. */
    menu_list *list;
    bool list_pending,close_retry;
    os64_gui_rect_t anchor;
    os64_ui_menu_invocation_t kind;
    menu_panel panels[OS64_UI_MENU_DEPTH];
    size_t depth,pending_depth,pressed_panel,pressed_row,failed_panel,failed_row;
    uint16_t pressed_key;
    uint8_t decoder,suppress_button;
    uint8_t held[64];
    uint64_t help_token;
    bool active,alt_armed,f10_held,suppress_pending,dismissed;
    os64_ui_widget_t *saved_focus,*fallback;
    void (*layout)(void *);
    void (*error)(void *,os64_ui_status_code_t);
    void *user;
};
bool menu_model_enabled(os64_ui_menu_model_t *,os64_ui_action_view_t *);
size_t menu_count(const os64_ui_popup_t *);
bool menu_separator(const os64_ui_popup_t *,size_t);
bool menu_selectable(os64_ui_popup_t *,size_t);
const char *menu_label(const os64_ui_popup_t *,size_t);
char menu_mnemonic(const os64_ui_popup_t *,size_t);
os64_ui_status_code_t menu_popup_create_list(os64_ui_t *,const menu_list *,int32_t max_w,int32_t max_h,
    os64_ui_popup_t **);
void menu_popup_relist(os64_ui_popup_t *,const menu_list *);
bool menu_bar_enabled(os64_ui_menubar_t *,size_t);
bool menu_bar_enabled_view(os64_ui_menubar_t *,size_t,os64_ui_action_view_t *);
size_t menu_bar_visible(os64_ui_menubar_t *);
size_t menu_popup_scroll_top(const os64_ui_popup_t *,int32_t);
#endif
