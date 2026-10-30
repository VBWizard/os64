/* The menu session as the yonder hosts see it (yonder_host_build.sh): no
 * popup, only a record of the list a dropdown last asked for, so a case can
 * play the person's pick (yonder_host_menus.h). os64_ui_shortcut_match
 * answers no named key: a host types characters and ESC [ bursts. And the
 * icon theme: a provider of solid pictures, counted. */
#include "yonder_host_menus.h"
#include <string.h>

yonder_host_list_t yonder_host_list;

os64_ui_status_code_t os64_ui_menu_open_list(os64_ui_menu_session_t *s, const os64_ui_menu_list_t *l,
                                             os64_gui_rect_t anchor)
{
    (void)s;
    (void)anchor;
    yonder_host_list.opens++;
    yonder_host_list.open = true;
    yonder_host_list.count = l->count;
    yonder_host_list.selected = l->selected;
    yonder_host_list.pick = l->pick;
    yonder_host_list.user = l->user;
    size_t n = l->count < YONDER_HOST_LIST_ROWS ? l->count : YONDER_HOST_LIST_ROWS;
    for (size_t i = 0; i < n; i++) {
        snprintf(yonder_host_list.labels[i], sizeof(yonder_host_list.labels[i]), "%s", l->labels[i]);
        yonder_host_list.enabled[i] = l->enabled[i];
    }
    return OS64_UI_OK;
}

void os64_ui_menu_list_detach(os64_ui_menu_session_t *s, void *user)
{
    (void)s;
    if (!yonder_host_list.open || yonder_host_list.user != user) return;
    yonder_host_list.open = false;
    yonder_host_list.pick = NULL;
    yonder_host_list.detaches++;
}

os64_ui_status_code_t os64_ui_menu_list_relabel(os64_ui_menu_session_t *s, void *user, const char *const *labels,
                                                const bool *enabled, size_t count)
{
    (void)s;
    if (!yonder_host_list.open || yonder_host_list.user != user) return OS64_UI_STALE;
    if (count != yonder_host_list.count) return OS64_UI_INVALID;
    size_t n = count < YONDER_HOST_LIST_ROWS ? count : YONDER_HOST_LIST_ROWS;
    for (size_t i = 0; i < n; i++) {
        snprintf(yonder_host_list.labels[i], sizeof(yonder_host_list.labels[i]), "%s", labels[i]);
        yonder_host_list.enabled[i] = enabled[i];
    }
    yonder_host_list.relabels++;
    return OS64_UI_OK;
}

/* A row's own menu opens over a picked downloads row: the hosts have no
 * popups, so it is only counted. */
os64_ui_status_code_t os64_ui_actions_refresh(os64_ui_action_view_t *view)
{
    (void)view;
    return OS64_UI_OK;
}

os64_ui_status_code_t os64_ui_menu_open(os64_ui_menu_session_t *s, os64_ui_menu_model_t *model,
                                        os64_ui_action_view_t *view, os64_gui_rect_t anchor,
                                        os64_ui_menu_invocation_t invocation)
{
    (void)s;
    (void)model;
    (void)view;
    (void)anchor;
    (void)invocation;
    yonder_host_list.menus++;
    return OS64_UI_OK;
}

bool os64_ui_shortcut_match(os64_ui_shortcut_t shortcut, const os64_gui_event_t *ev)
{
    (void)shortcut;
    (void)ev;
    return false;
}

bool yonder_host_list_pick(size_t row)
{
    if (!yonder_host_list.open || !yonder_host_list.pick) return false;
    void (*pick)(void *, size_t) = yonder_host_list.pick;
    void *user = yonder_host_list.user;
    yonder_host_list.open = false;          /* the list closes, then the pick */
    yonder_host_list.pick = NULL;
    pick(user, row);
    return true;
}

/* The icon theme the head row asks: every key a solid square of the size
 * asked, its leases counted out and back. */
yonder_host_icons_t yonder_host_icons;
static uint32_t host_icon_pixels[64 * 64];

static os64_ui_icon_status_t host_icon_acquire(void *user, const char *key, uint32_t size,
                                               os64_ui_icon_lease_t *out)
{
    (void)user;
    (void)key;
    if (yonder_host_icons.refuse) return OS64_UI_ICON_MISSING;
    for (size_t i = 0; i < sizeof(host_icon_pixels) / sizeof(host_icon_pixels[0]); i++)
        host_icon_pixels[i] = 0xff40a040u;
    *out = (os64_ui_icon_lease_t){.pixels = host_icon_pixels, .width = size, .height = size, .pitch = size};
    yonder_host_icons.out++;
    return OS64_UI_ICON_OK;
}

static void host_icon_release(void *user, os64_ui_icon_lease_t *lease)
{
    (void)user;
    (void)lease;
    yonder_host_icons.out--;
}

static const os64_ui_icon_provider_t host_icon_provider = {NULL, host_icon_acquire, host_icon_release};

const os64_ui_icon_provider_t *os64_icon_theme_provider(os64_icon_theme_t *theme)
{
    (void)theme;
    return &host_icon_provider;
}

os64_ui_status_code_t os64_icon_theme_create(os64_icon_theme_t **out)
{
    *out = NULL;
    return OS64_UI_ERROR;
}

os64_ui_status_code_t os64_icon_theme_destroy(os64_icon_theme_t *theme)
{
    (void)theme;
    return OS64_UI_OK;
}
