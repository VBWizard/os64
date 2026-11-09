// os64/rect.h — the rectangle, one definition for the kernel and ring 3.
//
// A screen-space or surface-space rectangle, signed on purpose: a window
// frame can hang partly off the screen (a negative origin) while it is
// dragged, and clipping is cleaner when intermediate results may go
// negative instead of wrapping. The kernel's rect_t is this type
// (gui/gui_types.h), so code shared by both sides (os64/damage.h) works on
// one type and not on two that merely agree.
#ifndef OS64_RECT_H
#define OS64_RECT_H

#include <stdint.h>

typedef struct os64_gui_rect
{
    int32_t x, y;
    int32_t w, h;
} os64_gui_rect_t;

#endif
