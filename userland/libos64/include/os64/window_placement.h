#ifndef OS64_WINDOW_PLACEMENT_H
#define OS64_WINDOW_PLACEMENT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os64/gui.h"

#define OS64_WINDOW_CONFIG_MAX 128u
#define OS64_WINDOW_PREFERENCE_MAX 32767u
typedef enum {
    OS64_WINDOW_REMEMBER_NONE,
    OS64_WINDOW_REMEMBER_SIZE,
    OS64_WINDOW_REMEMBER_POSITION_AND_SIZE
} os64_window_remember_mode_t;
typedef struct {
    int32_t x, y;
    uint32_t width, height;
} os64_window_geometry_t;
typedef struct {
    const char *config; // dedicated config filename for this application/window role
    os64_window_remember_mode_t mode;
} os64_window_remember_t;
typedef struct {
    char config[OS64_WINDOW_CONFIG_MAX];
    os64_window_remember_mode_t mode;
    os64_window_geometry_t preferred;
    int64_t status; // latest configuration read/publication result; zero on success
} os64_window_placement_t;

// App-owned storage, copied options. Coordinates are frame x/y with content w/h,
// matching create_content. Bad preferences fall back without a startup write.
// False means invalid options/defaults; a read failure is retained in status.
bool os64_window_placement_init(os64_window_placement_t *,
    os64_window_remember_t, os64_window_geometry_t defaults);
// Fit only the live request; retain preferred dimensions across smaller screens.
os64_window_geometry_t os64_window_placement_prepare(const os64_window_placement_t *);
// Consume explicit placement for an opted-in window, even if persistence fails.
// Re-read, merge masked components and publish. No close-time save or worker.
bool os64_window_placement_event(os64_window_placement_t *, const os64_gui_event_t *);
#endif
