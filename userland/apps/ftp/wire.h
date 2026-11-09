#ifndef FTP_WIRE_H
#define FTP_WIRE_H

// wire.h — the part of RFC 959 (1985) that is text, and none of the part
// that is a socket.
//
// FTP IS TWO CONNECTIONS AND THAT IS THE WHOLE DESIGN. A control connection
// carries commands and replies for the life of the session; a fresh DATA
// connection carries the bytes of exactly one transfer and then closes,
// because its close is how the end of the transfer is spelled — there is no
// length field anywhere in the protocol. Everything awkward about FTP falls
// out of that one decision, including the two ways a client can hang.
//
// This file speaks the control channel only. The data channel has no
// grammar: it is bytes until close, and the client reads it with the same
// os64_read every other handle takes.
//
// THE CONTROL CHANNEL IS SPECIFIED AS A TELNET CONNECTION. RFC 959 §2.3 says
// so outright, which is why commands end in CRLF and why the spec's own
// interrupt sequence is written in IAC codes. Nothing here interprets IAC: no
// server has negotiated an option at a client in decades, and a stray 0xFF in
// a reply line is passed through as the byte it is. FTP.md carries the
// argument and names the engine next door that would answer if one ever did.
//
// WHY THE PARSING IS SEPARATE FROM THE SOCKET, the lesson http.h paid for and
// gopher's wire.h paid again: every reader below takes its bytes from an
// `ftp_source_fn`, never from a handle. On os64 that is one os64_read_for of
// a dialed connection; in tools/test_ftp_host.c it is a memory buffer handing
// out one byte at a time, then two, then seventeen — because a stream
// parser's bugs live where a token straddles two reads, and a multiline reply
// is a token that can straddle a hundred of them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Sizes ───────────────────────────────────────────────────────────────

// A reply line. RFC 959 states no limit; RFC 1123 §4.1.2.8 asks a client to
// accept "long" ones without saying how long. This is the length past which a
// line is somebody's banner art rather than an answer, and an over-long line
// is TRUNCATED and its tail consumed rather than refused — losing the tail of
// one line of a welcome message is a better answer than losing the session.
#define FTP_LINE_MAX      1024

// The assembled text of one reply, lines joined by '\n'. A banner that
// outgrows it is kept to the cap and flagged; the reply is still read to its
// end, because a reply half-consumed leaves the channel answering every later
// command one reply late.
#define FTP_REPLY_MAX     4096

// The read buffer. One MSS of control traffic is generous for a channel whose
// busiest moment is a welcome message.
#define FTP_BUF_SIZE      2048

// How many bytes one reply may CONSUME before the channel is declared
// unusable. Distinct from FTP_REPLY_MAX, which bounds what is kept: a
// multiline reply that never sends its closing line is not a long reply, it
// is a peer that has stopped speaking the protocol, and the only sound answer
// is to stop trusting the connection. FTP_REPLY_TOO_LONG says exactly that.
#define FTP_REPLY_CONSUME_MAX  65536u

// A path in a command or a 257 reply.
#define FTP_PATH_MAX      1024

// A command as it goes on the wire: verb, space, argument, CRLF.
#define FTP_COMMAND_MAX   (FTP_PATH_MAX + 32)

// ── The control channel ─────────────────────────────────────────────────

// A source's answer when its DEADLINE expired with no bytes: "nothing yet,
// and the channel is fine". It is not an error and must not be reported as
// one — a deadline is the caller's own choice and says nothing about the
// connection, and a caller that reads a broken channel and a quiet one the
// same way cannot ask "is anything still coming?" without breaking what it
// asked about. A source with no deadline never returns it.
#define FTP_SOURCE_STALLED  ((int64_t)-2)

// Bytes from somewhere. Returns >0 read, 0 at end of input,
// FTP_SOURCE_STALLED for a deadline, and any other negative for an error —
// os64_read_for's contract with its timeout given a name.
typedef int64_t (*ftp_source_fn)(void *ctx, void *buf, size_t cap);

typedef struct {
    ftp_source_fn source;
    void         *ctx;
    char          buf[FTP_BUF_SIZE];
    size_t        len;
    size_t        pos;
    bool          eof;       // the source is finished
    bool          failed;    // the source returned an error
} ftp_control_t;

void ftp_control_init(ftp_control_t *c, ftp_source_fn source, void *ctx);

