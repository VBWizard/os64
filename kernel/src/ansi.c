// ansi.c — the escape-sequence reader. See ansi.h for what it reads and why
// the list is short. No kernel headers on purpose: the host harness compiles
// this file with plain cc.

#include "ansi.h"

enum {
    ST_GROUND = 0,   // ordinary text
    ST_ESC,          // an ESC arrived; the next byte says what kind
    ST_ESC_INT,      // ESC then an intermediate (`ESC ( B`): one more byte to eat
    ST_CSI,          // ESC [ — parameters, then a final byte
    ST_OSC,          // ESC ] — a string, ended by BEL or ESC \.
    ST_OSC_ESC,      // inside OSC, an ESC arrived: a '\' after it ends the string
};

// Begin a control sequence: the parameters are ZEROED, not merely counted
// empty. Leaving them was a bug the host harness caught before this file had
// ever run in the kernel — `ESC[31m` followed by `ESC[0m` accumulated into
// the leftover 31 and asked for colour 310, and the sequence after THAT
// overflowed and was discarded entirely. A parameter slot has to start at
// zero because an omitted parameter IS zero.
static void csi_begin(ansi_parser_t *p)
{
    p->state = ST_CSI;
    p->nparams = 0;
    p->overflow = false;
    p->params[0] = 0;
}

void ansi_reset(ansi_parser_t *p)
{
    p->state = ST_GROUND;
    p->nparams = 0;
    p->overflow = false;
    p->slen = 0;
}

static ansi_action_t nothing(void)
{
    ansi_action_t a = { .kind = ANSI_NOTHING };
    return a;
}

static ansi_action_t print(char byte)
{
    ansi_action_t a = { .kind = ANSI_PRINT, .byte = byte };
    return a;
}

// The parameters as they stand, handed to the caller. A sequence that
// overflowed hands back NOTHING instead: acting on the first sixteen numbers
// of a sequence that meant something else is a guess, and half of a colour
// change is a colour nobody chose.
static ansi_action_t finish(ansi_parser_t *p, ansi_action_kind_t kind)
{
    ansi_action_t a = { .kind = kind };
    if (p->overflow) {
        ansi_reset(p);
        return nothing();
    }
    for (uint8_t i = 0; i < p->nparams; i++)
        a.params[i] = p->params[i];
    a.nparams = p->nparams;
    ansi_reset(p);
    return a;
}

// "#rrggbb" or "#rgb" — xterm accepts more spellings (rgb:RR/GG/BB, colour
// names), and those wait for somebody who writes them. Returns false for
// anything else, which the caller turns into "ignored".
static bool parse_hash_color(const char *s, uint8_t len, uint32_t *out)
{
    if (len != 7 && len != 4)
        return false;
    if (s[0] != '#')
        return false;

    uint32_t v = 0;
    for (uint8_t i = 1; i < len; i++) {
        uint32_t d;
        char c = s[i];
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A') + 10;
        else return false;
        v = (v << 4) | d;
    }
    if (len == 4) {
        // #rgb is #rrggbb with each digit doubled — 0xf0f becomes 0xff00ff,
        // which is what the short form has meant since it was invented.
        uint32_t r = (v >> 8) & 0xf, g = (v >> 4) & 0xf, b = v & 0xf;
        v = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    }
    *out = v;
    return true;
}

// END OF AN OSC STRING, whichever of the two terminators arrived. ONE
// spelling, because both doors lead here and two copies of "is this the
// background sequence" would be two rules that could drift apart — the
// mistake this project has already paid for once, in a header parser that
// judged a field name by two different rules and let the malformed form
// through.
//
// Only OSC 11 is read. Its payload arrives as `11;#rrggbb`, so the number
// and its semicolon are matched here rather than by the parameter machinery
// above, which belongs to CSI.
static ansi_action_t finish_osc(ansi_parser_t *p)
{
    ansi_action_t a = { .kind = ANSI_NOTHING };
    bool usable = !p->overflow && p->slen > 3 &&
                  p->string[0] == '1' && p->string[1] == '1' && p->string[2] == ';';
    uint32_t color = 0;
    if (usable && parse_hash_color(p->string + 3, (uint8_t)(p->slen - 3), &color)) {
        a.kind = ANSI_GLASS_BG;
        a.color = color;
    }
    ansi_reset(p);
    return a;
}

// A C0 control byte arriving mid-sequence is EXECUTED, not swallowed. A
// program that writes a newline while a sequence is half-arrived (a crash
// between two printfs, a pipe that cut) would otherwise have its newline
// eaten, and every line after it would land on the wrong row forever. The
// sequence is abandoned; the control byte does its ordinary job. ESC is not
// in this set — an ESC mid-sequence starts a NEW one, which is how a
// truncated sequence recovers.
static bool is_c0_to_execute(char byte)
{
    unsigned char c = (unsigned char)byte;
    return c < 0x20 && c != 0x1B;
}

