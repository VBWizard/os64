#ifndef OS64_UI_MENU_H
#define OS64_UI_MENU_H
#include "os64/ui_action.h"
#include "os64/ui_status.h"
#include "os64/ui_icon.h"
#include "os64/ui_dispatcher.h"
#define OS64_UI_MENU_ENTRIES 64u
#define OS64_UI_MENU_GRAPH 256u
#define OS64_UI_MENU_DEPTH 16u
typedef struct os64_ui_menu_model os64_ui_menu_model_t;
typedef struct os64_ui_menu_session os64_ui_menu_session_t;
typedef enum { OS64_UI_MENU_ACTION,OS64_UI_MENU_SEPARATOR,OS64_UI_MENU_SUBMENU } os64_ui_menu_kind_t;
typedef struct {
    os64_ui_menu_kind_t kind;
    uint32_t action;
    const char *label;
    char mnemonic;
    os64_ui_menu_model_t *child;
} os64_ui_menu_entry_t;
/* Immutable copied descriptors; models retain their registry and children.
 * Construction from already immutable children makes cycles unrepresentable.
 * Registry identity must match throughout a graph and its session view. */
os64_ui_status_code_t os64_ui_menu_model_create(os64_ui_actions_t *,
    const os64_ui_menu_entry_t *,size_t,os64_ui_menu_model_t **);
os64_ui_status_code_t os64_ui_menu_model_retain(os64_ui_menu_model_t *);
void os64_ui_menu_model_release(os64_ui_menu_model_t *);
size_t os64_ui_menu_model_count(const os64_ui_menu_model_t *);
/* Application-owned widget; bind copies no borrowed captions. destroy before
 * releasing the owner font/context. Overflow retains complete child menus. */
typedef struct {
    os64_ui_widget_t w;
    os64_ui_menu_model_t *model;
    os64_ui_menu_session_t *session;
    os64_ui_action_view_t *view; // borrowed from the attached session
    void *labels[OS64_UI_MENU_ENTRIES],*staged[OS64_UI_MENU_ENTRIES];
    int32_t widths[OS64_UI_MENU_ENTRIES],staged_widths[OS64_UI_MENU_ENTRIES];
    size_t visible,selected;
    int32_t overflow_width,staged_overflow_width;
    bool temporary;
} os64_ui_menubar_t;
extern const os64_ui_class_t os64_ui_menubar_class;
void os64_ui_menubar_init(os64_ui_menubar_t *);
os64_ui_status_code_t os64_ui_menubar_bind(os64_ui_menubar_t *,os64_ui_menu_model_t *);
void os64_ui_menubar_destroy(os64_ui_menubar_t *);
/* A retained panel for a supplied surface, separate from the owner tree.
 * It borrows the owner's binding: destroy before adoption/rebind/release. */
typedef struct os64_ui_popup os64_ui_popup_t;
os64_ui_status_code_t os64_ui_popup_create(os64_ui_t *,os64_ui_menu_model_t *,
    os64_ui_action_view_t *,const os64_ui_icon_provider_t *,int32_t max_w,int32_t max_h,
    os64_ui_popup_t **);
void os64_ui_popup_destroy(os64_ui_popup_t *);
os64_ui_widget_t *os64_ui_popup_widget(os64_ui_popup_t *);
os64_gui_rect_t os64_ui_popup_row(const os64_ui_popup_t *,size_t);
int os64_ui_popup_hit(const os64_ui_popup_t *,int32_t x,int32_t y);
size_t os64_ui_popup_selected(const os64_ui_popup_t *);
void os64_ui_popup_select(os64_ui_popup_t *,size_t);
void os64_ui_popup_scroll(os64_ui_popup_t *,int32_t rows);
/* owner=0 creates a standalone unowned root (bar must be NULL and the UI
 * must not be registered as another native window); its anchor
 * is screen-local. The dispatcher reserves a maintenance slot until disposal.
 * Positive-owner anchor is content-local; cascades re-read parent screen geometry.
 * BUSY means queued until post-destruction drains complete. A failed native
 * destroy returns ERROR with surviving panels retained, preventing replacement.
 * An empty root returns EMPTY. New requests replace the old session on success. */
typedef enum { OS64_UI_MENU_CONTEXT,OS64_UI_MENU_BAR } os64_ui_menu_invocation_t;
typedef enum { OS64_UI_MENU_EXPLICIT,OS64_UI_MENU_DISMISSED,OS64_UI_MENU_LOST,
    OS64_UI_MENU_GEOMETRY,OS64_UI_MENU_FONT } os64_ui_menu_close_t;
os64_ui_status_code_t os64_ui_menu_session_create(os64_ui_dispatcher_t *,os64_ui_t *,int64_t owner,
    os64_ui_menubar_t *,os64_ui_status_t *,const os64_ui_icon_provider_t *,
    os64_ui_menu_session_t **);
/* Close/dispose return ERROR on native destruction failure. Keep the session,
 * owner tree, dispatcher and font context alive and retry before releasing them.
 * Disposal frees the session and detaches hooks only on OK. */
os64_ui_status_code_t os64_ui_menu_session_dispose(os64_ui_menu_session_t *);
os64_ui_status_code_t os64_ui_menu_open(os64_ui_menu_session_t *,os64_ui_menu_model_t *,
    os64_ui_action_view_t *,os64_gui_rect_t anchor,os64_ui_menu_invocation_t);
os64_ui_status_code_t os64_ui_menu_close(os64_ui_menu_session_t *,os64_ui_menu_close_t);
/* Fallback widget storage must outlive the session. Close before removing or
 * freeing widgets; hidden/detached restoration targets use this fallback. */
void os64_ui_menu_focus_fallback(os64_ui_menu_session_t *,os64_ui_widget_t *);
bool os64_ui_menu_active(const os64_ui_menu_session_t *);
os64_ui_status_code_t os64_ui_menu_refresh(os64_ui_menu_session_t *);
/* Bar context is selected explicitly by the app before dispatch. Retained so
 * focus changes cannot retarget an open session. Layout hook reflows temporary
 * reveal and collapse; error hook reports one refusal per explicit request. */
os64_ui_status_code_t os64_ui_menu_bar_view(os64_ui_menu_session_t *,os64_ui_action_view_t *);
void os64_ui_menu_hooks(os64_ui_menu_session_t *,void (*layout)(void *),
    void (*error)(void *,os64_ui_status_code_t),void *);
/* Public placement helper, signed/clamped; screen bounds include popup border. */
os64_ui_status_code_t os64_ui_menu_place(os64_gui_rect_t anchor,int32_t w,int32_t h,
    uint32_t screen_w,uint32_t screen_h,bool cascade,os64_gui_rect_t *);
#endif