// True when the channel has nothing buffered and its source is finished — the
// question "is there another reply coming?" asked without blocking on it.
bool ftp_control_drained(const ftp_control_t *c);

// ── A reply ─────────────────────────────────────────────────────────────
//
// THE FORMAT IS TWO FORMATS AND THE SECOND ONE IS THE TRAP.
//
//   200 Command okay                 one line: NNN, a SPACE, text
//
//   220-Welcome to the server        several: NNN, a HYPHEN, text
//   220-  it has things to say       ...anything at all, including a line
//   220 Ready.                       that looks like a reply...
//                                    until NNN, a SPACE — the SAME NNN.
//
// A client that scans for "three digits then a space" is fooled the first
// time a banner contains a line beginning `220 `, and it then reads the rest
// of that banner as replies to commands it has not sent yet. RFC 959 §4.2
// tells servers not to write such a line — which is not the same as servers
// not writing one. So the opening code is remembered and only that code, with
// a space, closes the reply. An intermediate line is text, whatever it looks
// like.
//
// A CODE THE CLIENT CANNOT PARSE IS NOT A DISAGREEMENT, IT IS A DESYNC. The
// reader has no way to resynchronise a stream of unframed text, so a
// malformed first line is reported as such and the caller's only sound move
// is to drop the connection.

// `text` IS WHAT THE SERVER SENT, code digits and all, with the lines joined
// by '\n' and the terminators gone. Nothing is stripped, for two reasons: it
// is what an FTP client has always printed — the `220-` down the left of a
// welcome message is part of the picture — and a reply carrying a payload
// (the 227 tuple, the 257 path) is then handed to its parser exactly as it
// arrived, with nothing removed on its way there.
typedef struct {
    int    code;                  // 100..599; 0 when there is no reply
    char   text[FTP_REPLY_MAX];
    size_t len;
    size_t lines;
    bool   truncated;             // the text outgrew FTP_REPLY_MAX
} ftp_reply_t;

typedef enum {
    FTP_REPLY_OK = 0,
    FTP_REPLY_END,         // the channel closed with no reply pending
    FTP_REPLY_FAILED,      // the source errored (see the control's `failed`)
    FTP_REPLY_MALFORMED,   // the first line is not `NNN` + space or hyphen
    FTP_REPLY_TOO_LONG,    // FTP_REPLY_CONSUME_MAX passed with no close
    // The source's deadline expired BEFORE a reply began. The channel is
    // untouched and may be read again, which is what makes "is anything else
    // coming?" a question a caller can ask. A deadline that expires PART WAY
    // through a reply is FTP_REPLY_FAILED instead, and not as a technicality:
    // half a reply has been consumed, the next read would start in the middle
    // of it, and every answer after that would belong to the command before.
    FTP_REPLY_STALLED,
} ftp_reply_result_t;

// Read exactly one reply, single-line or multiline, into *out.
ftp_reply_result_t ftp_reply_read(ftp_control_t *c, ftp_reply_t *out);

// The refusal in words — os64_dial_reason's shape and for its reason, so a
// program can say something a person can act on without a debugger.
const char *ftp_reply_reason(ftp_reply_result_t rc);

// ── What a code MEANS ───────────────────────────────────────────────────
//
// RFC 959 §4.2 gives the first digit the verdict and the second the subject,
// and a client that reads the first digit needs to know almost nothing else:
//
//   1yz  preliminary — the work has begun, a second reply follows
//   2yz  complete    — done, ask the next thing
//   3yz  intermediate — accepted, send the rest (USER wanting a PASS)
//   4yz  transient   — no, but the same command may work later
//   5yz  permanent   — no, and asking again the same way will not help
//
// The 4/5 split is the one worth honouring: it is the difference between a
// server that is busy and a file that does not exist.

static inline bool ftp_code_preliminary(int code)  { return code >= 100 && code < 200; }
static inline bool ftp_code_complete(int code)     { return code >= 200 && code < 300; }
static inline bool ftp_code_intermediate(int code) { return code >= 300 && code < 400; }
static inline bool ftp_code_transient(int code)    { return code >= 400 && code < 500; }
static inline bool ftp_code_permanent(int code)    { return code >= 500 && code < 600; }

// 1yz, 2yz and 3yz all mean "so far so good"; the rest mean no.
static inline bool ftp_code_positive(int code)     { return code >= 100 && code < 400; }

