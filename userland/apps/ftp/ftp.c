// ftp.c — /bin/ftp: a control connection that lasts, and a data connection
// that lasts one transfer.
//
// The protocol is next door in wire.c and touches no syscall; this file is
// the half that does. It dials, logs in, reads a command line, turns it into
// one exchange with the server, and prints what came back.
//
// ── The shape, and why it is not telnet's ───────────────────────────────
//
// telnet needed one loop watching two mouths because either end may speak at
// any moment. FTP's control channel is strictly request-and-answer, and the
// person at the keyboard drives every request — so this reads a line, sends a
// command, and reads a reply, with nothing to interleave. The only thing that
// arrives unbidden is the `421` a server sends when it has been waiting too
// long, and that is discovered on the next command, which is soon enough.
//
// No terminal mode is changed anywhere in this program. The console hands a
// program its keys and paints nothing, so the prompt paints what it reads
// (prompt_line) and the password prompt is the same read with the painting
// off — hiding it takes no mode, because nothing was going to show it.
//
// ── The transfer dance, which is the whole point of the program ─────────
//
// Every transfer is: PASV, read the address out of the reply, DIAL the data
// connection, and only then send RETR / STOR / LIST. In that order, because a
// server is entitled to wait for the data connection before answering, and a
// client that commands first can wait for an answer that is waiting for it.
//
// Then the two channels have to agree. The data connection ending is not the
// transfer succeeding: a server whose disk fills mid-write closes the data
// connection and says `451` on the control channel. So the bytes end at EOF
// and the VERDICT comes from the reply after it, and only a complete reply
// promotes the staging file to the name the person asked for.
//
// ── Where a download lives before it is a file ──────────────────────────
//
// `<local>.part`, renamed over `<local>` when both channels agree. Interrupted
// or refused, the `.part` stays and nothing is published — because a
// truncated download wearing the real name looks exactly like a good one, and
// the person who finds it a week later has no way to tell.

#include "wire.h"

#include "os64/args.h"
#include "os64/date.h"
#include "os64/dial.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/resolve.h"
#include "os64/signal.h"
#include "os64/str.h"

// ── Sizes and constants ─────────────────────────────────────────────────

#define FTP_DEFAULT_PORT   21

#define FTP_HOST_MAX       (OS64_RESOLVE_NAME_MAX + 1)
#define FTP_INPUT_MAX      1024      // one typed line

// 64KB, and the size is load-bearing rather than round: it is the block
// cache's line, so one write is a run the disk takes in a single pass, and
// one read drains a line's worth of whatever the receive ring holds. os64get
// learned this first and wrote it down: at 4KB the file was written a block
// per syscall and the transfer waited on the disk, not the wire.
#define FTP_XFER_BUF       65536
#define FTP_LOCAL_MAX      (FTP_PATH_MAX + 8)   // room for the ".part"

// How long a server may say nothing before the control connection is
// declared gone. Generous, because a server thinking about a large directory
// is still a server that is answering.
#define FTP_REPLY_PATIENCE_MS   60000

// After an interrupted transfer, how long to look for the reply the server
// still owes. Short on purpose: a reply that has not arrived by now stays on
// the owed count and the next command's reply read waits for it properly, so
// the only thing a longer wait buys is a longer pause before the prompt.
#define FTP_DRAIN_PATIENCE_MS   3000

// Exit codes. 0 is a session that ended the way you asked.
#define FTP_OK             0
#define FTP_USAGE          2
#define FTP_FAILED         3

// ── State ───────────────────────────────────────────────────────────────
//
// One session at a time, so this is file scope rather than a struct threaded
// through every function: the signal handler has to reach it, and a handler
// takes only a signal number.

static ftp_control_t s_control;
static int32_t   s_conn = -1;              // the control connection, -1 = none
static bool      s_loggedIn;

static char      s_host[FTP_HOST_MAX];     // what was typed
static char      s_peerText[16];           // what it resolved to, for dialing
static uint32_t  s_peerIp;
static uint16_t  s_port;

// The deadline the control source reads with. A variable rather than a
// constant because draining after an abort asks a different question with a
// different right answer.
static uint64_t  s_patience = FTP_REPLY_PATIENCE_MS;

static bool      s_aborted;   // a read ended because the person asked it to

// Replies the server still owes and nobody is waiting for. A transfer that
// opened with a `150` is promised exactly one more reply, and walking away
// from the transfer does not cancel that promise — so an abandoned transfer
// leaves this at one, and the next reply read spends it before it believes
// anything it is told. Without the count, a slow server's `426` arrives as
// the answer to whatever was typed next, and every reply after that belongs
// to the command before it.
static int       s_owed;

// Raised by the handler and read by the transfer loops. The handler does
// nothing else: a handler that printed would paint over a half-drawn line,
// and one that closed a handle would free it under the read using it.
static volatile int32_t s_interrupt;

