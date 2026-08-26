// grootmenu(1) — the root menu. Click the wallpaper and the system offers
// itself to you: twm's "defops" menu, 1987, and the piece of GNOME 2 Chris
// missed the most, rebuilt as the smallest possible program.
//
// IT IS NOT PART OF THE DESKTOP. /bin/desktop owns the wallpaper window and
// gui.conf names this program as its `launcher`; a click on bare desktop
// spawns it with the click's position, it draws a tree it did not parse
// (os64/menu.h — the grammar every launcher shares), and it exits the
// moment you choose something or look away. The chosen program is spawned
// from here and orphaned on purpose: the kernel re-parents it to ktask with
// autoReap, so nothing waits on it and nothing leaks. Tomorrow's dock is
// another program reading the same menu.conf, added by another gui.conf
// line — that, not this file, is the design.
//
//   grootmenu [x y] [name]     open `name` (default "root") with its top-left
//                              corner at screen (x, y); default 0,0
//
// HOW IT GOES AWAY: a menu must vanish when you click anywhere else, and
// "anywhere else" is a focus change — OS64_GUI_EVENT_WINDOW_FOCUS, added for
// exactly this program. Focus leaving for a window that is NOT ours ends
// the menu; focus moving between our own windows (a cascade) does not.
// Escape ends it too. There is no "click outside" event and never will be:
// the click landed on someone else's window and is theirs.
//
// CASCADES are windows: hovering a submenu row opens a second, PINNED,
// undecorated window beside the row, and so on down. Every level is a
// small struct here; the tree stays in the library's arena.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/ui.h"
#include "os64/menu.h"
#include "os64/proc.h"
#include "os64/io.h"
#include "os64/fmt.h"
#include "os64/str.h"

// Open levels at once. The grammar's own ceiling, not a number of our own:
// a cascade the parser accepts and the launcher will not open is an arrow
// that answers a hover with nothing.
#define MENU_DEPTH_MAX  OS64_MENU_DEPTH_MAX
#define MENU_ROWS_MAX   64       // rows per level
#define ROW_PAD         3        // pixels above and below a row's text
#define SEP_H           7        // a separator row
#define MIN_WIDTH       96
#define CASCADE_OVERLAP 3        // a child sits this many pixels into its parent
#define ARGV_MAX        16

typedef struct
{
    int64_t         win;
    os64_draw_ctx_t ctx;
    int32_t         x, y;            // frame origin on screen
    uint32_t        fw, fh;          // frame size
    int32_t         cw, ch;          // content size
    int16_t         rows[MENU_ROWS_MAX];   // node per row
    int32_t         row_y[MENU_ROWS_MAX + 1];   // content-local top of each row; [n] = bottom
    int             n;
    int             hi;              // highlighted row, -1 = none
    int             open_row;        // the row whose cascade is open, -1 = none
    bool            has_sub;         // any cascade rows (width reserves the arrow)
} level_t;

static os64_menu_t     gMenu;
static os64_ui_theme_t gTheme;
static level_t         gLevels[MENU_DEPTH_MAX];
static int             gDepth;
static uint32_t        gScreenW, gScreenH;
static bool            gQuit;

// ── painting ────────────────────────────────────────────────────────────────

static void paint_row(level_t *lv, int r)
{
    const os64_menu_node_t *node = &gMenu.nodes[lv->rows[r]];
    os64_gui_surface_t *s = &lv->ctx.surf;
    os64_gui_rect_t rect = { 0, lv->row_y[r], lv->cw, lv->row_y[r + 1] - lv->row_y[r] };

    if (node->kind == OS64_MENU_SEPARATOR) {
        os64_draw_fill_rect(s, rect, gTheme.menu_bg);
        os64_draw_hline(s, gTheme.pad / 2, rect.y + SEP_H / 2,
                        lv->cw - gTheme.pad, gTheme.menu_sep);
        return;
    }
    bool hi = (r == lv->hi);
    uint32_t bg = hi ? gTheme.menu_hi_bg : gTheme.menu_bg;
    uint32_t fg = hi ? gTheme.menu_hi_fg : gTheme.menu_fg;
    os64_draw_fill_rect(s, rect, bg);
    os64_draw_text(s, gTheme.pad, rect.y + ROW_PAD, node->label,
                   os64_strlen(node->label), fg, bg);
    if (node->kind == OS64_MENU_SUBMENU)
        os64_draw_text(s, lv->cw - gTheme.pad - gTheme.font_w, rect.y + ROW_PAD,
                       ">", 1, fg, bg);
}

static void paint(level_t *lv)
{
    os64_gui_rect_t all = { 0, 0, lv->cw, lv->ch };
    os64_draw_fill_rect(&lv->ctx.surf, all, gTheme.menu_bg);
    for (int r = 0; r < lv->n; r++)
        paint_row(lv, r);
    os64_draw_publish(&lv->ctx, NULL);
}

static void repaint_row(level_t *lv, int r)
{
    if (r < 0 || r >= lv->n)
        return;
    paint_row(lv, r);
    os64_gui_rect_t rect = { 0, lv->row_y[r], lv->cw, lv->row_y[r + 1] - lv->row_y[r] };
    os64_draw_publish(&lv->ctx, &rect);
}

