#ifndef TELNET_PROTOCOL_H
#define TELNET_PROTOCOL_H

// telnet_protocol.h — RFC 854's Network Virtual Terminal, the three options a
// login session actually needs, and RFC 1143's cure for the negotiation loop.
// Bytes in, bytes out; no syscalls in this file or its .c.
//
// TELNET IS OLDER THAN TCP. It was specified in 1971 (RFC 97, then 137/139)
// and spoken over NCP; TCP was not described until 1974 and the ARPANET did
// not switch to it until 1 January 1983 — four months before RFC 854, the
// re-specification of telnet for the new stack, which is the document this
// file implements. That is why the protocol assumes so little: it was written
// for a network of machines that agreed on almost nothing, and its whole
// design is a NEGOTIATION between two ends that each start out refusing.
//
// THE PROTOCOL IS THREE SENTENCES. Both ends pretend to be talking to a
// Network Virtual Terminal — a half-duplex printer with a keyboard, whose end
// of line is CR LF. Byte 255 (IAC, "interpret as command") escapes everything
// that is not data: doubled it means a literal 255, and followed by a command
// byte it means a command. Options are asked for and answered with WILL/WONT
// (about the sender) and DO/DONT (about the receiver), and a subnegotiation
// carries an option's parameters between IAC SB and IAC SE.
//
// WHY THE PARSER IS SEPARATE FROM THE SOCKET, the lesson http.h and wire.h
// both paid for: everything here takes bytes from the caller and hands bytes
// back. On os64 those come from one os64_read_for of a dialed connection; in
// tools/test_telnet_host.sh they come from a memory buffer handing out one
// byte at a time, then two, then seventeen — because a stream parser's bugs
// live where a token straddles two reads, and a parser only a network can
// drive is a parser nobody drives across that boundary.
//
// THE DATA IS PASSED THROUGH VERBATIM, which is the opposite of what
// /bin/gopher does to a menu line, and the difference is worth stating. A
// gopher item is a data structure whose fields get DRAWN, so an escape
// sequence hiding in one is a stranger repainting your screen. A telnet
// stream IS the terminal session: the escapes are the remote program talking
// to your terminal, and refusing them refuses the program. What protects you
// here is that os64's terminal obeys a short, known list of sequences and
// consumes the rest (ansi.c) — not anything this file removes.
//
// ── The option scope, and why it is not "refuse everything" ─────────────
//
// A client that answers every WILL with DONT is easy to write and wrong in
// two ways a person notices immediately. It prints your password on the
// glass, because a Unix login turns echoing off by taking ECHO for itself and
// a refuse-all client keeps echoing locally. And it collects GO-AHEAD bytes
// from any half-duplex server, because GA is exactly what SUPPRESS-GO-AHEAD
// exists to switch off. So three options are in, and they are the floor
// rather than a widening:
//
//   ECHO (RFC 857)  — the PEER's, accepted. When the server echoes, we stop.
//   SGA  (RFC 858)  — both directions, so neither end waits for a turn.
//   NAWS (RFC 1073) — ours, offered. Terminals stopped being 24x80 in 1988
//                     and the option exists because full-screen programs
//                     needed to know.
//
// Everything else is refused, TERMINAL-TYPE included: os64's terminal reads
// SGR, CUP, ED, EL and OSC 11 and consumes the rest, so the honest name to
// offer is `ANSI` and not a VT100 this renderer cannot be. Binary mode,
// environment exchange, authentication and encryption are out. Telnet traffic
// is plaintext, and always was.
//
// ── RFC 1143, and why option state is six values and not a bool ─────────
//
// If both ends want an option and both ask at once, the naive implementation
// answers each request with an acknowledgement, reads the other end's
// acknowledgement as a fresh request, and the two of them trade WILL and DO
// forever. D. J. Bernstein wrote RFC 1143 in 1990 to end that, and its answer
// is the Q Method: each option gets TWO state machines (what WE do, what HE
// does), each with NO / YES / WANTNO / WANTYES plus a one-slot queue for a
// change of mind that arrives mid-negotiation. A request is answered only
// when it CHANGES something, so the trade cannot start. That is the whole
// reason telnet_option_state_t has six values instead of two.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── The bytes (RFC 854) ─────────────────────────────────────────────────

#define TELNET_IAC   255   // interpret as command — the escape
#define TELNET_DONT  254
#define TELNET_DO    253
#define TELNET_WONT  252
#define TELNET_WILL  251
#define TELNET_SB    250   // subnegotiation begins
#define TELNET_GA    249   // go ahead — your turn to transmit (half duplex)
#define TELNET_EL    248   // erase line
#define TELNET_EC    247   // erase character
#define TELNET_AYT   246   // are you there?
#define TELNET_AO    245   // abort output
#define TELNET_IP    244   // interrupt process — what Ctrl+C becomes
#define TELNET_BRK   243   // break
#define TELNET_DM    242   // data mark (the Synch's marker)
#define TELNET_NOP   241
#define TELNET_SE    240   // subnegotiation ends

