// wend.c — a line-mode web browser you steer with the keyboard.
//
// BROWSER.md's ladder ends here: libfetch dials, redirects and hands over
// decoded bytes; libhtml turns those bytes into the standard's tree;
// libpage says what the tree MEANS — where each link goes, what each form
// sends; this program walks the tree onto a terminal and lets a person move
// around in it. It is the gopher client's shape — a title row, content rows,
// a status row, arrows and a history stack — pointed at the web.
//
// THE RENDERER IS THROWAWAY AND THE REST IS NOT (BROWSER.md § The face).
// render.c walks and prints; it does not lay out, and the graphical browser
// will not inherit it. What that browser does inherit is everything under
// it: the fetch, the parse, the model, and the navigator in this file.
//
// NO ESCAPE SEQUENCE FROM THE WIRE REACHES THE TERMINAL, and it takes two
// guards because there are two kinds of stranger's bytes here. PAGE TEXT is
// folded to printable Latin-1 on its way out of the tree (render.c), which
// is also what makes a byte a cell. An ADDRESS is not folded — a URL, a
// title in the bar, a reason from a server — so everything painted goes
// through draw_cells, which escapes what would otherwise steer the glass.
//
// RAW MODE IS WHAT MAKES Ctrl+C MEAN "STOP THIS PAGE". The terminal hands
// over 0x03 as a byte instead of raising SIGINT (SIGINT.md § Raw mode), so
// the fetch can be cancelled at the key everybody presses without a signal
// arriving in the middle of a library's read. Where the terminal will not go
// raw, the SIGINT handler sets the same flag and everything still works.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render.h"

#include "fetch/fetch.h"
#include "html/html.h"
#include "page/page.h"
#include "os64/args.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/procfs.h"
#include "os64/signal.h"
#include "os64/str.h"
#include "os64/url.h"

// WHO THIS SAYS IT IS. A meaningful part of the web answers a request with
// no user agent with a refusal, so the browser names itself and its OS.
#define WEND_AGENT  "wend/1.0 (os64)"
// What the loader will actually render, said out loud: a server choosing
// between representations should know that any text one will do, and a
// server that would answer 406 to a narrower list should not.
//
// XHTML IS ACCEPTED AND PARSED AS HTML, which is a divergence and not an
// oversight. There is no XML parser here, and the HTML tree builder reads
// all but the constructs XML spells differently — a self-closing `<script/>`
// ends where XML says and not where HTML does, so the text after it is
// swallowed. Refusing the type outright would turn every XHTML page into
// "that is not a page", which is worse for a reader than a rare page with a
// swallowed tail. Booked in BROWSER.md.
#define WEND_ACCEPT "text/html, application/xhtml+xml, text/*;q=0.8"

#define WEND_HISTORY_MAX 64
#define WEND_STATUS_MAX  512

// Exit codes in os64get's and the gopher client's shape: a number a script
// can act on, each naming a different thing to go and fix. They apply to the
// address a person TYPED — inside a session a failure is a sentence on the
// status row and the session goes on.
#define WEND_OK        0
#define WEND_USAGE     2
#define WEND_BAD_URL   13
#define WEND_FAILED    5

// ── Where we are ────────────────────────────────────────────────────────

// A page, and everything needed to draw it again at a different width. The
// TREE is kept, not just the lines: a resize re-wraps from the parse instead
// of asking the network for a page we already have.
typedef struct {
    char     url[OS64_FETCH_URL_MAX];   // the address the body came from
    os64_html_document_t *doc;          // HTML: the tree, kept for re-wrapping
    char    *text;                      // text/plain: the bytes, same reason
    size_t   textlen;
    bool     text_utf8;
    wend_page_t *page;                  // the lines, at the current width
    // WHAT THE PAGE MEANS, as opposed to how it reads (LIBPAGE.md): where
    // every link goes, what every control holds, what a form sends. Built
    // once per page because it does not depend on the width, and it is where
    // a person's edits are kept — so a re-wrap throws the lines away and
    // draws them again from this, and what was typed into a search box
    // survives it.
    os64_page_t *model;
    bool refresh_handled;             // per loaded document, including refused attempts
    int32_t  top;                       // first visible row
    int32_t  sel;                       // the selected spot, -1 for none
    char     note[WEND_STATUS_MAX];     // what this page's status row says
} view_t;

typedef struct {
    char    url[OS64_FETCH_URL_MAX];
    int32_t top, sel;
} crumb_t;

static crumb_t s_history[WEND_HISTORY_MAX];
static int32_t s_depth;

static int32_t s_rows = 25;
static int32_t s_cols = 80;
static int64_t s_keys = OS64_STDIN;
static char    s_status[WEND_STATUS_MAX];
static bool    s_raw;

// Set by a handler, read by the loops. A resize is noticed wherever the
// program next looks; a cancel ends the fetch that is in flight; a hangup
// ends the session rather than the process, because a terminal that is going
// away is still owed its screen back.
static volatile int s_want_size;
static volatile int s_cancel;
static volatile int s_want_quit;
static volatile int s_quit_signal;

// ── The glass ───────────────────────────────────────────────────────────

// Whether this program has written on the screen it was handed. An exit puts
// the terminal back only where there is something to put back: clearing a
// screen we never wrote on would throw away whatever the person had on it.
static bool s_painted;

static void out(const char *s) { s_painted = true; os64_write(OS64_STDOUT, s, os64_strlen(s)); }
static void outn(const char *s, size_t n) { s_painted = true; os64_write(OS64_STDOUT, s, n); }

static void screen_clear(void) { out("\033[2J"); }
static void screen_home(void)  { out("\033[H"); }

static void cursor_to(int32_t row, int32_t col)
{
    char seq[32];
    os64_snprintf(seq, sizeof(seq), "\033[%d;%dH", (int)row, (int)col);
    out(seq);
}

static void erase_to_eol(void) { out("\033[K"); }

static void sgr(const char *params)
{
    char seq[32];
    os64_snprintf(seq, sizeof(seq), "\033[%sm", params);
    out(seq);
}

static void sgr_reset(void) { out("\033[0m"); }

// A BAR IS PAINTED WITH A BACKGROUND, NOT WITH REVERSE VIDEO, because
// `ESC[K` fills the rest of the row with the PEN's background: a coloured
// bar runs to the margin, a reverse-video one stops at its last character.
// Padding with spaces instead would write the bottom-right cell, and writing
// there scrolls the screen in any terminal that wraps.
static void bar_on(void) { sgr("30;47"); }

// Hand the terminal back the way it was found, and be safe to call twice.
// `main` calls this after the session returns, which is what makes it happen
// on every path rather than on the paths somebody remembered.
static void screen_restore(void)
{
    if (!s_painted)
        return;
    screen_clear();
    screen_home();
    sgr_reset();
    s_painted = false;
}

// Draw at most `width` cells and answer how many were used.
//
// THIS IS WHERE A STRANGER'S BYTES STOP BEING OBEYED. Page text arrives
// already folded to printable Latin-1, but an address, a server's reason and
// a page's title do not, and a guard written separately at each of those
// roads is one the next road is added without. The notation is `cat -v`'s,
// for its reason: `^[` says WHICH byte was there where a dropped byte says
// only that something was.
static int32_t draw_cells(const char *text, size_t len, int32_t width)
{
    int32_t n = 0;
    for (size_t i = 0; i < len && n < width; i++) {
        unsigned char b = (unsigned char)text[i];
        if (b >= 0x20 && b != 0x7F) {
            outn(text + i, 1);
            n++;
        } else if (b == '\t') {
            outn(" ", 1);
            n++;
        } else if (n + 2 <= width) {
            char caret[2] = { '^', (char)(b == 0x7F ? '?' : b + 0x40) };
            outn(caret, 2);
            n += 2;
        } else {
            break;                       // no room for both halves
        }
    }
    return n;
}

static int32_t draw_text(const char *text, int32_t width)
{
    return draw_cells(text, os64_strlen(text), width);
}

static int32_t content_rows(void) { return s_rows - 2; }

static void geometry(void)
{
    os64_tty_info_t tty;
    if (os64_tty_read(&tty) == 0 && tty.rows > 4 && tty.cols > 20) {
        s_rows = (int32_t)tty.rows;
        s_cols = (int32_t)tty.cols;
    }
}

// ── Keys ────────────────────────────────────────────────────────────────

// How long to wait for the rest of an escape sequence before deciding the
// Escape KEY was pressed. Long enough for the bytes of one sequence to
// arrive together, far too short for a person to notice.
#define WEND_ESC_LOOKAHEAD_MS 50

typedef enum {
    KEY_NONE = 0, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, KEY_ENTER, KEY_BACKTAB,
    KEY_EOF, KEY_OTHER,
} key_t;