// Declared here and defined with the prompt code, where a reader looks for
// it: the login path needs it long before the command loop does.
static int64_t prompt_line(char *buf, size_t cap, bool echo);

// ── Signals ─────────────────────────────────────────────────────────────

static void on_interrupt(int signo)
{
    (void)signo;
    s_interrupt = 1;
}

// ── The control connection ──────────────────────────────────────────────

// A WAIT THAT CAME BACK EMPTY IS NOT A FAILURE, and the two ways it happens
// want different answers. A deadline that expired means nothing arrived in
// time, which the reader is told in its own word so it can leave the channel
// usable. A signal that ended the park means the caller decides (DIVERGENCES
// § Signals — os64 has no SA_RESTART), and unless the person asked to stop,
// what the caller wants is to keep waiting.
static int64_t control_source(void *ctx, void *buf, size_t cap)
{
    (void)ctx;

    for (;;) {
        int64_t n = os64_read_for(s_conn, buf, cap, s_patience);

        if (n == OS64_INTERRUPTED) {
            if (s_interrupt) { s_aborted = true; return -1; }
            continue;
        }
        if (n == OS64_ERR_TIMEOUT)
            return FTP_SOURCE_STALLED;
        return n;
    }
}

static void session_drop(void)
{
    if (s_conn >= 0)
        os64_close(s_conn);
    s_conn = -1;
    s_loggedIn = false;
    s_owed = 0;             // nobody owes anything down a connection that is gone
    s_host[0] = '\0';
    s_peerIp = 0;
    s_port = 0;
}

// Read one reply and print it the way every FTP client has printed one: the
// server's own lines, code digits and all. Returns the code, or -1 with the
// session dropped — there is no resynchronising a control channel, so a
// reply that cannot be read ends the connection rather than leaving the next
// command to be answered by the wreckage.
// Read one of the replies the server owes, print it, and stop owing it. The
// verdict goes back untouched because the two callers want opposite things
// from a stall: the drain is asking "is it here yet?", and reply_get is
// waiting on it with the full patience and has nowhere else to go.
static ftp_reply_result_t owed_collect_one(void)
{
    ftp_reply_t leftover;
    ftp_reply_result_t rc = ftp_reply_read(&s_control, &leftover);

    if (rc == FTP_REPLY_OK) {
        s_owed--;
        os64_printf("%s\n", leftover.text);
    }
    return rc;
}

static int reply_get(ftp_reply_t *out)
{
    s_aborted = false;

    // SPEND WHAT IS OWED BEFORE BELIEVING ANYTHING. These replies belong to a
    // transfer that was abandoned; reading one as the answer to the command
    // just sent is how a client starts reporting the previous command's
    // verdict forever.
    while (s_owed > 0) {
        ftp_reply_result_t owed = owed_collect_one();
        if (owed != FTP_REPLY_OK) {
            os64_printf("ftp: %s\n", ftp_reply_reason(owed));
            session_drop();
            return -1;
        }
    }

    ftp_reply_result_t rc = ftp_reply_read(&s_control, out);
    if (rc != FTP_REPLY_OK) {
        // A stall here IS the end of the session: this read was waiting for
        // an answer to a command that was sent, and a server that owes one and
        // does not send it has stopped talking. The drain below is the only
        // caller for whom a stall is the good outcome.
        if (s_aborted)
            os64_printf("ftp: stopped\n");
        else
            os64_printf("ftp: %s\n", ftp_reply_reason(rc));
        session_drop();
        return -1;
    }

    os64_printf("%s\n", out->text);
    if (out->truncated)
        os64_printf("ftp: (the reply above was longer than this client keeps)\n");
    return out->code;
}

static bool command_send(const char *verb, const char *arg)
{
    char wire[FTP_COMMAND_MAX];
    size_t len = 0;

    ftp_command_result_t rc = ftp_command(verb, arg, wire, sizeof(wire), &len);
    if (rc != FTP_COMMAND_OK) {
        os64_printf("ftp: %s\n", ftp_command_reason(rc));
        return false;
    }

    // A control connection's send ring is never the bottleneck for a command
    // this size, so a short write here means the connection is going.
    int64_t n = os64_write(s_conn, wire, len);
    if (n != (int64_t)len) {
        os64_printf("ftp: the control connection failed\n");
        session_drop();
        return false;
    }
    return true;
}

// Send a command and read its reply. Returns the code, or -1 with the session
// dropped.
static int command_do(const char *verb, const char *arg, ftp_reply_t *out)
{
    if (!command_send(verb, arg))
        return -1;
    return reply_get(out);
}

// ── Connecting and logging in ───────────────────────────────────────────

