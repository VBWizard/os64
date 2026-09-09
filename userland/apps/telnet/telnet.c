// telnet.c — /bin/telnet: a session, a command prompt, and ONE loop.
//
// The protocol is next door in telnet_protocol.c and touches no syscall; this
// file is the half that does. It reads the terminal, reads the connection,
// hands each what the other said, and paints what comes back.
//
// ── The shape, and why it is 4.2BSD's and not a thread's ────────────────
//
// One loop. Each pass takes a short timed read of the terminal and a short
// timed read of the connection, and does a bounded amount of work with
// whatever each returned; neither can starve the other because neither blocks
// longer than its deadline. That is only correct because a write to a TCP
// connection QUEUES into the send ring and returns — a keystroke no longer
// costs a round trip during which the client is deaf, so the writer thread
// this client was once designed around has nothing left to do.
//
// The engine holds every byte of protocol state, so the loop's whole job is
// to move bytes: what the peer said goes into telnet_receive, what the person
// typed goes into telnet_send_text, and telnet_pending is drained to the
// wire. A short write is ordinary and telnet_sent says so.
//
// BACK-PRESSURE IS THE ENGINE'S BOUNDED QUEUE, and this file's two holding
// buffers are what let it be. When the queue will not take more, the reader
// stops consuming instead of growing: the undrained tail of the terminal read
// stays in s_kb and the undrained tail of the connection read stays in s_net,
// and the next pass tries again after a flush. Nothing here allocates from a
// length the peer supplied, and nothing grows without a bound.
//
// ── The two typing modes, and the one rule that picks them ──────────────
//
// SUPPRESS-GO-AHEAD is the switch. A peer that has taken SGA is not waiting
// for a turn, so every keystroke can leave the moment it is typed —
// character-at-a-time, which is what a password prompt, a full-screen editor
// and a board's menu all need. A peer that has not is the half-duplex printer
// RFC 854 describes, and the client holds a line, lets you edit it, and sends
// it when you press Enter. ECHO is a separate question with a separate
// answer: whoever holds it does the echoing, which is what makes a password
// dark (telnet_local_echo).
//
// ── The prompt ──────────────────────────────────────────────────────────
//
// Ctrl+] opens `telnet>` — 4.2BSD's escape character and its prompt, kept
// because it is the one thing every person who has ever used telnet already
// knows. Remote output keeps arriving while the prompt is up, so the prompt
// is erased before that output is painted and redrawn after it, with whatever
// was half-typed still on it. `close` and a peer that hangs up both come back
// HERE rather than ending the program: the last screen stays readable and you
// can reconnect.
//
// ── What the terminal is told, and what it is owed back ─────────────────
//
// Two asks, and the client owes an answer to only one of them. RAW MODE is
// asked for at connect and released at close, so that Ctrl+C and Ctrl+D reach
// THIS PROGRAM as bytes instead of being spent by the console — Ctrl+D then
// travels to the peer as the data byte a Unix login logs out on, and Ctrl+C
// becomes IAC IP. The kernel restores cooked when the asking task exits, so a
// crash cannot leave a terminal deaf and this client owes it nothing. THE
// CHARACTER SET is the opposite: nothing restores it, so a client that
// selected CP437 for a board's art hands Latin-1 back on the way out, or
// every command typed on that terminal afterwards is drawn in a set nobody
// chose.

#include "telnet_protocol.h"

#include "os64/args.h"
#include "os64/dial.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/procfs.h"
#include "os64/resolve.h"
#include "os64/signal.h"
#include "os64/str.h"

// ── Sizes and constants ─────────────────────────────────────────────────

#define TELNET_ESCAPE      0x1D    // Ctrl+] — 4.2BSD's, and everyone's since
#define TELNET_PORT        23

// One scheduler tick each. A pass that found nothing parks for two ticks
// split between the two questions, which is 20ms of latency in each
// direction and no spinning; a pass that found something asks with no
// patience at all, so a board painting a screen streams at the wire's pace.
#define TELNET_POLL_MS     10

#define TELNET_CMD_MAX     256     // a command line, and the session's line
#define TELNET_KB_MAX      512     // one terminal read, paste included
#define TELNET_NET_MAX     4096
#define TELNET_HOST_MAX    (OS64_RESOLVE_NAME_MAX + 1)

// The character-set selections, in the Linux console's spelling — which is
// what os64's terminal reads (ansi.c) and what `ansiprobe cp437` paints.
#define SELECT_CP437       "\x1b(U"
#define SELECT_LATIN1      "\x1b(B"

// Exit codes. 0 is a session that ended the way you asked.
#define TELNET_OK          0
#define TELNET_USAGE       2

// ── State ───────────────────────────────────────────────────────────────
//
// One session at a time, so this is file scope rather than a struct passed
// through every function: the signal handlers have to reach it, and a handler
// takes only a signal number.

static telnet_t  s_engine;
static int32_t   s_conn = -1;              // the dialed connection, -1 = none
static int32_t   s_keys = OS64_STDIN;      // the terminal, whatever handle 0 became