static int row_at(const level_t *lv, int32_t y)
{
    for (int r = 0; r < lv->n; r++)
        if (y >= lv->row_y[r] && y < lv->row_y[r + 1])
            return r;
    return -1;
}

// ── levels ──────────────────────────────────────────────────────────────────

static void close_deeper_than(int d)
{
    while (gDepth > d + 1) {
        level_t *lv = &gLevels[--gDepth];
        os64_gui_window_destroy(lv->win);
        lv->win = 0;
    }
    if (d >= 0 && d < gDepth)
        gLevels[d].open_row = -1;
}

// Open the menu whose first child is `first`, top-left at screen (sx, sy),
// clamped so the whole thing is on the glass. Returns false if the tree is
// empty or the window could not be made.
static bool open_level(int16_t first, int32_t sx, int32_t sy)
{
    if (gDepth >= MENU_DEPTH_MAX || first < 0)
        return false;
    level_t *lv = &gLevels[gDepth];
    os64_memset(lv, 0, sizeof(*lv));
    lv->hi = -1;
    lv->open_row = -1;

    // Lay the rows out and measure the widest label.
    size_t widest = 0;
    int32_t y = 1;   // one pixel of the panel colour above the first row
    int16_t i = first;
    for (; i >= 0 && lv->n < MENU_ROWS_MAX; i = gMenu.nodes[i].next) {
        const os64_menu_node_t *node = &gMenu.nodes[i];
        lv->rows[lv->n] = i;
        lv->row_y[lv->n] = y;
        if (node->kind == OS64_MENU_SEPARATOR) {
            y += SEP_H;
        } else {
            y += gTheme.font_h + 2 * ROW_PAD;
            size_t len = os64_strlen(node->label);
            if (len > widest)
                widest = len;
            if (node->kind == OS64_MENU_SUBMENU)
                lv->has_sub = true;
        }
        lv->n++;
    }
    // A level past its capacity is REFUSED, not shown in part: half a menu is
    // a different menu, and the rows that vanished cannot be launched or even
    // seen to be missing. Same ruling as os64_menu_argv's.
    if (i >= 0) {
        os64_complain("grootmenu: a menu has more than %d rows — refused rather than shown in part\n",
                     MENU_ROWS_MAX);
        return false;
    }
    if (lv->n == 0)
        return false;
    lv->row_y[lv->n] = y;
    lv->ch = y + 1;
    lv->cw = 2 * gTheme.pad + (int32_t)widest * gTheme.font_w +
             (lv->has_sub ? 2 * gTheme.font_w : 0);
    if (lv->cw < MIN_WIDTH)
        lv->cw = MIN_WIDTH;

    const uint64_t flags = OS64_GUI_WINDOW_NO_DECORATIONS | OS64_GUI_WINDOW_PINNED;
    os64_gui_frame_for_content((uint32_t)lv->cw, (uint32_t)lv->ch, flags, &lv->fw, &lv->fh);

    // A level bigger than the glass cannot be slid into view — the clamp
    // below would leave its far edge permanently off-screen, where rows can
    // be neither seen nor chosen. Refused, like a level past the row ceiling.
    if (lv->fw > gScreenW || lv->fh > gScreenH) {
        os64_complain("grootmenu: a menu is %ux%u, larger than the %ux%u screen — refused\n",
                     lv->fw, lv->fh, gScreenW, gScreenH);
        return false;
    }

    // Keep it on the glass: slide left/up rather than clip, the way every
    // menu since the Lisa has.
    if (sx + (int32_t)lv->fw > (int32_t)gScreenW) sx = (int32_t)gScreenW - (int32_t)lv->fw;
    if (sy + (int32_t)lv->fh > (int32_t)gScreenH) sy = (int32_t)gScreenH - (int32_t)lv->fh;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    lv->x = sx;
    lv->y = sy;

    lv->win = os64_gui_window_create("grootmenu", sx, sy, lv->fw, lv->fh, flags);
    if (lv->win <= 0) {
        os64_complain("grootmenu: window_create failed (%ld)\n", (long)lv->win);
        return false;
    }
    if (os64_draw_ctx_init(&lv->ctx, lv->win) != 0) {
        os64_gui_window_destroy(lv->win);
        lv->win = 0;
        return false;
    }
    gDepth++;
    paint(lv);
    return true;
}

// Open row r's cascade beside it (closing any other cascade of this level).
static void open_cascade(int d, int r)
{
    level_t *lv = &gLevels[d];
    if (lv->open_row == r)
        return;
    close_deeper_than(d);
    const os64_menu_node_t *node = &gMenu.nodes[lv->rows[r]];
    if (node->first_child < 0)
        return;   // an empty cascade opens nothing, and says nothing
    int32_t sx = lv->x + (int32_t)lv->fw - CASCADE_OVERLAP;
    int32_t sy = lv->y + lv->row_y[r];
    if (open_level(node->first_child, sx, sy))
        lv->open_row = r;
}