// A password: the same read as the command prompt with the painting turned
// off. Nothing else is needed, because nothing was going to show it.
static bool read_secret(const char *prompt, char *out, size_t cap)
{
    os64_printf("%s", prompt);
    return prompt_line(out, cap, false) == 1;
}

// The password an anonymous login sends without asking. Since the 1980s the
// convention has been an email address, which is a courtesy to whoever reads
// the server's logs rather than a credential — and a machine that does not
// have one should not invent a plausible-looking lie, so this says what it is.
#define FTP_ANONYMOUS_PASS "os64@"

// USER, then PASS if the server asks for one. A 2yz to USER means the server
// wanted no password at all, which is how some anonymous servers answer.
//
// ANONYMOUS DOES NOT PROMPT. Being asked to invent a password for a login
// whose whole point is that it needs none is a question with no useful
// answer, and every client has sent the convention automatically for as long
// as there has been anonymous FTP. Any other name is a real account, and that
// one is asked for.
static bool session_login(const char *user, const char *password)
{
    ftp_reply_t reply;
    char secret[FTP_INPUT_MAX];

    int code = command_do("USER", user, &reply);
    if (code < 0)
        return false;
    if (ftp_code_complete(code)) {
        s_loggedIn = true;
        return true;
    }
    if (!ftp_code_intermediate(code))
        return false;

    if (password == NULL && os64_streq_nocase(user, "anonymous"))
        password = FTP_ANONYMOUS_PASS;

    if (password == NULL) {
        if (!read_secret("Password: ", secret, sizeof(secret)))
            return false;
        password = secret;
    }

    code = command_do("PASS", password, &reply);
    if (code < 0)
        return false;
    if (!ftp_code_complete(code))
        return false;

    s_loggedIn = true;
    return true;
}

// TYPE I, once, and never mentioned again. FTP.md ruling 3: os64 agrees with
// Unix about line endings, so the translation ASCII mode exists to do can
// only corrupt a binary somebody forgot to switch modes for.
static void session_set_binary(void)
{
    ftp_reply_t reply;
    int code = command_do("TYPE", "I", &reply);
    if (code >= 0 && !ftp_code_complete(code))
        os64_printf("ftp: the server would not take binary mode; "
                    "transfers may be altered in flight\n");
}

static bool session_open(const char *host, uint16_t port, const char *user)
{
    if (s_conn >= 0) {
        os64_printf("ftp: already connected to %s; close first\n", s_host);
        return false;
    }

    uint32_t ip = 0;
    int64_t rc = os64_resolve(host, &ip);
    if (rc < 0) {
        os64_printf("ftp: %s: %s\n", host, os64_dial_reason(rc));
        return false;
    }
    os64_format_ipv4(ip, s_peerText, sizeof(s_peerText));

    // Dial the ADDRESS, not the name, and keep it: the data connections dial
    // the same machine, and resolving twice could answer with two of them.
    char dialstring[64];
    if (os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%u",
                      s_peerText, (unsigned)port) <= 0)
        return false;

    os64_printf("Connecting to %s (%s) port %u...\n", host, s_peerText,
                (unsigned)port);

    int64_t conn = os64_dial(dialstring);
    if (conn < 0) {
        os64_printf("ftp: %s: %s\n", host, os64_dial_reason(conn));
        return false;
    }

    s_conn = (int32_t)conn;
    s_peerIp = ip;
    s_port = port;
    os64_strcopy(s_host, sizeof(s_host), host);
    ftp_control_init(&s_control, control_source, NULL);

    // The greeting. A 1yz here is a server saying "ready shortly" and a
    // second reply follows it; anything but a 2yz after that is a server
    // refusing the connection in words.
    ftp_reply_t reply;
    int code = reply_get(&reply);
    if (code >= 0 && ftp_code_preliminary(code))
        code = reply_get(&reply);
    if (code < 0)
        return false;
    if (!ftp_code_complete(code)) {
        session_drop();
        return false;
    }

    if (!session_login(user, NULL)) {
        if (s_conn >= 0)
            os64_printf("ftp: login failed\n");
        return false;
    }
    session_set_binary();
    return true;
}

static void session_quit(void)
{
    if (s_conn < 0)
        return;

    ftp_reply_t reply;
    // The server's goodbye is worth reading, but a server that hangs up
    // without one has still said everything it is going to say.
    command_do("QUIT", NULL, &reply);
    session_drop();
}

// ── The data connection ─────────────────────────────────────────────────

