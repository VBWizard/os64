// Drive husk's real line editor on the host and watch what it draws.
//
// read_line runs against scripted keystrokes, and every byte it writes goes
// to TWO model terminals at once: one that wraps the way os64's tty does (the
// moment a glyph lands in the last column) and one that wraps the way a VT100
// does (the cursor waits on the last column for the next glyph). Before each
// key is handed over, both screens must show the prompt and the line laid out
// at the terminal's width, blank after the end, with the cursor on the caret
// — the same picture on both kinds of terminal is the whole point of the
// editor's screen model. A reference editor says what the line should hold.
//
// The VT100 model also refuses a cursor move made while the cursor waits on
// the last column: what a terminal does then is exactly where they disagree,
// so the editor must never ask.
#include "os64/os64.h"

static int64_t test_signal_set_handler(int signo, os64_signal_fn handler);
#define os64_signal_set_handler test_signal_set_handler
#define main husk_main
#include "../userland/apps/husk/husk.c"
#undef main
#undef os64_signal_set_handler
#include "ansi.h"

extern void abort(void);
extern int puts(const char *s);
extern int printf(const char *fmt, ...);
extern void *memset(void *s, int c, size_t n);
extern void *memmove(void *dst, const void *src, size_t n);
extern int fflush(void *stream);

// Flushed first: stdout is buffered, and an abort would take the reason with it.
static void fail(void)
{
    fflush(NULL);
    abort();
}

static void check(bool ok, const char *why)
{
    if (!ok) { puts(why); fail(); }
}

// ── the model terminals ─────────────────────────────────────────────────────
enum { ROWS_MAX = 48, COLS_MAX = 128 };

typedef struct {
    const char *name;
    bool vt;                 // VT100 last-column rule; false = os64's tty
    int w, h;
    char cell[ROWS_MAX][COLS_MAX];
    int row, col;
    bool pending;            // VT100: a glyph is sitting in the last column
    long scrolled;           // rows scrolled off the top, ever
    ansi_parser_t parser;
    long prompt_abs;         // absolute row the current prompt was drawn on
} term_t;

static term_t s_terms[2];

static void term_init(term_t *t, const char *name, bool vt, int w, int h)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->vt = vt;
    t->w = w;
    t->h = h;
    memset(t->cell, ' ', sizeof(t->cell));
}

static void term_linefeed(term_t *t)
{
    if (++t->row < t->h)
        return;
    memmove(t->cell[0], t->cell[1], (size_t)(t->h - 1) * sizeof(t->cell[0]));
    memset(t->cell[t->h - 1], ' ', sizeof(t->cell[0]));
    t->row = t->h - 1;
    t->scrolled++;
}