// ── choosing ────────────────────────────────────────────────────────────────

static void launch(const os64_menu_node_t *node)
{
    char  buf[OS64_MENU_COMMAND_MAX];
    char *argv[ARGV_MAX];
    int64_t argc = os64_menu_argv(node->command, buf, sizeof(buf), argv, ARGV_MAX);
    if (argc < 0) {
        os64_complain("grootmenu: \"%s\": command refused — over %d words, or too long\n",
                     node->label, ARGV_MAX - 1);
        return;
    }
    if (argc == 0) {
        os64_complain("grootmenu: \"%s\": empty command\n", node->label);
        return;
    }
    int64_t pid = os64_spawn(argv[0], argv);
    if (pid < 0)
        os64_complain("grootmenu: \"%s\": could not start %s (%ld)\n",
                     node->label, argv[0], (long)pid);
}

static void handle(int d, const os64_gui_event_t *ev)
{
    level_t *lv = &gLevels[d];
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_MOVE: {
        int r = row_at(lv, ev->mouse.y);
        if (r >= 0 && gMenu.nodes[lv->rows[r]].kind == OS64_MENU_SEPARATOR)
            r = -1;
        if (r != lv->hi) {
            int old = lv->hi;
            lv->hi = r;
            repaint_row(lv, old);
            repaint_row(lv, r);
        }
        if (r >= 0) {
            if (gMenu.nodes[lv->rows[r]].kind == OS64_MENU_SUBMENU)
                open_cascade(d, r);
            else if (lv->open_row >= 0)
                close_deeper_than(d);
        }
        break;
    }
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
        int r = row_at(lv, ev->mouse.y);
        if (r < 0)
            break;
        const os64_menu_node_t *node = &gMenu.nodes[lv->rows[r]];
        if (node->kind == OS64_MENU_ITEM) {
            launch(node);
            gQuit = true;
        } else if (node->kind == OS64_MENU_SUBMENU) {
            open_cascade(d, r);
        }
        break;
    }
    case OS64_GUI_EVENT_KEY_DOWN:
        if (os64_ui_key_is_esc(ev))
            gQuit = true;
        break;
    case OS64_GUI_EVENT_WINDOW_FOCUS:
        // Focus left one of our windows for somebody else's: the user
        // looked away, and a menu that stays is a menu in the way. Focus
        // moving between our own levels is the cascade working.
        if (!ev->focus.gained && !ev->focus.sibling)
            gQuit = true;
        break;
    case OS64_GUI_EVENT_WINDOW_CLOSE:
        gQuit = true;
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    // `grootmenu [x y] [name]`, decided by ARITY, not by what the argument
    // looks like. Sniffing the first character for a digit or '-' made a
    // menu legitimately named "123" or "-hot" impossible to open: it was
    // read as a third coordinate and the name silently stayed "root". A
    // name is data, so nothing about its spelling may change how it is read.
    int32_t x = 0, y = 0;
    const char *name = "root";
    if (argc == 2) {
        name = argv[1];              // a name alone; one coordinate means nothing
    } else if (argc >= 3) {
        x = (int32_t)os64_atoi(argv[1]);
        y = (int32_t)os64_atoi(argv[2]);
        if (argc >= 4)
            name = argv[3];
    }

    if (os64_gui_screen_info(&gScreenW, &gScreenH) != 0 || gScreenW == 0) {
        os64_complain("grootmenu: no GUI here\n");
        return 1;
    }

    char err[160];
    os64_menu_status_t st = os64_menu_load(&gMenu, "menu.conf", err, sizeof(err));
    if (st != OS64_MENU_OK) {
        // Say WHICH file and WHY: a menu that does not appear is a config
        // bug with no other handle on it.
        if (err[0])
            os64_complain("grootmenu: %s\n", err);
        else
            os64_complain("grootmenu: %s: %s\n", gMenu.path, os64_menu_status_name(st));
        return 1;
    }
    if (!os64_menu_named_exists(&gMenu, name)) {
        os64_complain("grootmenu: %s defines no menu named \"%s\"\n",
                     gMenu.path, name);
        return 1;
    }

    os64_ui_theme_init(&gTheme);
    if (!open_level(os64_menu_find(&gMenu, name), x, y)) {
        os64_complain("grootmenu: menu \"%s\" is empty\n", name);
        return 1;
    }

    // Several windows, one program: poll every level's queue at frame
    // cadence. event_wait blocks on ONE window, and the pointer wanders
    // between levels; a short-lived menu can afford the ~30Hz poll.
    os64_frame_clock_t clock;
    os64_frame_clock_init(&clock);
    while (!gQuit) {
        for (int d = 0; d < gDepth && !gQuit; d++) {
            os64_gui_event_t ev;
            while (os64_gui_event_poll(gLevels[d].win, &ev) == 1) {
                handle(d, &ev);
                if (gQuit || d >= gDepth)
                    break;   // this level may have been closed by the handler
            }
        }
        if (!gQuit)
            os64_frame_wait(&clock, 33);
    }

    close_deeper_than(-1);
    return 0;
}
