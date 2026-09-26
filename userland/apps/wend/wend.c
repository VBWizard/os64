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
// it: the fetch, the parse, the model, and the session — the history, the
// loading, the judgements about what a page asks for — which is libway's,
// so this file is the terminal half of wend and nothing more.
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
#include "way/way.h"
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

#define WEND_STATUS_MAX  WAY_SENTENCE_MAX

// Exit codes in os64get's and the gopher client's shape: a number a script
// can act on, each naming a different thing to go and fix. They apply to the
// address a person TYPED — inside a session a failure is a sentence on the
// status row and the session goes on.
#define WEND_OK        0
#define WEND_USAGE     2
#define WEND_BAD_URL   13
#define WEND_FAILED    5

// ── Where we are ────────────────────────────────────────────────────────

// A page and where the reader is on it. The page itself — the tree or the
// text, what it means, the reader's `details` — is libway's, kept whole so a
// resize re-wraps from the parse instead of asking the network for a page we
// already have. The lines are this face's, drawn from it at the current
// width.
typedef struct {
    way_page_t way;
    wend_page_t *page;                  // the lines, at the current width
    int32_t  top;                       // first visible row
    int32_t  sel;                       // the selected spot, -1 for none
} view_t;

// The session: the history, and the transient sentence on the status row,
// which libway writes as well as this file.
static way_session_t s_way;

static int32_t s_rows = 25;
static int32_t s_cols = 80;
static int64_t s_keys = OS64_STDIN;
static bool    s_raw;
static bool    s_raw_refused;         // raw mode was refused; not yet said

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
    os64_vsnprintf(s_way.status, sizeof(s_way.status), fmt, args);
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

// ── What the library asks us ────────────────────────────────────────────

// Whether to stop. Asked (through libway) by libfetch before every wait, which is also the
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

// How libway reaches the person: the same question row, the same fetch
// supervision, the status row painted while a page loads.
static bool face_confirm(void *ctx, const char *question, bool security, const char *refused)
{
    (void)ctx;
    // libway's questions say nothing about how to answer; on this row it
    // is a key.
    char asked[WEND_STATUS_MAX];
    os64_snprintf(asked, sizeof(asked), "%s (y/n) ", question);
    return confirm(asked, security, refused);
}

static void face_progress(void *ctx, const char *sentence)
{
    (void)ctx;
    status_paint(sentence);
}

// ── A page ──────────────────────────────────────────────────────────────

static void view_clear(view_t *v)
{
    way_page_clear(&v->way);
    wend_page_free(v->page);
    v->page = NULL;
}

// Wrap the page this view is holding to the current width. The tree (or the
// text) is what was kept, so a resize costs a re-wrap and not a fetch — and
// the model holds the edits, so the box still holds what was typed into it.
static void view_layout(view_t *v)
{
    wend_page_free(v->page);
    v->page = v->way.doc ? wend_render_html(v->way.doc, v->way.model, s_cols,
                                        (const os64_html_node_t *const *)v->way.flipped,
                                        v->way.nflipped)
                     : wend_render_text(v->way.text, v->way.textlen, v->way.text_utf8, s_cols);
    if (!v->page)
        status_set(" out of memory laying the page out at %d columns", (int)s_cols);
}