// PASV, then dial what it named. Returns a handle, or -1 with a message
// already printed.
static int32_t data_open(void)
{
    ftp_reply_t reply;
    int code = command_do("PASV", NULL, &reply);
    if (code < 0)
        return -1;
    if (!ftp_code_complete(code)) {
        os64_printf("ftp: the server refused passive mode\n");
        return -1;
    }

    uint32_t ip = 0;
    uint16_t port = 0;
    ftp_pasv_result_t rc = ftp_pasv_parse(reply.text, &ip, &port);
    if (rc != FTP_PASV_OK) {
        os64_printf("ftp: %s\n", ftp_pasv_reason(rc));
        return -1;
    }

    // THE ADDRESS IS THE ONE WE ARE ALREADY TALKING TO. A server behind its
    // own NAT names an address that means nothing here, and a hostile one
    // could name somebody else's machine to make this one dial it. The PORT
    // is the server's to choose; the machine is not.
    if (ip != s_peerIp) {
        char named[16];
        os64_format_ipv4(ip, named, sizeof(named));
        os64_printf("ftp: the server named %s for data; using %s\n",
                    named, s_peerText);
    }

    char dialstring[64];
    if (os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%u",
                      s_peerText, (unsigned)port) <= 0)
        return -1;

    int64_t data = os64_dial(dialstring);
    if (data < 0) {
        os64_printf("ftp: data connection: %s\n", os64_dial_reason(data));
        return -1;
    }
    return (int32_t)data;
}

// Collect what an abandoned transfer left owing, if it has arrived. Called
// right after walking away, with a short patience, so a prompt server's
// verdict is printed where it belongs instead of surfacing later.
//
// A STALL IS THE GOOD OUTCOME HERE, and the only place in this program where
// it is: the reply simply is not here yet. It stays on `s_owed` and the next
// command's reply read waits for it properly — which is why this can afford
// to be impatient, and why FTP_REPLY_STALLED has to leave the channel usable.
static void control_drain(void)
{
    uint64_t saved = s_patience;
    s_patience = FTP_DRAIN_PATIENCE_MS;

    while (s_owed > 0 && s_conn >= 0) {
        s_aborted = false;
        ftp_reply_result_t rc = owed_collect_one();

        if (rc == FTP_REPLY_STALLED)
            break;
        if (rc != FTP_REPLY_OK) {
            os64_printf("ftp: %s\n", ftp_reply_reason(rc));
            session_drop();
            break;
        }
    }

    s_patience = saved;
}

// ── Transfers ───────────────────────────────────────────────────────────

typedef struct {
    uint64_t bytes;
    int64_t  startEpoch;
    uint32_t startTicks;
    uint32_t ticksPerSecond;
} transfer_t;

static void transfer_start(transfer_t *t)
{
    os64_time_t now;
    t->bytes = 0;
    t->startEpoch = 0;
    t->startTicks = 0;
    t->ticksPerSecond = 0;
    if (os64_time(&now) == 0) {
        t->startEpoch = now.epoch;
        t->startTicks = now.ticks_into_second;
        t->ticksPerSecond = now.ticks_per_second;
    }
}

// "1234567 bytes received in 3.4 seconds (354.2 KB/s)" — ftp(1)'s line since
// 4.2BSD, in integer arithmetic because the rate is the only reason anyone
// reads it and one decimal place is all the rate is worth.
static void transfer_report(const transfer_t *t, const char *verb)
{
    os64_time_t now;
    uint64_t tenths = 0;

    if (t->ticksPerSecond != 0 && os64_time(&now) == 0 &&
        now.ticks_per_second == t->ticksPerSecond) {
        int64_t seconds = now.epoch - t->startEpoch;
        int64_t ticks = seconds * (int64_t)t->ticksPerSecond +
                        (int64_t)now.ticks_into_second - (int64_t)t->startTicks;
        if (ticks > 0)
            tenths = (uint64_t)ticks * 10u / t->ticksPerSecond;
    }

    if (tenths == 0) {
        os64_printf("%lu bytes %s\n", t->bytes, verb);
        return;
    }

    // Bytes per tenth of a second, scaled to KB/s with one decimal: the
    // multiply happens before the divide so a slow transfer does not report
    // zero.
    uint64_t rateTenths = t->bytes * 100u / (tenths * 1024u);
    os64_printf("%lu bytes %s in %lu.%lu seconds (%lu.%lu KB/s)\n",
                t->bytes, verb, tenths / 10u, tenths % 10u,
                rateTenths / 10u, rateTenths % 10u);
}

