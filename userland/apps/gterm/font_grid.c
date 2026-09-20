#include "font_grid.h"
#include "os64/charset.h"

/* Retained one-cell layouts cover the finite PTY byte alphabet. F2 owns the
 * glyph cache and masks; these runs make painting allocation-free even when
 * new output arrives immediately after a successful resize barrier. */
struct gterm_font_plan {
    os64_font_set_t *set;
    os64_font_role_view_t font;
    os64_text_run_t *runs[2][224];
    uint32_t cols, rows;
};

bool gterm_grid_geometry(uint32_t width, uint32_t height, int32_t cw,
    int32_t ch, uint32_t *cols, uint32_t *rows)
{
    if (!cols || !rows || cw <= 0 || ch <= 0 || width > INT32_MAX || height > INT32_MAX)
        return false;
    uint32_t c = width / (uint32_t)cw, r = height / (uint32_t)ch;
    if (c < 2 || c > 512 || r < 2 || r > 256 || c * r > GTERM_MAX_CELLS)
        return false;
    *cols = c; *rows = r;
    return true;
}

static void dispose(gterm_grid_t *g, gterm_font_plan_t *p)
{
    if (!p) return;
    for (size_t s = 0; s < 2; s++)
        for (size_t b = 0; b < 224; b++) os64_text_run_release(p->runs[s][b]);
    os64_font_set_release(p->set);
    g->memory.free(g->memory.context, p, sizeof(*p));
}

static os64_font_status_t prepare(void *user, os64_font_set_t *set, void **out)
{
    gterm_grid_t *g = user;
    *out = NULL;
    os64_font_role_view_t font;
    os64_font_status_t status = os64_font_set_view(set, OS64_FONT_ROLE_TERMINAL, &font);
    if (status != OS64_FONT_OK) return status;
    uint32_t cols, rows;
    if (!gterm_grid_geometry(g->width, g->height, font.cell_width_px,
                            font.row_height_px, &cols, &rows)) return OS64_FONT_LIMIT;
    gterm_font_plan_t *p = g->memory.alloc(g->memory.context, sizeof(*p));
    if (!p) return OS64_FONT_NO_MEMORY;
    *p = (gterm_font_plan_t){.font = font, .cols = cols, .rows = rows};
    status = os64_font_set_retain(set);
    if (status != OS64_FONT_OK) { dispose(g, p); return status; }
    p->set = set;
    os64_text_layout_t layout = {.fonts = font.fonts, .font_count = font.font_count,
        .tab_interval = font.cell_width_px * OS64_FONT_UNIT,
        .cell_advance = font.cell_width_px * OS64_FONT_UNIT};
    for (size_t s = 0; s < 2; s++) {
        layout.encoding = s ? OS64_TEXT_CP437 : OS64_TEXT_LATIN1;
        for (unsigned b = 32; b < 256; b++) {
            uint8_t byte = (uint8_t)b;
            status = os64_text_layout(font.text, &byte, 1, &layout, &p->runs[s][b - 32]);
            if (status != OS64_FONT_OK) { dispose(g, p); return status; }
        }
    }
    *out = p;
    return OS64_FONT_OK;
}

static os64_font_status_t barrier(void *user, void *plan)
{
    gterm_grid_t *g = user;
    gterm_font_plan_t *p = plan;
    if ((g->cols != p->cols || g->rows != p->rows) &&
        g->resize(g->user, p->cols, p->rows) != 0) return OS64_FONT_ENGINE_ERROR;
    return OS64_FONT_OK;
}

static void commit(void *user, void *plan)
{
    gterm_grid_t *g = user;
    gterm_font_plan_t *old = g->active;
    g->active = plan;
    g->cols = g->active->cols; g->rows = g->active->rows;
    g->invalidate(g->user);
    dispose(g, old);
}

static void abort_plan(void *user, void *plan) { dispose(user, plan); }

os64_font_consumer_t gterm_grid_consumer(gterm_grid_t *g)
{
    return (os64_font_consumer_t){g, prepare, barrier, commit, abort_plan};
}

void gterm_grid_destroy(gterm_grid_t *g) { dispose(g, g->active); g->active = NULL; }
const os64_font_role_view_t *gterm_grid_font(const gterm_grid_t *g)
{ return g->active ? &g->active->font : NULL; }

bool gterm_grid_resize(gterm_grid_t *g, uint32_t width, uint32_t height)
{
    g->width = width; g->height = height;
    const os64_font_role_view_t *f = gterm_grid_font(g);
    uint32_t cols, rows;
    if (!f || !gterm_grid_geometry(width, height, f->cell_width_px, f->row_height_px,
                                  &cols, &rows)) return false;
    if (cols == g->cols && rows == g->rows) return true;
    if (g->resize(g->user, cols, rows) != 0) return false;
    g->cols = cols; g->rows = rows;
    g->invalidate(g->user);
    return true;
}

bool gterm_grid_snapshot_matches(const gterm_grid_t *g, const os64_pty_header_t *h,
    int64_t copied)
{
    return h->cols == g->cols && h->rows == g->rows &&
        copied == (int64_t)(g->cols * g->rows);
}

void gterm_grid_cell_at(const gterm_grid_t *g, int32_t x, int32_t y,
    uint32_t *row, uint32_t *col)
{
    const os64_font_role_view_t *f = gterm_grid_font(g);
    uint32_t r = y < 0 ? 0 : (uint32_t)y / (uint32_t)f->row_height_px;
    uint32_t c = x < 0 ? 0 : (uint32_t)x / (uint32_t)f->cell_width_px;
    *row = r < g->rows ? r : g->rows - 1;
    *col = c < g->cols ? c : g->cols - 1;
}

os64_gui_rect_t gterm_grid_cell_rect(const gterm_grid_t *g, uint32_t r, uint32_t c)
{
    const os64_font_role_view_t *f = gterm_grid_font(g);
    return (os64_gui_rect_t){(int32_t)c * f->cell_width_px,
        (int32_t)r * f->row_height_px, f->cell_width_px, f->row_height_px};
}

uint8_t gterm_grid_byte(uint8_t byte) { return byte < 32 ? ' ' : byte; }

void gterm_grid_draw_cell(const gterm_grid_t *g, os64_gui_surface_t *surface,
    uint32_t row, uint32_t col, uint8_t byte, uint8_t charset, uint32_t fg)
{
    os64_gui_rect_t cell = gterm_grid_cell_rect(g, row, col);
    os64_text_run_t *run = g->active->runs[charset == OS64_CHARSET_CP437][gterm_grid_byte(byte) - 32];
    (void)os64_text_draw(run, surface, cell, cell.x,
        cell.y + g->active->font.baseline_px, fg);
}