// Bytes that arrived while nobody was asking for them: typed ahead during a
// fetch, or read while looking for the rest of a sequence that turned out
// not to be one. They belong to whoever asks next, because throwing them
// away would lose keystrokes a person actually meant — with one exception,
// which `confirm` argues: a question about safety must be answered after it
// is asked. Room is finite and the overflow is dropped; holding keys is a
// courtesy, and the fetch that drops them is still watching for Ctrl+C.
static unsigned char s_pending[64];
static int32_t s_npending;

static void pending_push(unsigned char c)
{
    if (s_npending < (int32_t)sizeof(s_pending))
        s_pending[s_npending++] = c;
}

static void pending_push_front(unsigned char c)
{
    if (s_npending >= (int32_t)sizeof(s_pending))
        return;
    for (int32_t i = s_npending; i > 0; i--)
        s_pending[i] = s_pending[i - 1];
    s_pending[0] = c;
    s_npending++;
}

static bool pending_pop(char *c)
{
    if (s_npending == 0)
        return false;
    *c = (char)s_pending[0];
    for (int32_t i = 1; i < s_npending; i++)
        s_pending[i - 1] = s_pending[i];
    s_npending--;
    return true;
}

// Whether this input can be asked to wait a BOUNDED time. The console can and
// a file cannot (os64/io.h refuses a finite patience elsewhere), so this is
// false only where the keys are not a terminal, and there the lookahead
// blocks exactly as a blocking read always did.
static bool s_timed_keys = true;

#define KEY_BYTE_GOT         1
#define KEY_BYTE_TIMEOUT     0
#define KEY_BYTE_ENDED     (-1)
#define KEY_BYTE_INTERRUPT (-2)

static int key_byte(char *c, uint64_t patience_ms)
{
    if (pending_pop(c))
        return KEY_BYTE_GOT;
    if (!s_timed_keys)
        patience_ms = OS64_WAIT_FOREVER;
    int64_t n = os64_read_for((int32_t)s_keys, c, 1, patience_ms);
    if (n == 1)                  return KEY_BYTE_GOT;
    if (n == OS64_ERR_TIMEOUT)   return KEY_BYTE_TIMEOUT;
    // A CAUGHT SIGNAL IS NOT THE END OF THE INPUT. SIGWINCH interrupts the
    // wait for a key, and reading that as a closed terminal would end the
    // session every time the window changed size.
    if (n == OS64_INTERRUPTED)   return KEY_BYTE_INTERRUPT;
    return KEY_BYTE_ENDED;
}

static void keys_probe_patience(void)
{
    char c;
    int64_t n = os64_read_for((int32_t)s_keys, &c, 1, 0);
    if (n == 1)
        pending_push(c);
    else if (n != OS64_ERR_TIMEOUT && n != OS64_INTERRUPTED)
        s_timed_keys = false;
}

static key_t key_read(char *literal)
{
    char c;
    *literal = '\0';
    int got = key_byte(&c, OS64_WAIT_FOREVER);
    if (got == KEY_BYTE_INTERRUPT)
        return KEY_NONE;                 // a signal; the caller looks around
    if (got != KEY_BYTE_GOT)
        return KEY_EOF;
    *literal = c;

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c != 0x1B)              return KEY_OTHER;

    char bracket;
    got = key_byte(&bracket, WEND_ESC_LOOKAHEAD_MS);
    if (got == KEY_BYTE_ENDED)
        return KEY_EOF;
    if (got != KEY_BYTE_GOT)
        return KEY_NONE;                 // the Escape KEY: nothing followed
    if (bracket != '[') {
        pending_push_front((unsigned char)bracket);
        return KEY_NONE;
    }

    char final;
    got = key_byte(&final, WEND_ESC_LOOKAHEAD_MS);
    if (got == KEY_BYTE_ENDED)
        return KEY_EOF;
    if (got != KEY_BYTE_GOT)
        return KEY_NONE;
    switch (final) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'Z': return KEY_BACKTAB;    // Shift+Tab, where a terminal sends it
        default: break;
    }
    if (final >= '0' && final <= '9') {
        char tilde;
        if (key_byte(&tilde, WEND_ESC_LOOKAHEAD_MS) == KEY_BYTE_GOT && tilde != '~')
            pending_push_front((unsigned char)tilde);
        if (final == '5') return KEY_PGUP;
        if (final == '6') return KEY_PGDN;
    }
    return KEY_NONE;
}

// EVERYTHING TYPED BEFORE NOW, THROWN AWAY — both the keys this program is
// holding and the ones still sitting in the terminal. Two queues, because
// nothing polls the terminal continuously: libfetch asks whether to stop
// only between waits, so a key struck after the last of those questions and
// before the next paint is in the tty and in no buffer of ours.
//
// A Ctrl+C among them still STOPS a fetch that is running, which is what it
// was struck for. It does not answer the question: only `y` ever agrees, so
// dropping the keys is already enough to keep a stale one from approving
// anything, and a question that answered itself would be a question asked
// with no visible reply.
//
// Bounded, because this must end: a zero-patience read stops when the queue
// is empty, and the count is what stops it if something is feeding the
// terminal faster than it can be drained.
#define WEND_DRAIN_MAX 4096

static void keys_drop_typeahead(void)
{
    s_npending = 0;
    // An input that cannot be asked to wait a bounded time cannot be POLLED
    // either, and blocking here would hang on a question nobody has read yet.
    if (!s_timed_keys)
        return;
    for (int32_t i = 0; i < WEND_DRAIN_MAX; i++) {
        char c;
        if (os64_read_for((int32_t)s_keys, &c, 1, 0) != 1)
            return;
        if (c == 0x03)
            s_cancel = 1;                // the fetch it interrupts still stops
    }
}

static void status_set(const char *fmt, ...);

// Put the question and wait for the answer to it.
static bool confirm_asked(const char *question)
{
    keys_drop_typeahead();
    // A HANGUP DOES NOT WAIT TO BE ASKED. The loop below ends on one, but
    // only once a signal interrupts the read it is already sitting in — and
    // a session already told to go has nobody left to answer.
    if (s_want_quit)
        return false;
    cursor_to(s_rows, 1);
    bar_on();
    int32_t used = draw_text(question, s_cols - 1);
    erase_to_eol();
    sgr_reset();
    cursor_to(s_rows, used + 1);

    for (;;) {
        char c;
        int got = key_byte(&c, OS64_WAIT_FOREVER);
        // A CAUGHT SIGNAL IS NOT AN ANSWER. A resize while this is up would
        // otherwise be read as "no" and take the question off the screen
        // with it — the safe direction, but not one anybody chose.
        if (got == KEY_BYTE_INTERRUPT) {
            if (s_want_quit)
                return false;
            continue;
        }
        if (got != KEY_BYTE_GOT)
            return false;                // nobody is there to ask
        return c == 'y' || c == 'Y';
    }
}

// Ask a yes/no question on the status row. Only `y` agrees: the dangerous
// direction is always the one a stray keystroke should not pick.
//
// AND THE ANSWER MUST COME AFTER THE QUESTION. Keys typed while a page was
// loading are held for whoever asks next (fetch_cancelled keeps them), and a
// `y` meant for something else would otherwise answer a question it never
// saw — including "shall I send this in clear?", where a stale keystroke
// could do real harm. So everything typed before the question is dropped
// here, and only here: everywhere else, type-ahead is a person working
// faster than the network.
//
// A SECURITY question put to keys that are not a terminal is answered NO
// without reading. Such input cannot be polled, so what was typed before the
// question cannot be told from what came after it, and an answer that cannot
// be shown to be fresh is not a person's decision.
//
// `refused` is the caller's sentence for a no, put on the status row here so
// that a refusal nobody typed can say why; NULL says nothing.
static bool confirm(const char *question, bool security, const char *refused)
{
    if (security && !s_timed_keys) {
        status_set("%s - this input is not a terminal, so it cannot answer",
                   refused != NULL ? refused : "");
        return false;
    }
    bool yes = confirm_asked(question);
    if (!yes && refused != NULL)
        status_set("%s", refused);
    return yes;
}

// WHAT A PROMPT CAME BACK WITH takes three words rather than two: "nothing
// typed" and "changed my mind" are different answers, and a caller holding a
// default must use it for the first and abandon the job for the second.
typedef enum {
    PROMPT_TYPED = 0,
    PROMPT_EMPTY,
    PROMPT_CANCELLED,
} prompt_result_t;

