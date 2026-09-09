// telnet_protocol.c — RFC 854 on the wire, RFC 1143 in the negotiation. See
// telnet_protocol.h for what is here and why the option scope is what it is.

#include "telnet_protocol.h"

#include "os64/str.h"

// ── The parse state ─────────────────────────────────────────────────────
//
// Every one of these can be left standing at the end of a read: a connection
// is cut wherever the peer stopped talking, and telnet's sequences are two,
// three and many bytes long. That is the entire reason the state lives in the
// struct instead of in a local.

enum {
    P_DATA = 0,   // ordinary bytes, on their way to the screen
    P_IAC,        // saw IAC — the next byte says what kind of command
    P_VERB,       // saw IAC WILL/WONT/DO/DONT — the next byte is the option
    P_SB_OPT,     // saw IAC SB — the next byte is the option being negotiated
    P_SB,         // inside a subnegotiation, skipping to its IAC SE
    P_SB_IAC,     // saw IAC inside a subnegotiation
};

// ── The outbound queue ──────────────────────────────────────────────────
//
// One queue, because negotiation replies and typed data have to reach the
// peer IN THE ORDER THEY HAPPENED. Linear rather than circular: the amounts
// are a keystroke and a three-byte reply, so the memmove when the caller
// reports a partial write costs less than the arithmetic a ring would need,
// and `telnet_pending` can hand back one contiguous span.

static size_t room(const telnet_t *t)
{
    return TELNET_OUT_MAX - t->out_len;
}

static void push(telnet_t *t, uint8_t byte)
{
    if (t->out_len < TELNET_OUT_MAX)
        t->out[t->out_len++] = byte;
}

// ── What this client agrees to ──────────────────────────────────────────
//
// Two questions, deliberately asked separately, because the answers differ
// for the same option: ECHO is something the SERVER may do and this client
// may not (a client echoing to the server is how you get every character
// twice), and NAWS is something this client does and no server has any
// business doing at us.

static bool we_agree(uint8_t option)
{
    return option == TELNET_OPT_SGA || option == TELNET_OPT_NAWS;
}

static bool he_may(uint8_t option)
{
    return option == TELNET_OPT_ECHO || option == TELNET_OPT_SGA;
}

// ── RFC 1143, the Q Method ──────────────────────────────────────────────
//
// Two state machines per option — what WE do, what HE does — and the rule
// that makes them loop-free: A REQUEST IS ANSWERED ONLY WHEN IT CHANGES
// SOMETHING. A second WILL for an option already on is met with silence, and
// a WILL that crosses our own DO in flight completes the negotiation instead
// of starting a new one. The naive implementation answers everything, reads
// the answer as a fresh request, and trades bytes with the peer forever.
//
// The OPPOSITE states are entered by a change of mind mid-negotiation, and
// this client's asking is all done from NO at the moment it connects, so
// those two arms are the METHOD's rather than this client's. They are
// implemented because a Q method with pieces missing is precisely the thing
// RFC 1143 exists to replace, and because they are where the next option
// this client learns to turn off will land.

// Told only when the state ACTUALLY MOVED, which is why each caller keeps
// the value it started with. A notice is a thing the caller has to act on —
// repaint the echo policy, put a window size on the wire — and one that fires
// for a repeat that changed nothing would have the client answering a
// talkative peer's every restatement.
static void changed(telnet_t *t, uint8_t option, bool ours)
{
    // ECHO is HIS and decides whether we echo what is typed; NAWS is OURS and
    // decides whether a window size can go out at all. An option's other
    // direction is one this client refuses, so it never moves.
    if (!ours && option == TELNET_OPT_ECHO)
        t->notices |= TELNET_NOTE_ECHO;
    if (ours && option == TELNET_OPT_NAWS && t->us[option] == TELNET_OPT_YES)
        t->notices |= TELNET_NOTE_SIZE;
}

static void reply(telnet_t *t, uint8_t verb, uint8_t option)
{
    push(t, TELNET_IAC);
    push(t, verb);
    push(t, option);
}

