#ifndef OS64_UI_DATA_INTERNAL_H
#define OS64_UI_DATA_INTERNAL_H
#include "os64/ui_data.h"
typedef struct {
    os64_ui_data_projection_t *projection;
    void *owner;
    uint64_t revision;
    os64_ui_data_id_t focus, anchor;
    size_t count, bytes, storage;
    unsigned char *selected;
} ui_selection_state;
/* Private candidate operations use detached copies. The app-owned handle is
 * stable while publication exchanges its state. */
__attribute__((visibility("hidden")))
os64_ui_status_code_t ui_selection_copy(const os64_ui_selection_t *, os64_ui_selection_t *);
__attribute__((visibility("hidden")))
void ui_selection_publish(os64_ui_selection_t *, os64_ui_selection_t *, void *owner);
#endif