// Ask for a line on the status row, with echo this program does itself — the
// console does not echo, which is what a full-screen program wants
// everywhere except here. WHATEVER IS IN THE BUFFER IS WHAT THE ANSWER
// STARTS AS, so a form field is edited rather than retyped; a caller wanting
// a blank line passes a blank buffer.
static prompt_result_t prompt(const char *label, char *out_text, size_t cap, bool mask)
{
    size_t n = os64_strlen(out_text);
    if (n + 1 > cap)
        n = cap ? cap - 1 : 0;
    out_text[n] = '\0';
    for (;;) {
        cursor_to(s_rows, 1);
        bar_on();
        int32_t used = draw_text(label, s_cols - 1);
        if (mask) {
            // A PASSWORD IS NOT ECHOED, here or anywhere. The row is a
            // shoulder away from whoever is walking past, and this is the
            // one field whose whole point is that it is not read aloud.
            for (size_t i = 0; i < n && used < s_cols - 1; i++)
                used += draw_cells("*", 1, s_cols - 1 - used);
        } else {
            used += draw_cells(out_text, n, s_cols - 1 - used);
        }
        erase_to_eol();
        sgr_reset();
        cursor_to(s_rows, used + 1);

        char c;
        int got = key_byte(&c, OS64_WAIT_FOREVER);
        // A TERMINATION ASKED FOR OUTSIDE IS ANSWERED INSIDE. The handler
        // sets the flag and interrupts this read; resuming the loop would
        // hold the whole session at a half-typed line until somebody
        // pressed a key, which is not what "end the session" means.
        if (s_want_quit)
            return PROMPT_CANCELLED;
        if (got == KEY_BYTE_INTERRUPT)
            continue;                            // a resize; the question stands
        if (got != KEY_BYTE_GOT)
            return PROMPT_CANCELLED;
        if (c == '\r' || c == '\n')
            return n > 0 ? PROMPT_TYPED : PROMPT_EMPTY;
        if (c == 0x1B || c == 0x03)
            return PROMPT_CANCELLED;             // changed your mind
        if (c == '\b' || c == 0x7F) {
            if (n > 0)
                out_text[--n] = '\0';
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F)
            continue;                            // never take a byte that paints
        if (n + 1 < cap) {
            out_text[n++] = c;
            out_text[n] = '\0';
        }
    }
}

// ── Signals ─────────────────────────────────────────────────────────────

static void on_interrupt(int signo)
{
    (void)signo;
    s_cancel = 1;
}

static void on_resize(int signo)
{
    (void)signo;
    s_want_size = 1;
}

// A hangup asks the session to END rather than to die where it stands: the
// terminal is owed its screen back, and the exit code is the one the
// kernel's own default would have written, so a script cannot tell a handled
// death from an unhandled one.
static void on_hangup(int signo)
{
    s_want_quit = 1;
    s_quit_signal = signo;
}

// ── Saying things ───────────────────────────────────────────────────────

// The transient sentence on the status row: what just happened, shown until
// the next keystroke. A page's own note (its reply status) lives in the view
// and outlives it.
static void status_set(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    os64_vsnprintf(s_status, sizeof(s_status), fmt, args);
    va_end(args);
}

// One row of chrome, painted to the margin. The last cell is left alone and
// `ESC[K` fills the rest: writing the bottom-right cell scrolls the screen in
// any terminal that wraps, and the erase never moves the cursor.
static void bar_paint(int32_t row, const char *text)
{
    cursor_to(row, 1);
    bar_on();
    draw_text(text, s_cols - 1);
    erase_to_eol();
    sgr_reset();
}

static void status_paint(const char *text) { bar_paint(s_rows, text); }

// Say something and paint it NOW, without waiting for the next full draw: a
// fetch takes as long as the network takes, and a person deserves to see what
// is being fetched while it happens.
static void status_show(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    os64_vsnprintf(s_status, sizeof(s_status), fmt, args);
    va_end(args);
    status_paint(s_status);
}

// ── What the library asks us ────────────────────────────────────────────

// Whether to stop. Asked by libfetch before every wait, which is also the
// only chance this program gets to look at the keyboard while a fetch is in
// flight — so this is where a raw-mode Ctrl+C is noticed. Anything else
// typed was typed AHEAD and is still owed to whoever asks next.
static bool fetch_cancelled(void *ctx)
{
    (void)ctx;
    // An input that cannot be asked to wait a bounded time cannot be POLLED
    // either: the read would block, and blocking here would hang the fetch
    // it was called to supervise. A signal still reaches the flag.
    // A hangup ends the fetch as surely as a Ctrl+C: the session it belongs
    // to is going away, and waiting out a slow server first helps nobody.
    if (s_want_quit)
        return true;
    if (!s_timed_keys)
        return s_cancel != 0;
    // KEEP LOOKING EVEN WHEN THERE IS NOWHERE LEFT TO PUT IT. The held keys
    // are a courtesy; finding Ctrl+C is the job. A full buffer that stopped
    // reading would leave the one key that ends a slow fetch sitting unread
    // in the terminal until the fetch ended by itself.
    for (;;) {
        char c;
        int64_t n = os64_read_for((int32_t)s_keys, &c, 1, 0);
        if (n != 1)
            break;
        if (c == 0x03) {
            s_cancel = 1;
            break;
        }
        pending_push((unsigned char)c);   // drops it when there is no room
    }
    return s_cancel != 0;
}

// A DOWNGRADE IS A PERSON'S DECISION, which is the whole reason libfetch
// takes a callback: a script may not follow https into http, and somebody at
// a keyboard may. Every other hop takes the library's own verdict.
static os64_fetch_verdict_t hop_ask(void *ctx, const os64_fetch_hop_t *hop)
{
    (void)ctx;
    if (hop->kind != OS64_FETCH_HOP_DOWNGRADE)
        return OS64_FETCH_HOP_DEFAULT;
    char question[WEND_STATUS_MAX];
    os64_snprintf(question, sizeof(question),
                  " %s sends you to unencrypted http - follow? (y/n) ",
                  hop->target.host);
    bool yes = confirm(question, true, " stopped at the unencrypted hop");
    if (yes)
        status_set(" following an unencrypted hop");
    return yes ? OS64_FETCH_HOP_FOLLOW : OS64_FETCH_HOP_STOP;
}

// ── A page ──────────────────────────────────────────────────────────────

static void view_clear(view_t *v)
{
    // The model points into the tree, so it goes first.
    os64_page_free(v->model);
    if (v->doc)
        os64_html_document_free(v->doc);
    os64_free(v->text);
    wend_page_free(v->page);
    v->model = NULL;
    v->doc = NULL;
    v->text = NULL;
    v->page = NULL;
    v->textlen = 0;
}

// Wrap the page this view is holding to the current width. The tree (or the
// text) is what was kept, so a resize costs a re-wrap and not a fetch — and
// the model holds the edits, so the box still holds what was typed into it.
static void view_layout(view_t *v)
{
    wend_page_free(v->page);
    v->page = v->doc ? wend_render_html(v->doc, v->model, s_cols)
                     : wend_render_text(v->text, v->textlen, v->text_utf8, s_cols);
    if (!v->page)
        status_set(" out of memory laying the page out at %d columns", (int)s_cols);
}

// A media type from libfetch is already lowercased, so these compare
// verbatim.
static bool type_is_text(const char *type)
{
    return type[0] == 't' && type[1] == 'e' && type[2] == 'x' && type[3] == 't'
           && type[4] == '/';
}

// Read the body into the tree. Feeding stops at the parser's first refusal —
// which still leaves a document, because a page that was too large or too
// deep is a page you can read the beginning of.
static os64_html_document_t *parse_body(os64_fetch_t *f, const char *charset,
                                        const char *url)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = charset && charset[0] ? charset : NULL;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (!p)
        return NULL;
    char buf[8192];
    uint64_t shown = 0;
    for (;;) {
        int64_t n = os64_fetch_read(f, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (os64_html_parser_feed(p, buf, (size_t)n) != OS64_HTML_OK)
            break;
        const os64_fetch_progress_t *progress = os64_fetch_progress(f);
        if (progress->produced >= shown + 64u * 1024u) {
            shown = progress->produced;
            status_show(" reading %lu KB of %s", (unsigned long)(shown / 1024), url);
        }
    }
    return os64_html_parser_finish(p);
}

// Read the body as bytes. The cap is the parser's, because a page is a page
// whichever way it is written.
static char *read_body(os64_fetch_t *f, size_t cap, size_t *len, const char *url,
                       bool *whole)
{
    size_t at = 0, size = 16u * 1024u;
    *len = 0;
    *whole = true;
    char *text = os64_malloc(size);
    if (!text)
        return NULL;
    uint64_t shown = 0;
    for (;;) {
        if (at == size) {
            if (size >= cap) {
                // ASK FOR THE BYTE THAT WOULD CROSS THE CAP. libfetch refuses
                // a body at `max_body` on the read that would pass it, not
                // before, so stopping at a full buffer leaves the fetch
                // reporting OK and this page claiming to be whole when it is
                // the first 8 MB of something longer.
                char probe;
                (void)os64_fetch_read(f, &probe, 1);
                break;
            }
            size_t want = size * 2 > cap ? cap : size * 2;
            char *grown = os64_realloc(text, want);
            if (!grown) {
                // OUR failure, not the server's: libfetch will report OK
                // because no read of ours ever failed, so the truncation has
                // to be carried out of here by hand.
                *whole = false;
                break;
            }
            text = grown;
            size = want;
        }
        int64_t n = os64_fetch_read(f, text + at, size - at);
        if (n <= 0)
            break;
        at += (size_t)n;
        if (at >= shown + 64u * 1024u) {
            shown = at;
            status_show(" reading %lu KB of %s", (unsigned long)(shown / 1024), url);
        }
    }
    *len = at;
    return text;
}

