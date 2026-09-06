// gopher.c — a gopher client you steer with the arrow keys.
//
// BROWSER.md's rung 4(b), and the first program on this machine with a FACE:
// it clears the screen, paints where it likes, and reads keys instead of
// lines. Everything it needs to do that arrived the day before it did (the
// five escape sequences, rung 4(a)); everything it says on the wire is in
// wire.c. What is left here is the part that is neither — a screen, a
// selection, and a stack of where you have been.
//
// WHY IT IS NOT A MODE OF os64get. The protocols differ, but the KIND differs
// more: os64get is a one-shot fetcher that writes a file and exits with a
// code, and this is a session. They share the dial and (since the URL parser
// moved into libos64) the grammar of an address, which is all two programs
// should share.
//
// THE ARROW KEYS ARE CHRIS'S RULING (2026-09-03). A numbered menu reads
// backwards to him, and he is right that a browser picks a link by pointing
// at it. Bindings are lynx's, whose path this re-walks in the same order it
// was walked in 1992: Up/Down to move, Enter to follow, LEFT ARROW for back,
// `q` to quit.
//
// NO ESCAPE SEQUENCE FROM THE WIRE REACHES THE TERMINAL, and that takes two
// guards rather than one, because there are two kinds of stranger's bytes
// here. A MENU LINE is refused whole at the parse (wire.c) — a doctored item
// is never followed, which no print-time check could achieve. A TEXT FILE's
// lines and an `h` link's fetched HTML pass through no parse at all: they
// are somebody's document, and refusing one for a stray byte would be
// refusing the page. Those are escaped where they are DRAWN, by
// draw_clipped, which every painted string on this screen goes through.
// Both matter now that os64's terminal obeys: a page is a stranger with a
// paintbrush otherwise.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wire.h"

#include "os64/args.h"
#include "os64/dial.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/procfs.h"
#include "os64/str.h"

// A page holds either items or lines, never both — the type that led here
// decided which, before a byte was read.
#define GOPHER_MAX_ITEMS  4096
#define GOPHER_MAX_LINES  65536

// Thirty seconds of silence ends a READ. It is IDLE time — the wait between
// bytes — so a slow server still arrives; os64get uses the same number for
// the same reason.
//
// IT DOES NOT COVER THE REQUEST, and that is not this program's choice:
// `os64_read_for` carries a patience and there is no `os64_write_for`, so the
// write that asks for a page has no deadline to be given. A peer that
// completes the handshake advertising a zero window stalls that write in the
// kernel indefinitely — DEBTS.md, tcp_conn_write — and Ctrl+C is what ends it
// until the kernel grows the bound. Saying "thirty seconds ends a fetch" here
// was half true, and the half it left out is the half a hostile server picks.
#define GOPHER_IDLE_MS    30000

#define GOPHER_HISTORY_MAX 64

// Exit codes, in os64get's shape: a number a script can act on, and each one
// naming a different thing to go fix.
#define GOPHER_OK          0
#define GOPHER_USAGE       2
#define GOPHER_BAD_ADDRESS 13
#define GOPHER_UNREACHABLE 5
// 9 is os64get's "the page could not be written", and it means the same
// thing here: the bytes arrived and the disk is where it went wrong.
#define GOPHER_SAVE_FAILED 9

// What page_fetch answers with when the failure was not the network's.
// Positive, so neither can ever be mistaken for a dial reason (negative).
#define GOPHER_FETCH_LOCAL  1
#define GOPHER_FETCH_BINARY 2

typedef struct {
    gopher_addr_t addr;
    bool          isMenu;
    gopher_item_t *items;
    int32_t       nitems;
    char        **lines;
    int32_t       nlines;
    int32_t       top;          // first visible content row
    int32_t       sel;          // selected item (menus only)
    bool          truncated;    // the answer hit a cap
    bool          terminated;   // the server sent its period
    bool          stalled;      // it went quiet instead of hanging up
    // Whether ANY item on this menu can be followed. A menu with none is a
    // DOCUMENT that happens to be spelled as a menu — a phlog index, a
    // header of `i` lines, a page of prose — and it has no selection to
    // move, which is a different thing from having one that is stuck at the
    // top. Answered once when the page arrives; asking it per keystroke was
    // a scan of the whole menu inside the arrow keys.
    bool          hasLinks;
    // What the title bar says, when the page did not come from a gopher
    // address — an `h` link's page is fetched by os64get and belongs to the
    // web, so spelling its gopher_addr_t would name a place it never was.
    const char   *title;
} page_t;

typedef struct {
    gopher_addr_t addr;
    int32_t       top;
    int32_t       sel;
} crumb_t;

static crumb_t s_history[GOPHER_HISTORY_MAX];
static int32_t s_depth;

static int32_t s_rows = 25;
static int32_t s_cols = 80;
static int64_t s_keys = OS64_STDIN;
static char    s_status[256];

// ── The glass ───────────────────────────────────────────────────────────
//
// Five sequences, which is the whole vocabulary rung 4(a) built (and it built
// exactly those because this program asked for them): clear, position, erase
// to end of line, and set colour or attribute.

static void out(const char *s)      { os64_write(OS64_STDOUT, s, os64_strlen(s)); }
static void outn(const char *s, size_t n) { os64_write(OS64_STDOUT, s, n); }

static void screen_clear(void)      { out("\033[2J"); }
static void screen_home(void)       { out("\033[H"); }
static void cursor_to(int32_t row, int32_t col)
{
    char seq[32];
    os64_snprintf(seq, sizeof(seq), "\033[%d;%dH", (int)row, (int)col);
    out(seq);
}
static void erase_to_eol(void)      { out("\033[K"); }
static void sgr(const char *params)
{
    char seq[32];
    os64_snprintf(seq, sizeof(seq), "\033[%sm", params);
    out(seq);
}
static void sgr_reset(void)         { out("\033[0m"); }

// A BAR IS PAINTED WITH A BACKGROUND, NOT WITH REVERSE VIDEO, and the reason
// is `ESC[K`: erase-to-end-of-line fills the rest of the row with the PEN's
// background and clears attributes, so a reverse-video bar stops dead at its
// last character while a coloured one runs to the margin.
//
// That matters for more than looks. The alternative — padding with spaces out
// to the last column — writes the bottom-right cell, and writing there
// SCROLLS THE SCREEN in any terminal that wraps: the title row goes over the
// top edge and every row lands one line high. `ESC[K` never advances the
// cursor, so it cannot scroll. (Seen on the first boot of this program: the
// title bar was simply gone.)
static void bar_on(void)            { sgr("30;47"); }

// THREE INKS, AND EACH ANSWERS A DIFFERENT QUESTION about a row.
//
//   the TYPE label wears the type's own colour  — "what is this thing?"
//   a LINK's words are white                    — "can I go there?"
//   prose is grey                               — "this is just text"
//
// BOLD IS NOT A SIGNAL IN THIS PALETTE, which is why the pair above is a
// greyscale STEP and not a weight. `ESC[1m` brightens the foreground rather
// than thickening the glyph (os64/ansi.h — the font has one weight), and
// bright-white against white is 0xe5e5e5 against 0xffffff: invisible at a
// glance. The same half-step is why plain red and bold red are hard to tell
// apart in /tests/ansiprobe. Grey against white is 0x7f7f7f against
// 0xe5e5e5 — two palette entries apart instead of a brightening, and the
// difference survives being looked at rather than stared at.
//
// And the SELECTION is deliberately NOT the chrome's black-on-white: the
// title and status bars already wear that, so a white selection bar read as
// a third piece of furniture rather than as a place you had put the cursor.
// Cyan is where a text UI has kept its highlight since Turbo Vision and
// Norton Commander, and it stays legible on the blue the glass boots to.
#define INK_LINK      "37"      // white: a thing you can follow
#define INK_PROSE     "1;30"    // grey (bright black): a thing you can only read
#define INK_BROKEN    "1;31"    // a line that will not be followed
static void select_on(void)         { sgr("30;46"); }