static void term_glyph(term_t *t, char c)
{
    if (t->vt)
    {
        if (t->pending)
        {
            t->pending = false;
            t->col = 0;
            term_linefeed(t);
        }
        t->cell[t->row][t->col] = c;
        if (t->col == t->w - 1)
            t->pending = true;
        else
            t->col++;
        return;
    }
    t->cell[t->row][t->col] = c;
    if (++t->col >= t->w)
    {
        t->col = 0;
        term_linefeed(t);
    }
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void term_feed(term_t *t, char byte)
{
    ansi_action_t a = ansi_feed(&t->parser, byte);
    switch (a.kind)
    {
    case ANSI_NOTHING:
    case ANSI_SGR:
        return;
    case ANSI_PRINT:
        if (a.byte == '\n')           // os64's newline; sshd and telnetd send CR LF
        {
            t->pending = false;
            t->col = 0;
            term_linefeed(t);
        }
        else if (a.byte == '\r')
        {
            t->pending = false;
            t->col = 0;
        }
        else
        {
            check((unsigned char)a.byte >= 0x20, "editor wrote a control byte the models do not speak");
            term_glyph(t, a.byte);
        }
        return;
    case ANSI_CURSOR_MOVE:
        check(!t->pending, "cursor moved while waiting on the last column");
        t->row = clamp(t->row + a.drow, 0, t->h - 1);
        t->col = clamp(t->col + a.dcol, 0, t->w - 1);
        return;
    case ANSI_ERASE_DISPLAY:
        check(!t->pending, "erase while waiting on the last column");
        check(a.nparams == 0 || a.params[0] == 0, "only erase-below is expected");
        for (int c = t->col; c < t->w; c++)
            t->cell[t->row][c] = ' ';
        for (int r = t->row + 1; r < t->h; r++)
            memset(t->cell[r], ' ', sizeof(t->cell[0]));
        return;
    default:
        check(false, "editor wrote an escape the models do not speak");
    }
}

// A window resize that keeps every row where it was and each row's left edge,
// as os64's tty_resize does (and xterm, which does not reflow).
static void term_resize(term_t *t, int w)
{
    for (int r = 0; r < t->h; r++)
        for (int c = w; c < COLS_MAX; c++)
            t->cell[r][c] = ' ';
    t->w = w;
    t->pending = false;
    if (t->col >= w)
        t->col = w - 1;
}

// ── the stubs husk.c links against ──────────────────────────────────────────
static int s_width;                   // what /proc/self/tty says; 0 = cannot say
static os64_signal_fn s_winch_handler = OS64_SIG_DEFAULT;

static int64_t test_signal_set_handler(int signo, os64_signal_fn handler)
{
    check(signo == OS64_SIGWINCH, "husk installed a handler for another signal");
    os64_signal_fn old = s_winch_handler;
    s_winch_handler = handler;
    return (int64_t)(intptr_t)old;
}

int32_t os64_tty_read(os64_tty_info_t *out)
{
    // prompt_draw asks for the width just before it writes the prompt, which
    // is how the checker learns where the prompt begins.
    for (int i = 0; i < 2; i++)
    {
        check(s_terms[i].col == 0 && !s_terms[i].pending, "prompt drawn away from column 0");
        s_terms[i].prompt_abs = s_terms[i].row + s_terms[i].scrolled;
    }
    if (s_width == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->cols = (uint32_t)s_width;
    out->rows = (uint32_t)s_terms[0].h;
    return 0;
}

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    check(handle == 1, "the line editor wrote somewhere other than stdout");
    for (size_t i = 0; i < len; i++)
        for (int t = 0; t < 2; t++)
            term_feed(&s_terms[t], ((const char *)buf)[i]);
    return (int64_t)len;
}

const char *s_prompt_fmt;             // $PROMPT; NULL = the default
const char *os64_getenv(const char *name)
{
    return str_eq(name, "PROMPT") ? s_prompt_fmt : NULL;
}
int64_t os64_getcwd(char *buf, size_t len) { (void)buf; (void)len; return -1; }
int64_t os64_date_now(os64_date_t *out, os64_time_t *raw) { (void)out; (void)raw; return -1; }
int64_t os64_opendir(const char *path) { (void)path; return -1; }
int64_t os64_readdir(int32_t handle, os64_dirent_t *out) { (void)handle; (void)out; return 0; }
int64_t os64_close(int32_t handle) { (void)handle; return 0; }
size_t os64_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
bool os64_streq(const char *a, const char *b) { return str_eq(a, b); }
size_t os64_strcopy(char *dst, size_t cap, const char *src)
{
    size_t len = os64_strlen(src);
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
    return len;
}

// ── the reference editor ────────────────────────────────────────────────────
static struct {
    char buf[LINE_MAX];
    int n, pos;
    char hist[32][LINE_MAX];
    int hist_count, browse;
    char live[LINE_MAX];
} R;

static void ref_set(const char *s)
{
    R.n = (int)os64_strcopy(R.buf, LINE_MAX, s);
    R.pos = R.n;
}

static void ref_insert(const char *s)
{
    for (; *s && R.n < LINE_MAX - 1; s++)
    {
        memmove(R.buf + R.pos + 1, R.buf + R.pos, (size_t)(R.n - R.pos));
        R.buf[R.pos++] = *s;
        R.n++;
    }
    R.browse = 0;
}

static void ref_delete(int start, int count)
{
    memmove(R.buf + start, R.buf + start + count, (size_t)(R.n - start - count));
    R.n -= count;
    R.pos = start;
    R.browse = 0;
}

static const char *ref_hist(int back)
{
    return (back >= 1 && back <= R.hist_count) ? R.hist[R.hist_count - back] : NULL;
}

static void ref_key(const char *k)
{
    if (k[0] == 0x1B)
    {
        if (!str_eq(k, "\x1b[A") && !str_eq(k, "\x1b[B") && !str_eq(k, "\x1b[3~"))
        {
            if (str_eq(k, "\x1b[D") && R.pos > 0) R.pos--;
            if (str_eq(k, "\x1b[C") && R.pos < R.n) R.pos++;
            if (str_eq(k, "\x1b[H")) R.pos = 0;
            if (str_eq(k, "\x1b[F")) R.pos = R.n;
            return;
        }
        if (str_eq(k, "\x1b[3~"))
        {
            if (R.pos < R.n) ref_delete(R.pos, 1);
            R.browse = 0;
            return;
        }
        if (str_eq(k, "\x1b[A"))
        {
            if (ref_hist(R.browse + 1) == NULL) return;
            if (R.browse == 0) { R.buf[R.n] = 0; os64_strcopy(R.live, LINE_MAX, R.buf); }
            ref_set(ref_hist(++R.browse));
            return;
        }
        if (R.browse == 0) return;
        R.browse--;
        ref_set(R.browse == 0 ? R.live : ref_hist(R.browse));
        return;
    }
    char c = k[0];
    if (c == 0x7F || c == 0x08) { if (R.pos > 0) ref_delete(R.pos - 1, 1); R.browse = 0; }
    else if (c == 0x01) R.pos = 0;
    else if (c == 0x05) R.pos = R.n;
    else if (c == 0x15) { if (R.pos > 0) ref_delete(0, R.pos); R.browse = 0; }
    else if (c == 0x0B) { if (R.pos < R.n) ref_delete(R.pos, R.n - R.pos); R.browse = 0; }
    else if (c == 0x17)
    {
        int j = R.pos;
        while (j > 0 && R.buf[j - 1] == ' ') j--;
        while (j > 0 && R.buf[j - 1] != ' ') j--;
        if (j < R.pos) ref_delete(j, R.pos - j);
        R.browse = 0;
    }
    else ref_insert(k);
}

static void ref_submit(void)
{
    R.buf[R.n] = 0;
    if (R.n > 0 && (R.hist_count == 0 || !str_eq(R.hist[R.hist_count - 1], R.buf)))
    {
        check(R.hist_count < 32, "scenario outgrew the reference history");
        os64_strcopy(R.hist[R.hist_count++], LINE_MAX, R.buf);
    }
}

// ── the checker ─────────────────────────────────────────────────────────────
static const char *s_prompt_shown;    // the prompt as it appears, escapes spent

// Lay the prompt and line[0, upto) out from column 0 of the prompt's row, the
// way a terminal that wraps at once would; returns the absolute row and column
// that follow, and fills `want` with what each cell should hold.
static void layout(const term_t *t, int upto, const char *extra, long *row, int *col,
                   char want[ROWS_MAX * 4][COLS_MAX])
{
    long r = 0;
    int c = 0;
    if (want)
        memset(want, ' ', (size_t)ROWS_MAX * 4 * COLS_MAX);
    const char *parts[3] = { s_prompt_shown, NULL, extra };
    for (int p = 0; p < 3; p++)
    {
        int len = p == 1 ? upto : (parts[p] ? (int)os64_strlen(parts[p]) : 0);
        for (int i = 0; i < len; i++)
        {
            char ch = p == 1 ? R.buf[i] : parts[p][i];
            if (ch == '\n') { r++; c = 0; continue; }
            if (want) { check(r < ROWS_MAX * 4, "layout too tall"); want[r][c] = ch; }
            if (++c == t->w) { r++; c = 0; }
        }
    }
    *row = t->prompt_abs + r;
    *col = c;
}

static void expect_screen(const char *when)
{
    static char want[ROWS_MAX * 4][COLS_MAX];
    for (int i = 0; i < 2; i++)
    {
        term_t *t = &s_terms[i];
        long row;
        int col;
        layout(t, R.n, NULL, &row, &col, want);
        for (int sr = 0; sr < t->h; sr++)
        {
            long rel = sr + t->scrolled - t->prompt_abs;
            if (rel < 0)
                continue;                 // above the prompt: not this line's
            for (int c = 0; c < t->w; c++)
                if (t->cell[sr][c] != want[rel][c])
                {
                    printf("%s: %s terminal, width %d, row %d col %d holds '%c', want '%c'\n",
                           when, t->name, t->w, sr, c, t->cell[sr][c], want[rel][c]);
                    fail();
                }
        }
        layout(t, R.pos, NULL, &row, &col, NULL);
        if (t->pending || t->row + t->scrolled != row || t->col != col)
        {
            printf("%s: %s terminal, width %d, cursor at %ld,%d%s, caret %d wants %ld,%d\n",
                   when, t->name, t->w, t->row + t->scrolled, t->col,
                   t->pending ? " (waiting)" : "", R.pos, row, col);
            fail();
        }
    }
}

// Where the cursor must be once the line is left: after the line and `tail`,
// on a fresh row, with no blank row between.
static void expect_left(const char *tail)
{
    for (int i = 0; i < 2; i++)
    {
        term_t *t = &s_terms[i];
        long row;
        int col;
        layout(t, R.n, tail, &row, &col, NULL);
        long want = col == 0 && row > t->prompt_abs ? row : row + 1;
        if (t->pending || t->col != 0 || t->row + t->scrolled != want)
        {
            printf("%s terminal, width %d: left the line at %ld,%d, want %ld,0\n",
                   t->name, t->w, t->row + t->scrolled, t->col, want);
            fail();
        }
    }
}

// ── scripts ─────────────────────────────────────────────────────────────────
#define RESIZE "\x01RESIZE"   // not a key: the window changes width
#define COMPLETE "\x01TAB"    // Tab, with what the reference should insert next
enum { SCRIPT_MAX = 4096 };

typedef struct { const char *key; const char *ref; int width; } step_t;
static step_t s_script[SCRIPT_MAX];
static int s_steps, s_step, s_byte;

static void add(const char *key) { check(s_steps < SCRIPT_MAX, "script too long"); s_script[s_steps++] = (step_t){ key, NULL, 0 }; }
static void add_resize(int w) { s_script[s_steps++] = (step_t){ RESIZE, NULL, w }; }
static void add_tab(const char *inserts) { s_script[s_steps++] = (step_t){ COMPLETE, inserts, 0 }; }

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    check(handle == 0 && len == 1, "the line editor read in an unexpected shape");
    check(s_step < s_steps, "script ran out before the line was submitted");
    step_t *st = &s_script[s_step];
    if (s_byte == 0)
    {
        expect_screen("before a key");
        check(s_winch_handler == on_resize, "no SIGWINCH handler while the prompt is up");
        if (str_eq(st->key, RESIZE))
        {
            for (int i = 0; i < 2; i++)
                term_resize(&s_terms[i], st->width);
            s_width = st->width;
            s_step++;
            s_winch_handler(OS64_SIGWINCH);
            return OS64_INTERRUPTED;
        }
        if (str_eq(st->key, COMPLETE))
        {
            ref_insert(st->ref);
            *(char *)buf = '\t';
            s_step++;
            return 1;
        }
        if (st->key[0] != '\r' && st->key[0] != 0x03)
            ref_key(st->key);
    }
    *(char *)buf = st->key[s_byte++];
    if (st->key[s_byte] == '\0')
    {
        s_step++;
        s_byte = 0;
    }
    return 1;
}

