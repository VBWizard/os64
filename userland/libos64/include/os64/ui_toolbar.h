#ifndef OS64_UI_TOOLBAR_H
#define OS64_UI_TOOLBAR_H
#include "os64/ui_action.h"
#include "os64/ui_icon.h"
#define OS64_UI_TOOLBAR_MAX 32u
/* Command buttons in order. A group starts with extra space, except at a row
 * boundary. NULL label uses the action label; overrides are copied at bind. */
typedef struct {
    uint32_t action;
    const char *label;
    bool group;
} os64_ui_toolbar_entry_t;
typedef struct {
    os64_ui_widget_t w;
    os64_ui_actions_t *actions;
    size_t count;
    os64_ui_icon_button_t buttons[OS64_UI_TOOLBAR_MAX];
    os64_ui_action_spec_t specs[OS64_UI_TOOLBAR_MAX];
    char labels[OS64_UI_TOOLBAR_MAX][128];
    bool groups[OS64_UI_TOOLBAR_MAX];
    void (*command)(os64_ui_widget_t *button, uint32_t action, void *user);
    void *user;
} os64_ui_toolbar_t;
/* Initialize, then bind once outside dispatch/painting. Binding retains the
 * registry; only COMMAND actions are accepted. The callback chooses the view
 * and invokes its action, allowing the app to preserve its editing target.
 * Keep widget storage alive through dispatch and font cleanup. Destroy before
 * the icon provider or registry; destroy is idempotent and also runs at tree
 * teardown. A destroyed toolbar must not be dispatched or rebound. */
void os64_ui_toolbar_init(os64_ui_toolbar_t *);
os64_ui_status_code_t os64_ui_toolbar_bind(os64_ui_toolbar_t *, os64_ui_actions_t *,
    const os64_ui_toolbar_entry_t *, size_t,
    void (*command)(os64_ui_widget_t *, uint32_t, void *), void *);
void os64_ui_toolbar_destroy(os64_ui_toolbar_t *);
/* Acquire outside painting. Failure preserves each button's previous image
 * or text fallback. Returns the first failure after attempting the remaining
 * icons; successful acquisitions are kept even if another icon fails. */
os64_ui_icon_status_t os64_ui_toolbar_icons(os64_ui_toolbar_t *, const os64_ui_icon_provider_t *);
/* Refresh enabled states after dispatch. No view is retained. A mismatched
 * registry disables the buttons and returns INVALID. */
os64_ui_status_code_t os64_ui_toolbar_refresh(os64_ui_toolbar_t *, os64_ui_action_view_t *);
/* Pointer wins over keyboard focus. Returns 0 if hidden, disabled or idle;
 * optional help text is borrowed from the retained registry. */
uint32_t os64_ui_toolbar_help(const os64_ui_toolbar_t *, const char **text);
/* Measure against the active/candidate font without changing live geometry.
 * Output is unchanged on failure. Treat a successful plan as read-only and
 * apply to the same toolbar before changing its items/fonts. Staged apply is
 * for the app's font planner; normal apply dirties old and new rectangles. */
typedef struct {
    const os64_ui_toolbar_t *owner;
    size_t count;
    os64_gui_rect_t bounds, buttons[OS64_UI_TOOLBAR_MAX];
} os64_ui_toolbar_plan_t;
os64_font_status_t os64_ui_toolbar_measure(os64_ui_toolbar_t *, os64_ui_t *,
    int32_t x, int32_t y, int32_t width, os64_ui_toolbar_plan_t *);
void os64_ui_toolbar_apply(os64_ui_toolbar_t *, const os64_ui_toolbar_plan_t *, bool staged);
#endif