static void got_will(telnet_t *t, uint8_t option)
{
    uint8_t was = t->him[option];

    switch (t->him[option]) {
    case TELNET_OPT_NO:
        if (he_may(option)) {
            t->him[option] = TELNET_OPT_YES;
            reply(t, TELNET_DO, option);
        } else {
            reply(t, TELNET_DONT, option);
        }
        break;
    case TELNET_OPT_YES:
        break;                                  // already on — silence, no loop
    case TELNET_OPT_WANTNO:
        // We said DONT and he answered WILL. RFC 1143 names this exactly:
        // "DONT answered by WILL". Take his word for the state and say so.
        t->him[option] = TELNET_OPT_NO;
        t->notices |= TELNET_NOTE_CONTRADICT;
        break;
    case TELNET_OPT_WANTNO_OPPOSITE:
        t->him[option] = TELNET_OPT_YES;
        t->notices |= TELNET_NOTE_CONTRADICT;
        break;
    case TELNET_OPT_WANTYES:
        t->him[option] = TELNET_OPT_YES;        // our DO, answered
        break;
    case TELNET_OPT_WANTYES_OPPOSITE:
        t->him[option] = TELNET_OPT_WANTNO;
        reply(t, TELNET_DONT, option);
        break;
    }

    if (t->him[option] != was)
        changed(t, option, false);
}

static void got_wont(telnet_t *t, uint8_t option)
{
    uint8_t was = t->him[option];

    switch (t->him[option]) {
    case TELNET_OPT_NO:
        break;                                  // already off — silence
    case TELNET_OPT_YES:
        t->him[option] = TELNET_OPT_NO;
        reply(t, TELNET_DONT, option);
        break;
    case TELNET_OPT_WANTNO:
    case TELNET_OPT_WANTYES:
    case TELNET_OPT_WANTYES_OPPOSITE:
        t->him[option] = TELNET_OPT_NO;
        break;
    case TELNET_OPT_WANTNO_OPPOSITE:
        t->him[option] = TELNET_OPT_WANTYES;
        reply(t, TELNET_DO, option);
        break;
    }

    if (t->him[option] != was)
        changed(t, option, false);
}

static void got_do(telnet_t *t, uint8_t option)
{
    uint8_t was = t->us[option];

    switch (t->us[option]) {
    case TELNET_OPT_NO:
        if (we_agree(option)) {
            t->us[option] = TELNET_OPT_YES;
            reply(t, TELNET_WILL, option);
        } else {
            reply(t, TELNET_WONT, option);
        }
        break;
    case TELNET_OPT_YES:
        break;
    case TELNET_OPT_WANTNO:
        t->us[option] = TELNET_OPT_NO;          // "WONT answered by DO"
        t->notices |= TELNET_NOTE_CONTRADICT;
        break;
    case TELNET_OPT_WANTNO_OPPOSITE:
        t->us[option] = TELNET_OPT_YES;
        t->notices |= TELNET_NOTE_CONTRADICT;
        break;
    case TELNET_OPT_WANTYES:
        t->us[option] = TELNET_OPT_YES;         // our WILL, answered
        break;
    case TELNET_OPT_WANTYES_OPPOSITE:
        t->us[option] = TELNET_OPT_WANTNO;
        reply(t, TELNET_WONT, option);
        break;
    }

    if (t->us[option] != was)
        changed(t, option, true);
}

static void got_dont(telnet_t *t, uint8_t option)
{
    uint8_t was = t->us[option];

    switch (t->us[option]) {
    case TELNET_OPT_NO:
        break;
    case TELNET_OPT_YES:
        t->us[option] = TELNET_OPT_NO;
        reply(t, TELNET_WONT, option);
        break;
    case TELNET_OPT_WANTNO:
    case TELNET_OPT_WANTYES:
    case TELNET_OPT_WANTYES_OPPOSITE:
        t->us[option] = TELNET_OPT_NO;
        break;
    case TELNET_OPT_WANTNO_OPPOSITE:
        t->us[option] = TELNET_OPT_WANTYES;
        reply(t, TELNET_WILL, option);
        break;
    }

    if (t->us[option] != was)
        changed(t, option, true);
}

// Ask for an option. Only ever called from NO — this client makes its
// requests once, at the moment it connects — so the queued-request arms of
// RFC 1143's ask() have no caller and are not written down. A state that is
// anything but NO is already on or already asked, and asking again is the
// repeated request the method exists to suppress.

static void ask_him(telnet_t *t, uint8_t option)
{
    if (t->him[option] != TELNET_OPT_NO)
        return;
    t->him[option] = TELNET_OPT_WANTYES;
    reply(t, TELNET_DO, option);
}

static void ask_us(telnet_t *t, uint8_t option)
{
    if (t->us[option] != TELNET_OPT_NO)
        return;
    t->us[option] = TELNET_OPT_WANTYES;
    reply(t, TELNET_WILL, option);
}