// Draw `text` truncated to `width` cells. Truncation and not wrapping: a menu
// line is one item, and an item split across two rows is an item the arrow
// keys would have to learn to skip half of.
//
// THIS IS WHERE A STRANGER'S BYTES STOP BEING OBEYED. Everything this program
// paints comes through here, which is the point: a text file's lines and an
// `h` link's fetched HTML reach the screen having passed no parse at all, and
// a guard written at each of those roads separately is a guard the next road
// is added without.
//
// The notation is `cat -v`'s, and for its reason: `^[` says WHICH byte was
// there, where a dropped byte or a bare `?` says only that something was.
// TAB is the exception and becomes one SPACE: the glass advances a single
// cell for a tab anyway (its deliberate quirk — not a tab stop), so a space
// costs the same column and paints the gap rather than leaving whatever the
// cell held before. A text file's indentation is layout, not an attack.
// High bytes are left alone — a menu written in Latin-1 in 1994 is
// somebody's language, and the font decides what it looks like.
static void draw_clipped(const char *text, int32_t width)
{
    int32_t n = 0;
    for (const char *p = text; *p != '\0' && n < width; p++) {
        unsigned char b = (unsigned char)*p;
        if (b >= 0x20 && b != 0x7F) {
            outn(p, 1);
            n++;
        } else if (b == '\t') {
            outn(" ", 1);
            n++;
        } else if (n + 2 <= width) {
            char caret[2] = { '^', (char)(b == 0x7F ? '?' : b + 0x40) };
            outn(caret, 2);
            n += 2;
        } else {
            break;              // no room for both halves; the clip lands here
        }
    }
}

static void geometry(void)
{
    os64_tty_info_t tty;
    if (os64_tty_read(&tty) == 0 && tty.rows > 4 && tty.cols > 20) {
        s_rows = (int32_t)tty.rows;
        s_cols = (int32_t)tty.cols;
    }
}

// ── Reading a page ──────────────────────────────────────────────────────

typedef struct {
    int64_t handle;
    bool    stalled;    // the idle deadline expired with the answer unfinished
} source_t;

static int64_t source_read(void *ctx, void *buf, size_t cap)
{
    source_t *src = ctx;
    int64_t n = os64_read_for((int32_t)src->handle, buf, cap, GOPHER_IDLE_MS);
    if (n == OS64_ERR_TIMEOUT) {
        // The stream's vocabulary is bytes / end / broke, so a stall becomes
        // a broke — and the REASON is remembered here, because "the server
        // went quiet" and "the server finished" are the same short page with
        // two different things to do about it. A close is an honest 0 and
        // needs none of this.
        src->stalled = true;
        return -1;
    }
    return n;
}

static void page_free(page_t *page)
{
    if (page->items != NULL)
        os64_free(page->items);
    if (page->lines != NULL) {
        for (int32_t i = 0; i < page->nlines; i++)
            os64_free(page->lines[i]);
        os64_free(page->lines);
    }
    page->items = NULL;
    page->lines = NULL;
    page->nitems = 0;
    page->nlines = 0;
}

// Dial, ask, and read the answer according to the framing the TYPE dictates.
//
// Returns 0; a NEGATIVE dial reason; GOPHER_FETCH_LOCAL for a failure that
// is this machine's rather than the network's; or GOPHER_FETCH_BINARY for an
// address that is a download and not a page. The dial reason is kept apart
// from the rest because os64_dial_reason would happily turn "out of memory"
// into a sentence about the network, and a person acting on that would go
// and check their cable.
static int64_t page_fetch(const gopher_addr_t *addr, page_t *page)
{
    os64_memset(page, 0, sizeof(*page));
    page->addr = *addr;

    // A BINARY IS NOT A PAGE, and the type says so before a socket is opened.
    // A page holds a menu or lines of text; a binary framing is neither, and
    // reading one as lines split a zip file on its newlines and painted it.
    // Whoever wanted this address wanted save_item.
    if (gopher_framing_for(addr->type) == GOPHER_FRAMING_BINARY)
        return GOPHER_FETCH_BINARY;

    char dialstring[GOPHER_HOST_MAX + 32];
    if (os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%u",
                      addr->host, (unsigned)addr->port) <= 0)
        return GOPHER_FETCH_LOCAL;

    int64_t conn = os64_dial(dialstring);
    if (conn < 0)
        return conn;

    char request[GOPHER_SELECTOR_MAX + GOPHER_QUERY_MAX + 8];
    size_t reqlen = gopher_request(addr, request, sizeof(request));
    if (reqlen == 0 || os64_write((int32_t)conn, request, reqlen) < 0) {
        os64_close((int32_t)conn);
        return GOPHER_FETCH_LOCAL;
    }

    source_t src = { conn, false };
    gopher_stream_t stream;
    gopher_stream_init(&stream, source_read, &src);

    page->isMenu = gopher_framing_for(addr->type) == GOPHER_FRAMING_MENU;

    // Out of memory partway through an answer is a FAILED fetch, not a short
    // one. A page that is missing lines nobody was told about would replace
    // the page you were reading and look like the whole document.
    bool starved = false;

    char line[GOPHER_LINE_MAX];
    if (page->isMenu) {
        page->items = os64_malloc(sizeof(gopher_item_t) * GOPHER_MAX_ITEMS);
        if (page->items == NULL) { os64_close((int32_t)conn); return GOPHER_FETCH_LOCAL; }
        while (page->nitems < GOPHER_MAX_ITEMS &&
               gopher_stream_line(&stream, line, sizeof(line), false) == 1) {
            gopher_item_t item;
            gopher_item_parse(line, &item);
            page->items[page->nitems++] = item;
        }
        if (page->nitems == GOPHER_MAX_ITEMS)
            page->truncated = true;
    } else {
        page->lines = os64_malloc(sizeof(char *) * GOPHER_MAX_LINES);
        if (page->lines == NULL) { os64_close((int32_t)conn); return GOPHER_FETCH_LOCAL; }
        while (page->nlines < GOPHER_MAX_LINES &&
               gopher_stream_line(&stream, line, sizeof(line), true) == 1) {
            size_t len = os64_strlen(line);
            char *copy = os64_malloc(len + 1);
            if (copy == NULL) { starved = true; break; }
            os64_memcpy(copy, line, len + 1);
            page->lines[page->nlines++] = copy;
        }
        if (page->nlines == GOPHER_MAX_LINES)
            page->truncated = true;
    }

    page->truncated = page->truncated || stream.truncated;
    page->terminated = stream.terminated;
    page->stalled = src.stalled;
    os64_close((int32_t)conn);

    if (starved) {
        page_free(page);
        return GOPHER_FETCH_LOCAL;
    }

    // A BROKEN CONNECTION IS NOT AN ENDED ONE. A stall has its own word on
    // the status bar and leaves a short page standing, because a slow server
    // is still a server; a reset is the fetch failing, and the page you were
    // reading must survive it. `failed` covers both, so the stall is what
    // separates them.
    if (stream.failed && !src.stalled) {
        page_free(page);
        return GOPHER_FETCH_LOCAL;
    }

    // A menu's first selectable item, so Enter does something sensible the
    // moment a page arrives — and whether there is one at all, which decides
    // whether this page has a selection or is only something to read.
    page->sel = 0;
    if (page->isMenu)
        for (int32_t i = 0; i < page->nitems; i++)
            if (page->items[i].followable) {
                page->sel = i;
                page->hasLinks = true;
                break;
            }
    return 0;
}