// Run one line: the script so far, then `end` (Enter or Ctrl+C).
static void drive_line(const char *end)
{
    add(end);
    s_step = s_byte = 0;
    char line[LINE_MAX];
    int got = read_line(line, sizeof(line));
    check(s_step == s_steps, "the line ended before its script did");
    check(s_winch_handler == OS64_SIG_DEFAULT, "SIGWINCH handler left installed after the prompt");
    if (end[0] == '\r')
    {
        check(got == R.n && str_eq(line, (R.buf[R.n] = 0, R.buf)), "submitted line differs from the reference");
        expect_left(NULL);
        ref_submit();
    }
    else
    {
        check(got == -1, "Ctrl+C did not abandon the line");
        expect_left("^C");
    }
    R.n = R.pos = R.browse = 0;
    s_steps = 0;
}

static void fresh(int width, int height, const char *fmt, const char *shown)
{
    term_init(&s_terms[0], "os64", false, width ? width : COLS_MAX, height);
    term_init(&s_terms[1], "vt100", true, width ? width : COLS_MAX, height);
    s_width = width;
    s_prompt_fmt = fmt;
    s_prompt_shown = shown;
    s_block.depth = 0;
    memset(&R, 0, sizeof(R));
    s_hist_count = 0;
    s_hist_next = s_hist_head = s_hist_used = 0;
}