// Move bytes from the data connection to `out` until the connection closes —
// which is the only end-of-transfer FTP has, there being no length anywhere in
// the protocol.
//
// Returns the count moved, or -1 for a local failure (the disk). An INTERRUPT
// is not a failure of the transfer, it is the person changing their mind, and
// it is reported through s_interrupt so the caller can decide what to keep.
static int64_t data_to_file(int32_t data, int32_t out, transfer_t *t)
{
    char buf[FTP_XFER_BUF];
    bool closed = false;

    while (!closed) {
        // FILL THE BUFFER BEFORE WRITING IT. A read of a connection answers
        // with what has ARRIVED — one segment, or a scheduler pass's worth —
        // so writing each read hands ext2 a block or two per TCP segment, and
        // ext2 is write-through: every one of those is a disk transaction the
        // wire waits behind. Accumulating first turns a transfer into one
        // whole-chunk write per 64KB, however the wire delivered it.
        //
        // The cost is that a LISTING appears when it is complete rather than
        // line by line, since stdout takes the same path. A directory is
        // bounded by what a person will read; a download is not, which is
        // what this is sized for.
        size_t have = 0;
        while (have < sizeof(buf)) {
            if (s_interrupt)
                break;

            int64_t n = os64_read_for(data, buf + have, sizeof(buf) - have,
                                      OS64_WAIT_FOREVER);
            if (n == OS64_INTERRUPTED)
                continue;           // s_interrupt decides at the loop top
            if (n == 0) { closed = true; break; }
            if (n < 0) {
                os64_printf("ftp: the data connection failed\n");
                return -1;
            }
            have += (size_t)n;
        }

        // Whatever was gathered goes to the disk even when the fill ended
        // early: an interrupted transfer's `.part` should hold everything
        // that actually arrived.
        if (have > 0) {
            if (os64_write(out, buf, have) != (int64_t)have) {
                os64_printf("ftp: writing locally failed (disk full?)\n");
                return -1;
            }
            t->bytes += (uint64_t)have;
        }

        if (s_interrupt)
            break;
    }

    return (int64_t)t->bytes;
}

// The name a downloaded file takes when the person did not say. The last
// path component of what was asked for, so `get /pub/a/b.txt` lands in b.txt
// and not in a directory nobody made.
static const char *path_basename(const char *path)
{
    const char *last = path;
    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '/')
            last = s + 1;
    }
    return last;
}

// LIST, printed as it arrives. FTP.md ruling 4: what a server puts here was
// never specified, so parsing it means a table of heuristics for Unix,
// Windows, VMS and three kinds of wrong. This prints what the server sent.
static void do_list(const char *path)
{
    int32_t data = data_open();
    if (data < 0)
        return;

    ftp_reply_t reply;
    if (!command_send("LIST", (path && path[0]) ? path : NULL)) {
        os64_close(data);
        return;
    }

    int code = reply_get(&reply);
    if (code < 0) { os64_close(data); return; }
    if (!ftp_code_preliminary(code) && !ftp_code_complete(code)) {
        os64_close(data);
        return;
    }

    transfer_t t;
    transfer_start(&t);
    s_interrupt = 0;
    data_to_file(data, OS64_STDOUT, &t);
    os64_close(data);

    if (s_interrupt) {
        s_interrupt = 0;
        os64_printf("ftp: stopped\n");
        s_owed = ftp_code_preliminary(code) ? 1 : 0;
        control_drain();
        return;
    }

    // A preliminary reply promised a second one; a complete reply already
    // was the whole answer and asking for another would wait forever.
    if (ftp_code_preliminary(code))
        reply_get(&reply);
}

static void do_get(const char *remote, const char *local)
{
    char target[FTP_LOCAL_MAX];
    char part[FTP_LOCAL_MAX];

    if (os64_strcopy(target, sizeof(target),
                     (local && local[0]) ? local : path_basename(remote))
        >= sizeof(target)) {
        os64_printf("ftp: that local name is too long\n");
        return;
    }
    if (target[0] == '\0') {
        os64_printf("ftp: no local name to save that as\n");
        return;
    }
    if (os64_snprintf(part, sizeof(part), "%s.part", target) <= 0) {
        os64_printf("ftp: that local name is too long\n");
        return;
    }

    int32_t data = data_open();
    if (data < 0)
        return;

    // Opened AFTER the data connection, so a server that refuses passive mode
    // does not leave an empty staging file behind.
    int64_t out = os64_open(part, "w");
    if (out < 0) {
        os64_printf("ftp: cannot create %s\n", part);
        os64_close(data);
        return;
    }

    ftp_reply_t reply;
    if (!command_send("RETR", remote)) {
        os64_close(data);
        os64_close((int32_t)out);
        os64_unlink(part);
        return;
    }

    int code = reply_get(&reply);
    if (code < 0 || !ftp_code_positive(code)) {
        // `550 No such file` arrives HERE, on the control channel, with the
        // data connection never fed. Waiting on the data handle first is how
        // a client hangs on a file that does not exist.
        os64_close(data);
        os64_close((int32_t)out);
        os64_unlink(part);
        return;
    }

    transfer_t t;
    transfer_start(&t);
    s_interrupt = 0;
    int64_t moved = data_to_file(data, (int32_t)out, &t);
    os64_close(data);
    os64_close((int32_t)out);

    bool stopped = (s_interrupt != 0);
    s_interrupt = 0;

    if (stopped || moved < 0) {
        os64_printf("ftp: %s%s holds what arrived\n",
                    stopped ? "stopped; " : "", part);
        s_owed = ftp_code_preliminary(code) ? 1 : 0;
        control_drain();
        return;
    }

    // THE VERDICT IS THE CONTROL CHANNEL'S. The data connection closing means
    // the server stopped sending, which is what a full disk at the far end
    // looks like too.
    int final = ftp_code_preliminary(code) ? reply_get(&reply) : code;
    if (final < 0 || !ftp_code_complete(final)) {
        os64_printf("ftp: the transfer did not complete; %s holds what "
                    "arrived\n", part);
        return;
    }

    if (os64_rename(part, target) < 0) {
        os64_printf("ftp: cannot rename %s to %s; the bytes are in %s\n",
                    part, target, part);
        return;
    }
    transfer_report(&t, "received");
}

