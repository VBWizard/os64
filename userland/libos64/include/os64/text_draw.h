#ifndef OS64_TEXT_DRAW_H
#define OS64_TEXT_DRAW_H
#include "os64/text.h"
#include "os64/gui.h"

/* Paint retained masks, without layout or allocation.
 * Baseline coordinates are integer surface pixels. Clip intersects the surface.
 * Glyph positions use floor((position + 32)/64); bounds use outward rounding.
 * Grayscale source-over onto opaque XRGB, with (a*fg+(255-a)*bg+127)/255 per
 * channel. Empty clips succeed. Background/selection painting is the caller's
 * responsibility. Does not publish the surface or alter the run. */
os64_font_status_t os64_text_draw(const os64_text_run_t *, os64_gui_surface_t *,
    os64_gui_rect_t clip, int32_t baseline_x, int32_t baseline_y, uint32_t color);

/* The same, the ink itself `alpha` of 255 opaque: each mask's coverage is
 * scaled by (a*alpha+127)/255 before the mix above, so a translucent colour's
 * text lies over what is under it as its fill does. 255 is os64_text_draw. */
os64_font_status_t os64_text_draw_alpha(const os64_text_run_t *, os64_gui_surface_t *,
    os64_gui_rect_t clip, int32_t baseline_x, int32_t baseline_y, uint32_t color,
    uint8_t alpha);
#endif