static void type(const char *s)
{
    static char keys[SCRIPT_MAX][2];      // one per script slot, so none is reused early
    for (; *s; s++)
    {
        keys[s_steps][0] = *s;
        keys[s_steps][1] = 0;
        add(keys[s_steps]);
    }
}

static void repeat(const char *key, int times) { while (times-- > 0) add(key); }

static const char *LONG_A = "the quick brown fox jumps over the lazy dog and keeps running past the edge";

// ── the scenarios ───────────────────────────────────────────────────────────
static void scenario_widths(void)
{
    static const int widths[] = { 7, 10, 11, 16, 80 };
    for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
    {
        fresh(widths[w], 40, NULL, "husk> ");
        type(LONG_A);                       // wraps, and lands on the edge more than once
        repeat("\x7f", 30);                 // backspace back across the seams
        repeat("\x1b[D", 20);               // walk left across more of them
        type("XYZ");                        // insert mid-line, shifting the tail over wraps
        repeat("\x1b[C", 25);
        add("\x1b[3~");                     // delete at the caret
        add("\x1b[H");
        type(">>");
        add("\x1b[F");
        add("\x17");                        // Ctrl+W
        add("\x01");                        // Ctrl+A
        repeat("\x1b[C", 9);
        add("\x0b");                        // Ctrl+K
        add("\x05");
        add("\x15");                        // Ctrl+U: everything
        type(LONG_A);
        drive_line("\r");
    }
}

