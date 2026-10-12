#ifndef OS64_UI_ICON_H
#define OS64_UI_ICON_H
#include "os64/draw.h"
typedef enum { OS64_UI_ICON_OK, OS64_UI_ICON_MISSING, OS64_UI_ICON_INVALID,
    OS64_UI_ICON_LIMIT, OS64_UI_ICON_NO_MEMORY, OS64_UI_ICON_IO,
    OS64_UI_ICON_MALFORMED } os64_ui_icon_status_t;
typedef struct {
    const uint32_t *pixels;
    uint32_t width,height,pitch;
    uint64_t generation;
    void *cookie;
} os64_ui_icon_lease_t;
/* App-owned provider outlives leases. Preparation acquires; paint never does
 * I/O or allocation. Failure clears out; immutable pixels last until release. */
typedef struct {
    void *user;
    os64_ui_icon_status_t (*acquire)(void *,const char *,uint32_t,os64_ui_icon_lease_t *);
    void (*release)(void *,os64_ui_icon_lease_t *);
} os64_ui_icon_provider_t;
bool os64_ui_icon_key_valid(const char *);
uint32_t os64_ui_icon_menu_size(int32_t line_height);
os64_ui_icon_status_t os64_ui_icon_acquire(const os64_ui_icon_provider_t *,
    const char *,uint32_t,os64_ui_icon_lease_t *);
void os64_ui_icon_release(const os64_ui_icon_provider_t *,os64_ui_icon_lease_t *);
void os64_ui_icon_draw(os64_gui_surface_t *,os64_gui_rect_t slot,
    const os64_ui_icon_lease_t *,bool disabled);
#endif