// ── Building a command ──────────────────────────────────────────────────

typedef enum {
    FTP_COMMAND_OK = 0,
    FTP_COMMAND_TOO_LONG,
    // The argument carries a CR or an LF. THIS IS THE INJECTION DOOR: the
    // control channel frames commands by line ending and nothing else, so a
    // file name holding a CRLF is a second command the person never typed —
    // and file names arrive from a LIST, which is a stranger's bytes. Refused
    // by name rather than stripped, because a caller that meant it deserves
    // to hear that the protocol cannot carry it.
    FTP_COMMAND_NEWLINE,
} ftp_command_result_t;

// There is no refusal here for a NUL in the argument, and the reason is worth
// stating rather than leaving as an absence: `arg` is a C string, so a name
// carrying a zero byte lost its tail before this function was reached. The
// place that would have to catch one is a client that builds names out of a
// server's LIST output, and this one prints that output rather than parsing
// it (FTP.md ruling 4).

// Write `VERB[ arg]\r\n` into `out`, NUL-terminated, with *written set to the
// wire length. CRLF and not a bare LF: 1971 protocols mean it, and a server
// written to the letter of the spec is entitled to wait for the second byte.
// `arg` may be NULL or empty for a verb that takes none.
ftp_command_result_t ftp_command(const char *verb, const char *arg,
                                 char *out, size_t cap, size_t *written);

const char *ftp_command_reason(ftp_command_result_t rc);

// ── The 227 reply: where the data connection is ─────────────────────────
//
//   227 Entering Passive Mode (10,0,2,2,195,80)
//
// Four address bytes and two port bytes, as decimal text, because the reply
// had to stay legible to a person reading a Telnet session. The text AROUND
// the tuple is not specified, and servers have used parentheses, an `=`, and
// plain prose either side.
//
// So: read every comma-separated number list in the line WHOLE, keep the ones
// that are exactly six numbers each fitting a byte, prefer one that opens
// with '(' when there is one, and otherwise take the FIRST — which is what
// Python's ftplib and curl both do, and matching them is what lets the host
// harness diff against a reference instead of against an opinion.
//
// WHOLE is the load-bearing word. Taking six numbers and stopping lets a
// seven-number list match its own tail — the scan resumes inside it and
// `10,0,2,2,195,80,7` yields the address `0.2.2.195`, which is a different
// machine. A list that is not exactly six long is not an address, and a reply
// with no list at all is refused rather than dialed on a guess.
//
// WHAT THE ADDRESS IS FOR IS THE CALLER'S PROBLEM AND IT IS A REAL ONE. A
// server behind its own NAT advertises an address that means nothing here,
// and a hostile one can name a third party to make this machine dial it. The
// client dials the PORT at the address it is already talking to; this
// function reports what the server said and lets the caller notice the
// difference. FTP.md § The 227 reply carries the argument.

typedef enum {
    FTP_PASV_OK = 0,
    FTP_PASV_NO_TUPLE,   // nothing in the line is six numbers
    FTP_PASV_RANGE,      // six numbers, but one of them will not fit a byte
    FTP_PASV_PORT_ZERO,  // p1 and p2 are both zero; there is nothing to dial
} ftp_pasv_result_t;

// `ip` receives the advertised address in HOST order, high octet first — the
// same order os64_parse_ipv4 produces, so the two can be compared directly.
ftp_pasv_result_t ftp_pasv_parse(const char *text, uint32_t *ip, uint16_t *port);

const char *ftp_pasv_reason(ftp_pasv_result_t rc);

// ── The 257 reply: a path, in quotes ────────────────────────────────────
//
//   257 "/pub/archives" is the current directory
//
// The quoting rule is the one thing about this reply that is specified and
// the one thing clients forget: a double quote INSIDE the path is written
// TWICE. `257 "/a""b"` is the single path `/a"b`, not two paths and not a
// path ending at the third quote. A client that stops at the first closing
// quote reports the wrong directory to the person standing in it.
//
// Returns false when the reply carries no quoted path at all, which is legal:
// RFC 959 shows 257 with a quoted path but servers answer MKD with prose, and
// a client that treats prose as a path creates the wrong directory next.
bool ftp_path_from_257(const char *text, char *out, size_t cap);

#endif // FTP_WIRE_H