static char      s_host[TELNET_HOST_MAX];  // what we dialed, for status and messages
static uint16_t  s_port;

// Raised by a handler and read by the loop. A handler does nothing else: the
// work each of these implies — a command on the wire, a window size, a tidy
// exit — belongs in ordinary code where it can fail and say so.
static volatile int s_want_interrupt;
static volatile int s_want_size;
static volatile int s_want_quit;
static volatile int s_quit_signal;

static bool      s_raw;          // the terminal is raw at our asking
static bool      s_cp437;        // we selected CP437 and owe Latin-1 back
static bool      s_prompting;    // the telnet> prompt is on the glass
static bool      s_running = true;
static bool      s_char_mode;    // the peer has SGA: keystrokes leave at once

// The command line being typed at the prompt, and the session line being
// typed at a peer that has not taken SGA. Separate because the prompt can be
// opened in the middle of composing a line and must give it back untouched.
static char      s_cmd[TELNET_CMD_MAX];
static size_t    s_cmd_len;
static char      s_line[TELNET_CMD_MAX];
static size_t    s_line_len;

// The holding buffers: what a read handed us and the engine has not taken.
static uint8_t   s_kb[TELNET_KB_MAX];
static size_t    s_kb_len, s_kb_pos;
static uint8_t   s_net[TELNET_NET_MAX];
static size_t    s_net_len, s_net_pos;

// The terminal's end of input, seen twice with neither a byte nor a wait
// between. A cooked console spells Ctrl+D as one zero-length read and then
// reads normally again, so a single EOF is a KEYPRESS; a stream that has
// genuinely finished answers zero instantly however often it is asked, and
// polling that forever would be a spin.
static int       s_keys_eof;
static bool      s_keys_gone;

static bool      s_at_line_start = true;

// ── The glass ───────────────────────────────────────────────────────────

// Everything this program shows goes through here, because the cursor's
// column is a fact two other things need: a notice starts on a fresh line,
// and the prompt is redrawn where the last byte left off.
static void screen(const void *bytes, size_t len)
{
    const uint8_t *p = (const uint8_t *)bytes;
    if (len == 0)
        return;
    os64_write(OS64_STDOUT, p, len);
    s_at_line_start = (p[len - 1] == '\n' || p[len - 1] == '\r');
}

static void screen_str(const char *s)
{
    screen(s, os64_strlen(s));
}

// THE PROMPT OWNS ITS LINE, which is what makes erasing one safe. `\r ESC[K`
// clears from column zero, so anything else already on that row goes with it —
// and the peer's bytes end wherever the peer stopped, very often mid-line. So
// the erase records that it left the cursor at column zero, and the draw puts
// itself on a fresh row whenever it is not there, and between them the prompt
// can never come down on top of somebody else's text.
//
// A command line longer than the glass is wide wraps, and the erase then only
// reaches the last row. That is the cost of not tracking the terminal's width
// for the sake of an erase; a command line that long is a typo either way.
static void prompt_erase(void)
{
    screen_str("\r\x1b[K");
    s_at_line_start = true;
}

static void prompt_draw(void)
{
    if (!s_at_line_start)
        screen_str("\r\n");
    screen_str("telnet> ");
    screen(s_cmd, s_cmd_len);
}

// A message from the client itself, as opposed to a byte from the peer. It
// always starts on a fresh line, and it steps around the prompt when the
// prompt is up.
static OS64_PRINTF(1, 2) void notice(const char *fmt, ...)
{
    char text[512];
    va_list ap;

    va_start(ap, fmt);
    os64_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    if (s_prompting)
        prompt_erase();
    else if (!s_at_line_start)
        screen_str("\r\n");

    screen_str(text);
    screen_str("\r\n");

    if (s_prompting)
        prompt_draw();
}

// ── Signals ─────────────────────────────────────────────────────────────
//
// A flag and nothing else. SIGINT is the interrupt character on a terminal
// this client could not put into raw mode — where it could, Ctrl+C arrives as
// the byte 0x03 and never becomes a signal, and this handler is left
// installed only because `kill -2` is still a way to ask.

static void on_interrupt(int signo)
{
    (void)signo;
    s_want_interrupt = 1;
}

static void on_resize(int signo)
{
    (void)signo;
    s_want_size = 1;
}

// SIGHUP and SIGTERM ask the session to END, not to die where it stands: the
// terminal is owed a character set back, and a program that took a terminal
// raw owes the far end nothing but owes the person a tidy screen. The exit
// code is the one the kernel's own default would have written, so a script
// cannot tell a handled death from an unhandled one.
static void on_hangup(int signo)
{
    s_want_quit = 1;
    s_quit_signal = signo;
}

// ── The character set ───────────────────────────────────────────────────
//
// WHO SAYS CP437? Nobody on the wire does: a board sends the art and never
// announces the set, and `ESC ( U` is this terminal's own spelling rather
// than something a 1992 BBS has heard of. So the person says, either with -8
// before the session or with `charset` during it — explicit, changeable when
// a peer turns out to be UTF-8 after all, and never a guess made from a port
// number that a Unix login on 23 would have been wrong about.