// What the reply's own charset label says about a text/plain body. UTF-8 is
// the modern answer, and everything else is read as windows-1252 — which is
// what libhtml does with the markup half of the same web, so a smart quote
// cannot draw as `"` in a page and as `?` in the text file beside it. A
// server that says nothing about a .txt file written in 1994 is not talking
// about UTF-8. A label naming an encoding in neither family is booked in
// BROWSER.md; it reads as windows-1252 rather than as a refusal.
//
// BUT A FILE MAY SAY IT ITSELF. The three bytes `EF BB BF` are a UTF-8 byte
// order mark, put there to be read by whatever opens the file, and a server
// that mentioned no charset has not contradicted them. Taken as
// windows-1252 they draw as `ï»¿` and every accented character after them
// breaks into pieces. The mark itself needs no skipping: the fold spells
// U+FEFF as nothing.
static bool charset_is_utf8(const char *charset)
{
    return os64_streq_nocase(charset, "utf-8") || os64_streq_nocase(charset, "utf8");
}

static bool bytes_begin_utf8(const char *text, size_t len)
{
    return text && len >= 3 && (unsigned char)text[0] == 0xEF
           && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF;
}

// Fetch, parse and lay out one address. The view is built beside the one on
// screen and only replaces it when there is something to show — a page that
// will not load leaves the previous page where it is, with the reason under
// it (BROWSER.md § The face).
// `why` is the fetch's own verdict when there was one, for the caller that
// has to turn it into an exit code. NULL when nobody is asking.
static bool load(const char *url, view_t *out, os64_fetch_status_t *why)
{
    bool short_of_memory = false;        // a truncation of OURS, not the wire's
    if (why)
        *why = OS64_FETCH_OK;
    status_show(" fetching %s", url);
    s_cancel = 0;

    os64_html_options_t limits = os64_html_options_default();
    os64_fetch_options_t opt = { 0 };
    opt.user_agent = WEND_AGENT;
    opt.accept = WEND_ACCEPT;
    opt.max_body = limits.max_bytes;     // the same page, the same cap
    opt.cancelled = fetch_cancelled;
    opt.on_hop = hop_ask;

    os64_fetch_t *f = os64_fetch_open(url, &opt);
    if (!f) {
        status_set(" out of memory fetching %s", url);
        return false;
    }
    const os64_fetch_head_t *head = os64_fetch_head(f);
    if (!head) {
        if (why)
            *why = os64_fetch_status(f);
        status_set(" %s", os64_fetch_reason(f));
        os64_fetch_close(f);
        return false;
    }

    // A 404 IS A PAGE AND IS SHOWN AS ONE. The status goes in the note, not
    // in a refusal: servers say a great deal in the body of a reply a
    // downloader would throw away.
    const char *type = head->content_type;
    bool html = type[0] == '\0' || os64_streq(type, "text/html")
                || os64_streq(type, "application/xhtml+xml");
    bool plain = !html && type_is_text(type);
    if (!html && !plain) {
        // Not a page at all. Say what it is and how to keep it — quoted the
        // way os64get quotes an address, because husk splits a line at `;`.
        bool quotable = true;
        for (const char *q = head->url_text; *q != '\0'; q++)
            if (*q == '\'')
                quotable = false;
        if (quotable)
            status_set(" that is %s, not a page - save it with:  os64get '%s'",
                       type, head->url_text);
        else
            status_set(" that is %s, not a page - and its address holds a quote,"
                       " so save it by hand", type);
        os64_fetch_close(f);
        return false;
    }

    os64_strcopy(out->url, sizeof(out->url), head->url_text);
    out->sel = -1;
    out->top = 0;

    if (html) {
        out->doc = parse_body(f, head->charset, url);
        if (!out->doc) {
            status_set(" out of memory reading %s", url);
            os64_fetch_close(f);
            return false;
        }
        // WHAT THE PAGE MEANS, from the tree and the address it came from —
        // `<base href>` included, which libpage reads. A page it cannot build
        // a model of is still a page worth reading, so this is not a failure
        // to return: what it costs is the links and boxes, which are drawn
        // and cannot be used, and the status row says so.
        out->model = os64_page_build(out->doc, head->url_text, NULL);
    } else {
        bool whole = true;
        out->text = read_body(f, limits.max_bytes, &out->textlen, url, &whole);
        if (!whole)
            short_of_memory = true;
        out->text_utf8 = head->charset[0] != '\0'
                             ? charset_is_utf8(head->charset)
                             : bytes_begin_utf8(out->text, out->textlen);
        if (!out->text) {
            status_set(" out of memory reading %s", url);
            os64_fetch_close(f);
            return false;
        }
    }

    // EVERY WAY A PAGE CAN BE INCOMPLETE GETS A SENTENCE, because half a
    // page that says so is worth reading and half a page that pretends to be
    // whole is not.
    char trouble[WEND_STATUS_MAX];
    trouble[0] = '\0';
    os64_fetch_status_t st = os64_fetch_status(f);
    if (why)
        *why = st;
    if (short_of_memory)
        os64_strcopy(trouble, sizeof(trouble),
                     " - this machine ran out of memory partway, so the page"
                     " stops where it does");
    else if (st != OS64_FETCH_OK)
        os64_snprintf(trouble, sizeof(trouble), " - %s", os64_fetch_reason(f));
    else if (out->doc && out->doc->refusal)
        os64_snprintf(trouble, sizeof(trouble), " - the page is bigger than this"
                      " browser will parse (%s)",
                      os64_html_status_name(out->doc->refusal));
    os64_snprintf(out->note, sizeof(out->note), "%ld%s%s%s", (long)head->status,
                  head->reason[0] ? " " : "", head->reason, trouble);
    os64_fetch_close(f);

    view_layout(out);
    if (!out->page) {
        view_clear(out);                 // view_layout said why
        return false;
    }
    if (out->page->incomplete)
        status_set(" out of memory partway through the page - what is here is real");
    else if (out->doc && (!out->model || os64_page_incomplete(out->model)))
        status_set(" out of memory understanding the page - its links and boxes"
                   " may not work");
    else if (out->page->nlines == 0)
        // A BLANK SCREEN IS AMBIGUOUS AND THIS IS NOT. A reply that parsed
        // to nothing — a page that is all script, a body of zero bytes — is
        // a real answer, and saying so is the difference between "the server
        // sent nothing" and "this browser is broken".
        status_set(" that answer has nothing in it this browser can show");
    else
        s_status[0] = '\0';
    return true;
}

// ── Painting ────────────────────────────────────────────────────────────

// FOUR PENS, AND EACH ANSWERS A DIFFERENT QUESTION about a stretch of text.
// A link is cyan — "you can go there". A form control is green — "you can
// put something here", which is a different promise and worth a different
// colour. The selection is black on cyan, which
// is where a text interface has kept its highlight since Turbo Vision, and
// deliberately not the bars' black-on-white: the title and status rows
// already wear that, and a white selection reads as a third piece of
// furniture rather than as the place you put the cursor. Everything else is
// the terminal's own ink, because a page is mostly prose and prose that is
// painted is prose that is harder to read.
//
// Underline is asked for where a page says italic, which is what italic
// means on a terminal. A glass with no underline consumes the request.
static void pen_set(uint8_t attrs, const char *ink, bool selected)
{
    char params[24];
    os64_snprintf(params, sizeof(params), "0%s%s%s",
                  (attrs & WEND_ATTR_BOLD) ? ";1" : "",
                  (attrs & WEND_ATTR_UNDERLINE) ? ";4" : "",
                  selected ? ";30;46" : ink);
    sgr(params);
}

static void draw_line(const wend_page_t *page, const wend_line_t *line, int32_t selected)
{
    int32_t used = 0;
    for (int32_t i = 0; i < line->nruns && used < s_cols; i++) {
        const wend_run_t *run = &line->runs[i];
        const char *ink = "";
        if (run->spot > 0 && run->spot <= page->nspots)
            ink = page->spots[run->spot - 1].kind == WEND_SPOT_LINK ? ";36" : ";32";
        pen_set(run->attrs, ink, run->spot > 0 && run->spot == selected);
        used += draw_cells(line->text + run->start, run->len, s_cols - used);
    }
    sgr_reset();
}