// ── Life ────────────────────────────────────────────────────────────────

void telnet_init(telnet_t *t)
{
    os64_memset(t, 0, sizeof(*t));
    // Every zero here is the right start: P_DATA, NO for all 512 option
    // states, an empty queue, TELNET_ECHO_AUTO, no size known yet.
}

bool telnet_offer(telnet_t *t)
{
    // Three offers, three bytes each. Checked once, up front, because a
    // half-sent offer would leave an option's state claiming a request that
    // never reached the wire.
    if (room(t) < 9)
        return false;

    ask_him(t, TELNET_OPT_SGA);    // don't make us wait for a turn
    ask_us(t, TELNET_OPT_SGA);     // and we won't make you wait for one
    ask_us(t, TELNET_OPT_NAWS);    // we have a window size worth having
    return true;
}

// ── From the peer ───────────────────────────────────────────────────────

// A subnegotiation is read in order to be THROWN AWAY, and there is no
// buffer to throw it into. Nothing this client offers has parameters to
// receive — NAWS travels outbound only, and TERMINAL-TYPE, TSPEED, LFLOW and
// NEW-ENVIRON are all refused before they get this far — so the payload is
// skipped byte by byte to its IAC SE. Not storing it is the strongest form
// of "never sized from a length the peer supplied": there is no length to
// supply and nothing to overrun. What matters is that the skip is EXACT,
// because a subnegotiation read as data paints its payload on the screen.

size_t telnet_receive(telnet_t *t, const void *in, size_t len,
                      void *data, size_t cap, size_t *data_len)
{
    const uint8_t *p = (const uint8_t *)in;
    uint8_t *out = (uint8_t *)data;
    size_t used = 0;
    size_t i = 0;
    bool stalled = false;

    while (i < len && !stalled) {
        // Room to ANSWER the byte before reading it. Every reply this parser
        // can produce is one option command, so keeping TELNET_REPLY_MAX free
        // means the answer can never be half-written — and the caller learns
        // the queue is full by being handed back the bytes it did not read.
        if (room(t) < TELNET_REPLY_MAX)
            break;

        uint8_t c = p[i];

        switch (t->parse) {
        case P_DATA:
            if (c == TELNET_IAC) {
                t->parse = P_IAC;
                break;
            }
            // NUL IS THE NVT'S NO-OP, and dropping it here is what makes the
            // two-byte newline take care of itself. RFC 854 gives NUL no
            // meaning at the printer; CR NUL is how the protocol spells a
            // bare carriage return, the NUL being there only so that a CR is
            // never the last byte of a line; and a NUL on its own is the PAD
            // character terminfo has been sending to slow terminals since the
            // 1970s. Drop it, and CR NUL arrives at the screen as the CR it
            // means while CR LF arrives as both bytes, with no state to carry
            // between reads. os64's renderer would otherwise draw a glyph for
            // all three.
            if (c == 0)
                break;
            if (used == cap) {
                stalled = true;         // no room to show it; do not consume
                break;
            }
            out[used++] = c;
            break;

        case P_IAC:
            switch (c) {
            case TELNET_IAC:
                // A doubled escape is one data byte worth 255.
                if (used == cap) {
                    stalled = true;
                    break;
                }
                out[used++] = TELNET_IAC;
                t->parse = P_DATA;
                break;
            case TELNET_WILL:
            case TELNET_WONT:
            case TELNET_DO:
            case TELNET_DONT:
                t->verb = c;
                t->parse = P_VERB;
                break;
            case TELNET_SB:
                t->parse = P_SB_OPT;
                break;
            case TELNET_AYT:
                // "Are you there?" is the one command that asks for visible
                // evidence (RFC 854), so it is the one the caller is told
                // about. Answering is the caller's decision, not ours.
                t->notices |= TELNET_NOTE_AYT;
                t->parse = P_DATA;
                break;
            default:
                // NOP, GA, DM, BRK, IP, AO, EC, EL, a stray SE, and every
                // command nobody has defined. Consumed, never printed — a
                // client that prints an unknown command has just let the peer
                // write a byte of its choosing onto the glass, and a client
                // that prints GA collects one per line from every half-duplex
                // server on the planet.
                t->parse = P_DATA;
                break;
            }
            break;

        case P_VERB:
            t->parse = P_DATA;
            if (t->verb == TELNET_WILL)
                got_will(t, c);
            else if (t->verb == TELNET_WONT)
                got_wont(t, c);
            else if (t->verb == TELNET_DO)
                got_do(t, c);
            else
                got_dont(t, c);
            break;

        case P_SB_OPT:
            // The option byte is OPAQUE — it is not escaped, so a
            // subnegotiation of option 255 (RFC 861's extended options list)
            // is a subnegotiation and not an IAC. That is the whole reason
            // this is a state of its own rather than the first byte of P_SB.
            t->parse = P_SB;
            break;

        case P_SB:
            if (c == TELNET_IAC)
                t->parse = P_SB_IAC;
            break;                      // everything else is skipped payload

        case P_SB_IAC:
            if (c == TELNET_IAC) {
                t->parse = P_SB;        // a doubled escape inside the payload
                break;
            }
            if (c == TELNET_SE) {
                t->parse = P_DATA;      // the end, and the payload is gone
                break;
            }
            // ANYTHING ELSE ABANDONS THE SUBNEGOTIATION. Only IAC SE ends one
            // properly, so a command arriving mid-payload means the peer has
            // moved on — and reading it as what it is beats swallowing the
            // rest of the connection waiting for an SE that is not coming.
            // The byte is not consumed here: P_IAC reads it, and P_IAC always
            // consumes, so the loop cannot spin.
            t->parse = P_IAC;
            continue;
        }

        if (!stalled)
            i++;
    }

    if (data_len)
        *data_len = used;
    return i;
}