// ── Painting ────────────────────────────────────────────────────────────

// The colour a type wears. It is a hint, not information — the type NAME is
// printed beside every link, because a colour is not readable and this
// program should work on a terminal somebody set to two shades of amber.
static const char *type_colour(char type)
{
    switch (type) {
        case '1': return "1;34";   // a menu: bright blue, the way a directory has been since ls -G
        case '0': return "0";      // a text file: the terminal's own ink
        case '7': return "1;36";   // a search
        case 'h': return "1;35";   // leaves gopherspace
        case '3': return "1;31";   // the server complaining
        case 'i': return "0";
        default:  return "1;33";   // a file to save
    }
}

static int32_t content_rows(void) { return s_rows - 2; }

static void draw_title(const page_t *page)
{
    char text[GOPHER_SELECTOR_MAX + GOPHER_HOST_MAX + 32];
    if (page->title != NULL)
        os64_strcopy(text, sizeof(text), page->title);
    else
        gopher_url_text(&page->addr, text, sizeof(text));
    cursor_to(1, 1);
    bar_on();
    outn(" ", 1);
    draw_clipped(text, s_cols - 2);
    erase_to_eol();                 // the bar runs to the margin
    sgr_reset();
}

static void draw_status(const page_t *page)
{
    char text[256];
    if (s_status[0] != '\0') {
        os64_strcopy(text, sizeof(text), s_status);
    } else if (page->isMenu) {
        os64_snprintf(text, sizeof(text),
                      " %d items%s%s   arrows move  enter follows  left back  q quit",
                      (int)page->nitems,
                      page->truncated ? "  (truncated)" : "",
                      page->stalled ? "  (the server went quiet)"
                                    : (page->terminated ? "" : "  (no end marker)"));
    } else {
        // A GOPHER TEXT FILE IS DOT-TERMINATED TOO, so a missing period says
        // the same thing here it says on a menu: the server stopped talking
        // rather than finished. A page that arrived short and a page that
        // arrived whole must not read the same, whichever kind it is.
        // `terminated` is the question "did it end the way its protocol says
        // it ends?", which is why a handed-off page — no gopher framing at
        // all, and os64get already vouched for it — answers yes.
        os64_snprintf(text, sizeof(text),
                      " %d lines%s%s   arrows scroll  left back  q quit",
                      (int)page->nlines, page->truncated ? "  (truncated)" : "",
                      page->stalled ? "  (the server went quiet)"
                                    : (page->terminated ? "" : "  (no end marker)"));
    }
    cursor_to(s_rows, 1);
    bar_on();
    draw_clipped(text, s_cols - 1);
    erase_to_eol();
    sgr_reset();
}

static void draw_menu_row(const page_t *page, int32_t index, int32_t row)
{
    const gopher_item_t *item = &page->items[index];
    cursor_to(row, 1);

    // No links, no cursor. A highlight on a menu where nothing can be
    // followed offers something that is not there, and now that such a menu
    // SCROLLS, the bar would sit on whichever row happened to be row zero.
    bool selected = page->hasLinks && index == page->sel;
    if (selected)
        select_on();

    if (item->result == GOPHER_ITEM_INFO) {
        // An `i` line is prose in a menu. Indented past the type column so
        // the links line up as a list rather than as a paragraph with holes.
        if (!selected)
            sgr(INK_PROSE);
        outn("        ", 8);
        draw_clipped(item->display, s_cols - 9);
    } else if (item->result == GOPHER_ITEM_OK) {
        char label[16];
        os64_snprintf(label, sizeof(label), " %-7s", gopher_type_name(item->type));
        if (!selected)
            sgr(type_colour(item->type));
        draw_clipped(label, 8);
        if (!selected)
            sgr(INK_LINK);
        outn(" ", 1);
        draw_clipped(item->display, s_cols - 10);
    } else {
        // A line this client would not follow is SHOWN, not hidden. A menu
        // with a hole in it is a menu you cannot tell is broken.
        if (!selected)
            sgr(INK_BROKEN);
        draw_clipped(item->result == GOPHER_ITEM_REFUSED
                     ? " refused  (a control character in the line)"
                     : " unreadable menu line", s_cols - 1);
        if (!selected)
            sgr_reset();
    }

    // The erase runs the selection to the margin, or clears the row's tail
    // when this is not the selected line. The pen has to be back to ordinary
    // first in the unselected case, or the row's ink would fill its tail.
    if (!selected)
        sgr_reset();
    erase_to_eol();
    if (selected)
        sgr_reset();
}

static void draw(const page_t *page)
{
    screen_home();
    draw_title(page);

    int32_t rows = content_rows();
    int32_t count = page->isMenu ? page->nitems : page->nlines;
    for (int32_t r = 0; r < rows; r++) {
        int32_t index = page->top + r;
        if (index >= count) {
            cursor_to(2 + r, 1);
            erase_to_eol();
            continue;
        }
        if (page->isMenu) {
            draw_menu_row(page, index, 2 + r);
        } else {
            cursor_to(2 + r, 1);
            draw_clipped(page->lines[index], s_cols);
            erase_to_eol();
        }
    }
    draw_status(page);
    // Park the cursor on the status bar's end rather than mid-page, where it
    // would blink inside somebody's sentence.
    cursor_to(s_rows, s_cols);
}

// ── Keys ────────────────────────────────────────────────────────────────
//
// The keyboard delivers arrows as the VT100 spelling — ESC '[' A/B/C/D — and
// the digit-parameter family as ESC '[' <n> '~'. Decoding them here rather
// than in libos64 keeps the guess about a LONE escape local: this program
// ignores one, and a program with a text field would not.
//
// THE ESCAPE KEY AND AN ARROW KEY START WITH THE SAME BYTE, which is the
// oldest ambiguity in terminal input and has exactly one answer: WAIT A
// LITTLE. The rest of a real sequence is already in the input ring — the
// driver delivers a sequence's bytes back to back out of one interrupt —
// while the Escape KEY is one byte and nothing after it. So the lookahead is
// patient enough to collect a sequence that is already there and far too
// short for a person to notice. Blocking instead made Escape look like a
// hang until the next key was pressed, and then ate that key.
#define GOPHER_ESC_LOOKAHEAD_MS 50

typedef enum {
    KEY_NONE = 0, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, KEY_ENTER, KEY_QUIT, KEY_EOF,
    KEY_OTHER,
} key_t;

// A byte read while looking for the rest of a sequence that turned out not to
// be one. It belongs to whoever asks next; throwing it away would lose the
// keystroke a person actually meant.
static int32_t s_pushback = -1;

// Whether this input can be asked to wait a BOUNDED time. The console can and
// a file cannot (os64/io.h: everywhere else a finite patience is refused
// rather than silently blocking), and keys come from the terminal whenever
// there is one — so this is false only where there is none, and there the
// lookahead blocks exactly as it always did. The ambiguity is unresolvable on
// such a handle; pretending otherwise would end the session at the first
// Escape byte in a file.
static bool s_timed_keys = true;

// 1 a byte, 0 nothing within the patience, -1 the input ended or broke.
static int key_byte(char *c, uint64_t patience_ms)
{
    if (s_pushback >= 0) {
        *c = (char)s_pushback;
        s_pushback = -1;
        return 1;
    }
    if (!s_timed_keys)
        patience_ms = OS64_WAIT_FOREVER;
    int64_t n = os64_read_for((int32_t)s_keys, c, 1, patience_ms);
    if (n == 1)                return 1;
    if (n == OS64_ERR_TIMEOUT) return 0;
    return -1;                       // 0 is end of input; the rest is a broken handle
}