static void draw(const view_t *v)
{
    const wend_page_t *page = v->page;

    // A page with no title of its own is named by where it came from.
    bar_paint(1, page && page->title[0] ? page->title : v->url);

    int32_t rows = content_rows();
    for (int32_t row = 0; row < rows; row++) {
        cursor_to(row + 2, 1);
        int32_t index = v->top + row;
        if (page && index < page->nlines)
            draw_line(page, &page->lines[index], v->sel + 1);
        erase_to_eol();
    }

    // The transient sentence outranks the standing one: what just happened
    // matters more than where you are, until the next key.
    if (s_status[0] != '\0') {
        status_paint(s_status);
    } else {
        char line[WEND_STATUS_MAX];
        int32_t last = page ? v->top + rows : 0;
        if (page && last > page->nlines)
            last = page->nlines;
        os64_snprintf(line, sizeof(line), " %d-%d/%d  %s  %s",
                      page && page->nlines ? v->top + 1 : 0, last,
                      page ? page->nlines : 0, v->note, v->url);
        status_paint(line);
    }
    cursor_to(s_rows, s_cols);
}

// ── Moving around ───────────────────────────────────────────────────────

static void scroll_clamp(view_t *v)
{
    int32_t rows = content_rows();
    int32_t lines = v->page ? v->page->nlines : 0;
    int32_t last = lines > rows ? lines - rows : 0;
    if (v->top > last)
        v->top = last;
    if (v->top < 0)
        v->top = 0;
}

static void scroll_by(view_t *v, int32_t rows)
{
    v->top += rows;
    scroll_clamp(v);
}

// Bring the selection onto the screen, moving as little as possible: a spot
// one row above the window should scroll one row, not jump.
static void scroll_to_spot(view_t *v)
{
    if (!v->page || v->sel < 0 || v->sel >= v->page->nspots)
        return;
    int32_t line = v->page->spots[v->sel].line;
    if (line < 0)
        return;
    int32_t rows = content_rows();
    if (line < v->top)
        v->top = line;
    else if (line >= v->top + rows)
        v->top = line - rows + 1;
    scroll_clamp(v);
}

// The first spot at or after a row, searching either way. A page's spots are
// numbered in document order, so they are in row order too — which is what
// makes this a walk and not a search.
static int32_t spot_near(const view_t *v, int32_t row, int32_t direction)
{
    int32_t found = -1;
    for (int32_t i = 0; i < v->page->nspots; i++) {
        int32_t line = v->page->spots[i].line;
        if (line < 0)
            continue;
        if (direction > 0 && line >= row)
            return i;
        if (direction < 0 && line < row)
            found = i;
    }
    return found;
}

// AFTER THE PAGE MOVES UNDER THE SELECTION, TAKE THE SELECTION WITH IT. A
// screenful of scrolling leaves the old choice somewhere nobody is looking,
// and the next arrow would jump back to it — so a selection that has gone
// off the window is replaced by the first spot on the new one, or by nothing
// when the new screen has none.
static void selection_reanchor(view_t *v)
{
    // The bound is checked here and not only where a selection is set: this
    // runs after every scroll, so it is the one place every stale selection
    // passes through.
    if (!v->page || v->sel < 0 || v->sel >= v->page->nspots) {
        if (v->page && v->sel >= v->page->nspots)
            v->sel = -1;
        return;
    }
    int32_t rows = content_rows();
    int32_t line = v->page->spots[v->sel].line;
    if (line >= v->top && line < v->top + rows)
        return;
    v->sel = spot_near(v, v->top, 1);
    if (v->sel >= 0 && v->page->spots[v->sel].line >= v->top + rows)
        v->sel = -1;
}

// THE ARROWS WALK THE SPOTS, which is Chris's gopher ruling applied to the
// web: a browser picks a link by pointing at it. Past the last spot in the
// direction you are going, the same key scrolls — which is what keeps the
// arrows useful on the prose below the last link, and on a page that has no
// links at all.
static void move_selection(view_t *v, int32_t step)
{
    if (!v->page || v->page->nspots == 0) {
        scroll_by(v, step);
        return;
    }
    int32_t next;
    if (v->sel < 0)
        next = spot_near(v, step > 0 ? v->top : v->top + content_rows(), step);
    else
        next = v->sel + step;
    if (next < 0 || next >= v->page->nspots) {
        scroll_by(v, step);
        selection_reanchor(v);
        return;
    }
    v->sel = next;
    scroll_to_spot(v);
}

static void history_push(const view_t *v)
{
    if (s_depth >= WEND_HISTORY_MAX) {
        // Drop the OLDEST crumb rather than refuse to go forward: somebody
        // sixty-four pages in wants the sixty-fifth more than the first.
        for (int32_t i = 1; i < WEND_HISTORY_MAX; i++)
            s_history[i - 1] = s_history[i];
        s_depth = WEND_HISTORY_MAX - 1;
    }
    os64_strcopy(s_history[s_depth].url, sizeof(s_history[s_depth].url), v->url);
    s_history[s_depth].top = v->top;
    s_history[s_depth].sel = v->sel;
    s_depth++;
}

// Go somewhere. The new page is built beside the old one and only replaces
// it once it exists, so a dead link costs a sentence and not the page you
// were reading.
static bool go(view_t *v, const char *url, bool remember)
{
    view_t next = { 0 };
    next.sel = -1;
    if (!load(url, &next, NULL))
        return false;
    if (remember)
        history_push(v);
    view_clear(v);
    *v = next;
    return true;
}

// libpage selects the semantic target; the renderer supplies geometry for
// that exact node. Missing geometry leaves the reader's position intact.
static void jump_to_node(view_t *v, const os64_html_node_t *node)
{
    int32_t line = node != NULL ? wend_node_line(v->page, node) : 0;
    if (line < 0) {
        status_set(" that target has no rendered row on this page");
        return;
    }
    v->top = line;
    scroll_clamp(v);
    selection_reanchor(v);
}

static void jump_to_fragment(view_t *v, const char *name)
{
    const os64_html_node_t *node = NULL;
    os64_page_reason_t reason = os64_page_resolve_fragment(v->model, name, &node);
    if (reason != OS64_PAGE_REASON_OK) {
        status_set(" %s", os64_page_reason_name(reason));
        return;
    }
    jump_to_node(v, node);
}

// ── Doing what a page asks ──────────────────────────────────────────────
//
// EVERY FETCH A PAGE ASKS FOR ENDS HERE: a link followed, a form sent, a
// refresh the document declares. (A move within the page is no fetch and
// never comes here.) libpage has already decided where each one goes and says what it knows about it; what is left is this browser's
// own list of what it can carry, and the person's decision where one is
// owed. One door, because the same rules written once per road are how a
// road comes to miss one.

// WHO IS ASKED BEFORE AN ENCRYPTED PAGE'S REQUEST GOES OUT IN THE CLEAR.
// Nobody for a link: a person pressed it, and where it goes is written on
// the page. A person for a form, because what goes is what they typed, and
// for a refresh, because the page chose the destination and nobody pressed
// anything. libfetch's downgrade callback cannot see either: it judges the
// redirects inside one fetch, and each of these starts a new fetch where the
// page said.
typedef enum {
    ASK_NEVER = 0,
    ASK_SEND,         // a form: "shall what you typed go in clear?"
    ASK_GO,           // a refresh: "shall I take you there?"
} ask_t;

// Perform a NAVIGATE. True when the view moved. The request's strings are
// its own, so they outlive the view `go` replaces.
static bool perform(view_t *v, const os64_page_request_t *request, bool remember, ask_t ask)
{
    // libfetch sends no request body, so a POST is refused BY NAME rather
    // than sent as something else — as a GET, it would put a password in an
    // address that servers and proxies write down.
    if (request->method == OS64_PAGE_METHOD_POST) {
        status_set(" this form posts, and wend sends only forms that ask by address");
        return false;
    }
    // WHICH SCHEMES THIS BROWSER FOLLOWS IS ITS OWN LIST, which is why
    // libpage states the scheme instead of keeping one.
    if (os64_streq(request->scheme, "gopher")) {
        status_set(" that is gopherspace - read it with:  gopher '%s'", request->url);
        return false;
    }
    if (!os64_streq(request->scheme, "http") && !os64_streq(request->scheme, "https")) {
        status_set(" that is a %s: address, which wend does not fetch", request->scheme);
        return false;
    }
    if (ask != ASK_NEVER && request->downgrade) {
        char question[WEND_STATUS_MAX];
        os64_url_t target;
        const char *host = os64_url_parse(request->url, &target) == OS64_URL_OK
                               ? target.host : "that address";
        if (ask == ASK_SEND)
            os64_snprintf(question, sizeof(question),
                          " %s would get this unencrypted - send it? (y/n) ", host);
        else
            os64_snprintf(question, sizeof(question),
                          " this encrypted page sends you to %s unencrypted - go? (y/n) ", host);
        if (!confirm(question, true, ask == ASK_SEND ? " not sent" : " stayed here"))
            return false;
    }
    if (!go(v, request->url, remember))
        return false;
    // The new page's own model finds the section: the `#name` was asked of
    // a document that has only now arrived.
    if (request->has_fragment)
        jump_to_fragment(v, request->fragment != NULL ? request->fragment : "");
    return true;
}