static void charset_select(bool cp437)
{
    if (cp437 == s_cp437)
        return;
    screen_str(cp437 ? SELECT_CP437 : SELECT_LATIN1);
    s_cp437 = cp437;
}

// ── The connection ──────────────────────────────────────────────────────

// The window size, sent when the peer has agreed to take it. Asked of the
// terminal every time rather than remembered, because the answer is what
// changed — a SIGWINCH carries no payload and procfs renders /proc/self/tty
// at open, so a size read once is the size forever.
static void send_window_size(void)
{
    os64_tty_info_t tty;

    if (os64_tty_read(&tty) != 0 || tty.cols == 0 || tty.rows == 0)
        return;
    telnet_send_size(&s_engine, (uint16_t)tty.cols, (uint16_t)tty.rows);
}

static void session_reset(void)
{
    telnet_init(&s_engine);
    s_kb_len = s_kb_pos = 0;
    s_net_len = s_net_pos = 0;
    s_line_len = 0;
    s_char_mode = false;
    s_keys_eof = 0;
}

// Ask for raw mode, and report the refusal ONCE rather than at every
// keystroke. Refused is a working client, not a broken one: Ctrl+C still
// becomes SIGINT and this program still turns that into IAC IP, and Ctrl+D
// still has `send eof` at the prompt. What is lost is a remote program that
// wanted those two bytes for itself.
static void raw_acquire(void)
{
    if (s_raw)
        return;
    if (os64_tty_set_raw(true) == 0) {
        s_raw = true;
        return;
    }
    // ASCII ONLY IN ANYTHING PRINTED. The terminal draws a byte over 0x7F as
    // one glyph in the set the terminal is in, so the three bytes of a UTF-8
    // dash arrive as three glyphs. Comments may say what they like.
    notice("telnet: this terminal will not go raw - Ctrl+C interrupts the"
           " remote, and Ctrl+D is `send eof` at the prompt");
}

static void raw_release(void)
{
    if (!s_raw)
        return;
    os64_tty_set_raw(false);
    s_raw = false;
}

static void disconnect(const char *why)
{
    if (s_conn < 0)
        return;

    // Whatever the engine still owes the peer goes out before the close, so a
    // final IAC IP or a WONT is on the wire rather than in a freed buffer.
    // Best effort by design: the reason we are closing may be that the peer
    // stopped listening.
    size_t len = 0;
    const uint8_t *out = telnet_pending(&s_engine, &len);
    if (len > 0)
        os64_write(s_conn, out, len);

    os64_close(s_conn);
    s_conn = -1;

    // Raw goes back with the session, because the prompt wants the cooked
    // meanings of Ctrl+C and Ctrl+D. The CHARACTER SET does not: a board
    // drops you often, and having to say `charset cp437` again before every
    // reconnect would be its own small misery. It costs nothing to leave —
    // the prompt is ASCII, which both sets agree about — and the way out of
    // the program hands it back.
    raw_release();

    if (why != NULL)
        notice("Connection closed: %s.", why);
}

// True when a session is live BECAUSE OF THIS CALL, which is not the same
// question as "is there a session" and the difference is what decides where
// the person is left standing. An `open` typed at an already-connected client
// is refused and must leave them at the prompt — reading "connected" as
// success dropped them back into a session they had not asked to return to,
// and the next command they typed went down the wire as text.
static bool connect_to(const char *host, uint16_t port)
{
    char dialstring[TELNET_HOST_MAX + 32];

    if (s_conn >= 0) {
        notice("telnet: already connected to %s - `close` first.", s_host);
        return false;
    }
    if (os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%u",
                      host, (unsigned)port) >= (int32_t)sizeof(dialstring)) {
        notice("telnet: %s: name too long to dial.", host);
        return false;
    }

    notice("Trying %s...", host);

    int64_t conn = os64_dial(dialstring);
    if (conn < 0) {
        // The dial's own vocabulary, so a refusal and a timeout read the same
        // here as they do from every other program that dials. No full stop
        // of ours: those reasons are whole sentences and some of them are
        // questions.
        notice("telnet: %s: %s", host, os64_dial_reason(conn));
        return false;
    }

    s_conn = (int32_t)conn;
    os64_strcopy(s_host, sizeof(s_host), host);
    s_port = port;
    session_reset();

    notice("Connected to %s.", s_host);
    notice("Escape character is '^]'.");

    raw_acquire();

    // The client speaks first: DO and WILL suppress-go-ahead, WILL window
    // size. The size itself waits for the peer to agree — telnet_send_size
    // remembers it either way, so an agreement that arrives later has
    // something to announce.
    telnet_offer(&s_engine);
    send_window_size();
    return true;
}

// ── Editing a line ──────────────────────────────────────────────────────
//
// Shared by the command prompt and by the session's line mode, because they
// are the same act: bytes accumulate, Backspace takes one back, Ctrl+U takes
// the lot, and Enter says the line is finished. What differs is what the
// caller does with the result, which is why the result is a verdict and not
// an action.