// Ask the input, once, whether it takes a deadline — with a patience of zero,
// the poll that never blocks, so the question costs nothing and cannot hang.
// A byte it happens to find was already typed and is still owed to somebody.
static void keys_probe_patience(void)
{
    char c;
    int64_t n = os64_read_for((int32_t)s_keys, &c, 1, 0);
    if (n == 1)
        s_pushback = (unsigned char)c;
    else if (n != OS64_ERR_TIMEOUT)
        s_timed_keys = false;
}

static key_t key_read(char *literal)
{
    char c;
    if (key_byte(&c, OS64_WAIT_FOREVER) != 1)
        return KEY_EOF;              // input ended; nobody is left to ask
    *literal = c;

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 'q' || c == 'Q')   return KEY_QUIT;
    if (c != 0x1B)              return KEY_OTHER;

    char bracket;
    int got = key_byte(&bracket, GOPHER_ESC_LOOKAHEAD_MS);
    if (got < 0)
        return KEY_EOF;
    if (got == 0)
        return KEY_NONE;             // the Escape KEY: nothing followed it
    if (bracket != '[') {
        s_pushback = (unsigned char)bracket;
        return KEY_NONE;             // not a sequence; the byte is still owed
    }

    char final;
    got = key_byte(&final, GOPHER_ESC_LOOKAHEAD_MS);
    if (got < 0) return KEY_EOF;
    if (got == 0) return KEY_NONE;   // a sequence that stopped halfway
    switch (final) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: break;
    }
    if (final >= '0' && final <= '9') {
        char tilde;
        if (key_byte(&tilde, GOPHER_ESC_LOOKAHEAD_MS) == 1 && tilde != '~')
            s_pushback = (unsigned char)tilde;
        if (final == '5') return KEY_PGUP;
        if (final == '6') return KEY_PGDN;
    }
    return KEY_NONE;
}

// Ask a yes/no question on the status bar. Only `y` agrees; every other key
// is a no, because the dangerous direction here is the one that throws away
// a session and its history, and a stray keystroke should never pick it.
static bool confirm(const char *question)
{
    cursor_to(s_rows, 1);
    bar_on();
    draw_clipped(question, s_cols - 1);
    erase_to_eol();
    sgr_reset();
    cursor_to(s_rows, (int32_t)os64_strlen(question) + 1);

    char c;
    if (key_byte(&c, OS64_WAIT_FOREVER) != 1)
        return true;                     // input ended: there is nobody to ask
    return c == 'y' || c == 'Y';
}

// WHAT A PROMPT CAME BACK WITH, and it takes three words rather than two.
// "Nothing typed" and "changed my mind" are different answers — a caller
// holding a default must use it for the first and abandon the whole job for
// the second — and a bool made them the same, so Escape at `save as [x]:`
// downloaded x.
typedef enum {
    PROMPT_TYPED = 0,   // there is text in the buffer
    PROMPT_EMPTY,       // Enter on an empty line: whatever the default is
    PROMPT_CANCELLED,   // Escape, or the input ended
} prompt_result_t;

// Ask for a line, on the status bar, with echo this program does itself —
// the console does not echo, which is exactly what a full-screen program
// needs everywhere except here.
static prompt_result_t prompt(const char *label, char *out_text, size_t cap)
{
    size_t n = 0;
    out_text[0] = '\0';
    for (;;) {
        cursor_to(s_rows, 1);
        bar_on();
        char shown[256];
        os64_snprintf(shown, sizeof(shown), " %s%s", label, out_text);
        draw_clipped(shown, s_cols - 1);
        erase_to_eol();
        sgr_reset();

        char c;
        if (key_byte(&c, OS64_WAIT_FOREVER) != 1)
            return PROMPT_CANCELLED;    // nobody is there to finish answering
        if (c == '\r' || c == '\n')
            return n > 0 ? PROMPT_TYPED : PROMPT_EMPTY;
        if (c == 0x1B)
            return PROMPT_CANCELLED;            // changed your mind
        if (c == '\b' || c == 0x7F) {
            if (n > 0) out_text[--n] = '\0';
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F)
            continue;                           // never take a byte that paints
        if (n + 1 < cap) {
            out_text[n++] = c;
            out_text[n] = '\0';
        }
    }
}

// ── Following an item ───────────────────────────────────────────────────

// THE DEFAULT FILENAME IS THE SERVER'S SUGGESTION, and accepting it is one
// keystroke — so it has to be a name and not a route to somewhere else.
//
// A BACKSLASH SEPARATES PATH COMPONENTS ON FAT (FatFs `IsSeparator`, and the
// lifeboat partition is FAT), so splitting on '/' alone is not "the last
// component" anywhere it matters: a selector of `/bin/..\limine.conf` yields
// `..\limine.conf`, and pressing Enter at the prompt would have written the
// staging file a directory up and then renamed it over the boot config that
// exists for the day root is broken. Splitting on BOTH separators is half the
// answer; the other half is that a component of `.` or `..` is not a name
// either. Anything that fails leaves the default EMPTY, which the caller
// already knows means "ask the person" — a suggestion this program cannot
// vouch for is one it should not make.
//
// A name the person TYPES is their own business and is not touched: that is
// the difference between a path somebody chose and one a stranger chose for
// them.
static bool name_is_one_component(const char *name)
{
    if (name[0] == '\0')
        return false;
    for (const char *p = name; *p != '\0'; p++)
        if (*p == '/' || *p == '\\')
            return false;
    if (os64_streq(name, ".") || os64_streq(name, ".."))
        return false;
    return true;
}

static void selector_basename(const char *selector, char *out_name, size_t cap)
{
    const char *last = selector;
    for (const char *p = selector; *p != '\0'; p++)
        if (*p == '/' || *p == '\\')
            last = p + 1;
    os64_strcopy(out_name, cap, last);
    if (!name_is_one_component(out_name))
        out_name[0] = '\0';
}

// Stream a binary item to `<name>.part`, COMMIT it, then rename it into
// place. The stage-commit-rename discipline is os64get's, and it is here for
// the same reason: a transfer that dies halfway must not leave something that
// looks like a finished file. All three steps are the discipline — a rename
// that publishes bytes the disk never accepted is the same lie by a quieter
// route.
//
// Answers what became of the transfer; the sentence for the person is on the
// status bar in every case. CANCELLED is its own answer rather than a kind of
// failure, because a person who changed their mind did not hit a problem —
// and when this is the whole job it decides the exit code, where "you asked
// me not to" and "the disk refused" must not be the same number.
typedef enum {
    SAVE_DONE = 0,
    SAVE_CANCELLED,
    SAVE_FAILED,
} save_result_t;