// ── A page that asks to send you somewhere ──────────────────────────────
//
// `<meta http-equiv="refresh">` is a navigation the DOCUMENT declares, and
// following it is what makes a search engine's result links work: the HTML
// endpoints wrap every link in a click logger that answers 200 with no
// redirect, a script for a browser that runs one, and this for a browser
// that does not. libpage says whether one is declared and where to; the
// three rules about whether to OBEY are here, because they are about a
// person reading rather than about a page.
// A chain of these can go on forever, so it is capped the way a chain of
// redirects is — and for the same reason, which is that nobody reading can
// tell the difference between a slow one and a stuck one.
#define WEND_REFRESH_MAX 5

// ONE HOP OF A CHAIN. True means the view moved and the page it moved to has
// not been looked at yet, so the caller asks again: a redirector that lands
// on another redirector must not need a keypress between them, which is
// what following only one per trip through the key loop would have meant.
static bool refresh_once(view_t *v, int32_t *chain)
{
    if (v->refresh_handled)
        return false;
    const os64_page_refresh_t *refresh = v->model ? os64_page_refresh(v->model) : NULL;
    v->refresh_handled = true;
    if (refresh == NULL)
        return false;                    // an ordinary page ends the chain
    if (refresh->url.refused != OS64_PAGE_REASON_OK) {
        status_set(" refresh: %s", os64_page_reason_name(refresh->url.refused));
        return false;
    }
    // A DELAY IS A PERSON'S PATIENCE TO SPEND. wend has no timer in its key
    // loop and a page that moves under a reader mid-sentence is hostile, so
    // a delayed refresh is reported and left for them to act on. The "you
    // will be redirected in five seconds" page always carries a link too.
    if (refresh->seconds != 0) {
        if (refresh->names_this_document)
            status_set(" this page asks to reload itself every %u seconds",
                       (unsigned)refresh->seconds);
        else
            status_set(" this page asks to send you to %s in %u seconds - press g to go",
                       refresh->url.url, (unsigned)refresh->seconds);
        return false;
    }
    // A same-document refresh without a fragment would reload in a loop.
    // A fragment instead moves within this document and is handled below.
    if (refresh->names_this_document && !refresh->url.has_fragment) {
        status_set(" this page asks to reload itself immediately, which wend does not do");
        return false;
    }
    if (++*chain > WEND_REFRESH_MAX) {
        status_set(" this page is the end of a chain of redirects too long to follow");
        return false;
    }
    os64_page_what_t what = { OS64_PAGE_ACTIVATE_REFRESH, 0, 0, 0 };
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(v->model, what, &request);
    bool moved = false;
    if (verdict == OS64_PAGE_FRAGMENT)
        jump_to_node(v, request.anchor);
    else if (verdict == OS64_PAGE_NAVIGATE)
        // Redirectors do not occupy a history entry.
        moved = perform(v, &request, false, ASK_GO);
    else
        status_set(" this page asks to send you somewhere it does not name properly");
    os64_page_request_free(&request);
    return moved;
}

static void refresh_if_declared(view_t *v)
{
    int32_t chain = 0;
    while (refresh_once(v, &chain))
        ;
}

static void follow_link(view_t *v, int32_t index)
{
    os64_page_what_t what = { OS64_PAGE_ACTIVATE_LINK, v->page->spots[index].link, 0, 0 };
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(v->model, what, &request);
    v->sel = index;
    if (verdict == OS64_PAGE_FRAGMENT)
        jump_to_node(v, request.anchor);   // a place in this page: a move, no fetch
    else if (verdict == OS64_PAGE_NAVIGATE)
        perform(v, &request, true, ASK_NEVER);
    else
        status_set(" %s", os64_page_reason_name(request.reason));
    os64_page_request_free(&request);
}

// ── Sending a form ──────────────────────────────────────────────────────

// Ask the model what pressing (or finishing) a control sends, and do it.
// Everything a submission decides — which button, its overrides, the entry
// list, the encoding — is libpage's; what is here is the sentence for each
// way it can say no, and putting the cursor on a field the form requires.
static void form_send(view_t *v, int32_t control, os64_page_activation_t how)
{
    os64_page_what_t what = { how, control, 0, 0 };
    os64_page_request_t request;
    os64_page_verdict_t verdict = os64_page_activate(v->model, what, &request);
    if (verdict == OS64_PAGE_NAVIGATE) {
        perform(v, &request, true, ASK_SEND);
    } else if (verdict == OS64_PAGE_FRAGMENT) {
        jump_to_node(v, request.anchor);
    } else {
        status_set(" %s", os64_page_reason_name(request.reason));
        int32_t spot = request.reason == OS64_PAGE_REASON_INVALID
                           ? wend_spot_for_control(v->page, request.control) : -1;
        if (spot >= 0) {
            v->sel = spot;
            scroll_to_spot(v);
        }
    }
    os64_page_request_free(&request);
}

// Whether a control is something a person still has to ANSWER: not a
// button (a button is how you say you are done), not a value the page keeps
// or took away, not one it put out of sight.
static bool control_answerable(const os64_page_control_t *c)
{
    return !c->disabled && !c->readonly && !c->hidden_subtree && !c->has_datalist_ancestor &&
           c->input != OS64_PAGE_INPUT_HIDDEN && c->input != OS64_PAGE_INPUT_BUTTON &&
           c->element != OS64_PAGE_EL_BUTTON && !c->submits && !c->resets;
}

// A FORM WITH ONE THING TO ANSWER has nowhere else for you to go, so
// finishing that one thing finishes the form. Anything else to fill in —
// another box, a tick, a list — and the value is kept while the form waits,
// because sending early would send the rest at their defaults and a person
// working down the page in order would never get to them.
//
// Finishing it is the standard's IMPLICIT SUBMISSION, and libpage decides
// what that sends: the form's default button, with its name and whatever it
// overrules, wherever on the page that button stands.
static void form_send_if_alone(view_t *v, int32_t control)
{
    const os64_page_control_t *c = os64_page_control(v->model, control);
    if (c == NULL || c->form < 0)
        return;
    int32_t answers = 0;
    for (int32_t i = 0; i < os64_page_ncontrols(v->model); i++) {
        const os64_page_control_t *other = os64_page_control(v->model, i);
        if (other->form == c->form && control_answerable(other))
            answers++;
    }
    if (answers == 1)
        form_send(v, control, OS64_PAGE_ACTIVATE_IMPLICIT);
}

// ── Filling a form in ───────────────────────────────────────────────────

// TWO ALPHABETS MEET IN A FORM FIELD. The model and the wire are UTF-8; the
// keyboard and the glass are Latin-1. So a value is STORED as UTF-8 — which
// is what the page put there and what the server is expecting back — and
// converted at each end: folded for the prompt to show, encoded again when
// what was typed is taken.
static void value_to_typed(const char *value, size_t len, char *out, size_t cap)
{
    size_t at = 0, n = 0;
    while (at < len) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(value + at, len - at, &cp);
        at += took ? took : 1;
        char folded[WEND_FOLD_MAX];
        size_t got = wend_fold(cp, folded);
        for (size_t i = 0; i < got && n + 1 < cap; i++)
            out[n++] = folded[i];
    }
    out[n] = '\0';
}

static size_t typed_to_value(const char *typed, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = typed; *p != '\0'; p++) {
        char utf8[4];
        size_t got = os64_utf8_encode((unsigned char)*p, utf8);
        if (n + got + 1 > cap)
            break;
        for (size_t i = 0; i < got; i++)
            out[n++] = utf8[i];
    }
    out[n] = '\0';
    return n;
}

// Lay the page out again so a control shows what it now holds, and put the
// reader back where they were. The walk is deterministic for one tree, so
// the new page numbers the same control with the same spot.
static void edit_commit(view_t *v)
{
    int32_t sel = v->sel, top = v->top;
    view_layout(v);
    v->sel = sel;
    v->top = top;
    scroll_clamp(v);
}

// The model said no to an edit: say why, in its words.
static bool edit_refused(int64_t rc)
{
    if (rc >= 0)
        return false;
    status_set(" %s", os64_page_reason_name((os64_page_reason_t)-rc));
    return true;
}