// WILL `len` BYTES OF TYPED TEXT CERTAINLY FIT? Asked BEFORE the first byte
// goes in, never after: telnet_send_text takes what it can and reports how
// much, so a caller that handed it a whole line, got half of it in, and tried
// again later would put that half on the wire twice. A single byte is
// exempt — the engine takes it whole or not at all — but anything composed
// has to be all-or-nothing here, where the decision can still be made.
//
// The estimate is the pessimistic one: every byte can double, because an IAC
// is escaped and a newline becomes CR LF.
static bool room_for_text(size_t len)
{
    size_t pending = 0;
    telnet_pending(&s_engine, &pending);
    return TELNET_OUT_MAX - pending >= len * 2;
}

#define LINE_MORE       0    // the byte was taken; keep going
#define LINE_DONE       1    // Enter: the buffer is a finished line
#define LINE_INTERRUPT  2    // Ctrl+C: abandon what was typed
#define LINE_EOF        3    // Ctrl+D on an empty line

static int line_edit(char *buf, size_t *len, size_t cap, uint8_t c, bool echo)
{
    if (c == '\r' || c == '\n')
        return LINE_DONE;

    if (c == 0x03)                                   // Ctrl+C
        return LINE_INTERRUPT;

    if (c == 0x04 && *len == 0)                      // Ctrl+D on nothing typed
        return LINE_EOF;

    if (c == '\b' || c == 0x7F) {                    // Backspace, either spelling
        if (*len > 0) {
            (*len)--;
            if (echo)
                screen_str("\b \b");                 // erasure is overprint
        }
        return LINE_MORE;
    }

    if (c == 0x15) {                                 // Ctrl+U, the 1970s kill
        while (*len > 0) {
            (*len)--;
            if (echo)
                screen_str("\b \b");
        }
        return LINE_MORE;
    }

    // Anything else that is not a control byte. A control byte typed into a
    // line would be drawn as a glyph and sent as itself, and neither is what
    // was meant; the ones that mean something are above.
    if (c < 0x20)
        return LINE_MORE;

    if (*len + 1 < cap) {
        buf[(*len)++] = (char)c;
        if (echo)
            screen(&c, 1);
    }
    return LINE_MORE;
}

// ── The commands ────────────────────────────────────────────────────────

static void command_help(void)
{
    notice("  open HOST [PORT]   connect (port %u by default)", TELNET_PORT);
    notice("  close              end the session, stay at this prompt");
    notice("  quit               end the session and the program");
    notice("  status             the connection, the options, the set");
    notice("  echo auto|on|off   who echoes what you type");
    notice("  charset cp437|latin1   how bytes over 0x7F are drawn");
    notice("  crlf on|off        Return as CR LF, or CR NUL (the default)");
    notice("  send eof|escape|ip|ayt|brk|nop   one thing, down the wire");
    notice("  continue           back to the session (an empty line does too)");
    notice("  help               this");
}

static void command_status(void)
{
    if (s_conn < 0) {
        // The keys line is deliberately absent here. Its answer is about what
        // reaches the PEER, and at this prompt there isn't one — Ctrl+C
        // abandons the line you are typing and Ctrl+D leaves, whatever the
        // terminal's discipline is.
        notice("No connection.");
    } else {
        notice("Connected to %s port %u.", s_host, (unsigned)s_port);
        notice("  typing:  %s", s_char_mode
               ? "a character at a time (the peer suppresses go-ahead)"
               : "a line at a time (the peer has not suppressed go-ahead)");
        notice("  echo:    %s%s",
               telnet_local_echo(&s_engine) ? "local" : "the peer's",
               telnet_echo_mode(&s_engine) == TELNET_ECHO_AUTO ? "" : " (forced)");
        notice("  options: %s%s%s",
               telnet_option_his(&s_engine, TELNET_OPT_ECHO) ? "peer echo, " : "",
               telnet_option_his(&s_engine, TELNET_OPT_SGA) ? "peer sga, " : "",
               telnet_option_ours(&s_engine, TELNET_OPT_NAWS)
                   ? "window size" : "no window size");

        // ASKED OF THE KERNEL, not read back from our own flag: the
        // terminal's discipline is what decides whether Ctrl+C arrives here
        // as a byte or as a signal, and when that behaves unexpectedly the
        // answer worth printing is the one the kernel would give.
        bool raw = false;
        if (os64_tty_mode(&raw, NULL) == 0)
            notice("  keys:    %s", raw
                   ? "raw, so Ctrl+D reaches the peer as a byte"
                   : "cooked, so Ctrl+D is `send eof` at this prompt");
    }
    notice("Character set: %s.", s_cp437 ? "cp437" : "latin1");
    notice("Return goes out as %s.",
           telnet_eol_crlf(&s_engine) ? "CR LF" : "CR NUL");
    notice("Escape character is '^]'.");
}

// A word from the line, NUL-terminated in place, with `rest` left pointing at
// whatever follows it. The command line is ours and already a copy, so
// splitting it in place costs nothing and loses nothing.
static char *next_word(char **rest)
{
    char *s = *rest;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0') {
        *rest = s;
        return NULL;
    }

    char *word = s;
    while (*s != '\0' && *s != ' ' && *s != '\t')
        s++;
    if (*s != '\0')
        *s++ = '\0';
    *rest = s;
    return word;
}