static void scenario_prompts(void)
{
    // A prompt exactly as wide as the terminal, one wider, a two-line one, and
    // one whose colours must not count as columns.
    fresh(6, 40, NULL, "husk> ");
    type("abcdefghijklm");
    repeat("\x1b[D", 13);
    repeat("\x1b[3~", 13);
    drive_line("\r");

    fresh(5, 40, NULL, "husk> ");
    type("abcdefghijklm");
    add("\x01");
    type("_");
    drive_line("\r");

    fresh(9, 40, "top\\nhusk> ", "top\nhusk> ");
    type("0123456789abcdef");
    repeat("\x7f", 16);
    drive_line("\r");

    fresh(10, 40, "\\e[32mOK\\e[0m> ", "OK> ");
    type("0123456789abcdef");
    add("\x1b[H");
    add("\x0b");
    drive_line("\r");

    // Colour switched off AFTER the glyph that fills the row: the escape
    // draws nothing, so the prompt still ends on the edge and must settle.
    fresh(5, 40, "\\e[32mhusk>\\e[0m", "husk>");
    add_resize(7);
    type("abcdefghij");
    add("\x1b[H");
    drive_line("\r");

    // A width the terminal cannot report: nothing wraps in the model's eyes.
    fresh(0, 40, NULL, "husk> ");
    type("hello world");
    repeat("\x1b[D", 5);
    add("\x7f");
    drive_line("\r");
}