// Fetch, parse and lay out one address. The view is built beside the one on
// screen and only replaces it when there is something to show — a page that
// will not load leaves the previous page where it is, with the reason under
// it (BROWSER.md § The face).
// `why` is the fetch's own verdict when there was one, for the caller that
// has to turn it into an exit code. NULL when nobody is asking. `request`
// is the form being sent, NULL for a GET. `referrer` is the page a link,
// form or refresh was on, which the Referer names; NULL for none.
static bool load(const char *url, view_t *out, os64_fetch_status_t *why,
                 const os64_page_request_t *request, const char *referrer)
{
    s_cancel = 0;
    // One leg per load, on this thread: wend waits for its pages, so the
    // leg's sentence simply becomes the status row's.
    way_leg_t leg = way_leg(&s_way);
    if (referrer != NULL)
        os64_strcopy(leg.referrer, sizeof(leg.referrer), referrer);
    bool loaded = way_load(&leg, url, request, &out->way, why);
    os64_strcopy(s_way.status, sizeof(s_way.status), leg.status);
    if (!loaded)
        return false;
    out->sel = -1;
    out->top = 0;
    view_layout(out);
    if (!out->page) {
        view_clear(out);                 // view_layout said why
        return false;
    }
    if (out->page->incomplete)
        status_set(" out of memory partway through the page - what is here is real");
    else if (out->way.doc && (!out->way.model || os64_page_incomplete(out->way.model)))
        status_set(" out of memory understanding the page - its links and boxes"
                   " may not work");
    else if (out->page->nlines == 0)
        // A BLANK SCREEN IS AMBIGUOUS AND THIS IS NOT. A reply that parsed
        // to nothing — a page that is all script, a body of zero bytes — is
        // a real answer, and saying so is the difference between "the server
        // sent nothing" and "this browser is broken".
        status_set(" that answer has nothing in it this browser can show");
    else
        s_way.status[0] = '\0';
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
    bar_paint(1, page && page->title[0] ? page->title : v->way.url);

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
    if (s_way.status[0] != '\0') {
        status_paint(s_way.status);
    } else {
        char line[WEND_STATUS_MAX];
        int32_t last = page ? v->top + rows : 0;
        if (page && last > page->nlines)
            last = page->nlines;
        os64_snprintf(line, sizeof(line), " %d-%d/%d  %s  %s",
                      page && page->nlines ? v->top + 1 : 0, last,
                      page ? page->nlines : 0, v->way.note, v->way.url);
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

// Where the reader is, in the form libway keeps for the history: the row at
// the top and the selected spot.
static way_position_t position_of(const view_t *v)
{
    way_position_t p = {{0}};
    os64_memcpy(p.bytes, &v->top, sizeof(v->top));
    os64_memcpy(p.bytes + sizeof(v->top), &v->sel, sizeof(v->sel));
    return p;
}

static void history_push(const view_t *v)
{
    way_position_t where = position_of(v);
    way_remember(&s_way, v->way.url, &where);
}

// Go somewhere. The new page is built beside the old one and only replaces
// it once it exists, so a dead link costs a sentence and not the page you
// were reading.
static bool go(view_t *v, const char *url, bool remember, const os64_page_request_t *request)
{
    view_t next = { 0 };
    next.sel = -1;
    // A request is something the page asked for, so it names the page.
    if (!load(url, &next, NULL, request, request != NULL ? v->way.url : NULL))
        return false;
    if (remember)
        history_push(v);
    view_clear(v);
    *v = next;
    return true;
}

static void edit_commit(view_t *v);

// A TARGET FOLDED INSIDE A CLOSED `details` IS OPENED TO, the way a browser
// reveals it: every closed `details` between it and the root opens, and the
// page is drawn again so the target has a row.
static bool details_reveal(view_t *v, const os64_html_node_t *node)
{
    bool opened = false;
    for (const os64_html_node_t *up = node->parent; up != NULL; up = up->parent)
        if (up->kind == OS64_HTML_ELEMENT && up->ns == OS64_HTML_NS_HTML &&
            up->tag == OS64_HTML_TAG_DETAILS &&
            !wend_details_open(up, (const os64_html_node_t *const *)v->way.flipped, v->way.nflipped) &&
            way_details_flip(&v->way, up))
            opened = true;
    if (!opened)
        return false;
    // Opening inserts spots, so a selection BELOW the opened part would keep
    // its number and lose its meaning. It is found again by what it IS.
    const os64_html_node_t *selected =
        v->page && v->sel >= 0 && v->sel < v->page->nspots ? v->page->spots[v->sel].node : NULL;
    edit_commit(v);
    v->sel = -1;
    for (int32_t i = 0; selected != NULL && v->page != NULL && i < v->page->nspots; i++)
        if (v->page->spots[i].node == selected) {
            v->sel = i;
            break;
        }
    return true;
}

// libpage selects the semantic target; the renderer supplies geometry for
// that exact node. Missing geometry leaves the reader's position intact.
static void jump_to_node(view_t *v, const os64_html_node_t *node)
{
    int32_t line = node != NULL ? wend_node_line(v->page, node) : 0;
    if (line < 0 && node != NULL && details_reveal(v, node))
        line = wend_node_line(v->page, node);
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
    os64_page_reason_t reason = os64_page_resolve_fragment(v->way.model, name, &node);
    if (reason != OS64_PAGE_REASON_OK) {
        status_set(" %s", os64_page_reason_name(reason));
        return;
    }
    jump_to_node(v, node);
}

// ── Doing what a page asks ──────────────────────────────────────────────
//
// Every fetch a page asks for is judged by libway (way_judge): this
// browser's list of what it carries, and the question owed before anything
// goes out in clear. What is left here is going there, and finding the
// `#name` on the page that arrives.

// Perform a NAVIGATE. True when the view moved. The request's strings are
// its own, so they outlive the view `go` replaces.
static bool perform(view_t *v, const os64_page_request_t *request, bool remember, way_ask_t ask)
{
    if (!way_may_go(&s_way, request, ask))
        return false;
    if (!go(v, request->url, remember, request))
        return false;
    // The new page's own model finds the section: the `#name` was asked of
    // a document that has only now arrived.
    if (request->has_fragment)
        jump_to_fragment(v, request->fragment != NULL ? request->fragment : "");
    return true;
}

// ONE HOP OF A DECLARED REFRESH'S CHAIN, judged by libway. True means the
// view moved and the page it moved to has not been looked at yet, so the
// caller asks again: a redirector that lands on another redirector must not
// need a keypress between them, which is what following only one per trip
// through the key loop would have meant.
static bool refresh_once(view_t *v, int32_t *chain)
{
    os64_page_request_t request;
    bool moved = false;
    switch (way_refresh_step(&s_way, &v->way, chain, &request)) {
        case WAY_REFRESH_JUMP:
            jump_to_node(v, request.anchor);
            break;
        case WAY_REFRESH_GO:
            // Redirectors do not occupy a history entry.
            moved = perform(v, &request, false, WAY_ASK_GO);
            break;
        case WAY_REFRESH_NONE:
            return false;
    }
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
    os64_page_verdict_t verdict = os64_page_activate(v->way.model, what, &request);
    v->sel = index;
    if (verdict == OS64_PAGE_FRAGMENT)
        jump_to_node(v, request.anchor);   // a place in this page: a move, no fetch
    else if (verdict == OS64_PAGE_NAVIGATE)
        perform(v, &request, true, WAY_ASK_NEVER);
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
    os64_page_verdict_t verdict = os64_page_activate(v->way.model, what, &request);
    if (verdict == OS64_PAGE_NAVIGATE) {
        perform(v, &request, true, WAY_ASK_SEND);
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

// A FORM WITH ONE THING TO ANSWER has nowhere else for you to go, so
// finishing that one thing finishes the form. Anything else to fill in —
// another box, a tick, a list — and the value is kept while the form waits,
// because sending early would send the rest at their defaults and a person
// working down the page in order would never get to them.
//
// WHAT COUNTS IS WHAT THIS FACE LETS A PERSON OPERATE: the form's spots,
// less its buttons (a button is how you say you are done) and a box the
// page keeps fixed. A control drawn dead — a file input, one the page
// disabled or put out of sight — is not something anyone here can answer,
// and counting it would leave a form with no button unsendable.
//
// Finishing it is the standard's IMPLICIT SUBMISSION, and libpage decides
// what that sends: the form's default button, with its name and whatever it
// overrules, wherever on the page that button stands.
static void form_send_if_alone(view_t *v, int32_t control)
{
    const os64_page_control_t *c = os64_page_control(v->way.model, control);
    if (c == NULL || c->form < 0 || v->page == NULL)
        return;
    int32_t answers = 0;
    for (int32_t i = 0; i < v->page->nspots; i++) {
        const wend_spot_t *spot = &v->page->spots[i];
        if (spot->kind == WEND_SPOT_LINK || spot->kind == WEND_SPOT_SUBMIT)
            continue;
        const os64_page_control_t *other = os64_page_control(v->way.model, spot->control);
        if (other != NULL && other->form == c->form && !other->readonly)
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
    const os64_page_control_t *c = os64_page_control(v->way.model, control);
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
    if (edit_refused(os64_page_set_text(v->way.model, control, stored, len)))
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
    const os64_page_control_t *c = os64_page_control(v->way.model, control);
    if (c == NULL || edit_refused(os64_page_set_checked(v->way.model, control, !c->checked)))
        return;
    edit_commit(v);
}

// TICKING ONE OF A GROUP UNTICKS THE REST, which is what makes a radio a
// radio — and which radios are a group is the model's answer.
static void radio_pick(view_t *v, int32_t index)
{
    if (edit_refused(os64_page_set_checked(v->way.model, v->page->spots[index].control, true)))
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
    const os64_page_control_t *c = os64_page_control(v->way.model, control);
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
    if (edit_refused(os64_page_set_chosen(v->way.model, control, next, true)))
        return;
    if (c->multiple)
        for (int32_t i = 0; i < c->noptions; i++)
            if (i != next && c->options[i].selected &&
                edit_refused(os64_page_set_chosen(v->way.model, control, i, false)))
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
        case WEND_SPOT_TOGGLE:
            if (!way_details_flip(&v->way, spot->node))
                status_set(" out of memory opening that");
            else
                edit_commit(v);
            break;
    }
}

static void back(view_t *v)
{
    way_crumb_t crumb;
    if (!way_last(&s_way, &crumb))
        return;
    view_t next = { 0 };
    next.sel = -1;
    if (!load(crumb.url, &next, NULL, NULL, NULL))
        return;                          // the history is untouched; the row says why
    way_forget_last(&s_way);
    view_clear(v);
    *v = next;
    // Where you were on that page, as near as the page you get back allows.
    // There is no page cache (BROWSER.md books one), so going back is a
    // FETCH: the same address, and not necessarily the same page — a front
    // page loses a story, a search reruns. So the remembered selection is a
    // hint that has to be checked against what actually came back, not a
    // position that can be trusted into an array.
    int32_t top, sel;
    os64_memcpy(&top, crumb.position.bytes, sizeof(top));
    os64_memcpy(&sel, crumb.position.bytes + sizeof(top), sizeof(sel));
    v->top = top;
    v->sel = sel < v->page->nspots ? sel : -1;
    scroll_clamp(v);
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
    "button. GET forms send their values in the address; POST forms send\n"
    "them in the request body. Reload and Back fetch the address by GET.\n"
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
    os64_strcopy(help.way.url, sizeof(help.way.url), "the keys");
    os64_strcopy(help.way.note, sizeof(help.way.note), "any other key returns");
    help.way.text = (char *)WEND_KEYS;     // borrowed; never freed with the view
    help.way.textlen = sizeof(WEND_KEYS) - 1;
    help.way.text_utf8 = true;
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
        s_way.status[0] = '\0';
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
    if (!load(start, &view, &why, NULL, NULL)) {
        // THE FIRST PAGE IS THE COMMAND, and a command that failed owes an
        // exit code that says which thing to go and fix. An address nobody
        // could parse is the typist's to mend; everything else is the
        // network's, the server's or this machine's.
        screen_restore();
        os64_hprintf(OS64_STDERR, "wend:%s\n", s_way.status);
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
        // A refused raw mode is said on the first page PAINTED — not the
        // first one loaded, which a redirector may already have replaced —
        // and beside whatever that page said.
        if (s_raw_refused) {
            s_raw_refused = false;
            char page_said[WEND_STATUS_MAX];
            os64_strcopy(page_said, sizeof(page_said), s_way.status);
            status_set("%s%s this terminal will not go raw - Ctrl+C interrupts instead",
                       page_said, page_said[0] ? " -" : "");
        }

        draw(&view);
        char literal;
        key_t k = key_read(&literal);
        if (k == KEY_NONE)
            continue;                    // a signal, or half an escape sequence
        s_way.status[0] = '\0';              // the last sentence was about the last state

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
                        go(&view, view.way.url, false, NULL);
                        break;
                    case 'g': {
                        // An address is typed fresh — unless the page asked
                        // to send the reader somewhere after a delay, which
                        // the status row offered as "press g": then the
                        // prompt holds that address, and Enter is the
                        // reader's own decision to go.
                        char typed[OS64_FETCH_URL_MAX];
                        typed[0] = '\0';
                        const char *later = way_delayed_refresh(&view.way);
                        if (later != NULL && os64_strlen(later) < sizeof(typed))
                            os64_strcopy(typed, sizeof(typed), later);
                        if (prompt(" go to: ", typed, sizeof(typed), false) != PROMPT_TYPED) {
                            status_set(" stayed here");
                            break;
                        }
                        char whole[OS64_FETCH_URL_MAX];
                        if (!way_typed_address(typed, whole, sizeof(whole))) {
                            status_set(" that address is longer than one may be");
                            break;
                        }
                        go(&view, whole, true, NULL);
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
// lost is Ctrl+C meaning anything when nothing is loading. The report waits
// for the first page painted (session), because the fetches before it own
// the status row until then.
static void raw_acquire(void)
{
    if (os64_tty_set_raw(true) == 0) {
        s_raw = true;
        return;
    }
    s_raw_refused = true;
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
    if (!way_typed_address(where, start, sizeof(start))) {
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

    s_way.name = "wend";
    s_way.jar = way_jar_new();          // NULL keeps no cookies: the pages still load
    s_way.agent = WEND_AGENT;
    s_way.accept = WEND_ACCEPT;
    s_way.delayed_hint = " - press g to go";
    s_way.face = (way_face_t){NULL, face_confirm, fetch_cancelled, face_progress};

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
