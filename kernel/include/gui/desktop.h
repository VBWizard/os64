#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include <stdbool.h>
#include <stdint.h>
#include "gui/gui_types.h"

// The desktop background from /home/desktop.conf or /etc/desktop.conf —
// see desktop.c for the file's two keys and why PPM. Paints `desk` (fill,
// then the optional centered image) and returns true; returns false with
// `desk` untouched when no config exists, so the compositor keeps its
// built-in test pattern. Call once, from the compositor task, before the
// first frame; it reads files and must not run under kGuiLock.
bool desktop_paint_from_config(surface_t *desk);

#define DESKTOP_DEFAULT_COLOR GUI_COLOR_DESKTOP
#define DESKTOP_CONF_MAX      (8 * 1024)          // the 8KB cap every os64 config reader shares
#define DESKTOP_IMAGE_MAX     (16u * 1024 * 1024) // a 2048x2048 P6 is 12MB; nothing sane is bigger

typedef struct {
    uint32_t color;
    char     image[256];
} desktop_config_t;

#endif // GUI_DESKTOP_H