bool telnet_mid_sequence(const telnet_t *t)
{
    return t->parse != P_DATA;
}

// ── To the peer ─────────────────────────────────────────────────────────

size_t telnet_send_text(telnet_t *t, const void *text, size_t len)
{
    const uint8_t *p = (const uint8_t *)text;
    size_t i = 0;

    while (i < len) {
        uint8_t c = p[i];

        // The LF that belongs to the CR we just turned into a newline. One
        // newline arrived; one goes out.
        if (t->cr_pending_out) {
            t->cr_pending_out = false;
            if (c == '\n') {
                i++;
                continue;
            }
        }

        if (c == TELNET_IAC) {
            // A 255 in a paste must not become a command.
            if (room(t) < 2)
                break;
            push(t, TELNET_IAC);
            push(t, TELNET_IAC);
        } else if (c == '\r' || c == '\n') {
            // ANY OF CR, LF OR CR LF IS ONE PRESS OF RETURN, and it leaves as
            // two bytes either way: CR NUL by default, CR LF when the caller
            // has asked for it. telnet_set_eol_crlf carries the argument for
            // which — the short version is that CR NUL says "Return was
            // pressed" and CR LF says "start a new line", and a program that
            // reads a line and then reads a key can tell the difference.
            if (room(t) < 2)
                break;
            push(t, '\r');
            push(t, t->eol_crlf ? '\n' : '\0');
            t->cr_pending_out = (c == '\r');
        } else {
            if (room(t) < 1)
                break;
            push(t, c);
        }
        i++;
    }

    return i;
}

bool telnet_send_command(telnet_t *t, uint8_t command)
{
    if (room(t) < 2)
        return false;
    push(t, TELNET_IAC);
    push(t, command);
    // A newline is two ADJACENT bytes; a command between them ends the pair.
    t->cr_pending_out = false;
    return true;
}

static void push_size_byte(telnet_t *t, uint8_t byte)
{
    // THE BUG THIS OPTION IS FAMOUS FOR: a 255-column window writes an IAC
    // into the middle of the subnegotiation, and a peer that reads it as one
    // sees the size end early and the bytes after it become commands. RFC
    // 1073 says to double it like any other IAC, and it means inside the
    // payload too.
    push(t, byte);
    if (byte == TELNET_IAC)
        push(t, TELNET_IAC);
}

bool telnet_send_size(telnet_t *t, uint16_t cols, uint16_t rows)
{
    // Remembered whether or not it can be sent, so that an agreement that
    // arrives later has a size to announce.
    t->cols = cols;
    t->rows = rows;
    t->size_known = true;

    if (!telnet_option_ours(t, TELNET_OPT_NAWS))
        return false;

    // IAC SB NAWS, four dimension bytes that may each double, IAC SE.
    if (room(t) < 3 + 8 + 2)
        return false;

    push(t, TELNET_IAC);
    push(t, TELNET_SB);
    push(t, TELNET_OPT_NAWS);
    push_size_byte(t, (uint8_t)(cols >> 8));
    push_size_byte(t, (uint8_t)(cols & 0xFF));
    push_size_byte(t, (uint8_t)(rows >> 8));
    push_size_byte(t, (uint8_t)(rows & 0xFF));
    push(t, TELNET_IAC);
    push(t, TELNET_SE);
    return true;
}