// ── The options ─────────────────────────────────────────────────────────
//
// The three that are negotiated, plus the ones common enough that refusing
// them BY NAME is worth more in a status line than refusing "option 24".

#define TELNET_OPT_BINARY    0    // RFC 856
#define TELNET_OPT_ECHO      1    // RFC 857
#define TELNET_OPT_SGA       3    // RFC 858
#define TELNET_OPT_STATUS    5    // RFC 859
#define TELNET_OPT_TIMING    6    // RFC 860 — timing mark
#define TELNET_OPT_TTYPE     24   // RFC 1091
#define TELNET_OPT_EOR       25   // RFC 885 — end of record
#define TELNET_OPT_NAWS      31   // RFC 1073 — window size
#define TELNET_OPT_TSPEED    32   // RFC 1079
#define TELNET_OPT_LFLOW     33   // RFC 1372 — remote flow control
#define TELNET_OPT_LINEMODE  34   // RFC 1184
#define TELNET_OPT_XDISPLOC  35   // RFC 1096
#define TELNET_OPT_ENVIRON   39   // RFC 1572 — new environment

// ── Sizes ───────────────────────────────────────────────────────────────

// The outbound queue holds negotiation replies AND typed data IN PROTOCOL
// ORDER, which is why there is one queue and not two: a WONT that overtakes
// the keystrokes it was interleaved with is a different conversation than the
// one that actually happened.
#define TELNET_OUT_MAX    4096

// The most one arriving byte can add to the outbound queue: an option reply
// is IAC + verb + option. telnet_receive keeps this much room free so it can
// always answer the byte it is about to read.
#define TELNET_REPLY_MAX  3

// ── State ───────────────────────────────────────────────────────────────

// RFC 1143's six. WANTNO_OPPOSITE means "we asked him to stop and changed our
// mind before he answered" — the queued request that makes the method
// loop-free.
typedef enum {
    TELNET_OPT_NO = 0,
    TELNET_OPT_YES,
    TELNET_OPT_WANTNO,
    TELNET_OPT_WANTNO_OPPOSITE,
    TELNET_OPT_WANTYES,
    TELNET_OPT_WANTYES_OPPOSITE,
} telnet_option_state_t;

// What the client does with what you type before the server has an opinion.
// AUTO is the answer for every peer that negotiates: echo locally until the
// server takes ECHO, then stop, so a password prompt is dark. ON and OFF are
// the manual override for a peer that does neither.
typedef enum {
    TELNET_ECHO_AUTO = 0,
    TELNET_ECHO_ON,
    TELNET_ECHO_OFF,
} telnet_echo_mode_t;

// ── Notices ─────────────────────────────────────────────────────────────
//
// What the caller has to ACT on, as opposed to what it can read whenever it
// likes. Latched by the engine, read and cleared by telnet_notices.

#define TELNET_NOTE_ECHO       (1u << 0)  // local echo policy just changed
#define TELNET_NOTE_SIZE       (1u << 1)  // the peer takes NAWS now — send it
#define TELNET_NOTE_AYT        (1u << 2)  // the peer asked "are you there?"
#define TELNET_NOTE_CONTRADICT (1u << 3)  // RFC 1143 caught an answer that
                                          //  contradicts what was asked (a
                                          //  DONT answered by WILL). Not
                                          //  fatal: the method resolves it,
                                          //  and this is how it says so

typedef struct {
    // The parse state machine, and the byte it is waiting on. A
    // subnegotiation has no buffer here: nothing this client offers has
    // parameters to RECEIVE, so one is read only in order to be thrown away.
    uint8_t  parse;
    uint8_t  verb;            // WILL/WONT/DO/DONT awaiting its option byte

    // Outbound, a CR is only half a decision: the LF that may or may not
    // follow it belongs to the same newline, and can arrive in the next call.
    // Inbound needs no such state — see the NUL note in the .c.
    bool     cr_pending_out;

    // RFC 1143's two machines per option, indexed by the option byte. A full
    // 256 entries because a peer may name any option and every one of them
    // needs an answer that does not loop; 512 bytes is cheaper than deciding
    // which options deserve to be remembered.
    uint8_t  us[256];
    uint8_t  him[256];

    uint8_t  out[TELNET_OUT_MAX];
    size_t   out_len;

    uint32_t notices;
    uint8_t  echo_mode;

    uint16_t cols, rows;
    bool     size_known;      // telnet_send_size has been told a size

    // What Enter spells on the wire. FALSE — the default, and 4.2BSD's — is
    // CR NUL; TRUE is CR LF. See telnet_set_eol_crlf.
    bool     eol_crlf;
} telnet_t;

// ── Life ────────────────────────────────────────────────────────────────

void telnet_init(telnet_t *t);

// THE CLIENT SPEAKS FIRST, and this is what it says: DO SGA and WILL SGA
// (neither end should wait for a turn) and WILL NAWS (we have a window size
// worth having). ECHO is deliberately not offered — whether the server echoes
// is the server's to announce, at the moment it puts up a password prompt,
// and a client that asked for it would be asking to be echoed AT.
//
// For a fresh engine, where the queue cannot be too full to hold the offers.
// Returns false if it was not.
bool telnet_offer(telnet_t *t);