static void do_put(const char *local, const char *remote)
{
    int64_t in = os64_open(local, "r");
    if (in < 0) {
        os64_printf("ftp: cannot read %s\n", local);
        return;
    }

    const char *name = (remote && remote[0]) ? remote : path_basename(local);
    if (name[0] == '\0') {
        os64_printf("ftp: no remote name to store that as\n");
        os64_close((int32_t)in);
        return;
    }

    int32_t data = data_open();
    if (data < 0) {
        os64_close((int32_t)in);
        return;
    }

    ftp_reply_t reply;
    if (!command_send("STOR", name)) {
        os64_close(data);
        os64_close((int32_t)in);
        return;
    }

    int code = reply_get(&reply);
    if (code < 0 || !ftp_code_positive(code)) {
        os64_close(data);
        os64_close((int32_t)in);
        return;
    }

    transfer_t t;
    transfer_start(&t);
    s_interrupt = 0;

    char buf[FTP_XFER_BUF];
    bool failed = false;
    for (;;) {
        if (s_interrupt)
            break;

        int64_t n = os64_read(in, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) { failed = true; break; }

        // A write to a TCP connection queues into the send ring and returns
        // what it took, so a short write is ordinary back-pressure and the
        // rest goes on the next pass. It BLOCKS when the ring is full, which
        // is where an upload to a slow peer spends its time — and therefore
        // where Ctrl+C has to be able to land, or stopping a stalled upload
        // means waiting for the peer that stalled it.
        size_t sent = 0;
        while (sent < (size_t)n) {
            int64_t w = os64_write(data, buf + sent, (size_t)n - sent);
            if (w == OS64_INTERRUPTED) {
                if (s_interrupt)
                    break;
                continue;
            }
            if (w <= 0) { failed = true; break; }
            sent += (size_t)w;
        }
        t.bytes += (uint64_t)sent;
        if (failed || s_interrupt)
            break;
    }

    // CLOSING THE DATA CONNECTION IS HOW AN UPLOAD SAYS IT IS FINISHED. There
    // is no length anywhere in the protocol; the FIN is the end of the file.
    os64_close(data);
    os64_close((int32_t)in);

    bool stopped = (s_interrupt != 0);
    s_interrupt = 0;

    if (stopped || failed) {
        os64_printf("ftp: %s\n", stopped ? "stopped" : "the upload failed");
        s_owed = ftp_code_preliminary(code) ? 1 : 0;
        control_drain();
        return;
    }

    int final = ftp_code_preliminary(code) ? reply_get(&reply) : code;
    if (final < 0 || !ftp_code_complete(final)) {
        os64_printf("ftp: the server did not accept the file\n");
        return;
    }
    transfer_report(&t, "sent");
}

// ── The prompt ──────────────────────────────────────────────────────────

static void do_pwd(void)
{
    ftp_reply_t reply;
    int code = command_do("PWD", NULL, &reply);
    if (code < 0 || !ftp_code_complete(code))
        return;

    // The reply is printed already; this is the path pulled out of it, which
    // is the half a script would want and the half the quoting rule can be
    // got wrong on.
    char path[FTP_PATH_MAX];
    if (ftp_path_from_257(reply.text, path, sizeof(path)))
        os64_printf("Remote directory: %s\n", path);
}

static void do_status(void)
{
    if (s_conn < 0) {
        os64_printf("Not connected.\n");
        return;
    }
    os64_printf("Connected to %s (%s) port %u.\n", s_host, s_peerText,
                (unsigned)s_port);
    os64_printf("Logged in: %s.  Type: binary.  Mode: passive.\n",
                s_loggedIn ? "yes" : "no");
}