const uint8_t *telnet_pending(const telnet_t *t, size_t *len)
{
    if (len)
        *len = t->out_len;
    return t->out;
}

void telnet_sent(telnet_t *t, size_t n)
{
    if (n >= t->out_len) {
        t->out_len = 0;
        return;
    }
    os64_memmove(t->out, t->out + n, t->out_len - n);
    t->out_len -= n;
}

// ── What the caller asks about ──────────────────────────────────────────

bool telnet_local_echo(const telnet_t *t)
{
    switch (t->echo_mode) {
    case TELNET_ECHO_ON:
        return true;
    case TELNET_ECHO_OFF:
        return false;
    default:
        // AUTO: the server echoing is what turns a password prompt dark.
        return !telnet_option_his(t, TELNET_OPT_ECHO);
    }
}

void telnet_set_echo_mode(telnet_t *t, telnet_echo_mode_t mode)
{
    t->echo_mode = (uint8_t)mode;
}

telnet_echo_mode_t telnet_echo_mode(const telnet_t *t)
{
    return (telnet_echo_mode_t)t->echo_mode;
}

void telnet_set_eol_crlf(telnet_t *t, bool crlf)
{
    t->eol_crlf = crlf;
}

bool telnet_eol_crlf(const telnet_t *t)
{
    return t->eol_crlf;
}

bool telnet_option_ours(const telnet_t *t, uint8_t option)
{
    return t->us[option] == TELNET_OPT_YES;
}

bool telnet_option_his(const telnet_t *t, uint8_t option)
{
    return t->him[option] == TELNET_OPT_YES;
}

uint32_t telnet_notices(telnet_t *t)
{
    uint32_t notices = t->notices;
    t->notices = 0;
    return notices;
}

// ── Names ───────────────────────────────────────────────────────────────
//
// The unnamed ones are rendered into a small rotating set of buffers so that
// two can appear in one printf. That makes these calls unsafe from two
// threads at once, which the client's one loop never does.

static const char *number_name(const char *what, uint8_t value)
{
    static char slot[2][20];
    static unsigned next;

    char *dst = slot[next++ % 2];
    size_t at = 0;

    while (*what != '\0' && at < sizeof(slot[0]) - 5)
        dst[at++] = *what++;
    dst[at++] = ' ';

    if (value >= 100)
        dst[at++] = (char)('0' + value / 100);
    if (value >= 10)
        dst[at++] = (char)('0' + (value / 10) % 10);
    dst[at++] = (char)('0' + value % 10);
    dst[at] = '\0';
    return dst;
}

const char *telnet_option_name(uint8_t option)
{
    switch (option) {
    case TELNET_OPT_BINARY:   return "binary";
    case TELNET_OPT_ECHO:     return "echo";
    case TELNET_OPT_SGA:      return "suppress-go-ahead";
    case TELNET_OPT_STATUS:   return "status";
    case TELNET_OPT_TIMING:   return "timing-mark";
    case TELNET_OPT_TTYPE:    return "terminal-type";
    case TELNET_OPT_EOR:      return "end-of-record";
    case TELNET_OPT_NAWS:     return "window-size";
    case TELNET_OPT_TSPEED:   return "terminal-speed";
    case TELNET_OPT_LFLOW:    return "flow-control";
    case TELNET_OPT_LINEMODE: return "line-mode";
    case TELNET_OPT_XDISPLOC: return "x-display";
    case TELNET_OPT_ENVIRON:  return "environment";
    default:                  return number_name("option", option);
    }
}

const char *telnet_command_name(uint8_t command)
{
    switch (command) {
    case TELNET_IAC:  return "IAC";
    case TELNET_DONT: return "DONT";
    case TELNET_DO:   return "DO";
    case TELNET_WONT: return "WONT";
    case TELNET_WILL: return "WILL";
    case TELNET_SB:   return "SB";
    case TELNET_GA:   return "GA";
    case TELNET_EL:   return "EL";
    case TELNET_EC:   return "EC";
    case TELNET_AYT:  return "AYT";
    case TELNET_AO:   return "AO";
    case TELNET_IP:   return "IP";
    case TELNET_BRK:  return "BRK";
    case TELNET_DM:   return "DM";
    case TELNET_NOP:  return "NOP";
    case TELNET_SE:   return "SE";
    default:          return number_name("command", command);
    }
}
