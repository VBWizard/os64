#ifndef OS64_UI_ACTION_H
#define OS64_UI_ACTION_H
#include "os64/ui.h"

/* UI-thread APIs. Query/validate/release cannot allocate, mutate toolkit
 * ownership or pump events. Invocation may request teardown; the application
 * disposes its tree after dispatch returns, then detaches bindings. */
#include "os64/ui_result.h"

typedef enum {
    OS64_UI_KEY_NONE = 0,
    OS64_UI_KEY_ESCAPE = 256, OS64_UI_KEY_ENTER, OS64_UI_KEY_TAB,
    OS64_UI_KEY_BACKSPACE, OS64_UI_KEY_DELETE, OS64_UI_KEY_INSERT,
    OS64_UI_KEY_UP, OS64_UI_KEY_DOWN, OS64_UI_KEY_LEFT, OS64_UI_KEY_RIGHT,
    OS64_UI_KEY_HOME, OS64_UI_KEY_END, OS64_UI_KEY_PAGE_UP, OS64_UI_KEY_PAGE_DOWN,
    OS64_UI_KEY_F1, OS64_UI_KEY_F2, OS64_UI_KEY_F3, OS64_UI_KEY_F4,
    OS64_UI_KEY_F5, OS64_UI_KEY_F6, OS64_UI_KEY_F7, OS64_UI_KEY_F8,
    OS64_UI_KEY_F9, OS64_UI_KEY_F10, OS64_UI_KEY_F11, OS64_UI_KEY_F12
} os64_ui_key_t;
/* Printable keys are ASCII; letters are normalized to lowercase. Modifier
 * bits are OS64_GUI_MOD_SHIFT/CTRL/ALT, matched exactly. */
typedef struct { uint16_t key; uint8_t modifiers; } os64_ui_shortcut_t;
typedef enum { OS64_UI_ACTION_COMMAND, OS64_UI_ACTION_TOGGLE,
               OS64_UI_ACTION_RADIO } os64_ui_action_kind_t;
typedef struct {
    uint32_t id;
    os64_ui_action_kind_t kind;
    uint32_t radio_group;
    const char *label, *help, *icon;
    char mnemonic;
    os64_ui_shortcut_t shortcut;
} os64_ui_action_spec_t;
typedef struct { bool enabled, checked; } os64_ui_action_state_t;
typedef struct {
    void *cookie;
    uint64_t generation;
    bool (*retain)(void *cookie, uint64_t generation);
    void (*release)(void *cookie, uint64_t generation);
    bool (*validate)(void *cookie, uint64_t generation);
} os64_ui_action_context_t;
typedef struct { os64_ui_status_code_t status; int64_t app_result; }
    os64_ui_action_result_t;
/* Invoke returns OK (performed), REFUSED, or ERROR. app_result belongs to
 * the app; the toolkit preserves it without constructing a diagnostic. */
typedef struct {
    void (*query)(void *app, const os64_ui_action_context_t *context,
                  uint32_t id, os64_ui_action_state_t *out);
    os64_ui_action_result_t (*invoke)(void *app,
                  const os64_ui_action_context_t *context, uint32_t id);
} os64_ui_action_callbacks_t;
typedef struct os64_ui_actions os64_ui_actions_t;
typedef struct os64_ui_action_view os64_ui_action_view_t;
#define OS64_UI_ACTIONS_MAX 256u
#define OS64_UI_ACTION_VIEWS_MAX 32u

os64_ui_status_code_t os64_ui_actions_create(const os64_ui_action_spec_t *,
    size_t count, const os64_ui_action_callbacks_t *, void *app, os64_ui_actions_t **out);
os64_ui_status_code_t os64_ui_actions_destroy(os64_ui_actions_t *);
/* Registry references for immutable menu models; owner still calls destroy. */
os64_ui_status_code_t os64_ui_actions_retain(os64_ui_actions_t *);
void os64_ui_actions_release(os64_ui_actions_t *);
/* Copied-out descriptor strings are borrowed until registry destruction. */
os64_ui_status_code_t os64_ui_action_spec(const os64_ui_actions_t *, uint32_t,
                                         os64_ui_action_spec_t *out);
os64_ui_status_code_t os64_ui_action_view_create(os64_ui_actions_t *,
    const os64_ui_action_context_t *, os64_ui_action_view_t **out);
os64_ui_status_code_t os64_ui_action_view_destroy(os64_ui_action_view_t *);
os64_ui_status_code_t os64_ui_action_view_retain(os64_ui_action_view_t *);
void os64_ui_action_view_release(os64_ui_action_view_t *);
/* Refresh atomically publishes states and updates attached buttons. Invocation
 * refreshes states after the callback without touching buttons; refresh again
 * after dispatch to update presentations if teardown was not requested. */
os64_ui_status_code_t os64_ui_actions_refresh(os64_ui_action_view_t *);
os64_ui_status_code_t os64_ui_action_snapshot(const os64_ui_action_view_t *,
    uint32_t id, os64_ui_action_state_t *out);
os64_ui_action_result_t os64_ui_action_invoke(os64_ui_action_view_t *, uint32_t);
/* Ordinary buttons only. Bind/unbind outside dispatch of this button. Unbind
 * clears caption/callback and cancels presses; restore legacy behavior explicitly. */
os64_ui_status_code_t os64_ui_action_bind(os64_ui_widget_t *, os64_ui_action_view_t *, uint32_t);
os64_ui_status_code_t os64_ui_action_unbind(os64_ui_widget_t *);

/* Navigation escape bursts are unclaimed. Named navigation keys match
 * zero-ASCII events from sources providing key release edges. */
bool os64_ui_shortcut_match(os64_ui_shortcut_t, const os64_gui_event_t *);
/* App-owned router, initialized/reset with reset(). It retains no view and
 * routes only the view supplied by the application on this turn. */
typedef struct { uint8_t held[64]; } os64_ui_shortcut_router_t;
void os64_ui_shortcut_reset(os64_ui_shortcut_router_t *);
/* Call for key edges and focus/covered events, before widget dispatch. Disabled
 * actions consume their chord. Unclaimed text/clipboard input passes through. */
bool os64_ui_shortcut_route(os64_ui_shortcut_router_t *, os64_ui_action_view_t *,
    const os64_gui_event_t *, os64_ui_action_result_t *out);
#endif
