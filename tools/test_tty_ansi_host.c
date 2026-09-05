// Exercise the actual tty action application and selection repaint on a
// small in-memory grid. Renderer stubs record colours instead of using VRAM.
// Unused kernel entry points are discarded by --gc-sections at link time.
#include "../kernel/src/tty.c"
#include "../kernel/src/vt_select.c"

extern void abort(void);
extern int puts(const char *s);

volatile uint64_t kTicksSinceStart;
static uint32_t painted_fg[4][8], painted_bg[4][8];
static uint32_t paper;
static unsigned paints;

static void check(bool ok, const char *why)
{
    if (!ok) { puts(why); abort(); }
}

uint64_t renderer_glass_begin(void) { return 0; }
void renderer_glass_end(uint64_t f, uint32_t r, uint32_t c, bool show)
{ (void)f; (void)r; (void)c; (void)show; }
void renderer_glass_defer_locked(void) {}
void renderer_glass_blit_locked(void) { paints++; }
void renderer_glass_clear_locked(void) {}
void renderer_glass_background_locked(uint32_t c) { paper = c; }
void renderer_glass_putc_bg_locked(char ch, uint32_t r, uint32_t c,
                                  uint32_t fg, uint32_t bg)
{
    (void)ch;
    check(r < 4 && c < 8, "paint outside fixture");
    painted_fg[r][c] = fg;
    painted_bg[r][c] = bg;
}
bool gui_vt8_seated(void) { return false; }

static void feed(tty_t *t, const char *s)
{
    bool glass = false;
    for (; *s; s++) {
        ansi_action_t a = ansi_feed(&t->ansi, *s);
        tty_apply_locked(t, &a, &glass);
    }
}

int main(void)
{
    tty_cell_t cells[6 * 8] = {0};
    tty_t t = {.cells = cells, .cols = 8, .rows = 4, .total_lines = 6,
               .color = TTY_DEFAULT_FG, .fg_index = TTY_FG_NOT_INDEXED,
               .glass_bg = 0x123456};
    kTTYFocused = &t;
    feed(&t, "\033[1;7;32;44m");
    uint32_t fg = t.color;
    uint8_t bg = t.bg, attrs = t.attrs;
    const char *groups[] = {
        "\033[38;5;31m", "\033[48;2;255;0;0m", "\033[58;5;0m",
        "\033[38;2;0m", "\033[48;99;0m"
    };
    for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        feed(&t, groups[i]);
        check(t.color == fg && t.bg == bg && t.attrs == attrs,
              "unsupported colour changed rendition");
    }
    feed(&t, "\033[38;5;31;27m");
    check(t.attrs == OS64_ANSI_ATTR_BOLD, "SGR after extended group was lost");
    feed(&t, "\033[0mABCD\033[2;3H");
    tty_cell_t saved[6 * 8];
    memcpy(saved, cells, sizeof(cells));
    feed(&t, "\033[3J\033[99J\033[3K\033[99K");
    for (unsigned i = 0; i < sizeof(cells); i++)
        check(((char *)cells)[i] == ((char *)saved)[i], "unsupported erase changed grid");
    check(t.cur_row == 1 && t.cur_col == 2, "unsupported erase moved cursor");
    feed(&t, "\033[1;2H\033[1K");
    check(!cells[0].ch && !cells[1].ch && cells[2].ch == 'C', "EL1 span");
    feed(&t, "\033[2J");
    check(!cells[2].ch && t.cur_col == 1, "ED2 must erase without homing");

    feed(&t, "\033[H\033[31;44;7mX");
    s_have_ptr = false;
    paint_row_locked(&t, 0);
    check(painted_fg[0][0] == os64_ansi_color(4) &&
          painted_bg[0][0] == os64_ansi_color(1), "selection painter lost reverse or background");
    s_have_ptr = true; s_ptr_row = 0; s_ptr_col = 0;
    paint_row_locked(&t, 0);
    check(painted_fg[0][0] == os64_ansi_color(1) &&
          painted_bg[0][0] == os64_ansi_color(4), "pointer did not invert resolved colours");

    t.screen_top = 1; t.hist_lines = 1; t.view_offset = 1;
    s_glassStale = false;
    feed(&t, "\033]11;#abcdef\007");
    check(s_glassStale, "OSC in history did not request repaint");
    tty_repaint_locked(&t);
    check(paper == 0xabcdef && paints == 1 && t.view_offset == 1,
          "history repaint changed view or lost paper");
    check(painted_bg[0][1] == 0xabcdef, "history default cells retained old paper");

    // Erasure uses SGR's background, not the foreground that reverse video
    // puts behind text. Erasing must not change the pen for subsequent text.
    feed(&t, "\033[H\033[31;44;7mX\033[2K");
    tty_cell_t *line = tty_line(&t, tty_row_line(&t, 0));
    for (unsigned c = 0; c < t.cols; c++) {
        uint32_t erase_fg, erase_bg;
        tty_cell_colors(&t, &line[c], &erase_fg, &erase_bg);
        check(!line[c].ch && !line[c].attrs && erase_bg == os64_ansi_color(4),
              "reverse erasure must retain SGR background without reverse");
    }
    check(t.attrs == OS64_ANSI_ATTR_REVERSE, "erase changed the pen attributes");

    // Both newline and right-edge wrapping create a bottom row. Exercise
    // ring reuse as well as the first scroll, with reverse still enabled.
    for (unsigned scroll = 0; scroll < 8; scroll++) {
        feed(&t, scroll % 2 ? "\033[4;8HX" : "\033[4;1H\n");
        line = tty_line(&t, tty_row_line(&t, t.rows - 1));
        for (unsigned c = 0; c < t.cols; c++) {
            uint32_t scroll_fg, scroll_bg;
            tty_cell_colors(&t, &line[c], &scroll_fg, &scroll_bg);
            check(!line[c].ch && !line[c].attrs && scroll_bg == os64_ansi_color(4),
                  "scrolled-in blank lost SGR background");
        }
    }
    feed(&t, "\033[0m");
    uint32_t scroll_fg, scroll_bg;
    tty_cell_colors(&t, &line[0], &scroll_fg, &scroll_bg);
    check(scroll_bg == os64_ansi_color(4), "pen reset recoloured scrolled-in blank");
    puts("test_tty_ansi_host: all checks passed");
    return 0;
}
