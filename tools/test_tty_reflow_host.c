// Host harness for kernel/src/tty_reflow.c — built and run by
// test_tty_reflow_host.sh under ASan and UBSan.
//
// THE PROPERTY UNDER TEST is Chris's condition (CONSOLE_FONTS.md): a font
// change shows the same screen and the same scrollback. So the harness does
// not check a layout against a hand-drawn picture; it reduces a grid to what
// a PERSON would say is on it — the list of lines, each a run of whole cells
// with its trailing bare paper trimmed — and requires that list to come out
// of every reflow unchanged, through any chain of widths, with the cursor
// still on the same cell of the same line. Every result buffer is allocated
// at exactly the size the plan asked for, so writing one row too many is an
// ASan report.

#include "tty_reflow.h"
#include "os64/ansi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks, failures, clipped_chains;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

typedef struct
{
    tty_cell_t *cells;
    uint32_t cols, rows, total, screen_top, hist, view, cur_row, cur_col, save_row, save_col;
} grid_t;

static uint32_t rng = 0x05640564;
static uint32_t rnd(uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

static tty_cell_t *row_of(const grid_t *g, uint32_t logical)
{
    uint32_t base = (g->screen_top + g->total - g->hist) % g->total;
    return g->cells + (size_t)((base + logical) % g->total) * g->cols;
}

static bool blank(const tty_cell_t *c)
{ return (c->ch == 0 || c->ch == ' ') && c->bg == 0 && !(c->attrs & OS64_ANSI_ATTR_REVERSE); }

static tty_reflow_in_t as_input(const grid_t *g)
{
    return (tty_reflow_in_t){g->cells, g->cols, g->rows, g->total, g->screen_top, g->hist,
                             g->view, g->cur_row, g->cur_col, g->save_row, g->save_col};
}

// ── what a person would say is on the grid ─────────────────────────────────
typedef struct { tty_cell_t *cells; size_t len; } para_t;
typedef struct
{
    para_t *p; size_t count;
    size_t cur_para, cur_off;          // the cursor, as (which line, which cell of it)
    size_t top_para, top_off;          // the same for the top-left of the live screen
    size_t view_para;                  // the line at the top of the view
} story_t;

static story_t read_story(const grid_t *g)
{
    story_t s = {0};
    uint32_t logical = g->hist + g->rows, cursor = g->hist + g->cur_row;
    uint32_t last = logical - 1;
    for (;;) {
        uint32_t n = g->cols; const tty_cell_t *r = row_of(g, last);
        while (n && blank(&r[n - 1])) n--;
        if (n || last <= cursor) break;
        last--;
    }
    s.p = calloc(last + 1, sizeof(para_t));
    uint32_t view_row = g->hist - g->view;
    for (uint32_t p0 = 0; p0 <= last;) {
        uint32_t p1 = p0;
        while (p1 < last && (row_of(g, p1 + 1)[0].attrs & TTY_ATTR_WRAPPED)) p1++;
        para_t *pa = &s.p[s.count];
        size_t span = (size_t)(p1 - p0 + 1) * g->cols;
        pa->cells = calloc(span, sizeof(tty_cell_t));
        for (size_t k = 0; k < span; k++) {
            pa->cells[k] = row_of(g, p0 + (uint32_t)(k / g->cols))[k % g->cols];
            pa->cells[k].attrs &= (uint8_t)~TTY_ATTR_WRAPPED;
            // Bare paper is bare paper however it got there: a never-written
            // cell and a default-coloured space are the same thing to look at.
            if (blank(&pa->cells[k])) pa->cells[k] = (tty_cell_t){0};
        }
        pa->len = span;
        while (pa->len && blank(&pa->cells[pa->len - 1])) pa->len--;
        if (cursor >= p0 && cursor <= p1) {
            s.cur_para = s.count; s.cur_off = (size_t)(cursor - p0) * g->cols + g->cur_col;
        }
        if (g->hist >= p0 && g->hist <= p1) {
            s.top_para = s.count; s.top_off = (size_t)(g->hist - p0) * g->cols;
        }
        if (view_row >= p0 && view_row <= p1) s.view_para = s.count;
        s.count++;
        p0 = p1 + 1;
    }
    return s;
}

static void free_story(story_t *s)
{ for (size_t i = 0; i < s->count; i++) free(s->p[i].cells); free(s->p); }

// `b` must tell the same story as `a`, less `dropped` lines off the top.
static void same_story(const story_t *a, const story_t *b, size_t dropped, const char *what)
{
    CHECK(a->count == b->count + dropped, "%s: %zu lines became %zu (+%zu dropped)", what, a->count, b->count, dropped);
    if (a->count != b->count + dropped) return;
    for (size_t i = 0; i < b->count; i++) {
        const para_t *x = &a->p[i + dropped], *y = &b->p[i];
        // The first surviving line may have lost its own head to the drop.
        if (i == 0 && dropped) { CHECK(y->len <= x->len, "%s: surviving head grew", what); continue; }
        bool same = x->len == y->len && memcmp(x->cells, y->cells, x->len * sizeof(tty_cell_t)) == 0;
        CHECK(same, "%s: line %zu differs (len %zu vs %zu)", what, i, x->len, y->len);
        if (!same) return;
    }
    CHECK(a->cur_para == b->cur_para + dropped && a->cur_off == b->cur_off,
          "%s: cursor moved from line %zu cell %zu to line %zu cell %zu", what,
          a->cur_para, a->cur_off, b->cur_para + dropped, b->cur_off);

}

// WHERE THE LIVE SCREEN BEGINS, as a cell of a line rather than a row number
// — rows are not comparable across a reshape, cells are. ONE reflow at a
// time: the top may move a little on each, and those moves accumulate, so
// this is asked of a single call's input and output and never across a chain.
//
// It may move FORWARD through the content: a narrower grid needs more rows
// for the same text, and the top slides down so the last line still shows.
// Going BACKWARD it has two landing places and no others. The START OF THE
// ROW that holds the cell it began at, because the boundary must fall on a
// row boundary and the old one rarely does in the new width. Or the START OF
// THE LINE, when a wider grid has rejoined a line that straddled the
// boundary — its head comes back onto the screen with its tail, because the
// cursor is on that line and it cannot be left half in history. Reaching
// back to any OTHER row of the line is the failure: the part above the new
// top is a history line handed to the next paint to destroy.
//
// Asked apart from `same_story` because that derives the cursor as
// hist + cur_row: an error that moves the screen top and the cursor row
// together cancels in the subtraction and is invisible there.
static void screen_top_held(const story_t *a, const story_t *b, uint32_t nc, const char *what)
{
    bool forward  = b->top_para > a->top_para ||
                    (b->top_para == a->top_para && b->top_off >= a->top_off);
    bool same_row = b->top_para == a->top_para && b->top_off + nc > a->top_off;
    bool rejoined = b->top_para == a->top_para && b->top_off == 0;
    CHECK(forward || same_row || rejoined,
          "%s: the screen went back to line %zu cell %zu, neither its row nor its start, from line %zu cell %zu",
          what, b->top_para, b->top_off, a->top_para, a->top_off);
}

// ── building and reflowing ─────────────────────────────────────────────────
static grid_t make_grid(uint32_t cols, uint32_t rows, uint32_t total, uint32_t hist, uint32_t top)
{
    grid_t g = {.cols = cols, .rows = rows, .total = total, .screen_top = top, .hist = hist};
    g.cells = calloc((size_t)total * cols, sizeof(tty_cell_t));
    return g;
}

static void put_text(grid_t *g, uint32_t logical, const char *text, uint32_t color)
{
    tty_cell_t *r = row_of(g, logical);
    for (uint32_t i = 0; text[i] && i < g->cols; i++)
        r[i] = (tty_cell_t){.ch = text[i], .color = color};
}

// Reflow into a buffer of EXACTLY the planned size; returns the new grid.
static grid_t reflow_to(const grid_t *g, uint32_t nc, uint32_t nr, uint32_t max_total,
                        tty_reflow_out_t *report)
{
    tty_reflow_in_t in = as_input(g);
    tty_reflow_out_t plan, ran;
    bool ok = tty_reflow_plan(&in, nc, nr, max_total, &plan);
    CHECK(ok, "plan refused %ux%u -> %ux%u", g->cols, g->rows, nc, nr);
    grid_t n = {.cols = nc, .rows = nr};
    if (!ok) return n;
    n.total = plan.lines_needed;
    n.cells = calloc((size_t)n.total * nc, sizeof(tty_cell_t));
    ok = tty_reflow_run(&in, nc, nr, max_total, n.cells, n.total, &ran);
    CHECK(ok, "run refused what plan accepted");
    CHECK(memcmp(&plan, &ran, sizeof(plan)) == 0, "plan and run disagree");
    CHECK(ran.cur_row < nr && ran.cur_col < nc, "cursor off the new screen (%u,%u)", ran.cur_row, ran.cur_col);
    CHECK(ran.save_row < nr && ran.save_col < nc, "saved cursor off the new screen");
    CHECK(ran.view_offset <= ran.hist_lines, "view past the history");
    // The mark means something in ONE place. A cell that was a row's first
    // in the old shape lands mid-row in the new one, and must not bring its
    // mark with it; and the oldest row there is continues nothing.
    unsigned long stray = 0;
    for (uint32_t row = 0; row < n.total; row++)
        for (uint32_t x = row ? 1 : 0; x < nc; x++)
            stray += (n.cells[(size_t)row * nc + x].attrs & TTY_ATTR_WRAPPED) != 0;
    CHECK(stray == 0, "%lu cells carry the continuation mark where it means nothing", stray);
    n.hist = ran.hist_lines; n.screen_top = ran.hist_lines; n.view = ran.view_offset;
    n.cur_row = ran.cur_row; n.cur_col = ran.cur_col; n.save_row = ran.save_row; n.save_col = ran.save_col;
    if (!ran.history_dropped) {
        story_t was = read_story(g), is = read_story(&n);
        if (was.count == is.count) screen_top_held(&was, &is, nc, "screen top");
        free_story(&was); free_story(&is);
    }
    if (report) *report = ran;
    return n;
}

#define ROOMY 1000000u

static void test_wrap_and_rejoin(void)
{
    grid_t g = make_grid(20, 4, 16, 2, 5);           // ring rotated: screen_top 5
    put_text(&g, 0, "history line zero", 0xff0000);
    put_text(&g, 1, "0123456789ABCDEFGHIJ", 0x00ff00); // exactly full
    put_text(&g, 2, "short", 0x0000ff);
    put_text(&g, 3, "husk> ", 0xffffff);
    g.cur_row = 1; g.cur_col = 6;                     // after the prompt's trailing space

    tty_reflow_out_t r;
    grid_t n = reflow_to(&g, 8, 4, ROOMY, &r);
    story_t a = read_story(&g), b = read_story(&n);
    same_story(&a, &b, 0, "20 -> 8");
    CHECK(r.history_dropped == 0 && r.below_clipped == 0, "nothing lost narrowing");
    CHECK(row_of(&n, 0)[0].ch == 'h' && !(row_of(&n, 0)[0].attrs & TTY_ATTR_WRAPPED), "a line starts unmarked");
    CHECK(row_of(&n, 1)[0].ch == ' ' || row_of(&n, 1)[0].ch == 'l', "second row continues the first");
    CHECK(row_of(&n, 1)[0].attrs & TTY_ATTR_WRAPPED, "and says so");
    CHECK(row_of(&n, 3)[0].ch == '0' && row_of(&n, 5)[0].ch == 'G', "the full line became three rows");
    CHECK(n.cur_col == 6 && row_of(&n, n.hist + n.cur_row)[0].ch == 'h', "cursor kept its place after the prompt");

    grid_t back = reflow_to(&n, 20, 4, ROOMY, &r);
    story_t c = read_story(&back);
    same_story(&a, &c, 0, "20 -> 8 -> 20");
    for (uint32_t i = 0; i < a.count; i++)
        CHECK(!(row_of(&back, i)[0].attrs & TTY_ATTR_WRAPPED), "rejoined row %u carries no mark", i);
    CHECK(c.count == 4, "four lines again, not the eight rows they were (%zu)", c.count);

    free_story(&a); free_story(&b); free_story(&c);
    free(g.cells); free(n.cells); free(back.cells);
}

static void test_paint_is_content(void)
{
    // A space on a coloured background and a reversed blank are PICTURE.
    grid_t g = make_grid(10, 3, 6, 0, 0);
    put_text(&g, 0, "ab", 0xffffff);
    row_of(&g, 0)[9] = (tty_cell_t){.ch = ' ', .bg = 5};          // far end of the row
    row_of(&g, 1)[7] = (tty_cell_t){.ch = ' ', .attrs = OS64_ANSI_ATTR_REVERSE};
    g.cur_row = 2;                                  // under the picture, as a prompt would be
    grid_t n = reflow_to(&g, 4, 3, ROOMY, NULL);
    story_t a = read_story(&g), b = read_story(&n);
    same_story(&a, &b, 0, "painted blanks");
    CHECK(a.p[0].len == 10 && a.p[1].len == 8, "painted cells count toward a line's length");
    free_story(&a); free_story(&b); free(g.cells); free(n.cells);
}

static void test_history_fence(void)
{
    grid_t g = make_grid(12, 3, 12, 9, 0);
    for (uint32_t i = 0; i < 12; i++) { char t[16]; snprintf(t, sizeof t, "line %02u wide", i); put_text(&g, i, t, i); }
    g.cur_row = 2;
    tty_reflow_out_t r;
    grid_t n = reflow_to(&g, 6, 3, 10, &r);           // 24 rows wanted, 10 allowed
    CHECK(r.lines_needed == 10 && r.history_dropped == 14, "fence drops the oldest (%u needed, %u dropped)", r.lines_needed, r.history_dropped);
    CHECK(row_of(&n, 0)[0].ch != 0 && !(row_of(&n, 0)[0].attrs & TTY_ATTR_WRAPPED), "the first surviving row is nobody's continuation");
    story_t a = read_story(&g), b = read_story(&n);
    same_story(&a, &b, a.count - b.count, "fenced");
    CHECK(b.p[b.count - 1].len == 12 && b.p[b.count - 1].cells[6].ch == '1', "the newest line is whole");
    free_story(&a); free_story(&b); free(n.cells);

    // The same, with the fence falling in the MIDDLE of a line: what is left
    // of that line is its tail, and a tail at the top of the ring has no
    // head to continue. reflow_to's stray-mark check is what holds it.
    n = reflow_to(&g, 6, 3, 9, &r);
    CHECK(r.history_dropped == 15, "an odd drop cuts a two-row line in half (%u)", r.history_dropped);
    // "line 07 wide" at six columns is "line 0" then "7 wide".
    CHECK(row_of(&n, 0)[1].ch == ' ' && row_of(&n, 0)[2].ch == 'w',
          "the first surviving row is a line's second half ('%c%c%c')",
          row_of(&n, 0)[0].ch, row_of(&n, 0)[1].ch, row_of(&n, 0)[2].ch);
    free(n.cells);

    // Reading history while the fence throws away the line being read. The
    // view cannot point above the oldest line there still is, so it settles
    // on that line rather than off the top of the ring.
    g.view = 5;
    n = reflow_to(&g, 6, 3, 10, &r);
    CHECK(r.history_dropped > g.view, "the fence really did drop past the view (%u)", r.history_dropped);
    CHECK(r.view_offset == r.hist_lines, "the view settled on the oldest line (%u of %u)",
          r.view_offset, r.hist_lines);
    free(g.cells); free(n.cells);
}

static void test_screen_placement(void)
{
    // A short screen with no history: the screen must not move, and blank
    // rows under the text must not push text into history.
    grid_t g = make_grid(40, 10, 40, 0, 0);
    put_text(&g, 0, "boot line one", 1); put_text(&g, 1, "boot line two", 1); put_text(&g, 2, "husk> ", 1);
    g.cur_row = 2; g.cur_col = 6;
    tty_reflow_out_t r;
    grid_t n = reflow_to(&g, 80, 5, ROOMY, &r);
    CHECK(r.hist_lines == 0 && r.cur_row == 2 && r.cur_col == 6, "a short screen stays put (hist %u cur %u,%u)", r.hist_lines, r.cur_row, r.cur_col);
    free(n.cells);

    // Narrow enough that the cursor's line no longer fits: the TOP goes to
    // history, the cursor stays on the glass.
    n = reflow_to(&g, 4, 3, ROOMY, &r);
    CHECK(r.cur_row == 2 && r.hist_lines > 0, "cursor pinned to the last row, top pushed up");
    story_t a = read_story(&g), b = read_story(&n);
    same_story(&a, &b, 0, "pushed into history");
    free_story(&a); free_story(&b); free(n.cells);

    // A view scrolled back keeps the same line at its top.
    free(g.cells);
    g = make_grid(16, 3, 12, 6, 2);
    for (uint32_t i = 0; i < 9; i++) { char t[24]; snprintf(t, sizeof t, "L%u-abcdefghijkl", i); put_text(&g, i, t, i); }
    g.view = 4; g.cur_row = 2;                          // view top = logical row 2
    n = reflow_to(&g, 5, 3, ROOMY, &r);
    a = read_story(&g); b = read_story(&n);
    CHECK(r.view_offset > 0 && a.view_para == b.view_para && a.view_para == 2, "view stays on line %zu (was %zu)", b.view_para, a.view_para);
    g.view = 0;
    grid_t live = reflow_to(&g, 5, 3, ROOMY, &r);
    CHECK(r.view_offset == 0, "a live view stays live");
    free_story(&a); free_story(&b); free(g.cells); free(n.cells); free(live.cells);
}

// A blank marked row at the screen top, over a paragraph whose length is an
// exact multiple of the new width: the cursor sits past the end of a wrapped
// line whose head has since aged into history. Placing the screen anchor
// before the cursor grew the paragraph's row count started the live screen
// one row up, on the tail of a history line.
static void test_screen_top_over_a_wrapped_head(void)
{
    grid_t g = make_grid(10, 2, 6, 1, 1);
    put_text(&g, 0, "0123456789", 0xffffff);      // exactly one full row
    row_of(&g, 1)[0].attrs |= TTY_ATTR_WRAPPED;   // its blank continuation
    g.cur_row = 0;                                 // the cursor parks there

    tty_reflow_out_t r;
    grid_t n = reflow_to(&g, 5, 2, ROOMY, &r);     // 10 cells into rows of 5
    CHECK(r.hist_lines == 2 && r.cur_row == 0,
          "a history line stayed history (hist %u, cur_row %u)", r.hist_lines, r.cur_row);
    CHECK(row_of(&n, 0)[0].ch == '0' && row_of(&n, 1)[0].ch == '5',
          "both halves of the history line are above the screen");
    story_t a = read_story(&g), b = read_story(&n);
    same_story(&a, &b, 0, "wrapped head in history");
    free_story(&a); free_story(&b); free(g.cells); free(n.cells);

    // The same shape with the cursor MOVED ON to a later line. Growing the
    // cursor's own paragraph cannot cover this one: the screen top sits at
    // the exact end of an earlier paragraph's text, in a blank tail that is
    // rows in the old shape and no rows in the new, so it has to land after
    // that paragraph rather than being pulled back onto its last row.
    g = make_grid(10, 2, 6, 1, 1);
    put_text(&g, 0, "0123456789", 0xffffff);
    row_of(&g, 1)[0].attrs |= TTY_ATTR_WRAPPED;
    put_text(&g, 2, "next", 0xffffff);
    g.cur_row = 1;                                 // on "next", not on the blank tail

    n = reflow_to(&g, 5, 2, ROOMY, &r);
    CHECK(r.hist_lines == 2, "the whole wrapped line stayed history (hist %u)", r.hist_lines);
    CHECK(row_of(&n, n.hist + n.cur_row)[0].ch == 'n', "the cursor is still on 'next'");
    a = read_story(&g); b = read_story(&n);
    same_story(&a, &b, 0, "wrapped head, cursor moved on");
    free_story(&a); free_story(&b); free(g.cells); free(n.cells);
}

static void test_refusals(void)
{
    grid_t g = make_grid(10, 3, 6, 0, 0);
    tty_reflow_in_t in = as_input(&g), bad;
    tty_reflow_out_t out;
    tty_cell_t scratch[64] = {0};
    CHECK(!tty_reflow_plan(&in, 1, 3, ROOMY, &out), "one column");
    CHECK(!tty_reflow_plan(&in, 10, 1, ROOMY, &out), "one row");
    CHECK(!tty_reflow_plan(&in, 10, 3, 2, &out), "a fence under one screen");
    bad = in; bad.cur_col = 10; CHECK(!tty_reflow_plan(&bad, 5, 3, ROOMY, &out), "cursor past its own columns");
    bad = in; bad.cur_row = 3;  CHECK(!tty_reflow_plan(&bad, 5, 3, ROOMY, &out), "cursor past its own rows");
    bad = in; bad.hist_lines = 4; CHECK(!tty_reflow_plan(&bad, 5, 3, ROOMY, &out), "more history than the ring holds");
    bad = in; bad.cells = NULL; CHECK(!tty_reflow_plan(&bad, 5, 3, ROOMY, &out), "no ring");
    CHECK(!tty_reflow_run(&in, 4, 3, ROOMY, scratch, 2, &out), "a buffer under the plan");
    CHECK(scratch[0].ch == 0, "a refused run writes nothing");
    CHECK(!tty_reflow_run(&in, 4, 3, ROOMY, NULL, 3, &out), "no buffer");
    free(g.cells);
}

// Random grids through random chains of shapes and home again.
static void test_chains(unsigned rounds)
{
    for (unsigned t = 0; t < rounds; t++) {
        uint32_t cols = 8 + rnd(60), rows = 2 + rnd(20);
        uint32_t total = rows * (1 + rnd(4)), hist = rnd(total - rows + 1);
        grid_t g = make_grid(cols, rows, total, hist, rnd(total));
        uint32_t used = hist + 1 + rnd(rows);                 // rows with text; the rest is blank screen
        for (uint32_t r = 0; r < used; r++) {
            uint32_t len = rnd(4) == 0 ? 0 : 1 + rnd(cols);
            tty_cell_t *row = row_of(&g, r);
            for (uint32_t x = 0; x < len; x++)
                row[x] = (tty_cell_t){.ch = (char)('!' + rnd(90)), .attrs = (uint8_t)rnd(4),
                                      .bg = (uint8_t)rnd(3), .charset = (uint8_t)rnd(2), .color = rnd(0xffffff)};
            // MARKS IN THE STARTING GRID, not only in what a hop produced: a
            // grid arrives at a font change carrying the marks of the LAST
            // one, and a blank marked row — the tail of a wrapped line the
            // cursor parks past — is the shape that put a history line back
            // on the live screen.
            if (r > 0 && rnd(3) == 0)
                row[0].attrs |= TTY_ATTR_WRAPPED;
        }
        // The cursor is usually on the last written row, but not always: a
        // program that positions it leaves text below, and that text is what
        // decides how far the screen slides.
        g.cur_row = rnd(4) == 0 ? rnd(rows) : used - 1 - hist;
        g.cur_col = rnd(cols);
        g.save_row = rnd(rows); g.save_col = rnd(cols);
        g.view = rnd(hist + 1);

        story_t home = read_story(&g);
        grid_t at = g; bool owned = false, lost = false;
        unsigned hops = 1 + rnd(4);
        for (unsigned h = 0; h <= hops; h++) {
            uint32_t nc = h == hops ? cols : 2 + rnd(90), nr = h == hops ? rows : 2 + rnd(30);
            tty_reflow_out_t r;
            grid_t next = reflow_to(&at, nc, nr, ROOMY, &r);
            if (!next.cells) break;
            // THE ONE LOSS THE REFLOW ADMITS TO: text under the cursor that
            // no longer fits beneath it on a shorter screen. It is counted,
            // and a chain that hit it has no story left to compare — but it
            // may only happen when the cursor really is pinned to row 0.
            if (r.below_clipped) {
                clipped_chains++;
                CHECK(r.cur_row == 0, "clipped %u rows with the cursor on row %u", r.below_clipped, r.cur_row);
                if (owned) free(at.cells);
                at = next; owned = true; lost = true;
                break;
            }
            story_t now = read_story(&next);
            same_story(&home, &now, 0, "chain hop");
            free_story(&now);
            if (owned) free(at.cells);
            at = next; owned = true;
        }
        // Home again with the same lines it left with, which is what makes
        // the round trip REVERSIBLE and not merely lossless: a line split
        // across rows by a narrow grid has to rejoin into one line, not
        // stay two. The marks need no check of their own — a paragraph is
        // read back THROUGH them, so a mark that went missing or was
        // invented splits or merges a line and changes this count.
        if (owned && !lost && at.cols == cols) {
            story_t end = read_story(&at);
            CHECK(end.count == home.count, "came home with %zu lines, left with %zu", end.count, home.count);
            free_story(&end);
        }
        free_story(&home);
        if (owned) free(at.cells);
        free(g.cells);
    }
}

int main(void)
{
    test_wrap_and_rejoin();
    test_paint_is_content();
    test_history_fence();
    test_screen_placement();
    test_screen_top_over_a_wrapped_head();
    test_refusals();
    test_chains(4000);
    printf("test_tty_reflow_host: %lu checks, %lu failures (%lu chains ended at an admitted clip)\n",
           checks, failures, clipped_chains);
    return failures ? 1 : 0;
}