static void do_help(void)
{
    os64_printf(
        "open HOST [PORT]  connect and log in      close        hang up\n"
        "user [NAME]       log in again            quit / bye   leave\n"
        "ls / dir [PATH]   list a directory        pwd          where you are\n"
        "cd PATH           change directory        cdup         go up one\n"
        "get REMOTE [LOCAL]                        put LOCAL [REMOTE]\n"
        "del NAME          remove a file           mkdir NAME   make a directory\n"
        "rmdir NAME        remove a directory      status       what is connected\n"
        "quote WORDS...    send a command verbatim\n");
}

// Read one line from the terminal, painting it as it is typed when `echo` is
// set. Same returns as os64_readline, which is what this replaces: 1 for a
// line, 0 at end of input, OS64_INTERRUPTED for a cancelled one, negative
// otherwise.
//
// THE CONSOLE DOES NOT ECHO, and that is the design rather than an omission:
// os64 hands a program the bytes and lets it decide what appears. So a
// program with a prompt owns this loop — forget it and you get a prompt that
// answers commands nobody can see themselves typing — and a password prompt
// is the SAME loop with the painting turned off. No terminal mode is involved
// in hiding it, because nothing was going to show it.
//
// This is the SMALL reader: type, rub out, submit. husk's has history, a
// caret, tab completion and the emacs chords; none of that belongs to a
// client whose longest line is a path.
static int64_t prompt_line(char *buf, size_t cap, bool echo)
{
    size_t have = 0;

    for (;;) {
        char c;
        int64_t n = os64_read(OS64_STDIN, &c, 1);
        if (n == 0) {
            buf[have] = '\0';
            return 0;                    // Ctrl+D
        }
        if (n < 0)
            return n;                    // OS64_INTERRUPTED, or a real failure

        if (c == '\r' || c == '\n') {
            os64_write(OS64_STDOUT, "\n", 1);
            buf[have] = '\0';
            return 1;
        }

        if (c == 0x08 || c == 0x7F) {     // backspace, and the DEL some send
            if (have > 0) {
                have--;
                // Rub out: back over the glyph, paint a space, back again.
                // The renderer moves the cursor and never erases, so erasure
                // is the caller's overprint (CLAUDE.md § Keyboard). Nothing to
                // rub out when nothing was painted.
                if (echo)
                    os64_write(OS64_STDOUT, "\b \b", 3);
            }
            continue;
        }

        if (c == 0x1B) {
            // An arrow or a Home key: ESC '[' … final. Swallowed whole rather
            // than let into the line, where it would travel to the server as
            // part of a file name. The patience is short because a bare ESC
            // is also a key somebody can press.
            for (int i = 0; i < 8; i++) {
                char seq;
                if (os64_read_for(OS64_STDIN, &seq, 1, 20) != 1)
                    break;
                if (seq >= 0x40 && seq <= 0x7E && seq != '[')
                    break;
            }
            continue;
        }

        // Anything else below space is a key this prompt has no answer for,
        // and printing it would move the cursor somewhere the buffer does not
        // know about. Silence keeps the screen and the line in step.
        if ((unsigned char)c < 0x20)
            continue;

        if (have + 1 >= cap)
            continue;                    // full: the screen stops matching nothing

        buf[have++] = c;
        if (echo)
            os64_write(OS64_STDOUT, &c, 1);
    }
}

// The first word, and what follows it. `rest` points at the remainder with
// its leading blanks gone, which is what a one-operand verb takes VERBATIM —
// so a path with a space in it works everywhere except `get` and `put`, whose
// two operands have to be told apart somehow. 4.2BSD's client draws the line
// in the same place.
static char *split_word(char *line, char **rest)
{
    while (*line == ' ' || *line == '\t')
        line++;

    char *word = line;
    while (*line != '\0' && *line != ' ' && *line != '\t')
        line++;

    if (*line != '\0') {
        *line++ = '\0';
        while (*line == ' ' || *line == '\t')
            line++;
    }
    *rest = line;
    return word;
}

static bool need_connection(void)
{
    if (s_conn >= 0)
        return true;
    os64_printf("Not connected.\n");
    return false;
}

