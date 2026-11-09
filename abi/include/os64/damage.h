// os64/damage.h — the damage list: the rectangles that need repainting, kept
// short. One rule (shared/damage.c), built into the kernel, where the
// compositor keeps the screen's list, and into libos64.
#ifndef OS64_DAMAGE_H
#define OS64_DAMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "os64/rect.h"

// Add `r` to the list rects[0..*count), which holds at most `cap` (at least
// one, since a full list collapses into rects[0]): unioned
// into the first entry whose union wastes no more than `slack` pixels beyond
// the two areas, else appended, else, the list being full, the whole list
// and `r` become one rectangle, their union. An empty `r` changes nothing.
// True when the list collapsed that way.
bool os64_damage_add(os64_gui_rect_t *rects, uint32_t *count, uint32_t cap, int64_t slack,
                     os64_gui_rect_t r);

#endif