// ── From the peer ───────────────────────────────────────────────────────

// Decode `len` bytes that arrived from the peer. Application data is appended
// to `data` (up to `cap`, its length written to `*data_len`); replies to
// negotiation are queued for sending.
//
// RETURNS THE NUMBER OF BYTES CONSUMED, which is short when `data` filled or
// when the outbound queue no longer has TELNET_REPLY_MAX free. That is the
// whole back-pressure contract: drain what you got, send what is queued, call
// again with the rest. Nothing is dropped and nothing grows without a bound —
// the queue that saturates stops the reader instead.
size_t telnet_receive(telnet_t *t, const void *in, size_t len,
                      void *data, size_t cap, size_t *data_len);

// True when the input ended in the middle of an IAC sequence — a connection
// cut between IAC and its command byte, or inside a subnegotiation. The
// caller asks at end of input, because "the peer stopped talking mid-word" is
// a different report than "the peer hung up".
bool telnet_mid_sequence(const telnet_t *t);

// ── To the peer ─────────────────────────────────────────────────────────

// Queue what a person typed. IAC is doubled so a 255 in a paste cannot become
// a command, and any of CR, LF or CR LF becomes the NVT end of line, CR LF.
//
// RETURNS THE NUMBER OF BYTES ACCEPTED — short when the queue filled. Never
// half an escape: a byte that needs two is either queued whole or not taken.
size_t telnet_send_text(telnet_t *t, const void *text, size_t len);

// Queue IAC + `command` — TELNET_IP for Ctrl+C, TELNET_AYT, TELNET_BRK. All
// or nothing; false means the queue had no room for the pair.
//
// END OF INPUT IS NOT HERE, and that is a real gap rather than an oversight:
// RFC 854 has no EOF command (the one in RFC 1184 exists only inside
// LINEMODE, which this client refuses), so the byte a Unix login logs out on
// is an ordinary 0x04 sent as DATA. os64's console turns a typed Ctrl+D into
// end-of-input before any program sees it, which is why the client offers to
// send that byte from its own prompt instead.
bool telnet_send_command(telnet_t *t, uint8_t command);

// Queue our window size (RFC 1073) if the peer has agreed to NAWS. The size
// is remembered either way, so a later agreement can send a size we already
// know. Returns true when bytes were queued.
//
// A dimension of 255 has to be doubled INSIDE the subnegotiation like any
// other IAC, which is the bug this option is famous for: without it a 255
// silently ends the subnegotiation early and the bytes after it are read as
// commands.
bool telnet_send_size(telnet_t *t, uint16_t cols, uint16_t rows);

// The bytes waiting to go to the peer, and how many of them were sent. The
// caller writes what it can and reports back; a partial write is ordinary.
const uint8_t *telnet_pending(const telnet_t *t, size_t *len);
void telnet_sent(telnet_t *t, size_t n);

// ── What the caller asks about ──────────────────────────────────────────

// Should this client echo what is typed? The answer under TELNET_ECHO_AUTO is
// "unless the server is doing it".
bool telnet_local_echo(const telnet_t *t);
void telnet_set_echo_mode(telnet_t *t, telnet_echo_mode_t mode);
telnet_echo_mode_t telnet_echo_mode(const telnet_t *t);

// WHAT ENTER SPELLS ON THE WIRE, and it is CR NUL until somebody says
// otherwise — 4.2BSD's `toggle crlf`, whose initial value is likewise false.
//
// RFC 854 gives a keyboard's Return two spellings and they are not
// interchangeable. CR LF is a NEW LINE: end this line, start another. CR NUL
// is a CARRIAGE RETURN with the NUL there only so the CR is not the last byte
// of the record — the NVT's way of saying "the key was pressed" and nothing
// more about what the receiver should do with it.
//
// The difference is invisible against a Unix login, which strips the NUL,
// hands its pty a CR, and lets ICRNL make a newline of it — and decisive
// against a program that reads a LINE and then reads a KEY. That program
// takes the CR as the end of its line and finds the LF still waiting when it
// asks for the next keystroke, so it answers its own next question with a
// byte the person never typed. A DOS door does this constantly; it is what
// makes CR LF unable to get past a name-then-confirm prompt at all.
void telnet_set_eol_crlf(telnet_t *t, bool crlf);
bool telnet_eol_crlf(const telnet_t *t);

// Is `option` in force — on OUR side of the wire, or on his? What `status`
// prints, and what the echo and window-size decisions are made of.
bool telnet_option_ours(const telnet_t *t, uint8_t option);
bool telnet_option_his(const telnet_t *t, uint8_t option);

// Read and clear the TELNET_NOTE_* bits.
uint32_t telnet_notices(telnet_t *t);

// Names, for a status line and for a log that says which option was refused.
// Neither returns NULL: an option nobody named is rendered "option NNN" into
// a small rotating set of buffers, so two can be printed in one call.
const char *telnet_option_name(uint8_t option);
const char *telnet_command_name(uint8_t command);

#endif // TELNET_PROTOCOL_H