static bool command_send(const char *what)
{
    if (what == NULL) {
        notice("send what? eof, escape, ip, ayt, brk or nop.");
        return true;
    }
    if (s_conn < 0) {
        notice("telnet: not connected.");
        return true;
    }

    // END OF INPUT IS A BYTE, NOT A COMMAND. RFC 854 has none — the one in
    // RFC 1184 lives inside LINEMODE, which this client refuses — so what a
    // Unix login logs out on is an ordinary 0x04 travelling as data. In raw
    // mode a typed Ctrl+D gets there on its own and this is a convenience; in
    // cooked mode the console eats that key before any program sees it, and
    // this is the only door.
    if (os64_streq(what, "eof")) {
        const uint8_t eot = 0x04;
        telnet_send_text(&s_engine, &eot, 1);
        return true;
    }
    if (os64_streq(what, "escape")) {
        const uint8_t esc = TELNET_ESCAPE;
        telnet_send_text(&s_engine, &esc, 1);
        return true;
    }
    if (os64_streq(what, "ip")) {
        telnet_send_command(&s_engine, TELNET_IP);
        return true;
    }
    if (os64_streq(what, "ayt")) {
        telnet_send_command(&s_engine, TELNET_AYT);
        return true;
    }
    if (os64_streq(what, "brk")) {
        telnet_send_command(&s_engine, TELNET_BRK);
        return true;
    }
    if (os64_streq(what, "nop")) {
        telnet_send_command(&s_engine, TELNET_NOP);
        return true;
    }

    notice("telnet: send %s? eof, escape, ip, ayt, brk or nop.", what);
    return true;
}

// Runs one command line. Returns true to stay at the prompt, false to go back
// to the session.
static bool command_run(char *line)
{
    char *rest = line;
    char *verb = next_word(&rest);

    if (verb == NULL)                 // an empty line resumes, as BSD's does
        return false;

    // NO ONE-LETTER FORMS. `c` would have to choose between close, continue
    // and charset and `s` between send and status, and a prompt that guesses
    // which of two things you meant to do to a live connection is worse than
    // one that makes you type the word. `?` stays because it is not a prefix
    // of anything.
    if (os64_streq(verb, "continue"))
        return false;

    if (os64_streq(verb, "help") || os64_streq(verb, "?")) {
        command_help();
        return true;
    }

    if (os64_streq(verb, "status")) {
        command_status();
        return true;
    }

    if (os64_streq(verb, "open")) {
        char *host = next_word(&rest);
        char *port = next_word(&rest);
        uint64_t number = TELNET_PORT;

        if (host == NULL) {
            notice("open what? a host name or address, and a port if not %u.",
                   TELNET_PORT);
            return true;
        }
        if (port != NULL && (!os64_parse_u64(port, &number) ||
                             number == 0 || number > 65535)) {
            notice("telnet: %s is not a port.", port);
            return true;
        }
        // Stay here unless this call is what opened a session: an `open` that
        // was refused has not given you anywhere else to be.
        return !connect_to(host, (uint16_t)number);
    }

    if (os64_streq(verb, "close")) {
        if (s_conn < 0) {
            notice("telnet: not connected.");
            return true;
        }
        disconnect("you asked");
        return true;
    }

    if (os64_streq(verb, "quit")) {
        s_running = false;
        return true;
    }

    if (os64_streq(verb, "echo")) {
        char *how = next_word(&rest);

        if (how == NULL || os64_streq(how, "auto"))
            telnet_set_echo_mode(&s_engine, TELNET_ECHO_AUTO);
        else if (os64_streq(how, "on"))
            telnet_set_echo_mode(&s_engine, TELNET_ECHO_ON);
        else if (os64_streq(how, "off"))
            telnet_set_echo_mode(&s_engine, TELNET_ECHO_OFF);
        else {
            notice("telnet: echo %s? auto, on or off.", how);
            return true;
        }
        notice("Echo is %s.", telnet_local_echo(&s_engine) ? "local" : "the peer's");
        return true;
    }

    if (os64_streq(verb, "crlf")) {
        char *how = next_word(&rest);

        if (how == NULL || os64_streq(how, "on"))
            telnet_set_eol_crlf(&s_engine, true);
        else if (os64_streq(how, "off"))
            telnet_set_eol_crlf(&s_engine, false);
        else {
            notice("telnet: crlf %s? on or off.", how);
            return true;
        }
        notice("Return goes out as %s.",
               telnet_eol_crlf(&s_engine) ? "CR LF" : "CR NUL");
        return true;
    }

    if (os64_streq(verb, "charset")) {
        char *which = next_word(&rest);

        if (which == NULL || os64_streq(which, "cp437"))
            charset_select(true);
        else if (os64_streq(which, "latin1"))
            charset_select(false);
        else {
            notice("telnet: charset %s? cp437 or latin1.", which);
            return true;
        }
        notice("Bytes over 0x7F are %s.", s_cp437 ? "cp437" : "latin1");
        return true;
    }

    if (os64_streq(verb, "send"))
        return command_send(next_word(&rest));

    notice("telnet: %s? `help` lists what there is.", verb);
    return true;
}