static save_result_t save_item(const gopher_addr_t *addr)
{
    char name[256];
    selector_basename(addr->selector, name, sizeof(name));
    char asked[256];
    os64_snprintf(asked, sizeof(asked), "save as [%s]: ", name[0] != '\0' ? name : "");
    char typed[256];
    switch (prompt(asked, typed, sizeof(typed))) {
        case PROMPT_TYPED:
            os64_strcopy(name, sizeof(name), typed);
            break;
        case PROMPT_EMPTY:
            break;                       // the default in the brackets
        case PROMPT_CANCELLED:
            // Before anything is opened, so nothing is truncated: reading a
            // cancel as "yes, the default" opens <default>.part and writes
            // over whatever a previous transfer left there.
            os64_strcopy(s_status, sizeof(s_status), " nothing saved (cancelled)");
            return SAVE_CANCELLED;
    }
    if (name[0] == '\0') {
        // Enter on an empty default. Also a choice, not a fault: the person
        // was shown an empty bracket and pressed Enter anyway.
        os64_strcopy(s_status, sizeof(s_status), " nothing saved (no name)");
        return SAVE_CANCELLED;
    }

    char part[280];
    os64_snprintf(part, sizeof(part), "%s.part", name);

    char dialstring[GOPHER_HOST_MAX + 32];
    os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%u",
                  addr->host, (unsigned)addr->port);
    int64_t conn = os64_dial(dialstring);
    if (conn < 0) {
        os64_snprintf(s_status, sizeof(s_status), " cannot reach %s — %s",
                      addr->host, os64_dial_reason(conn));
        return SAVE_FAILED;
    }

    char request[GOPHER_SELECTOR_MAX + GOPHER_QUERY_MAX + 8];
    size_t reqlen = gopher_request(addr, request, sizeof(request));
    if (reqlen == 0 || os64_write((int32_t)conn, request, reqlen) < 0) {
        os64_close((int32_t)conn);
        os64_strcopy(s_status, sizeof(s_status), " the request could not be sent");
        return SAVE_FAILED;
    }

    int64_t file = os64_open(part, "w");
    if (file < 0) {
        os64_close((int32_t)conn);
        os64_snprintf(s_status, sizeof(s_status), " cannot write %s", part);
        return SAVE_FAILED;
    }

    source_t src = { conn, false };
    gopher_stream_t stream;
    gopher_stream_init(&stream, source_read, &src);

    uint64_t total = 0;
    bool broke = false;
    char buf[4096];
    for (;;) {
        int64_t n = gopher_stream_raw(&stream, buf, sizeof(buf));
        if (n < 0) { broke = true; break; }
        if (n == 0) break;
        if (os64_write((int32_t)file, buf, (size_t)n) != n) { broke = true; break; }
        total += (uint64_t)n;
    }
    // COMMIT BEFORE PUBLISHING, and ask whether the commit worked. A close
    // is where FAT writes its data and metadata (handle.c says so), and
    // close() has nowhere to hand a failure back to — the kernel logs it and
    // ring 3 is told nothing. So a `.part` full of bytes the disk never took
    // would have been renamed into place and called saved, which is the exact
    // thing the staging discipline exists to prevent. os64get asks this
    // question at every one of its downloads; this is the one that did not.
    // (On ext2 it is a formality — write-through leaves nothing to fail —
    // and the lifeboat is FAT.)
    bool uncommitted = !broke && os64_sync((int32_t)file) < 0;
    os64_close((int32_t)file);
    os64_close((int32_t)conn);

    // The .part stays either way. A person can see what arrived; nothing
    // wears the real name until all of it did — which is only TRUE with the
    // sync above, because the wire and the disk are two different ways for
    // this to go wrong and only one of them announces itself.
    if (uncommitted) {
        os64_snprintf(s_status, sizeof(s_status),
                      " %lu bytes arrived but the disk would not take them"
                      " — %s kept, %s NOT written",
                      (unsigned long)total, part, name);
        return SAVE_FAILED;
    }
    if (broke) {
        os64_snprintf(s_status, sizeof(s_status),
                      " transfer broke after %lu bytes — %s kept",
                      (unsigned long)total, part);
        return SAVE_FAILED;
    }
    if (os64_rename(part, name) < 0) {
        os64_snprintf(s_status, sizeof(s_status),
                      " %lu bytes saved, but %s could not be renamed",
                      (unsigned long)total, part);
        return SAVE_FAILED;
    }
    os64_snprintf(s_status, sizeof(s_status), " saved %s (%lu bytes)",
                  name, (unsigned long)total);
    return SAVE_DONE;
}

// An `h` item's selector is `URL:<address>`, and os64get is what fetches it.
// HANDING OFF RATHER THAN GUESSING is the point: this program speaks gopher,
// os64get speaks the web, and the exit code says what happened.
static bool starts_with_url_prefix(const char *s)
{
    // "URL:" is the convention, but servers write it in whatever case they
    // felt like that day, so the four bytes are compared without one.
    const char *want = "url:";
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + ('a' - 'A'));
        if (c != want[i])
            return false;
    }
    return true;
}

// WHAT OS64GET'S NUMBER MEANT, in a sentence.
//
// The handoff is only as good as this translation, which is the whole reason
// os64get's exit codes were made precise one code at a time. A status bar
// reading "os64get exited 2" asks the person to go and look it up, and the
// two answers they most often need — "os64 has no TLS, so an https address
// needs a proxy" and "it downloaded, here is where it went" — are exactly
// the two that a bare number hides.
static const char *os64get_said(int32_t code)
{
    switch (code) {
        case 0:  return "fetched";
        case 2:  return "os64 has no TLS: an https address needs $https_proxy set";
                 // 13 and 2 say the same thing about a web link, and which
                 // one arrives depends on WHERE the https showed up — typed
                 // in the item (13) or reached by a redirect (2). A person
                 // reading a status bar does not care which, and needs the
                 // same next move either way.
        case 3:  return "could not reach that host";
        case 4:  return "the request could not be sent";
        case 5:  return "the server refused the page";
        case 6:  return "the server answered something that was not HTTP";
        case 7:  return "the connection died before the page was whole";
        case 8:  return "the page arrived complete and corrupt";
        case 9:  return "the page could not be written";
        case 10: return "fetched, but could not be put in place";
        case 13: return "cannot fetch that address"
                        " (an https link needs $https_proxy set: os64 has no TLS)";
        case 14: return "the page is coded in a way os64get cannot read";
        case 15: return "the trail of redirects did not arrive"
                        " (an https target needs $https_proxy set)";
        default: return "the fetch failed";
    }
}

// Follow an `h` item. Its selector is `URL:<address>`, os64get is what
// fetches it, and this program is what SHOWS it — because a person who
// followed a link and got their menu repainted with a number at the bottom
// has not been shown anything at all.
//
// WHAT COMES BACK IS HTML SOURCE, and that is honest at this rung of the
// ladder rather than a placeholder: os64 has no renderer yet, and the source
// of a page is strictly more than the nothing that was on offer before.
//
// WHICH `h` ITEMS REACH HERE is the caller's decision: only the ones whose
// selector carries a `URL:`. The others are type h's ORIGINAL meaning — an
// HTML file served by the gopher server itself, which predates the `URL:`
// convention — and are fetched over gopher as text. The guard below is kept
// anyway, because a function that trusts its caller to have checked is a
// function that breaks quietly when a second caller appears.
//
// ANSWERS A GOPHER EXIT CODE — 0, or the one this program would exit with if
// the handoff were the whole job. Inside a session the caller drops it and
// the status bar carries the words; from the command line it IS the outcome,
// and every failure here used to be reduced to a sentence under a returned
// success. THE SENTENCE STAYS WHERE THE PRECISION IS: os64get's own codes are
// translated into English (os64get_said) rather than passed through, because
// two exit vocabularies in one number is worse than one blunt code, so the
// answer here is only ever "the address was wrong" or "the fetch did not
// happen".
static bool view_local_text(const char *path, const char *title);