// One typed line. Returns false when the person asked to leave.
static bool dispatch(char *line)
{
    char *rest = NULL;
    char *verb = split_word(line, &rest);

    if (verb[0] == '\0')
        return true;

    if (os64_streq_nocase(verb, "quit") || os64_streq_nocase(verb, "bye")) {
        session_quit();
        return false;
    }
    if (os64_streq_nocase(verb, "help") || os64_streq(verb, "?")) {
        do_help();
        return true;
    }
    if (os64_streq_nocase(verb, "status")) {
        do_status();
        return true;
    }
    if (os64_streq_nocase(verb, "open")) {
        char *host = split_word(rest, &rest);
        if (host[0] == '\0') {
            os64_printf("usage: open HOST [PORT]\n");
            return true;
        }
        uint16_t port = FTP_DEFAULT_PORT;
        if (rest[0] != '\0') {
            uint64_t value = 0;
            if (!os64_parse_u64(rest, &value) || value == 0 || value > 65535) {
                os64_printf("ftp: %s is not a port\n", rest);
                return true;
            }
            port = (uint16_t)value;
        }
        session_open(host, port, "anonymous");
        return true;
    }
    if (os64_streq_nocase(verb, "close")) {
        if (need_connection())
            session_quit();
        return true;
    }

    if (!need_connection())
        return true;

    if (os64_streq_nocase(verb, "user")) {
        char *who = split_word(rest, &rest);
        session_login(who[0] ? who : "anonymous", NULL);
        return true;
    }
    if (os64_streq_nocase(verb, "ls") || os64_streq_nocase(verb, "dir")) {
        do_list(rest);
        return true;
    }
    if (os64_streq_nocase(verb, "pwd")) {
        do_pwd();
        return true;
    }
    if (os64_streq_nocase(verb, "cdup")) {
        ftp_reply_t reply;
        command_do("CDUP", NULL, &reply);
        return true;
    }
    if (os64_streq_nocase(verb, "get") || os64_streq_nocase(verb, "put")) {
        char *first = split_word(rest, &rest);
        char *second = split_word(rest, &rest);
        if (first[0] == '\0') {
            os64_printf("usage: %s NAME [NAME]\n", verb);
            return true;
        }
        if (os64_streq_nocase(verb, "get"))
            do_get(first, second);
        else
            do_put(first, second);
        return true;
    }

    // The one-operand verbs, each taking the rest of the line verbatim so a
    // name with a space in it survives.
    static const struct { const char *word; const char *verb; } ONE_ARG[] = {
        { "cd",    "CWD"  },
        { "del",   "DELE" },
        { "mkdir", "MKD"  },
        { "rmdir", "RMD"  },
    };
    for (size_t i = 0; i < sizeof(ONE_ARG) / sizeof(ONE_ARG[0]); i++) {
        if (!os64_streq_nocase(verb, ONE_ARG[i].word))
            continue;
        if (rest[0] == '\0') {
            os64_printf("usage: %s NAME\n", ONE_ARG[i].word);
            return true;
        }
        ftp_reply_t reply;
        command_do(ONE_ARG[i].verb, rest, &reply);
        return true;
    }

    // `quote` is the escape hatch every FTP client has had: whatever this one
    // does not implement, the server still does.
    if (os64_streq_nocase(verb, "quote")) {
        if (rest[0] == '\0') {
            os64_printf("usage: quote WORDS...\n");
            return true;
        }
        char *raw = split_word(rest, &rest);
        ftp_reply_t reply;
        command_do(raw, rest[0] ? rest : NULL, &reply);
        return true;
    }

    os64_printf("ftp: %s? Try help.\n", verb);
    return true;
}

int main(int32_t argc, char **argv)
{
    const char *positional[2] = { NULL, NULL };

    os64_args_t args;
    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "transfer files with an FTP server";
    args.details = "passive mode, binary transfers; type help at the prompt";

    int32_t got = os64_args_parse(&args, "ftp [HOST [PORT]]", positional, 2);
    if (got < 0)
        return got == OS64_ARG_HELP ? FTP_OK : FTP_USAGE;

    // A transfer has to be stoppable without losing the session, so SIGINT is
    // caught here and read by the transfer loops rather than killing the
    // program the way it would by default.
    os64_signal_set_handler(OS64_SIGINT, on_interrupt);

    if (positional[0] != NULL) {
        uint16_t port = FTP_DEFAULT_PORT;
        if (positional[1] != NULL) {
            uint64_t value = 0;
            if (!os64_parse_u64(positional[1], &value) || value == 0 ||
                value > 65535) {
                os64_hprintf(OS64_STDERR, "ftp: %s is not a port\n",
                             positional[1]);
                return FTP_USAGE;
            }
            port = (uint16_t)value;
        }
        session_open(positional[0], port, "anonymous");
    }

    char line[FTP_INPUT_MAX];
    for (;;) {
        os64_printf("ftp> ");

        int64_t rc = prompt_line(line, sizeof(line), true);

        // A CTRL+C AT THE PROMPT CANCELS THE LINE, NOT THE PROGRAM — what it
        // does in every shell. It arrives as OS64_INTERRUPTED because the
        // signal ended the park and os64 has no SA_RESTART to hide that
        // (DIVERGENCES § Signals): the caller decides, and here the decision
        // is to throw away what was half-typed and ask again.
        if (rc == OS64_INTERRUPTED) {
            s_interrupt = 0;
            os64_printf("\n");
            continue;
        }
        if (rc == 0) {              // Ctrl+D
            os64_printf("\n");
            break;
        }
        if (rc < 0)
            break;

        s_interrupt = 0;
        if (!dispatch(line))
            break;
    }

    session_quit();
    return FTP_OK;
}