// ── What the person typed ───────────────────────────────────────────────

static void prompt_open(void)
{
    s_prompting = true;
    s_cmd_len = 0;
    prompt_draw();                    // which puts itself on a fresh row
}

static void prompt_close(void)
{
    if (!s_prompting)
        return;
    prompt_erase();
    s_prompting = false;
}

// Going back to the session with a line half-composed. The bytes were never
// lost — they are in s_line, which is why it is a separate buffer from the
// prompt's — but the screen has moved on since they were echoed, so they are
// echoed again and you can see what you are finishing. Only where this client
// is the one doing the echoing: a peer that holds ECHO would be the one to
// ask, and it was never told about any of this.
static void session_resume(void)
{
    if (s_char_mode || s_line_len == 0 || !telnet_local_echo(&s_engine))
        return;
    screen(s_line, s_line_len);
}

// A byte typed at the telnet> prompt.
static void key_at_prompt(uint8_t c)
{
    char line[TELNET_CMD_MAX];

    switch (line_edit(s_cmd, &s_cmd_len, sizeof(s_cmd), c, true)) {
    case LINE_DONE:
        s_cmd[s_cmd_len] = '\0';
        os64_strcopy(line, sizeof(line), s_cmd);
        screen_str("\r\n");
        s_cmd_len = 0;
        s_prompting = false;          // notices from the command print plainly
        bool stay = command_run(line);
        if (!s_running)
            break;                    // `quit`: there is nothing left to draw
        if (stay || s_conn < 0)
            prompt_open();            // with no session there is nowhere else
        else                          //  to be than here
            session_resume();
        break;

    case LINE_INTERRUPT:
        s_cmd_len = 0;
        screen_str("\r\n");
        prompt_draw();
        break;

    case LINE_EOF:
        // Ctrl+D on an empty prompt is what it is everywhere: nothing more is
        // coming. It ends the program rather than the session, because the
        // session already has `close`.
        screen_str("\r\n");
        s_prompting = false;
        s_running = false;
        break;

    default:
        break;
    }
}

// A byte typed during a session. Returns false when the engine's queue would
// not take it, so the caller can leave it where it is and try again.
static bool key_at_session(uint8_t c)
{
    // Ctrl+C is the interrupt, wherever it came from. In raw mode it arrives
    // as this byte; in cooked mode the console turned it into SIGINT and the
    // loop raised the same request. Either way it is IAC IP: RFC 854's own
    // answer, which reaches a remote whose own tty settings this end cannot
    // see, and which nearly every telnetd has understood since 1983.
    if (c == 0x03) {
        if (!telnet_send_command(&s_engine, TELNET_IP))
            return false;
        s_line_len = 0;
        return true;
    }

    bool echo = telnet_local_echo(&s_engine);

    if (s_char_mode) {
        if (telnet_send_text(&s_engine, &c, 1) != 1)
            return false;
        // CHARACTER MODE STILL ECHOES WHEN NOBODY ELSE IS. A peer can suppress
        // go-ahead without taking ECHO — a Unix login does exactly that until
        // the password prompt — and a client that only echoed in line mode
        // would type the username into the dark. There is no line buffer here
        // to erase from, so a backspace is drawn as the overprint it is and
        // the far end decides what it actually did.
        if (echo) {
            if (c == '\r' || c == '\n')
                screen_str("\r\n");
            else if (c == '\b' || c == 0x7F)
                screen_str("\b \b");
            else if (c >= 0x20)
                screen(&c, 1);
        }
        return true;
    }

    // Line mode: the peer has not suppressed go-ahead, so it is the
    // half-duplex printer of RFC 854 and a line is the unit it expects. The
    // line is echoed here if nobody else is echoing it.

    switch (line_edit(s_line, &s_line_len, sizeof(s_line), c, echo)) {
    case LINE_DONE: {
        // The line and its newline are one act, so the room for both is
        // settled before either is queued and the line is untouched if there
        // isn't any. The Enter that ended it stays in the terminal buffer and
        // arrives again after the next flush.
        if (!room_for_text(s_line_len + 1))
            return false;
        s_line[s_line_len] = '\n';
        telnet_send_text(&s_engine, s_line, s_line_len + 1);
        s_line_len = 0;
        if (echo)
            screen_str("\r\n");
        break;
    }
    case LINE_EOF: {
        const uint8_t eot = 0x04;
        return telnet_send_text(&s_engine, &eot, 1) == 1;
    }
    default:
        break;
    }
    return true;
}

// ── One pass of the loop ────────────────────────────────────────────────