static int32_t hand_to_os64get(const char *selector)
{
    if (!starts_with_url_prefix(selector)) {
        os64_strcopy(s_status, sizeof(s_status),
                     " that link has no address in it");
        return GOPHER_BAD_ADDRESS;
    }
    const char *url = selector + 4;
    if (url[0] == '\0') {
        os64_strcopy(s_status, sizeof(s_status), " that link carries no address");
        return GOPHER_BAD_ADDRESS;
    }
    // A SPAWN ARGUMENT IS BOUNDED AND A SELECTOR IS NOT, so the two caps
    // disagree and the gap has to be named rather than discovered. An
    // argument is capped at TASK_MAX_PATH_LEN (ABI.md) while a selector runs
    // to GOPHER_SELECTOR_MAX, so a long enough `URL:` is a link this program
    // accepted, parsed and cannot hand over — and the spawn's plain failure
    // read as "could not run os64get", which sends a person to look for a
    // missing binary that is sitting right there. Say what is actually
    // wrong; carrying such a URL through a wider channel is in DECLINED.md.
    if (os64_strlen(url) >= OS64_SPAWN_ARG_MAX) {
        os64_snprintf(s_status, sizeof(s_status),
                      " that link's address is longer than %d bytes,"
                      " which is more than os64get can be handed",
                      (int)OS64_SPAWN_ARG_MAX - 1);
        return GOPHER_BAD_ADDRESS;
    }

    // Somewhere to put it that is nobody's working directory. Named for the
    // task so two gophers cannot land on one file.
    char temp[64];
    os64_snprintf(temp, sizeof(temp), "/tmp/gopher.%ld.html", (long)os64_taskid());

    // os64get draws a progress meter, so it gets a clean screen to draw on
    // rather than half a menu.
    screen_clear();
    screen_home();
    sgr_reset();

    char *argv[] = { (char *)"os64get", (char *)url, temp, NULL };
    int64_t pid = os64_spawn("/bin/os64get", argv);
    if (pid < 0) {
        os64_strcopy(s_status, sizeof(s_status), " could not run os64get");
        screen_clear();
        return GOPHER_UNREACHABLE;
    }
    int32_t status = 0;
    os64_wait(pid, &status);

    if (status != 0) {
        os64_snprintf(s_status, sizeof(s_status), " %s — %s",
                      url, os64get_said(status));
        screen_clear();
        return GOPHER_UNREACHABLE;
    }

    // The fetch succeeded; SHOWING it is the other half, and it can fail on
    // its own (out of memory, or the disk refusing the file os64get just
    // wrote). A page nobody could be shown is not a handoff that worked.
    bool shown = view_local_text(temp, url);
    os64_unlink(temp);          // shown is the point; kept is os64get's job
    screen_clear();
    return shown ? GOPHER_OK : GOPHER_UNREACHABLE;
}

// ── The session ─────────────────────────────────────────────────────────

static void history_push(const gopher_addr_t *addr, int32_t top, int32_t sel)
{
    if (s_depth >= GOPHER_HISTORY_MAX) {
        // Drop the OLDEST crumb rather than refusing to go forward: a person
        // who has read sixty-four menus wants the sixty-fifth more than they
        // want the first one back.
        for (int32_t i = 1; i < GOPHER_HISTORY_MAX; i++)
            s_history[i - 1] = s_history[i];
        s_depth = GOPHER_HISTORY_MAX - 1;
    }
    s_history[s_depth].addr = *addr;
    s_history[s_depth].top = top;
    s_history[s_depth].sel = sel;
    s_depth++;
}

static void scroll_clamp(page_t *page)
{
    int32_t count = page->isMenu ? page->nitems : page->nlines;
    int32_t rows = content_rows();

    if (page->isMenu) {
        // `sel` stays a valid index whatever the arithmetic did to it, links
        // or no links: Enter reads items[sel], and END on an EMPTY menu asks
        // for item -1. This clamp is a bounds check, not a scrolling policy,
        // which is why it is not under the condition below.
        if (page->sel < 0) page->sel = 0;
        if (page->sel >= count) page->sel = count > 0 ? count - 1 : 0;

        // THE VIEWPORT FOLLOWS THE SELECTION, but only where there IS one. On
        // a menu with nothing followable the selection cannot move, so
        // pinning `top` to it pinned the whole page: the arrows bumped `top`
        // and this dragged it straight back, and a menu of prose taller than
        // the screen could not be read past its first screenful.
        if (page->hasLinks) {
            if (page->sel < page->top) page->top = page->sel;
            if (page->sel >= page->top + rows) page->top = page->sel - rows + 1;
        }
    }
    int32_t maxTop = count > rows ? count - rows : 0;
    if (page->top > maxTop) page->top = maxTop;
    if (page->top < 0) page->top = 0;
}

// Show a local text file full-screen: the same scroller a gopher text file
// gets, pointed at something already on disk. It is its own little loop
// rather than a page in the history, because what you go BACK to from here
// is the menu you were reading, and that menu is still sitting in `browse`'s
// frame waiting to be redrawn.
static bool view_local_text(const char *path, const char *title)
{
    page_t page;
    os64_memset(&page, 0, sizeof(page));
    page.title = title;
    // A LOCAL FILE HAS NO GOPHER FRAMING TO BE MISSING. `terminated` answers
    // "did the answer end the way its protocol says it ends?", and os64get
    // settled that before this file existed — an HTTP body has no dot marker
    // to look for. Left false, the status bar told the truth about a gopher
    // stream and a lie about every page that arrived through the handoff.
    page.terminated = true;

    page.lines = os64_malloc(sizeof(char *) * GOPHER_MAX_LINES);
    if (page.lines == NULL) {
        os64_strcopy(s_status, sizeof(s_status), " not enough memory to show it");
        return false;
    }

    os64_linereader_t reader;
    if (os64_linereader_open(&reader, path) < 0) {
        os64_free(page.lines);
        os64_snprintf(s_status, sizeof(s_status), " %s could not be read", path);
        return false;
    }
    char line[GOPHER_LINE_MAX];
    int64_t read_rc = 0;
    while (page.nlines < GOPHER_MAX_LINES &&
           (read_rc = os64_linereader_line(&reader, line, sizeof(line))) == 1) {
        size_t len = os64_strlen(line);
        // A LONG LINE ARRIVES SHORTENED AND SAYS NOTHING ABOUT IT. The reader
        // truncates to the buffer and swallows the rest of the line by
        // contract (os64/io.h), which is right for a config file and wrong
        // for minified HTML — one line, and the page would look whole. A full
        // buffer is the only evidence there is, so it is what marks the page.
        // It over-reports by one case (a line of exactly this length), and
        // "may not be whole" is the honest side to be wrong on.
        if (len == sizeof(line) - 1)
            page.truncated = true;
        char *copy = os64_malloc(len + 1);
        if (copy == NULL) {
            // Same rule as a fetched page: a document missing lines nobody
            // was told about must not be shown as the document.
            os64_linereader_close(&reader);
            page_free(&page);
            os64_strcopy(s_status, sizeof(s_status), " not enough memory to show it");
            return false;
        }
        os64_memcpy(copy, line, len + 1);
        page.lines[page.nlines++] = copy;
    }
    os64_linereader_close(&reader);
    // A READ THAT FAILED IS NOT A FILE THAT ENDED. The reader answers 1, 0
    // and negative, and a loop testing `== 1` reads the third as the second
    // — so a page cut short by the disk was shown as the whole document, and
    // the caller then deleted the file it came from.
    if (read_rc < 0) {
        page_free(&page);
        os64_snprintf(s_status, sizeof(s_status), " %s could not be read", path);
        return false;
    }
    if (page.nlines == GOPHER_MAX_LINES)
        page.truncated = true;

    screen_clear();
    for (;;) {
        geometry();
        scroll_clamp(&page);
        draw(&page);
        s_status[0] = '\0';

        char literal = 0;
        key_t key = key_read(&literal);
        int32_t rows = content_rows();
        // `q` closes a document without asking — what it discards is a page,
        // not the session. KEY_EOF has to be here too, or a terminal that
        // went away leaves this loop reading nothing forever.
        if (key == KEY_QUIT || key == KEY_LEFT || key == KEY_EOF)
            break;
        switch (key) {
            case KEY_UP:   page.top -= 1; break;
            case KEY_DOWN: page.top += 1; break;
            case KEY_PGUP: page.top -= rows; break;
            case KEY_PGDN: page.top += rows; break;
            case KEY_HOME: page.top = 0; break;
            case KEY_END:  page.top = page.nlines; break;
            default: break;
        }
    }
    page_free(&page);
    return true;
}