static void field_type(view_t *v, int32_t index)
{
    int32_t control = v->page->spots[index].control;
    const os64_page_control_t *c = os64_page_control(v->model, control);
    if (c == NULL)
        return;
    if (c->readonly) {
        status_set(" the page keeps that one as it is");
        return;
    }
    char shown[WEND_STATUS_MAX], before[WEND_STATUS_MAX];
    char stored[WEND_STATUS_MAX * 2];
    char label[64];
    const char *name = c->name ? c->name : "";
    value_to_typed(c->value, c->value_len, shown, sizeof(shown));
    // WHAT THE GLASS CANNOT SHOW, THE PROMPT CANNOT HAND BACK. A value is
    // edited as its Latin-1 shadow, so one holding a character this terminal
    // has no glyph for, a line break, or more than the prompt holds would
    // come back as something else: a curly quote straight, an em dash a
    // hyphen, a textarea one line, its tail gone. Such a value is offered
    // only to be REPLACED: the prompt starts empty and says so, and nothing
    // is sent that the person did not type.
    size_t round = typed_to_value(shown, stored, sizeof(stored));
    bool faithful = round == c->value_len && os64_memcmp(stored, c->value, round) == 0;
    if (!faithful)
        shown[0] = '\0';
    os64_strcopy(before, sizeof(before), shown);
    os64_snprintf(label, sizeof(label), faithful ? " %s: " : " replace %s: ",
                  name[0] ? name : "text");
    prompt_result_t got = prompt(label, shown, sizeof(shown),
                                 c->input == OS64_PAGE_INPUT_PASSWORD);
    // AN UNTOUCHED VALUE IS LEFT ALONE, not re-typed back over itself — and
    // a replacement nobody typed is no replacement.
    if (got == PROMPT_CANCELLED || os64_streq(shown, before)) {
        status_set(" left as it was");
        if (got != PROMPT_CANCELLED)
            form_send_if_alone(v, control);
        return;
    }
    size_t len = typed_to_value(shown, stored, sizeof(stored));
    if (edit_refused(os64_page_set_text(v->model, control, stored, len)))
        return;
    edit_commit(v);
    // ENTER IN A SEARCH BOX SENDS IT: a form whose only box you can type in
    // has nowhere else for you to go, and stopping there to hunt for a
    // button is the step nobody expects.
    form_send_if_alone(v, control);
}

static void check_toggle(view_t *v, int32_t index)
{
    int32_t control = v->page->spots[index].control;
    const os64_page_control_t *c = os64_page_control(v->model, control);
    if (c == NULL || edit_refused(os64_page_set_checked(v->model, control, !c->checked)))
        return;
    edit_commit(v);
}

// TICKING ONE OF A GROUP UNTICKS THE REST, which is what makes a radio a
// radio — and which radios are a group is the model's answer.
static void radio_pick(view_t *v, int32_t index)
{
    if (edit_refused(os64_page_set_checked(v->model, v->page->spots[index].control, true)))
        return;
    edit_commit(v);
}

// A list CYCLES rather than opening a menu of its own. A menu is a second
// kind of screen and this browser has one kind; stepping through the options
// in place shows each answer where the answer will be.
//
// WHAT A PERSON PICKS IS EXACTLY ONE THING, on a `multiple` list too: one
// key can say "this one" and has no way to say "these three", so stepping a
// multiple list leaves it holding the one it stepped to. Until it is touched
// the page's own several go out untouched, which is the half that matters
// for a page you are only reading. Picking SEVERAL is booked in BROWSER.md.
static void choice_cycle(view_t *v, int32_t index)
{
    int32_t control = v->page->spots[index].control;
    const os64_page_control_t *c = os64_page_control(v->model, control);
    if (c == NULL || c->noptions == 0)
        return;
    // Step from what the row shows, to the next option the page will take.
    // A disabled one is shown when it is what the list holds — the "choose
    // one" placeholder — and is never stepped ONTO.
    int32_t current = -1;
    for (int32_t i = 0; i < c->noptions && current < 0; i++)
        if (c->options[i].selected && !(c->multiple && c->options[i].disabled))
            current = i;
    int32_t next = -1;
    for (int32_t step = 1; step <= c->noptions && next < 0; step++) {
        int32_t try = ((current < 0 ? -1 : current) + step + c->noptions) % c->noptions;
        if (try != current && !c->options[try].disabled)
            next = try;
    }
    if (next < 0) {
        status_set(" that list offers nothing else");
        return;
    }
    if (edit_refused(os64_page_set_chosen(v->model, control, next, true)))
        return;
    if (c->multiple)
        for (int32_t i = 0; i < c->noptions; i++)
            if (i != next && c->options[i].selected &&
                edit_refused(os64_page_set_chosen(v->model, control, i, false)))
                break;
    edit_commit(v);
}

// ENTER, ON WHATEVER IS SELECTED. One key for every kind of spot, because to
// the person pressing it there is one question — "do the thing this is" —
// and the page decides what that is.
static void activate(view_t *v, int32_t index)
{
    if (index < 0) {
        status_set(" nothing is picked - the arrows walk the links and boxes");
        return;
    }
    if (!v->page || index >= v->page->nspots) {
        status_set(" this page has nothing numbered %d", index + 1);
        return;
    }
    v->sel = index;
    scroll_to_spot(v);
    const wend_spot_t *spot = &v->page->spots[index];
    switch (spot->kind) {
        case WEND_SPOT_LINK:   follow_link(v, index); break;
        case WEND_SPOT_TEXT:   field_type(v, index); break;
        case WEND_SPOT_CHECK:  check_toggle(v, index); break;
        case WEND_SPOT_RADIO:  radio_pick(v, index); break;
        case WEND_SPOT_CHOICE: choice_cycle(v, index); break;
        case WEND_SPOT_SUBMIT: form_send(v, spot->control, OS64_PAGE_ACTIVATE_CONTROL); break;
    }
}

static void back(view_t *v)
{
    if (s_depth == 0) {
        status_set(" this is where you came in");
        return;
    }
    crumb_t crumb = s_history[s_depth - 1];
    view_t next = { 0 };
    next.sel = -1;
    if (!load(crumb.url, &next, NULL))
        return;                          // the history is untouched; the row says why
    s_depth--;
    view_clear(v);
    *v = next;
    // Where you were on that page, as near as the page you get back allows.
    // There is no page cache (BROWSER.md books one), so going back is a
    // FETCH: the same address, and not necessarily the same page — a front
    // page loses a story, a search reruns. So the remembered selection is a
    // hint that has to be checked against what actually came back, not a
    // position that can be trusted into an array.
    v->top = crumb.top;
    v->sel = crumb.sel < v->page->nspots ? crumb.sel : -1;
    scroll_clamp(v);
}

// An address a person typed. A bare `host/path` means http, the way a bare
// host means gopher to the gopher client: guessing what a word means belongs
// to whoever is entitled to guess, and here that is the browser.
// AN ADDRESS THAT DOES NOT FIT IS REFUSED, NOT TRIMMED. A truncated URL is
// not a shorter way of saying the same thing — it is a different address,
// and one nobody typed.
static bool typed_address(const char *typed, char *out, size_t cap)
{
    for (const char *p = typed; *p != '\0' && *p != '/'; p++)
        if (p[0] == ':' && p[1] == '/' && p[2] == '/')
            return os64_strcopy(out, cap, typed) < cap;
    return (size_t)os64_snprintf(out, cap, "http://%s", typed) < cap;
}

// ── The keys, written down ──────────────────────────────────────────────

static const char WEND_KEYS[] =
    "Moving\n"
    "\n"
    "  Up, Down        the next link or box, and the one before. n and p do\n"
    "                  the same, and so do Tab and Shift+Tab. Past the last\n"
    "                  one, the page scrolls.\n"
    "  PgUp, PgDn      one screen, and so does the space bar\n"
    "  Home, End       the top, the bottom\n"
    "  Enter, Right    do the selected thing\n"
    "  a number, Enter the same, to the thing wearing that number\n"
    "  b, Backspace    back to the page before (Left too)\n"
    "\n"
    "Elsewhere\n"
    "\n"
    "  g               go to an address\n"
    "  r               fetch this page again\n"
    "  ?               this list\n"
    "  q               leave\n"
    "\n"
    "Ctrl+C stops a page that is still loading.\n"
    "\n"
    "Doing the selected thing\n"
    "\n"
    "A link is followed. A box is opened for typing, and Enter keeps what\n"
    "you typed. A tick box is ticked or cleared, and a list steps to its\n"
    "next option. A button sends its form.\n"
    "\n"
    "A form with one box to type in sends itself when you finish the box,\n"
    "because there is nowhere else in it to go. One with more waits for its\n"
    "button. A form that POSTS is refused by name: wend asks only by\n"
    "address, so nothing of yours is sent anywhere by halves.\n"
    "\n"
    "Links are cyan and boxes are green; both wear [n], and the selected\n"
    "one is highlighted. Text is folded onto this terminal's Latin-1, so a\n"
    "character it cannot draw is shown as ? rather than quietly dropped.\n";