// Move what the terminal gave us into the engine, a byte at a time, stopping
// the moment the engine will not take one. Returns true when something moved.
static bool feed_keys(void)
{
    bool moved = false;

    while (s_kb_pos < s_kb_len) {
        uint8_t c = s_kb[s_kb_pos];

        if (s_prompting) {
            s_kb_pos++;
            moved = true;
            key_at_prompt(c);
            if (!s_running)
                return true;
            continue;
        }

        if (c == TELNET_ESCAPE) {
            s_kb_pos++;
            moved = true;
            prompt_open();
            continue;
        }

        if (s_conn < 0) {
            // Not connected and not prompting cannot happen for long: the
            // prompt IS the program when there is no session. Drop the byte
            // rather than spin on it.
            s_kb_pos++;
            moved = true;
            continue;
        }

        if (!key_at_session(c))
            break;                    // the queue is full; the byte waits here
        s_kb_pos++;
        moved = true;
    }

    if (s_kb_pos == s_kb_len)
        s_kb_pos = s_kb_len = 0;
    return moved;
}

// Read the terminal, if there is room to put what it says.
static bool read_keys(uint64_t patience_ms)
{
    if (s_keys_gone || s_kb_pos < s_kb_len)
        return false;

    int64_t n = os64_read_for(s_keys, s_kb, sizeof(s_kb), patience_ms);
    if (n > 0) {
        s_kb_len = (size_t)n;
        s_kb_pos = 0;
        s_keys_eof = 0;
        return true;
    }
    if (n == 0) {
        // End of input. On a cooked console this is one Ctrl+D — the read
        // after it works normally again — so it is a KEYPRESS, and what it
        // asks for during a session is an end of input at the far end. What
        // it cannot be is a stream that has genuinely finished, and the
        // difference is visible in the timing: a finished stream answers 0
        // instantly every time it is asked, so two of them with no wait in
        // between is nobody there rather than two keypresses.
        if (++s_keys_eof >= 2) {
            s_keys_gone = true;
            if (s_conn < 0 || s_prompting)
                s_running = false;
            return false;
        }
        if (s_conn >= 0 && !s_prompting) {
            const uint8_t eot = 0x04;
            telnet_send_text(&s_engine, &eot, 1);
        } else {
            s_running = false;        // Ctrl+D at the prompt: nothing more is coming
        }
        return true;
    }

    // A timeout is the patience expiring, which is a WAIT and not an answer —
    // so it separates one Ctrl+D from the next.
    s_keys_eof = 0;
    return false;
}

// Drain the engine's queue to the peer.
static bool flush_to_peer(void)
{
    size_t len = 0;
    const uint8_t *out = telnet_pending(&s_engine, &len);

    if (len == 0 || s_conn < 0)
        return false;

    int64_t n = os64_write(s_conn, out, len);
    if (n < 0) {
        disconnect("the connection broke");
        return true;
    }
    telnet_sent(&s_engine, (size_t)n);
    return n > 0;
}

// Everything the engine latched that ordinary code has to act on.
static void serve_notices(void)
{
    uint32_t notices = telnet_notices(&s_engine);

    if (notices & TELNET_NOTE_SIZE)
        send_window_size();

    if (notices & TELNET_NOTE_AYT) {
        // "Are you there?" is the one command that asks for visible evidence
        // (RFC 854), and the evidence is DATA — sent only when the queue has
        // room for all of it, since half an answer is worse than none.
        static const char yes[] = "\r\n[os64 telnet: yes]\r\n";
        if (room_for_text(sizeof(yes) - 1))
            telnet_send_text(&s_engine, yes, sizeof(yes) - 1);
    }

    if (notices & TELNET_NOTE_CONTRADICT)
        notice("telnet: the peer answered an option request with its"
               " opposite; settled its way.");
}

// Read the peer and paint what was data.
static bool serve_peer(uint64_t patience_ms)
{
    uint8_t data[TELNET_NET_MAX];
    bool moved = false;

    if (s_conn < 0)
        return false;

    if (s_net_pos == s_net_len) {
        s_net_pos = s_net_len = 0;
        int64_t n = os64_read_for(s_conn, s_net, sizeof(s_net), patience_ms);
        if (n > 0) {
            s_net_len = (size_t)n;
            moved = true;
        } else if (n == 0) {
            // The peer hung up. A cut inside an IAC sequence is a different
            // report than a clean goodbye, and the engine is the only thing
            // that knows which one it was.
            disconnect(telnet_mid_sequence(&s_engine)
                       ? "the peer stopped mid-command" : "the peer hung up");
            return true;
        } else if (n != OS64_ERR_TIMEOUT) {
            disconnect("the connection broke");
            return true;
        }
    }

    while (s_net_pos < s_net_len) {
        size_t shown = 0;
        size_t took = telnet_receive(&s_engine, s_net + s_net_pos,
                                     s_net_len - s_net_pos,
                                     data, sizeof(data), &shown);
        if (shown > 0) {
            // The prompt steps aside for the peer's bytes and comes back with
            // whatever was half-typed still on it.
            if (s_prompting)
                prompt_erase();
            screen(data, shown);
            if (s_prompting)
                prompt_draw();
            moved = true;
        }
        s_net_pos += took;
        if (took == 0)
            break;                    // the outbound queue is full; try later
        moved = true;
    }

    if (s_net_pos == s_net_len)
        s_net_pos = s_net_len = 0;
    return moved;
}