static void scenario_history_and_endings(void)
{
    fresh(12, 40, NULL, "husk> ");
    type(LONG_A);
    drive_line("\r");
    type("abcdefghijklmnop");                // parked while browsing
    add("\x1b[A");                           // the long one, over the short one
    add("\x1b[A");                           // nothing further back
    add("\x1b[B");                           // the parked line comes back
    add("\x1b[A");
    repeat("\x1b[D", 40);
    add("\x7f");                             // an edit makes it ours
    add("\x1b[B");                           // no longer browsing: nothing happens
    drive_line("\x03");                        // Ctrl+C lands after the line

    // Lines ending exactly on the edge must not leave a blank row behind.
    fresh(10, 40, NULL, "husk> ");
    type("abcd");
    drive_line("\r");
    type("abcdefghijklmn");
    drive_line("\r");
    type("abcdefgh");
    drive_line("\x03");
}

static void scenario_completion(void)
{
    // "ex" matches exit and export and extends no further: the listing prints
    // under the line and the line is drawn again below it.
    fresh(8, 40, NULL, "husk> ");
    type("ex");
    add_tab("");
    type("i");
    add_tab("t ");
    type("0123456789");
    add("\x1b[H");
    add_tab("");                             // an empty first word completes nothing
    drive_line("\r");
}

static void scenario_resize(void)
{
    fresh(20, 40, NULL, "husk> ");
    type(LONG_A);
    repeat("\x1b[D", 17);
    add_resize(11);                          // narrower: the line takes more rows
    type("abc");
    add_resize(37);                          // wider: fewer
    repeat("\x7f", 5);
    add_resize(9);
    add("\x1b[F");
    drive_line("\r");

    // A resize the moment the prompt is up, before anything is typed.
    fresh(14, 40, NULL, "husk> ");
    add_resize(6);
    type("abcdefg");
    drive_line("\r");
}

static void scenario_scrolling(void)
{
    // Start low on a short screen, so typing scrolls it: the line's rows move
    // up under the editor and every move must still land.
    fresh(10, 9, NULL, "husk> ");
    for (int i = 0; i < 6; i++)
        drive_line("\r");                      // empty lines walk the prompt down
    type("0123456789abcdefghijklmnopqrstu");
    repeat("\x7f", 12);
    add("\x1b[H");
    type("^");
    add("\x1b[F");
    type("0123456789012");
    drive_line("\r");
}

static uint32_t s_seed = 12345;
static uint32_t rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 8; }

static void scenario_fuzz(void)
{
    static const char *keys[] = {
        "a", "b", " ", "z", "1", "/", "\x7f", "\x7f", "\x1b[3~", "\x1b[D", "\x1b[D",
        "\x1b[C", "\x1b[H", "\x1b[F", "\x01", "\x05", "\x15", "\x0b", "\x17", "\x1b[A", "\x1b[B",
    };
    static const int widths[] = { 5, 7, 8, 13, 17, 24 };
    for (int round = 0; round < 300; round++)
    {
        int w = widths[rnd() % (sizeof(widths) / sizeof(widths[0]))];
        // At most 190 keys a line, so even the narrowest line (and any
        // history it recalls) stays shorter than the screen is tall: a row
        // that has scrolled off the top cannot be climbed back to.
        fresh(w, 44, NULL, "husk> ");
        for (int line = 0; line < 3; line++)
        {
            int count = 40 + (int)(rnd() % 150);
            for (int k = 0; k < count; k++)
            {
                unsigned pick = rnd() % 32;
                if (pick < 21)
                    add(keys[pick]);
                else
                    add("q");                // typing outweighs everything else
            }
            if (rnd() % 5 == 0)
                add_resize(widths[rnd() % (sizeof(widths) / sizeof(widths[0]))]);
            drive_line(rnd() % 4 == 0 ? "\x03" : "\r");
        }
    }
}

int main(void)
{
    scenario_widths();
    scenario_prompts();
    scenario_history_and_endings();
    scenario_completion();
    scenario_resize();
    scenario_scrolling();
    scenario_fuzz();
    puts("test_husk_line_host: all checks passed");
    return 0;
}