// The keys, shown in the same scroller everything else uses. Its own little
// loop rather than a page in the history, because what you go back to from
// here is the page you were reading, and that page is still sitting in the
// session's frame waiting to be redrawn.
static void help_show(void)
{
    view_t help = { 0 };
    help.sel = -1;
    os64_strcopy(help.url, sizeof(help.url), "the keys");
    os64_strcopy(help.note, sizeof(help.note), "any other key returns");
    help.text = (char *)WEND_KEYS;     // borrowed; never freed with the view
    help.textlen = sizeof(WEND_KEYS) - 1;
    help.text_utf8 = true;
    view_layout(&help);

    for (;;) {
        if (s_want_size) {
            s_want_size = 0;
            geometry();
            view_layout(&help);
            scroll_clamp(&help);
        }
        draw(&help);
        char literal;
        key_t k = key_read(&literal);
        s_status[0] = '\0';
        if (k == KEY_UP)        scroll_by(&help, -1);
        else if (k == KEY_DOWN) scroll_by(&help, 1);
        else if (k == KEY_PGUP) scroll_by(&help, -content_rows());
        else if (k == KEY_PGDN) scroll_by(&help, content_rows());
        else if (k == KEY_HOME) { help.top = 0; }
        else if (k == KEY_END)  { help.top = help.page ? help.page->nlines : 0;
                                  scroll_clamp(&help); }
        else if (k == KEY_NONE) { if (s_want_quit) break; continue; }
        else if (k == KEY_OTHER && literal == ' ') scroll_by(&help, content_rows());
        else break;
        if (s_want_quit)
            break;                       // a hangup ends the session from in here too
    }
    wend_page_free(help.page);         // the text is static; only the lines are ours
}

// ── The session ─────────────────────────────────────────────────────────

static int32_t session(const char *start)
{
    view_t view = { 0 };
    view.sel = -1;
    os64_fetch_status_t why = OS64_FETCH_OK;
    if (!load(start, &view, &why)) {
        // THE FIRST PAGE IS THE COMMAND, and a command that failed owes an
        // exit code that says which thing to go and fix. An address nobody
        // could parse is the typist's to mend; everything else is the
        // network's, the server's or this machine's.
        screen_restore();
        os64_hprintf(OS64_STDERR, "wend:%s\n", s_status);
        return (why == OS64_FETCH_BAD_URL || why == OS64_FETCH_UNSUPPORTED_SCHEME)
               ? WEND_BAD_URL : WEND_FAILED;
    }

    screen_clear();
    char number[8];
    int32_t digits = 0;

    for (;;) {
        if (s_want_size) {
            s_want_size = 0;
            geometry();
            view_layout(&view);
            if (view.sel >= 0)
                scroll_to_spot(&view);
            scroll_clamp(&view);
        }
        if (s_want_quit)
            break;
        // After any navigation has settled and before anything is painted:
        // a redirector is a page nobody wants to see, and the reader's own
        // `#name` jump has already happened.
        refresh_if_declared(&view);
        if (s_want_quit)
            break;

        draw(&view);
        char literal;
        key_t k = key_read(&literal);
        if (k == KEY_NONE)
            continue;                    // a signal, or half an escape sequence
        s_status[0] = '\0';              // the last sentence was about the last state

        bool keeps_number = false;
        switch (k) {
            case KEY_UP:    move_selection(&view, -1); break;
            case KEY_DOWN:  move_selection(&view, 1); break;
            case KEY_PGUP:
                scroll_by(&view, -content_rows());
                selection_reanchor(&view);
                break;
            case KEY_PGDN:
                scroll_by(&view, content_rows());
                selection_reanchor(&view);
                break;
            case KEY_HOME:
                view.top = 0;
                selection_reanchor(&view);
                break;
            case KEY_END:
                view.top = view.page ? view.page->nlines : 0;
                scroll_clamp(&view);
                selection_reanchor(&view);
                break;
            case KEY_LEFT:  back(&view); break;
            case KEY_RIGHT: activate(&view, view.sel); break;
            case KEY_BACKTAB: move_selection(&view, -1); break;

            case KEY_ENTER:
                if (digits > 0) {
                    number[digits] = '\0';
                    activate(&view, (int32_t)os64_atou(number) - 1);
                } else {
                    activate(&view, view.sel);
                }
                break;

            case KEY_EOF:
                // Not a decision anybody made — the terminal went away.
                view_clear(&view);
                return WEND_OK;

            case KEY_OTHER:
                switch (literal) {
                    case ' ':
                        scroll_by(&view, content_rows());
                        selection_reanchor(&view);
                        break;
                    case 'b':
                    case '\b':
                    case 0x7F: back(&view); break;
                    case 'n':
                    case '\t': move_selection(&view, 1); break;
                    case 'p':  move_selection(&view, -1); break;
                    case 'r':
                        // Again, and not into the history: you have not gone
                        // anywhere.
                        go(&view, view.url, false);
                        break;
                    case 'g': {
                        char typed[OS64_FETCH_URL_MAX];
                        typed[0] = '\0';         // an address is typed fresh
                        if (prompt(" go to: ", typed, sizeof(typed), false) != PROMPT_TYPED) {
                            status_set(" stayed here");
                            break;
                        }
                        char whole[OS64_FETCH_URL_MAX];
                        if (!typed_address(typed, whole, sizeof(whole))) {
                            status_set(" that address is longer than one may be");
                            break;
                        }
                        go(&view, whole, true);
                        break;
                    }
                    case '?':  help_show(); break;
                    case 'q':
                        // LEAVING THROWS AWAY THE SESSION AND ITS HISTORY,
                        // which is why it is the move that gets asked about:
                        // `q` sits one key from the arrows.
                        if (!confirm(" leave wend? (y/n) ", false, NULL))
                            break;
                        view_clear(&view);
                        return WEND_OK;
                    case 0x03:
                        s_cancel = 0;    // nothing is loading; the key meant "never mind"
                        break;
                    default:
                        if (literal >= '0' && literal <= '9'
                            && digits < (int32_t)sizeof(number) - 1) {
                            number[digits++] = literal;
                            number[digits] = '\0';
                            status_set(" follow link %s, or Enter", number);
                            keeps_number = true;
                        }
                        break;
                }
                break;

            default:
                break;
        }
        if (!keeps_number)
            digits = 0;
    }

    view_clear(&view);
    return WEND_OK;
}

// ── Raw mode ────────────────────────────────────────────────────────────

// Ask for raw mode, and report a refusal once rather than at every
// keystroke. Refused is a working browser, not a broken one: Ctrl+C becomes
// SIGINT, the handler sets the same flag, and a page still stops. What is
// lost is Ctrl+C meaning anything when nothing is loading.
static void raw_acquire(void)
{
    if (os64_tty_set_raw(true) == 0) {
        s_raw = true;
        return;
    }
    status_set(" this terminal will not go raw - Ctrl+C interrupts instead");
}

static void raw_release(void)
{
    if (!s_raw)
        return;
    os64_tty_set_raw(false);
    s_raw = false;
}

int main(int argc, char **argv)
{
    os64_args_t args;
    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Arrows walk the links and boxes, Enter does the selected"
                 " thing, b goes back, ? lists the keys, q quits.";
    args.details = "An address is http://host/path or https://host/path;"
                   " a bare host/path is read as http.";

    const char *where = NULL;
    int32_t n = os64_args_parse(&args, "wend [address]", &where, 1);
    if (n < 0)
        return n == OS64_ARG_HELP ? WEND_OK : WEND_USAGE;
    if (n == 0)
        where = "example.com";           // the page that is always there

    char start[OS64_FETCH_URL_MAX];
    if (!typed_address(where, start, sizeof(start))) {
        os64_hprintf(OS64_STDERR, "wend: that address is longer than one may be\n");
        return WEND_BAD_URL;
    }

    // Keys come from the TERMINAL, not from handle 0, so `wend < file` and
    // a browser in a pipeline still read the person's arrows.
    s_keys = os64_tty_handle();
    if (s_keys < 0)
        s_keys = OS64_STDIN;
    keys_probe_patience();

    os64_signal_set_handler(OS64_SIGINT, on_interrupt);
    os64_signal_set_handler(OS64_SIGWINCH, on_resize);
    os64_signal_set_handler(OS64_SIGHUP, on_hangup);
    os64_signal_set_handler(OS64_SIGTERM, on_hangup);

    geometry();
    raw_acquire();
    int32_t status = session(start);
    raw_release();
    // The backstop, and the reason no `return` inside the session has to
    // remember: a program that painted a screen owes it back before the
    // shell draws on it.
    screen_restore();
    os64_close((int32_t)s_keys);
    if (s_want_quit)
        return OS64_EXIT_FOR_SIGNAL(s_quit_signal);
    return status;
}
