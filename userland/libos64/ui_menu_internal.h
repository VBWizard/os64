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
bool menu_model_enabled(os64_ui_menu_model_t *,os64_ui_action_view_t *);
bool menu_selectable(os64_ui_popup_t *,size_t);
bool menu_bar_enabled(os64_ui_menubar_t *,size_t);
size_t menu_bar_visible(os64_ui_menubar_t *);
#endif
