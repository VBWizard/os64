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
struct os64_ui_popup {
    os64_ui_widget_t w;
    os64_ui_menu_model_t *model;
    os64_ui_action_view_t *view;
    const os64_ui_icon_provider_t *provider;
    size_t selected,top,bottom;
    int32_t row_h,icon_size,check_w,label_x,label_w,legend_x,legend_w,arrow_x;
    bool icons,scroll;
    menu_row rows[OS64_UI_MENU_ENTRIES];
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
bool menu_selectable(os64_ui_popup_t *,size_t);
bool menu_bar_enabled(os64_ui_menubar_t *,size_t);
size_t menu_bar_visible(os64_ui_menubar_t *);
size_t menu_popup_scroll_top(const os64_ui_popup_t *,int32_t);
#endif