// Put the selection on a followable item, starting from where it already is
// and walking in `step` direction. A row is only somewhere to BE if pressing
// Enter there would do something — landing on the prose at the top of a menu
// leaves the cursor somewhere the whole rest of the keyboard disagrees with.
//
// The start is clamped first, because a caller that MOVES BY A DISTANCE can
// hand this an index off either end of the menu, and a walk beginning outside
// the array finds nothing and reports success by leaving `sel` where it was.
static void select_snap(page_t *page, int32_t step)
{
    if (!page->isMenu || page->nitems <= 0)
        return;
    if (page->sel < 0) page->sel = 0;
    if (page->sel >= page->nitems) page->sel = page->nitems - 1;
    for (int32_t i = page->sel; i >= 0 && i < page->nitems; i += step)
        if (page->items[i].followable) { page->sel = i; return; }
}

// The same, and if that direction has nothing left, back the other way. It is
// what a PAGE move wants: a screenful is a distance, not a destination, so
// there is no reason to prefer the far side of the menu to the near one when
// the landing area is all prose.
static void select_snap_either(page_t *page, int32_t step)
{
    select_snap(page, step);
    if (page->isMenu && page->nitems > 0 &&
        page->sel >= 0 && page->sel < page->nitems &&
        !page->items[page->sel].followable)
        select_snap(page, -step);
}

// Move the selection to the next followable item in `step` direction, so the
// arrow keys walk LINKS and skip the prose between them — which is what
// makes a menu full of `i` lines navigable at all.
//
// A MENU WITH NO LINKS SCROLLS INSTEAD, because there is nothing to walk: it
// is a document spelled as a menu, and the arrows are the only way through
// it. That needs `scroll_clamp` to stop pinning the viewport to a selection
// that cannot move, which is where this used to be undone one line later.
static void select_step(page_t *page, int32_t step)
{
    if (!page->isMenu || !page->hasLinks) {
        page->top += step;
        return;
    }
    for (int32_t i = page->sel + step; i >= 0 && i < page->nitems; i += step) {
        if (page->items[i].followable) {
            page->sel = i;
            return;
        }
    }
    // The end of the links in that direction: stay where the last one is
    // rather than running off it.
}

// WHAT FOLLOWING AN ADDRESS MEANS — decided ONCE, because there are two doors
// into it: an item somebody pressed Enter on, and an address somebody typed.
//
// They had drifted, and in the direction that matters: the typed door asked
// `page_fetch` its framing question directly, and everything that is not a
// menu or a text file frames as BINARY — so a typed `h` offered to SAVE the
// HTML file a menu would have SHOWN, and a typed `3`, `8` or `T` offered to
// save things the menu path refuses to follow at all. Two doors onto one
// question is how a rule ends up true at one of them.
//
// `out` receives the address as RESOLVED: an `h` that is a gopher-served file
// becomes type '0', and a `7` carries the query that was asked for. A `7`
// that already names its query (a TAB in the address a person typed) is not
// asked again — it was answered before it arrived.
// "a error item" is what one refusal read as, in the two places that refuse.
// The type names are ordinary English words and none of them is a silent-h or
// a "eu-", so the first letter settles it.
static const char *article_for(const char *word)
{
    char c = word[0];
    if (c >= 'A' && c <= 'Z')
        c = (char)(c + ('a' - 'A'));
    return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? "an" : "a";
}

typedef enum {
    FOLLOW_REFUSED = 0,   // the type is not one this client fetches
    FOLLOW_HANDOFF,       // an `h` carrying a `URL:` — os64get's job
    FOLLOW_SAVE,          // a binary framing — save_item's job
    FOLLOW_PAGE,          // fetch it and show it
    FOLLOW_CANCELLED,     // a search the person decided against
} follow_t;

static follow_t follow_decide(const gopher_addr_t *in, gopher_addr_t *out)
{
    *out = *in;

    if (!gopher_type_followable(in->type))
        return FOLLOW_REFUSED;

    // TYPE `h` MEANS TWO DIFFERENT THINGS, and the SELECTOR says which.
    // `URL:http://…` is the convention the web era added, and os64get fetches
    // it. Anything else is type h's ORIGINAL meaning — an HTML file served by
    // this very gopher server — which is fetched over gopher like any other
    // text. Reading the second as a broken first would refuse pages that are
    // working perfectly.
    if (in->type == 'h') {
        if (starts_with_url_prefix(in->selector))
            return FOLLOW_HANDOFF;
        out->type = '0';
    }

    if (in->type == '7' && in->query[0] == '\0') {
        char query[GOPHER_QUERY_MAX];
        // Only typed text is a search. There is no default here to fall back
        // to, so an empty line and an Escape want the same thing — unlike a
        // save, which has a name in brackets and must tell the two apart.
        if (prompt("search for: ", query, sizeof(query)) != PROMPT_TYPED)
            return FOLLOW_CANCELLED;
        os64_strcopy(out->query, sizeof(out->query), query);
    }

    if (gopher_framing_for(out->type) == GOPHER_FRAMING_BINARY)
        return FOLLOW_SAVE;
    return FOLLOW_PAGE;
}

// GO SOMEWHERE, OR STAY EXACTLY WHERE YOU ARE.
//
// The fetch lands in a SECOND page and the current one is not touched until
// it succeeds, which is the whole point: a link into a dead host, a burrow
// that has gone off the air, a menu whose server is being rebuilt — none of
// those are reasons to lose the session and the history that got you there.
// The failure becomes a sentence on the status bar and the page you were
// reading is still under it.
//
// `landTop`/`landSel` are where to sit on the new page; -1 means the top,
// which is right for going forward. Only Back has somewhere to return to.
static bool navigate_to(page_t *page, gopher_addr_t *addr,
                        const gopher_addr_t *target,
                        int32_t landTop, int32_t landSel)
{
    page_t next;
    int64_t rc = page_fetch(target, &next);
    if (rc == GOPHER_FETCH_BINARY) {
        // A download is not somewhere to BE, so this is not a navigation
        // whatever the answer. A caller that asks the framing first never
        // arrives here; this keeps the rule true for one that does not.
        save_item(target);
        return false;
    }
    if (rc != 0) {
        if (rc < 0)
            os64_snprintf(s_status, sizeof(s_status), " cannot reach %s:%u — %s",
                          target->host, (unsigned)target->port,
                          os64_dial_reason(rc));
        else
            os64_snprintf(s_status, sizeof(s_status),
                          " %s did not answer whole — out of memory, a broken"
                          " connection, or an address too long", target->host);
        return false;
    }

    page_free(page);
    *page = next;
    *addr = *target;
    if (landTop >= 0) {
        page->top = landTop;
        // A CRUMB REMEMBERS AN INDEX, AND AN INDEX IS NOT A PLACE. Back
        // REFETCHES (DEBTS: there is no cache), so the menu underneath the
        // remembered number may have changed shape while you were away, and
        // installing it raw overwrote the followable row page_fetch had just
        // chosen. Snap it back to something Enter would act on — the rule is
        // that the selection only ever sits where pressing Enter does
        // something, and a restored one is not exempt from it.
        page->sel = landSel;
        select_snap_either(page, +1);
    }
    screen_clear();
    return true;
}

