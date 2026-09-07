#ifndef OS64_KERNEL_ANSI_H
#define OS64_KERNEL_ANSI_H

// ansi.h — the escape-sequence reader for os64's terminals.
//
// WHAT IT IS. A byte at a time in, one ACTION out. It holds no terminal
// state, touches no lock, calls nothing: `ansi_feed` is a state machine over
// bytes, and tty.c is what decides that a CUP means "move this terminal's
// cursor". That split is not tidiness — it is what lets the whole parser be
// driven on the HOST at every chunk size from one byte up
// (tools/test_ansi_host.sh), which matters because a sequence arrives in
// whatever pieces write() was called with. `printf("\033[31mred")` can put
// the ESC in one write and the '[' in the next, and a parser that only ever
// sees whole sequences is a parser nobody has tested at the seam.
//
// WHAT IT READS, and why the list is short: an escape is implemented when
// something asks for it (Chris's ruling, 2026-09-03). The gopher browser
// asked for colour, absolute positioning and the two erases; ANSI ART asked
// for the relative moves, the saved cursor and a character set for the high
// half. Scroll regions, insert/delete
// line, the alternate screen, DEC private modes and 256-colour wait for the
// consumer that names them — and each one that never arrives is a sequence
// that cannot mislead a terminal into obeying a stranger's page.
//
//   ESC [ <n> m           SGR: colours and attributes
//   ESC [ <r> ; <c> H     CUP: put the cursor there (also 'f')
//   ESC [ <n> A B C D     CUU/CUD/CUF/CUB: move it from where it is
//   ESC [ s     ESC [ u   SCP/RCP: remember where it is, put it back
//   ESC [ <n> J           ED:  erase display
//   ESC [ <n> K           EL:  erase line
//   ESC ] 11 ; <colour> BEL   OSC 11: the terminal's own background
//   ESC ( U     ESC ( B   which set a byte over 0x7F draws as
//
// THE RELATIVE MOVES AND THE SAVED CURSOR ARRIVED TOGETHER, and they had to.
// A terminal asked for its height by saving the cursor, driving it down 255
// lines, asking where it ended up and restoring — so implementing the move
// without the restore is WORSE than implementing neither: ignored, the probe
// costs nothing; obeyed halfway, the cursor is driven to the bottom of the
// screen and left there, and everything the program draws next starts from
// the wrong place. A half-answered question is not a smaller answer.
//
// WHAT IT DOES WITH EVERYTHING ELSE: consumes it and says nothing. Not
// because silence is a virtue — this house prefers tripwires — but because
// the alternatives are worse in a way that is easy to check. A terminal that
// PRINTS the bytes of a sequence it does not know spills `[38;5;208m` across
// somebody's screen; one that LOGS them floods the log the first time a
// program with better taste in terminals runs. Ignoring an unknown sequence
// is what every terminal does, and it is the only choice that leaves the
// screen readable.

#include <stdbool.h>
#include <stdint.h>

#define ANSI_PARAM_MAX  16    // `ESC[0;1;4;7;31;44m` is six; sixteen is room
#define ANSI_STRING_MAX 64    // an OSC 11 payload: "#rrggbb" and slack

typedef enum {
    ANSI_NOTHING = 0,   // the byte belonged to a sequence; nothing to do yet
    ANSI_PRINT,         // an ordinary byte — `byte` holds it
    ANSI_SGR,           // set graphic rendition (params)
    ANSI_CURSOR_POS,    // CUP: params[0] row, params[1] col, 1-based
    ANSI_CURSOR_MOVE,   // CUU/CUD/CUF/CUB: `drow`/`dcol` say how far, signed
    ANSI_CURSOR_SAVE,   // SCP: remember where the cursor is
    ANSI_CURSOR_RESTORE,// RCP: put it back where it was remembered
    ANSI_ERASE_DISPLAY, // ED
    ANSI_ERASE_LINE,    // EL
    ANSI_GLASS_BG,      // OSC 11: `color` holds an XRGB
    ANSI_CHARSET,       // ESC ( U / ESC ( B: params[0] is an OS64_CHARSET_*
} ansi_action_kind_t;

typedef struct {
    ansi_action_kind_t kind;
    char               byte;                    // ANSI_PRINT
    uint16_t           params[ANSI_PARAM_MAX];
    uint8_t            nparams;
    uint32_t           color;                   // ANSI_GLASS_BG
    // ANSI_CURSOR_MOVE, already signed and already defaulted: which of the
    // four letters it was is escape-sequence knowledge, so it is spent here
    // rather than handed to a terminal to decode a second time. Wide enough
    // for any count a sequence can carry, because clamping to the screen is
    // the terminal's job and it needs the real number to clamp.
    int32_t            drow, dcol;
} ansi_action_t;

// Zero is a valid, idle parser — a tty gets one by being zeroed at init, and
// nothing has to remember to call an initialiser.
typedef struct {
    uint8_t  state;
    uint8_t  nparams;
    // Which intermediate opened an `ESC <int> <final>` sequence. `ESC ( U`
    // and `ESC ) U` name different character-set slots, so the byte has to
    // survive until the final one arrives.
    uint8_t  inter;
    bool     overflow;                    // this sequence outgrew us: discard it
    uint16_t params[ANSI_PARAM_MAX];
    uint8_t  slen;
    char     string[ANSI_STRING_MAX];     // OSC payload
} ansi_parser_t;

// One byte in, one action out. Never fails: a byte is printed, consumed, or
// turned into an action.
ansi_action_t ansi_feed(ansi_parser_t *p, char byte);

// Is the parser mid-sequence? State belongs to the byte stream and survives
// tty resize; completed actions are applied against the current geometry.
static inline bool ansi_in_sequence(const ansi_parser_t *p) { return p->state != 0; }

// Forget a half-arrived sequence.
void ansi_reset(ansi_parser_t *p);

#endif // OS64_KERNEL_ANSI_H