// The peer taking or dropping SUPPRESS-GO-AHEAD is what moves this client
// between a line at a time and a character at a time. A line half-typed when
// the switch happens goes as it stands: it was already on its way, and
// holding it for an Enter that character mode will never deliver would lose it.
static void follow_typing_mode(void)
{
    bool now = telnet_option_his(&s_engine, TELNET_OPT_SGA);

    if (now == s_char_mode)
        return;
    if (now && s_line_len > 0) {
        if (!room_for_text(s_line_len))
            return;                   // no room yet; ask again next pass, with
        telnet_send_text(&s_engine, s_line, s_line_len);   // the line intact
        s_line_len = 0;
    }
    s_char_mode = now;
}

// ── The loop ────────────────────────────────────────────────────────────

static void run(void)
{
    // A pass that found nothing parks; a pass that found something asks with
    // no patience, so work keeps its pace and idling costs two ticks.
    uint64_t patience = TELNET_POLL_MS;

    while (s_running) {
        bool moved = false;

        if (s_want_quit) {
            s_want_quit = 0;
            break;
        }

        // The interrupt request is CLEARED WHEN IT IS SERVED, not when it is
        // noticed: a full outbound queue is a reason to try again next pass,
        // and dropping the request there would lose the one keypress a person
        // presses hardest. The prompt answers it the way it answers a typed
        // Ctrl+C — by abandoning the line — so that the cooked terminal and
        // the raw one behave alike.
        if (s_want_interrupt) {
            if (s_prompting) {
                s_want_interrupt = 0;
                s_cmd_len = 0;
                screen_str("\r\n");
                prompt_draw();
            } else if (s_conn < 0) {
                s_want_interrupt = 0;
            } else if (telnet_send_command(&s_engine, TELNET_IP)) {
                s_want_interrupt = 0;
                moved = true;
            }
        }

        if (s_want_size) {
            s_want_size = 0;
            send_window_size();
            moved = true;
        }

        follow_typing_mode();

        if (read_keys(patience))
            moved = true;
        if (feed_keys())
            moved = true;
        if (flush_to_peer())
            moved = true;
        if (serve_peer(s_conn >= 0 ? patience : 0))
            moved = true;
        serve_notices();
        if (flush_to_peer())
            moved = true;

        // A pass that moved something asks the next one with no patience at
        // all, so a board painting a screen streams at the wire's pace; a pass
        // that found nothing parks on both questions, which is where the idle
        // cost of this loop lives and why the deadline is one tick and not ten.
        patience = moved ? 0 : TELNET_POLL_MS;

        if (!s_running)
            break;
        if (s_conn < 0 && !s_prompting)
            prompt_open();
    }
}

// ── main ────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    bool cp437 = false;
    const os64_optspec_t specs[] = {
        {'8', "cp437", 0, "draw bytes over 0x7F as CP437 (ANSI art), not Latin-1",
         .flag = &cp437},
    };

    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Talk to a telnet host. Ctrl+] opens the telnet> prompt.";
    args.details = "With no host, opens that prompt: `open HOST [PORT]`.";

    const char *positional[2] = { NULL, NULL };
    int32_t n = os64_args_parse(&args, "telnet [-8] [host [port]]",
                                positional, 2);
    if (n < 0)
        return n == OS64_ARG_HELP ? TELNET_OK : TELNET_USAGE;

    uint64_t port = TELNET_PORT;
    if (n == 2 && (!os64_parse_u64(positional[1], &port) ||
                   port == 0 || port > 65535)) {
        os64_hprintf(OS64_STDERR, "telnet: %s is not a port\n", positional[1]);
        return TELNET_USAGE;
    }

    // Keys come from the TERMINAL, not from handle 0, so `telnet host < file`
    // and a telnet inside a pipeline still read the person's keyboard.
    int64_t keys = os64_tty_handle();
    s_keys = keys >= 0 ? (int32_t)keys : OS64_STDIN;

    // A handler that only raises a flag can be installed unconditionally: if
    // one is refused the loop simply never sees that request, which is the
    // behaviour without it.
    os64_signal_set_handler(OS64_SIGINT, on_interrupt);
    os64_signal_set_handler(OS64_SIGWINCH, on_resize);
    os64_signal_set_handler(OS64_SIGHUP, on_hangup);
    os64_signal_set_handler(OS64_SIGTERM, on_hangup);

    telnet_init(&s_engine);
    if (cp437)
        charset_select(true);

    if (n >= 1)
        connect_to(positional[0], (uint16_t)port);
    if (s_conn < 0)
        prompt_open();

    run();

    prompt_close();
    disconnect(NULL);
    charset_select(false);            // the terminal is owed its set back
    if (s_keys != OS64_STDIN)
        os64_close(s_keys);

    if (!s_at_line_start)
        screen_str("\r\n");

    // A death by signal is tagged the way the kernel's own default would have
    // tagged it, so a script cannot tell the two apart.
    if (s_quit_signal != 0)
        return OS64_EXIT_FOR_SIGNAL(s_quit_signal);
    return TELNET_OK;
}
