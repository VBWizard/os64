#ifndef GTERM_FONT_GRID_H
#define GTERM_FONT_GRID_H

#include "os64/font_adopt.h"
#include "os64/text_draw.h"
#include "os64/pty.h"
#include <stdbool.h>

#define GTERM_MAX_CELLS 16384u
typedef struct gterm_font_plan gterm_font_plan_t;
typedef struct {
    os64_font_memory_t memory;
    void *user;
    int64_t (*resize)(void *user, uint32_t cols, uint32_t rows);
    /* Invalidate the displayed snapshot and selection; must not fail/reenter. */
    void (*invalidate)(void *user);
    uint32_t width, height, cols, rows;
    gterm_font_plan_t *active;
} gterm_grid_t;

/* Caller serializes adoption, surface resizing and painting on the UI thread.
 * The descriptor can join a shared adoption batch as its single PTY barrier. */
os64_font_consumer_t gterm_grid_consumer(gterm_grid_t *);
void gterm_grid_destroy(gterm_grid_t *);
const os64_font_role_view_t *gterm_grid_font(const gterm_grid_t *);
bool gterm_grid_geometry(uint32_t width, uint32_t height, int32_t cell_w,
    int32_t cell_h, uint32_t *cols, uint32_t *rows);
/* Records the actual surface even on refusal; the old grid remains clipped or
 * letterboxed. A successful geometry change invalidates snapshot/selection. */
bool gterm_grid_resize(gterm_grid_t *, uint32_t width, uint32_t height);
bool gterm_grid_snapshot_matches(const gterm_grid_t *, const os64_pty_header_t *,
    int64_t copied);
void gterm_grid_cell_at(const gterm_grid_t *, int32_t x, int32_t y,
    uint32_t *row, uint32_t *col);
os64_gui_rect_t gterm_grid_cell_rect(const gterm_grid_t *, uint32_t row, uint32_t col);
/* The PTY has already interpreted low controls. Retain high bytes verbatim. */
uint8_t gterm_grid_byte(uint8_t byte);
void gterm_grid_draw_cell(const gterm_grid_t *, os64_gui_surface_t *,
    uint32_t row, uint32_t col, uint8_t byte, uint8_t charset, uint32_t foreground);
#endif