static int32_t browse(gopher_addr_t start)
{
    page_t page;

    // AN ADDRESS MEANS THE SAME THING WHEREVER IT CAME FROM, and this is the
    // line that makes that true: the address a person typed goes through the
    // same decision an item they pressed Enter on does. A typed `9` downloads,
    // a typed `h` file is shown as the text it is, a typed `7` asks for its
    // search, and a type this client does not follow is refused here rather
    // than offered as a download.
    //
    // What differs is only what there is to RETURN to. Inside a session these
    // outcomes are a sentence on the status bar; here each one is the whole
    // job, so each ends the program with a code.
    gopher_addr_t addr;
    follow_t action = follow_decide(&start, &addr);

    if (action == FOLLOW_REFUSED) {
        os64_hprintf(OS64_STDERR, "gopher: %s %s item is not something to"
                     " follow\n", article_for(gopher_type_name(start.type)),
                     gopher_type_name(start.type));
        return GOPHER_BAD_ADDRESS;
    }
    if (action == FOLLOW_CANCELLED)
        return GOPHER_OK;                 // asked for a search, thought better of it
    if (action == FOLLOW_HANDOFF) {
        int32_t rc = hand_to_os64get(addr.selector);
        screen_clear();
        screen_home();
        sgr_reset();
        // A page that was shown leaves nothing to say — the showing was the
        // saying. Only a failure has words, and they go where words about
        // failures go.
        if (s_status[0] != '\0')
            os64_hprintf(rc == GOPHER_OK ? OS64_STDOUT : OS64_STDERR,
                         "gopher:%s\n", s_status);
        return rc;
    }
    if (action == FOLLOW_SAVE) {
        // The prompt is the one the menu's Enter asks, on a screen cleared
        // for it — and then there is no session to have: a download is all
        // of it.
        screen_clear();
        screen_home();
        save_result_t saved = save_item(&addr);
        screen_clear();
        screen_home();
        sgr_reset();
        // A CANCEL IS NOT A FAILURE, and the exit code has to agree with the
        // one a cancelled search gives: the person answered the question, and
        // the answer was no. Only the disk saying no is worth a code a script
        // would act on.
        bool failed = saved == SAVE_FAILED;
        os64_hprintf(failed ? OS64_STDERR : OS64_STDOUT, "gopher:%s\n", s_status);
        return failed ? GOPHER_SAVE_FAILED : GOPHER_OK;
    }

    // A FETCH THAT FAILS IS FATAL HERE AND NOWHERE ELSE, and that asymmetry
    // is the design rather than an accident of where the code sits. `gopher
    // dead.host` is a command that failed and owes an exit code; a dead link
    // reached from inside a session is a page that did not load, and the
    // session is still perfectly alive.
    int64_t rc = page_fetch(&addr, &page);
    if (rc != 0) {
        if (rc < 0)
            os64_hprintf(OS64_STDERR, "gopher: cannot reach %s:%u — %s\n",
                         addr.host, (unsigned)addr.port, os64_dial_reason(rc));
        else
            os64_hprintf(OS64_STDERR, "gopher: %s:%u did not answer whole"
                         " — out of memory, a broken connection,"
                         " or an address too long\n",
                         addr.host, (unsigned)addr.port);
        return GOPHER_UNREACHABLE;
    }

    screen_clear();
    for (;;) {
        geometry();                   // the window may have been resized
        scroll_clamp(&page);
        draw(&page);
        s_status[0] = '\0';

        char literal = 0;
        key_t key = key_read(&literal);
        int32_t rows = content_rows();

        switch (key) {
            case KEY_UP:    select_step(&page, -1); break;
            case KEY_DOWN:  select_step(&page, +1); break;
            // A page move travels a screenful and then looks for somewhere to
            // land, the way the arrows and Home/End do. Without the snap the
            // highlight sat on whatever prose the arithmetic reached, and
            // Enter answered that an `i` line is not something to follow.
            case KEY_PGUP:
                page.top -= rows; page.sel -= rows;
                select_snap_either(&page, -1);
                break;
            case KEY_PGDN:
                page.top += rows; page.sel += rows;
                select_snap_either(&page, +1);
                break;
            case KEY_HOME:
                page.top = 0;
                page.sel = 0;
                select_snap(&page, +1);
                break;
            case KEY_END:
                page.top = (page.isMenu ? page.nitems : page.nlines);
                if (page.isMenu) {
                    page.sel = page.nitems - 1;
                    select_snap(&page, -1);
                }
                break;

            case KEY_LEFT: {
                if (s_depth == 0) {
                    os64_strcopy(s_status, sizeof(s_status),
                                 " this is where you started");
                    break;
                }
                // The crumb is only SPENT if the page it names comes back.
                const crumb_t *crumb = &s_history[s_depth - 1];
                if (navigate_to(&page, &addr, &crumb->addr, crumb->top, crumb->sel))
                    s_depth--;
                break;
            }

            case KEY_RIGHT:
            case KEY_ENTER: {
                // Both ends, not just the top: `sel` is moved by arithmetic
                // (a page, an end) before anything clamps it, and this is the
                // read that would pay for a negative one.
                if (!page.isMenu || page.sel < 0 || page.sel >= page.nitems)
                    break;
                gopher_item_t *item = &page.items[page.sel];
                gopher_addr_t next;
                switch (follow_decide(&item->addr, &next)) {
                    case FOLLOW_REFUSED:
                        os64_snprintf(s_status, sizeof(s_status),
                                      " %s %s item is not something to follow",
                                      article_for(gopher_type_name(item->type)),
                                      gopher_type_name(item->type));
                        break;
                    case FOLLOW_CANCELLED:
                        os64_strcopy(s_status, sizeof(s_status), " search cancelled");
                        break;
                    case FOLLOW_HANDOFF:
                        hand_to_os64get(next.selector);
                        break;
                    case FOLLOW_SAVE:
                        save_item(&next);
                        break;
                    case FOLLOW_PAGE: {
                        // Where we are NOW, captured before the move can
                        // overwrite it — a crumb is only worth dropping if
                        // the step succeeds.
                        gopher_addr_t from = addr;
                        int32_t fromTop = page.top;
                        int32_t fromSel = page.sel;
                        if (navigate_to(&page, &addr, &next, -1, -1))
                            history_push(&from, fromTop, fromSel);
                        break;
                    }
                }
                break;
            }

            case KEY_QUIT:
                // QUITTING THROWS AWAY THE SESSION AND ITS HISTORY, which is
                // the reason it is the move that gets asked about: everything
                // a person walked through to get here goes with it, and `q`
                // sits one key away from the arrows they were just using.
                if (!confirm(" leave gopher? (y/n) "))
                    break;
                page_free(&page);
                screen_clear();
                screen_home();
                sgr_reset();
                return GOPHER_OK;

            case KEY_EOF:
                // Not a decision anybody made — the terminal went away. There
                // is nothing to confirm with and nobody to confirm it.
                page_free(&page);
                screen_clear();
                screen_home();
                sgr_reset();
                return GOPHER_OK;

            default:
                break;
        }
    }
}

int main(int argc, char **argv)
{
    os64_args_t args;
    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Arrows move, enter follows, left goes back, q quits.";
    args.details = "An address is gopher://host[:port][/<type><selector>],"
                   " or a bare host name.";

    const char *where = NULL;
    int32_t n = os64_args_parse(&args, "gopher [address]", &where, 1);
    if (n < 0)
        return n == OS64_ARG_HELP ? GOPHER_OK : GOPHER_USAGE;
    if (n == 0)
        where = "gopher.floodgap.com";      // the burrow that is still lit

    gopher_addr_t addr;
    gopher_url_result_t rc = gopher_url_parse(where, &addr);
    if (rc != GOPHER_URL_OK) {
        os64_hprintf(OS64_STDERR, "gopher: %s — %s\n", where, gopher_url_reason(rc));
        return GOPHER_BAD_ADDRESS;
    }

    // Keys come from the TERMINAL, not from handle 0, so `gopher < file` and
    // a gopher inside a pipeline still read the person's arrows.
    s_keys = os64_tty_handle();
    if (s_keys < 0)
        s_keys = OS64_STDIN;
    keys_probe_patience();

    geometry();
    int32_t status = browse(addr);
    os64_close((int32_t)s_keys);
    return status;
}