ansi_action_t ansi_feed(ansi_parser_t *p, char byte)
{
    unsigned char c = (unsigned char)byte;

    if (p->state != ST_GROUND && p->state != ST_OSC && p->state != ST_OSC_ESC &&
        is_c0_to_execute(byte)) {
        ansi_reset(p);
        return print(byte);
    }

    switch (p->state) {
    case ST_GROUND:
        if (c == 0x1B) {
            ansi_reset(p);
            p->state = ST_ESC;
            return nothing();
        }
        return print(byte);

    case ST_ESC:
        if (c == '[') { csi_begin(p); return nothing(); }
        if (c == ']') { p->state = ST_OSC; p->slen = 0; p->overflow = false; return nothing(); }
        if (c == 0x1B) return nothing();   // ESC ESC: the second one starts over
        // An INTERMEDIATE byte means the escape is longer than two: `ESC ( B`
        // selects a character set, and ncurses sends it at startup. Eating
        // only the '(' would leave the 'B' to land on the screen as a letter
        // nobody typed — which is precisely what happened before this state
        // existed.
        if (c >= 0x20 && c <= 0x2f) { p->state = ST_ESC_INT; return nothing(); }
        // Every other two-byte escape (`ESC 7`, `ESC c` …) is consumed and
        // ignored: none has a consumer here, and printing the letter would
        // put a stray 'c' on the screen.
        ansi_reset(p);
        return nothing();

    case ST_ESC_INT:
        // Intermediates may repeat; the first byte outside their range ends
        // the sequence, whatever it was.
        if (c >= 0x20 && c <= 0x2f)
            return nothing();
        if (c == 0x1B) { p->state = ST_ESC; return nothing(); }
        ansi_reset(p);
        return nothing();

    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (p->nparams == 0)
                p->nparams = 1;                 // the first digit opens param 0
            // nparams is capped where it is incremented, so this index is
            // always inside the array. The ARITHMETIC is what needs care:
            // accumulate in 32 bits and judge the result, because
            // `6553 * 10 + 9` wraps a uint16_t to 3 and would turn an absurd
            // parameter into a plausible one.
            uint32_t value = (uint32_t)p->params[p->nparams - 1] * 10 + (uint32_t)(c - '0');
            if (value > 65535)
                p->overflow = true;
            else
                p->params[p->nparams - 1] = (uint16_t)value;
            return nothing();
        }
        if (c == ';') {
            // A SEMICOLON CLOSES ONE PARAMETER AND OPENS THE NEXT, which
            // means a leading one opens two: `ESC[;31m` is "parameter zero,
            // then 31", not "31". Reading it as one parameter made a
            // sequence mean something its sender did not say.
            if (p->nparams == 0)
                p->nparams = 1;                 // the empty first parameter
            if (p->nparams >= ANSI_PARAM_MAX)
                p->overflow = true;
            else
                p->params[p->nparams++] = 0;    // an omitted parameter is zero
            return nothing();
        }
        if (c == 0x1B) {
            // A fresh ESC abandons this sequence and starts another. It is
            // how a truncated sequence recovers — without it, the bytes of
            // the NEXT sequence were read as this one's final byte and its
            // parameters landed on the screen as text.
            p->state = ST_ESC;
            return nothing();
        }
        // EVERY OTHER PARAMETER OR INTERMEDIATE BYTE IS SWALLOWED, and the
        // ranges are ECMA-48's rather than a list of the ones seen so far:
        // 0x30-0x3F is the whole parameter space and 0x20-0x2F the
        // intermediates, with a sequence ending at the first byte from 0x40
        // up. Getting that boundary wrong does not merely misread a
        // sequence — it drops out of the sequence early and PRINTS the rest
        // on the screen.
        //
        // The byte that proved it was the colon. `ESC[38:2::255:0:0m` is how
        // a modern program asks for a true colour, and ':' had fallen
        // through to the final-byte switch below, ending the sequence at
        // the colon and spraying `2::255:0:0m` across the terminal. Marking
        // the sequence unusable is right — os64 acts on none of these — but
        // it has to be consumed WHOLE.
        if ((c >= 0x30 && c <= 0x3f) || (c >= 0x20 && c <= 0x2f)) {
            p->overflow = true;
            return nothing();
        }
        switch (c) {
        case 'm': return finish(p, ANSI_SGR);
        case 'H':
        case 'f': return finish(p, ANSI_CURSOR_POS);
        case 'J': return finish(p, ANSI_ERASE_DISPLAY);
        case 'K': return finish(p, ANSI_ERASE_LINE);
        default:
            // A final byte with no meaning here. Consumed, not printed.
            ansi_reset(p);
            return nothing();
        }

    case ST_OSC:
        if (c == 0x07)                          // BEL ends the string
            return finish_osc(p);
        if (c == 0x1B) { p->state = ST_OSC_ESC; return nothing(); }
        if (p->slen < ANSI_STRING_MAX)
        {
            p->string[p->slen++] = byte;
            return nothing();
        }
        // PAST THE BUFFER, THE BYTES ARE COUNTED RATHER THAN KEPT — and past
        // the COUNT, the sequence is abandoned and the terminal comes back.
        //
        // An OSC ends at a terminator that may never arrive: a program that
        // forgets its BEL, or a prompt whose `\a` was written in a shell
        // vocabulary that has no `\a` and came out as two literal
        // characters. Waiting forever for it means every byte the machine
        // prints after that disappears, which is a terminal somebody has to
        // reboot to get back. Bounded, the cost of a malformed sequence is a
        // couple of hundred bytes of output rather than all of it.
        if (p->slen < 255)
        {
            p->slen++;
            p->overflow = true;
            return nothing();
        }
        ansi_reset(p);
        return nothing();

    case ST_OSC_ESC:
        // ESC '\' is the other legal terminator (ST). Anything else after an
        // ESC means the string was abandoned mid-flight; the ESC starts a
        // fresh sequence rather than being swallowed.
        if (c == '\\')
            return finish_osc(p);
        ansi_reset(p);
        p->state = ST_ESC;
        return ansi_feed(p, byte);

    default:
        ansi_reset(p);
        return print(byte);
    }
}
