// os64get downloads from os64serve (GET/LIST with length and CRC) or HTTP.
// Valet names route through os64get.conf; URL destinations come from the
// command line or the original URL. Valet installs preserve replaced originals
// when archiving is enabled; URL downloads do not archive. install.c owns
// scratch files and publication for both modes.
//
// A batch receives and verifies downloads and old-file backups before its
// installation renames begin. Ctrl+C cancels preparation and cleans the run;
// during publication it is deferred until the renames and cleanup finish.
// Ext2 replacement has no missing-name window. FAT retains its legacy
// remove-first replacement behavior, so a backup is recovery data, not an
// atomicity guarantee. A batch is still a sequence of per-file renames.

#include "os64/os64.h"
#include "os64/crc32.h"
#include "os64/conf.h"
#include "os64/date.h"

#include "gzip/gzip.h"
#include "http.h"
#include "install.h"

#define GET_OK             0
#define GET_USAGE          2
#define GET_DIAL_FAILED    3
#define GET_REQUEST_FAILED 4
#define GET_REFUSED        5   // the server said NO
#define GET_BAD_HEADER     6   // the server said something we don't speak
#define GET_SHORT          7   // connection died mid-file
#define GET_CORRUPT        8   // arrived complete and WRONG — the CRC caught it
#define GET_WRITE_FAILED   9
#define GET_PUBLISH_FAILED 10  // downloaded fine, could not put it in place
#define GET_ARCHIVE_FAILED 11  // could not make or keep the archive copy
// The destination matches the server's length and CRC, so no payload was
// fetched. The run still owns its empty staging reservation. This status
// never escapes main (which reports it and exits 0) — it exists so the commit
// phase knows there is no downloaded file to prepare.
#define GET_UNCHANGED      12
// The operand carried a "scheme://" and was still not a URL this program can
// fetch — an unknown scheme, or a shape a
// URL cannot have. Distinct from GET_USAGE because the command line was
// well formed: it is the ADDRESS that cannot be used.
#define GET_BAD_URL        13
// The reply is well formed and this program cannot honestly read it yet — a
// transfer coding other than chunked, or a content coding it does not
// decode. Distinct from
// GET_BAD_HEADER, which means the server said something that is not HTTP:
// "I do not speak this" and "that was not speech" are different answers, and
// a script driving a dozen fetches should not have to tell them apart from
// the English.
#define GET_UNSUPPORTED    14
// The server sent this fetch somewhere else and it could not go: the trail
// ran past its hop limit, doubled back on itself, or ended at an address
// this machine has no road to (an https target with no proxy). Distinct from
// GET_REFUSED because the next move is different — a refusal is the server's
// final answer about the page, while this is a road that did not arrive, and
// the thing to change is usually on THIS side.
#define GET_REDIRECT       15
#define GET_CANCELLED      130



#define GET_PORT      6464
// 64KB per write: the fetch loop fills this whole buffer from the socket
// before writing it, so every write() hands ext2 sixteen blocks it can put
// on the disk as a single run — however the wire delivered them. At 4KB
// the file was written a block per syscall, and the transfer waited on the
// disk, not the wire.
#define GET_CHUNK     65536
#define GET_PATH_MAX  256

// A compressed response cannot spend the destination filesystem without a
// bound merely by describing the same byte millions of times. Content-Length
// counts wire bytes, so a length-framed response may expand at most 100x, with
// a 1 MiB floor that keeps small ordinary pages useful. Sixteen MiB is the
// ceiling in every case, and the only available bound when close frames the
// body and its wire length is unknowable in advance. Identity is unaffected:
// these limits pay specifically for compression's amplification hazard.
#define URL_GZIP_OUTPUT_FLOOR (1ull * 1024ull * 1024ull)
#define URL_GZIP_OUTPUT_MAX   (16ull * 1024ull * 1024ull)
#define URL_GZIP_RATIO_MAX    100ull

// Hold the manifest before receiving payloads, refusing an oversized list
// before publication. Batch metadata uses static storage to keep it off the
// user stack.
#define GET_NAME_MAX  64
#define GET_MAX_LIST  INSTALL_MAX_FILES

static install_file_t stages[GET_MAX_LIST];
static unsigned stage_count;
static bool quiet_run, batch_run;

// One line of the server's catalogue. The length and crc are not decoration:
// they are what lets -a decide a file is unchanged WITHOUT dialing for it (see
// local_matches), which is the difference between a refresh that re-fetches
// 86 files and one that fetches the three that changed.
// `lot` is the server's label for WHERE the file came from — its fourth LIST
// field — and it is what routes a file no name rule claims (`@tests`).
// Empty means the server offered none, which is what every server did before
// lots existed and still means "route me by name alone".
#define GET_LOT_MAX   32

typedef struct {
    char     name[GET_NAME_MAX];
    uint64_t length;
    uint32_t crc;
    char     lot[GET_LOT_MAX];
} get_entry_t;

// The catalogue, at file scope because BOTH fetch paths need it and it is far
// too fat for a stack: `-a` walks the whole thing, and a single-file fetch
// borrows it to look up that one file's lot (see lookup_lot). They never run
// in the same invocation, so one buffer is one buffer.
static get_entry_t entries[GET_MAX_LIST];

// ── The config: the valet's routing map ───────────────────────────────
// A small fixed table, because the whole install map is a handful of lines
// and a utility that mallocs to read its own config has its priorities wrong.
// Precedence is by SPECIFICITY (exact beats suffix beats `@lot` beats `*`),
// and within a class the LAST match in the file wins, so a /home copy can
// override by repeating a line.
//
// WHY A LOT SITS BELOW SUFFIX AND ABOVE `*`: a lot names where a file came
// FROM, so it is the weakest claim any rule can make about a particular file
// — weaker than one that spells the name and weaker than one that recognises
// the kind. That ordering is exactly what lets one served directory hold
// files bound for several places: kernel/bin carries fifteen fixtures for
// /tests, but also os64_kernel (an exact rule sends it to /fat/boot) and
// libtest.so (a `*.so` rule sends it to /lib). And it is still stronger than
// `*`, which is the rule that knows nothing at all.
#define CONF_RULES_MAX 16
#define CONF_DIR_MAX   128

typedef struct {
    char name[OS64_DIRENT_NAME_MAX + 1];   // "os64_kernel", or ".so" for a suffix rule
    char dir[CONF_DIR_MAX];
} conf_rule_t;

typedef struct {
    conf_rule_t exact[CONF_RULES_MAX];  size_t nexact;
    conf_rule_t suffix[CONF_RULES_MAX]; size_t nsuffix;
    conf_rule_t lot[CONF_RULES_MAX];    size_t nlot;   // `@tests = /tests`
    char star[CONF_DIR_MAX];            // the `*` rule; empty = none
    char archive[CONF_DIR_MAX];         // the `archive` key; empty = don't
    const char *path;                   // which file answered (for complaints)
    bool anyRule;                       // did the file route anything at all?
} conf_t;

// (The private { "/home/os64get.conf", "/etc/os64get.conf" } ladder retired
// 2026-08-23 — conf_load asks the system's search path now.)

// Trim a trailing '/' off a directory value so "/bin/" and "/bin" mean the
// same thing — the one courtesy the reader extends beyond the dialect.
//
// Returns false if the value is EMPTY or does not FIT, having touched nothing:
// the caller says so and drops the line. Three reasons this refuses instead of
// coping. First, os64_strcopy reports the UNTRUNCATED source length (strlcpy's
// contract, str.h), so copying first and measuring after would send the loop
// below walking off the end of a 128-byte slot — reading past it, and writing
// a NUL past it if the byte it finds there happens to be '/'. Second, even
// with the index clamped, a silently shortened path is a rule that quietly
// installs your files SOMEWHERE ELSE; "/usr/local/…/bin" truncated to
// "/usr/local/…" is a directory that may well exist. Third, an EMPTY value
// routed to "", which join_path formatted as "/husk" — a half-finished edit
// installing a program at the root of the filesystem, quietly, because an
// empty string is a perfectly good string. A routing rule nobody can trust is
// worth less than no rule at all. (Codex review, 2026-08-22/23.)
static bool conf_take_dir(char *dst, const char *value)
{
    size_t len = os64_strlen(value);

    if (len == 0)
        return false;
    if (len >= CONF_DIR_MAX)
        return false;   // dst keeps whatever an earlier line put there

    size_t n = os64_strcopy(dst, CONF_DIR_MAX, value);
    while (n > 1 && dst[n - 1] == '/')
        dst[--n] = '\0';
    return true;
}

// The one complaint every unusable directory value makes, in one voice — and
// it says WHICH kind, because "ignored" without a reason sends the reader back
// to the file to guess.
static void conf_bad_dir(const conf_t *c, const char *key, const char *value)
{
    if (value[0] == '\0')
        os64_hprintf(OS64_STDERR, "os64get: %s: '%s' has no directory after the '='"
                     " - ignored\n", c->path, key);
    else
        os64_hprintf(OS64_STDERR, "os64get: %s: directory for '%s' is longer than %d bytes"
                     " - ignored: %s\n", c->path, key, CONF_DIR_MAX - 1, value);
}

static bool conf_line(const char *key, const char *value, void *user)
{
    conf_t *c = (conf_t *)user;

    if (key == NULL) {
        // Not `key = value`. Say so and carry on: one bad line must not
        // silence the five good ones, and silence is how a setting "does
        // nothing" for an afternoon.
        os64_hprintf(OS64_STDERR, "os64get: %s: expected 'key = value' - ignored: %s\n",
                     c->path, value);
        return true;
    }

    if (os64_streq(key, "archive")) {
        if (!conf_take_dir(c->archive, value))
            conf_bad_dir(c, key, value);
        return true;
    }
    if (os64_streq(key, "*")) {
        if (!conf_take_dir(c->star, value))
            conf_bad_dir(c, key, value);
        else
            c->anyRule = true;
        return true;
    }
    if (key[0] == '@' && key[1] != '\0') {
        // A LOT rule: `@tests = /tests` routes everything the server labelled
        // "tests" and no name rule claimed. Stored without the '@' so it can
        // be compared straight against the LIST field.
        if (c->nlot < CONF_RULES_MAX) {
            if (!conf_take_dir(c->lot[c->nlot].dir, value)) {
                conf_bad_dir(c, key, value);
                return true;   // the slot stays free for the next rule
            }
            os64_strcopy(c->lot[c->nlot].name, sizeof(c->lot[0].name), key + 1);
            c->nlot++;
            c->anyRule = true;
        } else {
            os64_hprintf(OS64_STDERR, "os64get: %s: too many '@lot' rules (limit %d) - ignored: %s\n",
                         c->path, CONF_RULES_MAX, key);
        }
        return true;
    }
    if (key[0] == '*' && key[1] == '.') {
        // A suffix rule: stored as ".so", matched against the name's tail.
        // Later lines append; the matcher walks the table backwards so the
        // last one wins.
        if (c->nsuffix < CONF_RULES_MAX) {
            if (!conf_take_dir(c->suffix[c->nsuffix].dir, value)) {
                conf_bad_dir(c, key, value);
                return true;   // the slot stays free for the next rule
            }
            os64_strcopy(c->suffix[c->nsuffix].name, sizeof(c->suffix[0].name), key + 1);
            c->nsuffix++;
            c->anyRule = true;
        } else {
            os64_hprintf(OS64_STDERR, "os64get: %s: too many '*.' rules (limit %d) - ignored: %s\n",
                         c->path, CONF_RULES_MAX, key);
        }
        return true;
    }
    if (key[0] == '*' || os64_strlen(key) > OS64_DIRENT_NAME_MAX) {
        os64_hprintf(OS64_STDERR, "os64get: %s: not a name, '*.suffix' or '*' - ignored: %s\n",
                     c->path, key);
        return true;
    }

    if (c->nexact < CONF_RULES_MAX) {
        if (!conf_take_dir(c->exact[c->nexact].dir, value)) {
            conf_bad_dir(c, key, value);
            return true;   // the slot stays free for the next rule
        }
        os64_strcopy(c->exact[c->nexact].name, sizeof(c->exact[0].name), key);
        c->nexact++;
        c->anyRule = true;
    } else {
        os64_hprintf(OS64_STDERR, "os64get: %s: too many name rules (limit %d) - ignored: %s\n",
                     c->path, CONF_RULES_MAX, key);
    }
    return true;
}

static void conf_load(conf_t *c)
{
    // ASK THE SYSTEM (2026-08-23). The loop that used to be here walked a
    // private { "/home/os64get.conf", "/etc/os64get.conf" } — copied from
    // logd, which is exactly how six programs came to spell one ladder five
    // ways. /etc/os64.conf's `conf =` is the ladder now, walked in the
    // kernel; the walker returns the file that answered, so the "try each in
    // order ourselves so the name is right" problem solves itself: the name
    // arrives WITH the answer.
    static char chosen[OS64_CONF_PATH_MAX];   // static: c->path outlives this call
    if (os64_conf_find("os64get.conf", chosen, sizeof(chosen)) != 0) {
        c->path = NULL;   // no file: cwd semantics, no archive
        return;
    }

    c->path = chosen;
    int64_t rc = os64_conf_read(chosen, conf_line, c);
    if (rc == OS64_CONF_NO_FILE) {
        // It opened for the walker and not for us — vanished in between, or
        // unreadable. Same outcome as never having had one, said out loud.
        os64_hprintf(OS64_STDERR, "os64get: %s went away before it could be read;"
                     " files land in the current directory\n", chosen);
        c->path = NULL;
    } else if (rc == OS64_CONF_TRUNCATED) {
        os64_hprintf(OS64_STDERR, "os64get: %s is larger than %d bytes - the tail was not read\n",
                     chosen, OS64_CONF_MAX);
    } else if (rc == OS64_CONF_NO_MEMORY) {
        os64_hprintf(OS64_STDERR, "os64get: out of memory reading %s - no routing rules;"
                     " files land in the current directory\n", chosen);
    }
}

// The directory a file installs into, or NULL for "no rule" (= cwd).
// `lot` is the server's source label for this file, or NULL/"" when it
// offered none — in which case the walk simply skips the lot class and
// behaves exactly as it did before lots existed.
static const char *conf_route(const conf_t *c, const char *name, const char *lot)
{
    for (size_t i = c->nexact; i > 0; i--)
        if (os64_streq(c->exact[i - 1].name, name))
            return c->exact[i - 1].dir;

    size_t nlen = os64_strlen(name);
    for (size_t i = c->nsuffix; i > 0; i--) {
        const char *suf = c->suffix[i - 1].name;
        size_t slen = os64_strlen(suf);
        if (nlen > slen && os64_streq(name + (nlen - slen), suf))
            return c->suffix[i - 1].dir;
    }

    if (lot != NULL && lot[0] != '\0')
        for (size_t i = c->nlot; i > 0; i--)
            if (os64_streq(c->lot[i - 1].name, lot))
                return c->lot[i - 1].dir;

    return c->star[0] ? c->star : NULL;
}

// ── Small file verbs ────────────────────────────────────────────────────

// Build "<dir>/<name>" (or just name when dir is NULL). False if it won't fit.
static bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    int32_t n = dir ? os64_snprintf(out, cap, "%s/%s", dir, name)
                    : os64_snprintf(out, cap, "%s", name);
    return n > 0 && (size_t)n < cap;
}

// ── Parsing the valet's header ─────────────────────────────────────────

// Parse a decimal from [s, end). Returns false on empty, on a non-digit, or
// on a value too large for 64 bits — refusal rather than a guess, because a
// malformed length is exactly the case where guessing writes a file of the
// wrong size. The overflow arm is not pedantry: "18446744073709551616" wraps
// to 0, and a zero length paired with the empty-file CRC would let a hostile
// or broken server publish an EMPTY file over a good one and call it verified.
static bool parse_u64(const char *s, const char *end, uint64_t *out)
{
    if (s >= end)
        return false;
    uint64_t v = 0;
    while (s < end)
    {
        if (*s < '0' || *s > '9')
            return false;
        uint64_t digit = (uint64_t)(*s - '0');
        // Ask BEFORE multiplying — once it has wrapped there is nothing left
        // to detect. (UINT64_MAX - digit) / 10 is the largest value that can
        // still absorb one more digit.
        if (v > (UINT64_MAX - digit) / 10)
            return false;
        v = v * 10 + digit;
        s++;
    }
    *out = v;
    return true;
}

// Parse exactly 8 hex digits (the CRC). Same strictness, same reason.
static bool parse_hex32(const char *s, const char *end, uint32_t *out)
{
    if (end - s != 8)
        return false;
    uint32_t v = 0;
    while (s < end)
    {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

// Compare local length and CRC with the valet manifest/header.
static bool local_matches(const char *path, uint64_t wantLen, uint32_t wantCrc)
{
    int64_t have = os64_open(path, "r");
    if (have < 0)
        return false;                  // nothing there — everything to fetch

    uint64_t len = 0;
    uint32_t crc = os64_crc32_begin();
    uint8_t buf[GET_CHUNK];
    int64_t n = -1;
    while (!install_cancelled() && (n = os64_read((int32_t)have, buf, sizeof(buf))) > 0)
    {
        crc = os64_crc32_update(crc, buf, (size_t)n);
        len += (uint64_t)n;
    }
    os64_close((int32_t)have);

    // n == 0 is a clean end-of-file; a negative n is a read error, and an
    // unreadable local file is emphatically not a reason to skip the download.
    return !install_cancelled() && n == 0 && len == wantLen && os64_crc32_end(crc) == wantCrc;
}

// Why a dial failed, in the words of someone standing at the other machine.
//
// One copy, used by BOTH the single-file fetch and the LIST that -a opens
// with. It was two copies for about an hour on 2026-08-22, and the LIST one
// said only "cannot reach HOST:PORT" — which cost real time during the very
// first -a test, because the true answer was "no network interface on this
// boot" (a QEMU launched without a NIC) and the generic message sent the
// search toward the server, the firewall and the routing instead. A
// diagnostic that omits the diagnosis is worse than none: it looks like it
// tried.
// "tcp!<host>!<port>", built in ONE place because every dialer here wants the
// same string and the same complaint when it will not fit — and a fix applied
// to one of several identical lines is the oldest bug in the trade. The PORT
// is a parameter rather than GET_PORT because the valet answers on 6464 and
// the world answers on whatever a URL names.
//
// Sized from the NAME LIMIT, not from a path limit: "tcp!" + a name of up to
// OS64_RESOLVE_NAME_MAX (253 — DNS's own ceiling, which the dial parser now
// accepts) + "!65535" + NUL. GET_PATH_MAX was 256, and a 250-byte hostname
// silently lost its "!6464" off the end, after which os64_dial rejected the
// string for having no service — a name the resolver would have handled
// perfectly, failing with an error about a port nobody mistyped. The result is
// checked anyway: a truncated dial string fails with the WRONG COMPLAINT, and
// that is the part worth spending three lines to prevent. (Codex review,
// 2026-08-22.)
#define GET_DIAL_MAX (OS64_RESOLVE_NAME_MAX + 16)

static bool build_dialstring(char *buf, size_t cap, const char *host, uint16_t port)
{
    int32_t n = os64_snprintf(buf, cap, "tcp!%s!%u", host, (unsigned)port);
    if (n >= 0 && (size_t)n < cap)
        return true;

    os64_hprintf(OS64_STDERR, "os64get: host name is too long to dial (limit %d)\n",
                 OS64_RESOLVE_NAME_MAX);
    return false;
}


// Resolve routing before receiving any batch file, so conflicting targets
// can be refused while installed files are still untouched.
static int resolve_destination(const char *name, const char *destOverride,
                               const char *lot, const conf_t *conf, char *dest)
{
    if (!name[0] || os64_streq(name, ".") || os64_streq(name, "..")) return GET_USAGE;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || (unsigned char)*p < 32 || (unsigned char)*p == 127) {
            os64_hprintf(OS64_STDERR, "os64get: valet names must be single path components\n");
            return GET_USAGE;
        }
    }
    if (destOverride != NULL)
    {
        // The command line's word is final — but A DIRECTORY NAMES A PLACE,
        // NOT A FILE. `os64get HOST prog /tmp` obviously means "put it in
        // /tmp", and it used to mean "write a file called /tmp": the whole
        // conf below deals in directories, the usage line called DEST a
        // directory, and only this branch disagreed. The failure was at least
        // loud — the publish rename hit the directory and said so — but loud
        // is not the same as right.
        //
        // The rule is cp(1)'s, and has been since 1971: an existing directory
        // receives the file under its own name; anything else IS the path, so
        // fetching under a different name still works.
        os64_dirent_t into;
        bool intoDir = (os64_stat(destOverride, &into) >= 0) &&
                       (into.flags & OS64_DE_DIR) != 0;
        if (!join_path(dest, GET_PATH_MAX, intoDir ? destOverride : NULL,
                                           intoDir ? name : destOverride))
        {
            os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
            return GET_USAGE;
        }
    }
    else
    {
        const char *dir = conf_route(conf, name, lot);
        if (!join_path(dest, GET_PATH_MAX, dir, name))
        {
            os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
            return GET_USAGE;
        }
        if (dir == NULL && conf->path != NULL && conf->anyRule)
            // A conf exists and routes things, just not THIS thing — say
            // so, because "it went to the cwd" is a surprise worth a line.
            os64_hprintf(OS64_STDERR, "os64get: %s has no rule for '%s'; installing in the current directory\n",
                         conf->path, name);
    }

    return GET_OK;
}

static int fetch_stage(const char *host, const char *name, install_file_t *stage,
                       bool quiet, bool force)
{
    const char *dest = stage->dest;
    const char *partPath = stage->part;
    if (install_cancelled()) return GET_CANCELLED;
    // ── Dial ────────────────────────────────────────────────────────────
    // Plan 9's bang path, which libos64 parses into an os64_netdest_t below
    // the syscall boundary (nothing textual crosses it).
    char dialstring[GET_DIAL_MAX];
    if (!build_dialstring(dialstring, sizeof(dialstring), host, GET_PORT))
        return GET_USAGE;

    int64_t conn = os64_dial(dialstring);
    if (conn < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot reach %s:%d — %s\n",
                     host, GET_PORT, os64_dial_reason(conn));
        return GET_DIAL_FAILED;
    }

    // ── Ask ─────────────────────────────────────────────────────────────
    char request[GET_PATH_MAX];
    int32_t reqlen = os64_snprintf(request, sizeof(request), "GET %s\n", name);
    if (reqlen < 0 || (size_t)reqlen >= sizeof(request))
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: could not create request, file name too long\n");
        os64_close((int32_t)conn);
        return GET_REQUEST_FAILED;
    }

    if (os64_write((int32_t)conn, request, (size_t)reqlen) != reqlen)
    {
        os64_hprintf(OS64_STDERR, "os64get: could not send the request\n");
        os64_close((int32_t)conn);
        return GET_REQUEST_FAILED;
    }

    // ── The header ──────────────────────────────────────────────────────
    // Read one byte at a time to the newline. Wasteful in principle and
    // exactly right here: the header is short, and reading in chunks would
    // swallow the front of the FILE into a header buffer we would then have
    // to hand back. A stream has no message boundaries — that is the whole
    // difference between TCP and UDP — so the only safe way to stop exactly
    // at the newline is to not read past it.
    char header[160];
    size_t hlen = 0;
    for (;;)
    {
        char c;
        int64_t n = install_cancelled() ? OS64_INTERRUPTED : os64_read((int32_t)conn, &c, 1);
        if (n != 1)
        {
            if (install_cancelled()) { os64_close((int32_t)conn); return GET_CANCELLED; }
            os64_hprintf(OS64_STDERR, "os64get: server hung up before answering\n");
            os64_close((int32_t)conn);
            return GET_BAD_HEADER;
        }
        if (c == '\n')
            break;
        if (c == '\r')
            continue;                     // a server written on Windows is still a server
        if (hlen + 1 >= sizeof(header))
        {
            os64_hprintf(OS64_STDERR, "os64get: header too long — is that an os64get server?\n");
            os64_close((int32_t)conn);
            return GET_BAD_HEADER;
        }
        header[hlen++] = c;
    }
    header[hlen] = '\0';

    if (header[0] == 'N' && header[1] == 'O')
    {
        // The server's own words, verbatim. It knows why and we do not.
        os64_hprintf(OS64_STDERR, "os64get: server refused '%s':%s\n",
                     name, header[2] ? header + 2 : " (no reason given)");
        os64_close((int32_t)conn);
        return GET_REFUSED;
    }
    if (!(header[0] == 'O' && header[1] == 'K' && header[2] == ' '))
    {
        os64_hprintf(OS64_STDERR, "os64get: unexpected reply '%s'\n", header);
        os64_close((int32_t)conn);
        return GET_BAD_HEADER;
    }

    // "OK <length> <crc>" — split on the single space between the fields.
    const char *lenStart = header + 3;
    const char *p = lenStart;
    while (*p && *p != ' ')
        p++;
    uint64_t expectLen = 0;
    uint32_t expectCrc = 0;
    if (*p != ' ' ||
        !parse_u64(lenStart, p, &expectLen) ||
        !parse_hex32(p + 1, p + 1 + os64_strlen(p + 1), &expectCrc))
    {
        os64_hprintf(OS64_STDERR, "os64get: malformed OK line '%s'\n", header);
        os64_close((int32_t)conn);
        return GET_BAD_HEADER;
    }

    // ── ALREADY HAVE IT? ────────────────────────────────────────────────
    // The header names the length and the CRC, so the file already at DEST
    // can be measured against it right HERE — the transfer is declined before
    // it starts rather than after. Hanging up on the valet mid-sentence costs
    // it nothing: serve_one logs the broken send and takes the next call.
    //
    // -f bypasses this network-saving check. Preparation still avoids
    // archiving and replacing an original that matches the staged download.
    if (!force && local_matches(dest, expectLen, expectCrc))
    {
        os64_close((int32_t)conn);
        if (!quiet)
            os64_printf("%s: unchanged (%lu bytes, crc %08x) — not fetched\n",
                        dest, (unsigned long)expectLen, expectCrc);
        return GET_UNCHANGED;
    }

    // ── Receive into managed scratch ──────────────────────────────────────
    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_close((int32_t)conn);
        return GET_WRITE_FAILED;
    }

    uint8_t buf[GET_CHUNK];
    uint64_t got = 0;
    uint32_t crc = os64_crc32_begin();
    int64_t status = GET_OK;

    while (got < expectLen && !install_cancelled())
    {
        uint64_t want = expectLen - got;
        if (want > sizeof(buf))
            want = sizeof(buf);

        // Fill the buffer before writing it: a socket read answers with what
        // has ARRIVED, a segment or a scheduler pass's worth, and writing
        // each of those would hand ext2 one or two blocks at a time. The
        // disk wants the whole chunk as one run, so reads accumulate until
        // the chunk is full or the stream ends.
        // Progress every 4KB of ARRIVAL, from inside the fill: on a slow
        // link a 64KB chunk takes a while to gather, and a meter that only
        // moved once per chunk would read as a hang. (The first version
        // updated so rarely that Chris watched a 146KB transfer sit on one
        // line for fifty seconds and concluded it had frozen — which, at
        // the speeds the stack then managed, was an entirely reasonable
        // reading. A progress meter exists to distinguish "slow" from
        // "dead", and one that updates less often than a human's patience
        // runs out is doing the opposite of its job.)
        int64_t n = 0;
        while ((uint64_t)n < want && !install_cancelled())
        {
            int64_t got_now = os64_read((int32_t)conn, buf + n, (size_t)(want - (uint64_t)n));
            if (got_now <= 0)
                break;
            n += got_now;
            uint64_t staged = got + (uint64_t)n;
            if (!quiet && (staged % 4096 < (uint64_t)got_now || staged == expectLen))
                os64_printf("\r%s: %lu/%lu bytes", name,
                            (unsigned long)staged, (unsigned long)expectLen);
        }
        if (install_cancelled()) { status = GET_CANCELLED; break; }
        if (n <= 0)
        {
            // Zero or negative means the conversation ended early, and the
            // file we have is a fragment. Say how far it got — "it failed"
            // and "it failed at 4MB of 5" are different amounts of help.
            os64_hprintf(OS64_STDERR,
                         "os64get: connection ended after %lu of %lu bytes\n",
                         (unsigned long)got, (unsigned long)expectLen);
            status = GET_SHORT;
            break;
        }

        if (os64_write((int32_t)out, buf, (size_t)n) != n)
        {
            os64_hprintf(OS64_STDERR, "os64get: write to %s failed (disk full?)\n", partPath);
            status = GET_WRITE_FAILED;
            break;
        }
        // Checksum as it flies past. The file is never held whole in memory,
        // which is why the streaming interface exists.
        crc = os64_crc32_update(crc, buf, (size_t)n);
        got += (uint64_t)n;
    }
    if (!quiet)
        os64_printf("\n");

    os64_close((int32_t)conn);

    if (status == GET_OK && os64_sync((int32_t)out) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: could not commit %s; %s NOT installed\n",
                     partPath, dest);
        status = GET_WRITE_FAILED;
    }

    if (os64_close((int32_t)out) < 0) status = GET_WRITE_FAILED;
    if (install_cancelled()) status = GET_CANCELLED;

    if (status != GET_OK)
    {
        // The run-level cleanup removes this provisional download.
        return (int)status;
    }

    // ── Verify, THEN publish the wire copy ──────────────────────────────
    uint32_t actual = os64_crc32_end(crc);
    if (actual != expectCrc)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: CHECKSUM MISMATCH for %s — got %08x, expected %08x. "
                     "temporary %s rejected; %s NOT installed.\n",
                     name, actual, expectCrc, partPath, dest);
        return GET_CORRUPT;
    }

    if (!quiet)
        os64_printf("%s: %lu bytes, crc %08x, verified\n",
                    dest, (unsigned long)expectLen, actual);
    return GET_OK;
}

// Prepare backups for the batch, recheck originals, then publish. No download
// or backup copying belongs inside the publication loop.
static int publish_run(const bool *downloaded, unsigned count, bool quiet,
                       unsigned unchanged, bool batch)
{
    for (unsigned i = 0; i < count; i++) {
        if (install_cancelled()) return GET_CANCELLED;
        if (downloaded[i]) {
            if (!quiet) os64_printf("%s: preparing replacement\n", stages[i].dest);
            if (!install_prepare(&stages[i])) return GET_ARCHIVE_FAILED;
        }
    }
    for (unsigned i = 0; i < count; i++)
        if (downloaded[i] && !install_recheck(&stages[i])) return GET_ARCHIVE_FAILED;
    if (!install_begin_commit()) return GET_CANCELLED;
    unsigned installed = 0, failed = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!downloaded[i]) continue;
        if (install_commit(&stages[i])) {
            if (stages[i].skip) unchanged++;
            else installed++;
        } else failed++;
    }
    if (batch || !quiet)
        os64_printf("os64get: %u installed, %u unchanged, %u failed\n",
                    installed, unchanged, failed);
    return failed ? GET_PUBLISH_FAILED : GET_OK;
}

// ── ASKING WHAT THE VALET HAS (the LIST verb, 2026-08-22) ───────────────
//
// One connection, one question:
//
//     client -> server:  LIST\n
//     server -> client:  <name> <length> <crc32hex>\n   (one per file)
//                        .\n
//
// A lone "." ends it, which is SMTP's terminator from 1982 and readable by a
// human driving the protocol with telnet — the property RTL8125.md insists on
// for every verb here, because a protocol you can type is one you can debug at
// 1am on a machine with no tooling.
//
// Returns the number of names read, or -1. Names beyond `max` are refused
// loudly rather than silently dropped — a refresh that quietly skips the tail
// of the system is worse than one that fails.
static int32_t fetch_list(const char *host, get_entry_t *entries, int32_t max,
                          uint64_t *totalBytes)
{
    char dialstring[GET_DIAL_MAX];
    if (!build_dialstring(dialstring, sizeof(dialstring), host, GET_PORT))
        return -1;

    int64_t conn = os64_dial(dialstring);
    if (conn < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot reach %s:%d to ask what it has — %s\n",
                     host, GET_PORT, os64_dial_reason(conn));
        return -1;
    }

    if (os64_write((int32_t)conn, "LIST\n", 5) != 5)
    {
        os64_hprintf(OS64_STDERR, "os64get: could not send LIST\n");
        os64_close((int32_t)conn);
        return -1;
    }

    int32_t count = 0;
    *totalBytes = 0;
    for (;;)
    {
        // Byte at a time to the newline, for the same reason the header
        // reader above does it: a stream has no message boundaries, and
        // reading ahead would swallow the next line.
        char line[GET_NAME_MAX + 64];
        size_t len = 0;
        for (;;)
        {
            char c;
            int64_t n = install_cancelled() ? OS64_INTERRUPTED : os64_read((int32_t)conn, &c, 1);
            if (n != 1)
            {
                if (install_cancelled()) { os64_close((int32_t)conn); return -1; }
                os64_hprintf(OS64_STDERR, "os64get: the server hung up in the middle of its list\n");
                os64_close((int32_t)conn);
                return -1;
            }
            if (c == '\n')
                break;
            if (len + 1 < sizeof(line))
                line[len++] = c;
        }
        line[len] = '\0';

        if (len == 1 && line[0] == '.')
            break;                       // the terminator: a complete list

        if (len > 3 && line[0] == 'N' && line[1] == 'O' && line[2] == ' ')
        {
            os64_hprintf(OS64_STDERR, "os64get: the server refused LIST — %s\n", line + 3);
            os64_close((int32_t)conn);
            return -1;
        }

        // "<name> <length> <crc>" — split at the first space; a server that
        // sends a bare name still works, which keeps the verb typeable.
        size_t sp = 0;
        while (sp < len && line[sp] != ' ')
            sp++;
        if (sp == 0 || sp >= GET_NAME_MAX)
        {
            os64_hprintf(OS64_STDERR, "os64get: unusable name in the server's list\n");
            os64_close((int32_t)conn);
            return -1;
        }
        if (count >= max)
        {
            os64_hprintf(OS64_STDERR, "os64get: the server offers more than %ld files — "
                                      "this build cannot hold the whole list, so nothing was fetched\n",
                         (long)max);
            os64_close((int32_t)conn);
            return -1;
        }
        os64_memcpy(entries[count].name, line, sp);
        entries[count].name[sp] = '\0';
        entries[count].length = 0;
        entries[count].crc = 0;
        entries[count].lot[0] = '\0';

        // "<name> <length> <crc> <lot>". A server that sends only the name
        // still works — the entry simply carries no length/crc, and the
        // unchanged check for it falls back to the per-file header test after
        // dialing. Each field is bounded by the NEXT SPACE rather than by the
        // end of the line, which is what lets the line keep growing: the crc
        // parse used to run to end-of-line and wanted exactly eight
        // characters there, so it saw the lot field and rejected the pair it
        // had already read correctly. That is precisely what an os64get older
        // than lots does when it meets a server that has them — it loses the
        // catalogue's crc, re-checks each file against its own per-file
        // header, and is slower for exactly one refresh.
        if (sp < len)
        {
            const char *p = line + sp + 1;
            const char *q = p;
            while (*q != '\0' && *q != ' ')
                q++;
            if (parse_u64(p, q, &entries[count].length))
                *totalBytes += entries[count].length;
            if (*q == ' ')
            {
                const char *cs = q + 1;
                const char *ce = cs;
                while (*ce != '\0' && *ce != ' ')
                    ce++;
                parse_hex32(cs, ce, &entries[count].crc);
                if (*ce == ' ')
                {
                    // "-" is the server's word for "this file came from a
                    // directory I was given no label for", and it must not
                    // become a lot name a conf rule could match.
                    const char *ls = ce + 1;
                    if (!(ls[0] == '-' && ls[1] == '\0'))
                        os64_strcopy(entries[count].lot, sizeof(entries[count].lot), ls);
                }
            }
        }
        count++;
    }

    os64_close((int32_t)conn);
    return count;
}

// A conf that routes `@lot`s against a server that labelled NOTHING is the
// exact shape of a valet started without its `dir=lot` arguments — and the
// symptom is a refresh that reports success while quietly routing everything
// by name. It cost an evening on 2026-08-29: 88 files fetched, no errors, and
// the whole of /tests neither served nor routed.
//
// The test is "no file carried a lot", not "this file carried none": a lot is
// optional per file, so an unlabelled file among labelled ones is ordinary and
// says nothing. Only a catalogue with no lots AT ALL, read by a conf that
// expects them, is evidence of a misconfigured server.
static void warn_if_server_has_no_lots(const conf_t *c, int32_t n)
{
    if (c->nlot == 0 || n <= 0)
        return;
    for (int32_t i = 0; i < n; i++)
        if (entries[i].lot[0] != '\0')
            return;

    os64_hprintf(OS64_STDERR,
                 "os64get: %s routes %ld lot(s), but the server labelled none of its %ld files.\n"
                 "  Everything will route by NAME alone — anything that needed its lot lands\n"
                 "  wherever the `*` rule points. Start os64serve with `<directory>=<lot>`\n"
                 "  arguments, and check the lot's directory is being served at all (a\n"
                 "  SUBDIRECTORY of a served directory is not served; it needs its own word).\n",
                 c->path ? c->path : "the conf", (long)c->nlot, (long)n);
}

// One file's lot, for the single-file fetch that would otherwise never learn
// it. `-a` already holds every lot in the catalogue it asked for; a lone
// `os64get HOST fputest` asks only for the file.
//
// A LIST that fails is NOT fatal here and must not be: the caller proceeds
// name-routed, which is exactly what os64get did before lots existed. Failing
// the fetch because an optional refinement was unavailable would trade a
// working command for a tidier one.
static void lookup_lot(const char *host, const char *name, const conf_t *c,
                       char *out, size_t cap)
{
    out[0] = '\0';
    uint64_t ignored = 0;
    int32_t n = fetch_list(host, entries, GET_MAX_LIST, &ignored);
    warn_if_server_has_no_lots(c, n);
    for (int32_t i = 0; i < n; i++)
        if (os64_streq(entries[i].name, name))
        {
            os64_strcopy(out, cap, entries[i].lot);
            return;
        }
}

// ── THE WORLD'S DIALECT (2026-09-02) ────────────────────────────────────
//
// An operand shaped like `http://host[:port]/path` means the internet. A bare
// word still means the valet, and nothing above this line changes: the two
// dialects are told apart by the `://` a URL must carry, never by guesswork
// about what a word might be. (wget accepts a bare `host/path` and assumes
// http; here that assumption would collide with every name the valet serves.)
//
// WHAT A URL FETCH DELIBERATELY DOES NOT DO, and why each:
//
//   - IT DOES NOT ROUTE THROUGH os64get.conf. That file maps the valet's
//     NAMES onto this machine's system directories, and its last rule is
//     `* = /bin`. Routing a URL through it would install a web page into
//     /bin as a program. The conf answers "where does this piece of the
//     SYSTEM belong", and a page off the internet is not one.
//   - IDENTITY-CODED HTTP HAS NO CHECKSUM, so it cannot tell "complete" from
//     "correct", and says so rather than implying otherwise. HTTP offers a
//     length or chunk framing; a body cut before either is satisfied fails
//     loudly in either coding, and everything past that is the server's
//     word. gzip's trailer does carry a CRC and decoded size, and both are
//     verified before publish; neither is authentication, so the server
//     still owns what the bytes mean.
//   - IT DOES NOT LET A REDIRECT NAME THE FILE. Following one is ordinary
//     (see url_ask); letting the far end choose what appears in somebody's
//     directory is not, so the name is settled from the typed address before
//     the first request goes out.
//
// What it KEEPS is the property this whole program is built around: the bytes
// land in managed scratch storage, and only a validated body reaches the real name. On
// ext2 that final replacement is atomic, so a transfer that dies halfway
// leaves the file that was there exactly where it was. A FAT destination has
// syscall 43's documented remove-first publication window.

// HOW LONG A SILENT ORIGIN IS WAITED ON. An internet host can finish the
// handshake and then say nothing — a stalled proxy, a server that fell over
// mid-reply, a middlebox that ate the rest — and a plain read waits
// OS64_WAIT_FOREVER for it, which turned every such stall into a fetch that
// hung until someone pressed Ctrl+C, and a script that never finished at
// all. Thirty seconds of silence is the deadline; a slow origin still
// arrives, because it is IDLE time — the wait between bytes — not the whole
// transfer that is bounded. (Codex review round 4, 2026-09-03.)
#define URL_IDLE_MS 30000

typedef struct {
    int32_t handle;
    bool    silent;    // the deadline expired: the failure is a stall, not a break
} url_source_t;

// The stream's bytes come from the dialed connection. A function and a
// context rather than a handle, because http.c is written to be driven by a
// memory buffer too — that is what lets tools/test_http_host.sh hand the
// parser a reply one byte at a time and find the bugs that live where a
// token straddles two reads.
static int64_t url_source_read(void *ctx, void *buf, size_t cap)
{
    url_source_t *src = (url_source_t *)ctx;
    if (install_cancelled()) return OS64_INTERRUPTED;
    int64_t n = os64_read_for(src->handle, buf, cap, URL_IDLE_MS);
    if (n == OS64_ERR_TIMEOUT)
    {
        // The stream's vocabulary is bytes / end / broke; a stall is a
        // broke with a better reason, and the reason is remembered here so
        // the message can say "silent", not "broke" — the difference between
        // blaming the wire and blaming the server.
        src->silent = true;
        return -1;
    }
    return n;
}

// URL fetches are sequential, so one pair of BSS buffers serves every one.
// Keeping 128 KiB off the stack matters even though the current user stack is
// larger: the buffers are transfer state, not call state, and gzip needs an
// input chunk and a decoded-output chunk alive at the same time.
static uint8_t urlWire[GET_CHUNK];
static uint8_t urlPlain[GET_CHUNK];

typedef struct {
    uint64_t received;       // body bytes the framing handed over: the wire's count
    uint64_t produced;       // representation bytes staged in the temporary file
} url_body_result_t;

static int gzip_body_result(os64_gzip_status_t status)
{
    // An unsupported method and a local expansion policy are both honest
    // "this reply cannot be decoded here" answers. Every other terminal gzip
    // result says the bytes claimed to be gzip but did not form one complete,
    // verified gzip stream, which is corruption rather than missing support.
    if (status == OS64_GZIP_UNSUPPORTED || status == OS64_GZIP_LIMIT)
        return GET_UNSUPPORTED;
    return GET_CORRUPT;
}

// The wire length is only known in advance under a Content-Length; a chunked
// or close-framed gzip body gets the ceiling and nothing finer.
static uint64_t gzip_body_limit(const http_response_t *reply)
{
    if (!reply->hasLength)
        return URL_GZIP_OUTPUT_MAX;

    uint64_t limit;
    if (reply->length > URL_GZIP_OUTPUT_MAX / URL_GZIP_RATIO_MAX)
        limit = URL_GZIP_OUTPUT_MAX;
    else
        limit = reply->length * URL_GZIP_RATIO_MAX;
    if (limit < URL_GZIP_OUTPUT_FLOOR)
        limit = URL_GZIP_OUTPUT_FLOOR;
    if (limit > URL_GZIP_OUTPUT_MAX)
        limit = URL_GZIP_OUTPUT_MAX;
    return limit;
}

// Read one HTTP message body into an already-open temporary file.
//
// THE FRAMING IS THE BODY READER'S BUSINESS (http.h): what comes out of it is
// the body's bytes and nothing else — a length, chunks, or the close, taken
// off — and when it answers 0 its `result` says whether the body ENDED or
// was merely STOPPED. This function decides only what those bytes ARE: the
// file itself, or a gzip stream the file is inside. The decoder's output is
// written as it streams but stays provisional by construction: the caller
// publishes the temporary file only after this returns OK, and for gzip that takes
// the framing satisfied AND OS64_GZIP_DONE — every member trailer verified,
// no trailing data.
//
// A body the framing calls CUT or BROKE is NOT this function's verdict to
// give. The decoder is told the input is final only when the body is WHOLE,
// so a truncated transfer reaches it as "no more for now" and this returns
// OK with the decoder's work unfinished; the caller reads `body->result` and
// says "cut short", the same as it would for an identity body. (The earlier
// shape, which framed the body itself, looped forever on a length-framed
// gzip reply that closed early — Codex review of PR #54.)
static int receive_url_body(http_body_t *body, const http_response_t *reply,
                            int32_t out, const char *partPath, const char *name,
                            bool quiet, bool gzipEncoded,
                            url_body_result_t *result)
{
    // The counters are the caller's to read on EVERY return — a refused
    // allocation included, since the meter's last tick reports them — so
    // they are set before anything can fail.
    result->received = 0;
    result->produced = 0;

    os64_gzip_t *decoder = NULL;
    uint64_t gzipLimit = 0;
    if (gzipEncoded) {
        gzipLimit = gzip_body_limit(reply);
        decoder = os64_gzip_create(gzipLimit);
        if (decoder == NULL) {
            os64_hprintf(OS64_STDERR, "os64get: cannot allocate the gzip decoder\n");
            return GET_WRITE_FAILED;
        }
    }

    int status = GET_OK;
    os64_gzip_status_t gzipStatus = OS64_GZIP_NEED_INPUT;
    const char *unit = gzipEncoded ? " wire" : "";
    bool over = false;

    while (!over && !install_cancelled()) {
        // Fill the buffer before writing it, for the disk's sake: a socket
        // read answers with what has ARRIVED — a segment, or a scheduler
        // pass's worth — and writing each of those hands ext2 a block or two
        // at a time. Progress ticks from INSIDE the fill, every 4KB of
        // arrival, so a slow link reads as slow rather than as hung. The
        // meter counts arrival, which for gzip is the wire's bytes even
        // though the file being staged grows faster.
        size_t filled = 0;
        while (filled < sizeof(urlWire) && !install_cancelled()) {
            int64_t n = http_body_read(body, urlWire + filled, sizeof(urlWire) - filled);
            if (n <= 0) { over = true; break; }
            filled += (size_t)n;
            uint64_t staged = result->received + filled;
            if (!quiet && (staged % 4096 < (uint64_t)n ||
                           (reply->hasLength && staged == reply->length))) {
                if (reply->hasLength)
                    os64_printf("\r%s: %lu/%lu%s bytes", name,
                                (unsigned long)staged, (unsigned long)reply->length, unit);
                else
                    os64_printf("\r%s: %lu%s bytes", name, (unsigned long)staged, unit);
            }
        }
        result->received += filled;

        bool finalInput = over && body->result == HTTP_BODY_DONE;

        if (!gzipEncoded) {
            if (filled != 0 &&
                os64_write(out, urlWire, filled) != (int64_t)filled) {
                os64_hprintf(OS64_STDERR,
                             "os64get: write to %s failed (disk full?)\n",
                             partPath);
                status = GET_WRITE_FAILED;
                break;
            }
            result->produced += filled;
        } else if (filled != 0 || finalInput) {
            const uint8_t *input = urlWire;
            size_t inputLeft = filled;

            for (;;) {
                if (install_cancelled()) { status = GET_CANCELLED; break; }
                uint8_t *output = urlPlain;
                size_t outputLeft = sizeof(urlPlain);
                gzipStatus = os64_gzip_process(decoder, &input, &inputLeft,
                                                &output, &outputLeft,
                                                finalInput);
                size_t produced = sizeof(urlPlain) - outputLeft;
                if (produced != 0 &&
                    os64_write(out, urlPlain, produced) != (int64_t)produced) {
                    os64_hprintf(OS64_STDERR,
                                 "os64get: write to %s failed (disk full?)\n",
                                 partPath);
                    status = GET_WRITE_FAILED;
                    break;
                }
                result->produced += produced;

                if (gzipStatus == OS64_GZIP_NEED_OUTPUT)
                    continue;
                if (gzipStatus == OS64_GZIP_NEED_INPUT) {
                    // Asked for more with the whole body already given is a
                    // stream that ends before its trailer; asked for more
                    // while holding some is the decoder breaking its word.
                    if (finalInput) {
                        gzipStatus = OS64_GZIP_TRUNCATED;
                        status = GET_CORRUPT;
                    } else if (inputLeft != 0) {
                        gzipStatus = OS64_GZIP_BAD_ARGUMENT;
                        status = GET_CORRUPT;
                    }
                    break;
                }
                if (gzipStatus == OS64_GZIP_DONE)
                    break;

                status = gzip_body_result(gzipStatus);
                break;
            }
        }

        if (status != GET_OK)
            break;
        if (finalInput && gzipEncoded && gzipStatus != OS64_GZIP_DONE)
            status = gzip_body_result(gzipStatus);
    }

    if (install_cancelled()) status = GET_CANCELLED;
    if (gzipEncoded && status != GET_OK && status != GET_CANCELLED) {
        if (gzipStatus == OS64_GZIP_LIMIT)
            os64_hprintf(OS64_STDERR,
                         "os64get: gzip response expands past its %lu-byte safety limit"
                         " (at most %lux wire size and %lu MiB overall)"
                         " — %s NOT written\n",
                         (unsigned long)gzipLimit,
                         (unsigned long)URL_GZIP_RATIO_MAX,
                         (unsigned long)(URL_GZIP_OUTPUT_MAX / 1024 / 1024),
                         partPath);
        else if (status != GET_WRITE_FAILED)
            os64_hprintf(OS64_STDERR, "os64get: bad gzip response: %s"
                         " — %s NOT written\n",
                         os64_gzip_status_name(gzipStatus), partPath);
    }
    os64_gzip_destroy(decoder);
    return status;
}

// The name a URL suggests for the thing it points at: the last path segment,
// query dropped. A path ending in '/' names a directory, and the web has
// answered that with index.html since the first server in 1993.
//
// "." and ".." are refused rather than translated. They name a DIRECTORY, and
// the interesting case is not the honest one — it is a URL ending in `/..`,
// where a tool that shrugged and picked something would be picking a place
// the person never typed.
static bool url_basename(const http_url_t *url, char *out, size_t cap)
{
    const char *end = url->path;
    while (*end != '\0' && *end != '?')
        end++;
    const char *start = end;
    while (start > url->path && start[-1] != '/')
        start--;

    size_t len = (size_t)(end - start);
    if (len == 0)
        return os64_strcopy(out, cap, "index.html") < cap;
    if ((len == 1 && start[0] == '.') ||
        (len == 2 && start[0] == '.' && start[1] == '.'))
        return false;
    if (len + 1 > cap)
        return false;

    for (size_t i = 0; i < len; i++)
        out[i] = start[i];
    out[len] = '\0';
    return true;
}

// ── THE TLS PROXY, AND WHAT IT COSTS (2026-09-02) ───────────────────────
//
// os64 has no TLS and is not going to grow one here; BROWSER.md's first
// ruling is that it gets BORROWED when its day comes. Until then the ruling
// names a stopgap, and this is it: a machine that DOES have a TLS makes the
// call and hands the answer back in plain HTTP. `$https_proxy` names it,
// `$http_proxy` names one for ordinary http, and `$no_proxy` names the hosts
// that skip both — the CERN convention from 1992, honoured by every fetcher
// since. Environment variables rather than a config file because a proxy is
// a property of where this machine is SITTING, not of what it is: carry the
// P5 to another network and the answer changes without the system changing.
//
//     export https_proxy=http://10.0.2.2:8888/
//     os64get https://example.com/
//
// THE SCHEME PICKS THE VARIABLE, and that is worth more than tidiness. os64's
// only reason for a proxy is TLS, so the ordinary setup is a proxy for https
// and a direct road for everything else — and one variable covering both
// would silently reroute the plain-HTTP fetches that were working perfectly,
// through a machine that may have no way to reach them. Learned live rather
// than reasoned out: with a single setting, a fetch of the local test server
// at 10.0.2.2 went out through a proxy on the host, where that address means
// nothing at all, and came back 502.
//
// WHAT IT COSTS, and this program says so out loud on every proxied fetch
// rather than letting it go quiet: THE PROXY SEES EVERYTHING IN THE CLEAR.
// The encrypted leg runs from the proxy to the origin; the leg from here to
// the proxy is plain text on the wire, and the proxy itself holds the whole
// conversation unencrypted in its memory. For reading public pages that is
// exactly the trade BROWSER.md intends. For anything carrying a password it
// is not TLS, must never be described as though it were, and the reason the
// notice is printed and not merely documented is that a security property
// nobody is reminded of is a security property people forget.
//
// Note what is NOT special-cased: a proxied fetch is an ordinary plain-HTTP
// conversation with a different peer and a longer request line. The header
// parser, the body loop, the staging discipline and every refusal above are
// untouched by it.
typedef struct {
    bool     inUse;
    char     host[HTTP_HOST_MAX];
    uint16_t port;
} proxy_t;

// ASCII case folding for one byte. A host out of http_url_parse is already
// lowercased (a DNS name is case-insensitive by definition); a `$no_proxy`
// entry is whatever somebody typed, so only that side needs folding.
static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

// A proxy setting's value, in either spelling people actually use: the full
// URL everyone writes ("http://host:8888/") and the bare "host:port" that
// gets typed in a hurry. A value that is neither is refused BY NAME rather
// than ignored, because a proxy setting that silently does nothing sends
// every https fetch to a "no TLS" refusal that names the wrong cause.
static bool proxy_take(const char *value, const char *variable, proxy_t *out)
{
    http_url_t url;
    http_url_result_t rc = http_url_parse(value, &url);

    if (rc == HTTP_URL_OK)
    {
        if (!os64_streq(url.scheme, "http"))
        {
            os64_hprintf(OS64_STDERR,
                         "os64get: $%s is %s, and the road TO a proxy is the plain one"
                         " — os64 has no TLS to reach it with\n", variable, url.scheme);
            return false;
        }
        os64_strcopy(out->host, sizeof(out->host), url.host);
        out->port = url.port;
        out->inUse = true;
        return true;
    }

    if (rc != HTTP_URL_NOT_A_URL)
    {
        os64_hprintf(OS64_STDERR, "os64get: $%s is not a proxy address (%s): %s\n",
                     variable, http_url_reason(rc), value);
        return false;
    }

    // "host:port", or a bare "host" meaning the customary 8080.
    const char *colon = NULL;
    for (const char *p = value; *p != '\0'; p++)
        if (*p == ':')
            colon = p;

    size_t hostLen = colon != NULL ? (size_t)(colon - value) : os64_strlen(value);
    if (hostLen == 0 || hostLen >= sizeof(out->host))
    {
        os64_hprintf(OS64_STDERR, "os64get: $%s names no usable host: %s\n", variable, value);
        return false;
    }
    for (size_t i = 0; i < hostLen; i++)
        out->host[i] = value[i];
    out->host[hostLen] = '\0';

    out->port = 8080;
    if (colon != NULL)
    {
        uint64_t port = 0;
        if (!os64_parse_u64(colon + 1, &port) || port == 0 || port > 65535)
        {
            os64_hprintf(OS64_STDERR, "os64get: $%s has no usable port: %s\n", variable, value);
            return false;
        }
        out->port = (uint16_t)port;
    }
    out->inUse = true;
    return true;
}

// `$no_proxy`: the hosts to reach DIRECTLY even when a proxy is set. Entries
// are separated by commas or spaces; `*` means every host; a bare name
// matches that host and anything under it, so "example.com" also covers
// "www.example.com" but never "notexample.com", and a leading dot is accepted
// because half the world writes ".example.com" for the same thing.
//
// THIS IS NOT DECORATION HERE, it is the difference between a working harness
// and a baffling one. QEMU's guest reaches the host at 10.0.2.2, an address
// that means nothing anywhere else — so a proxy running ON the host, asked to
// fetch `http://10.0.2.2:8080/`, dials into the void and times out. Every
// address that is only meaningful from where os64 is sitting has that shape.
static bool no_proxy_covers(const char *host)
{
    const char *list = os64_getenv("no_proxy");
    if (list == NULL || list[0] == '\0')
        return false;

    size_t hostLen = os64_strlen(host);
    const char *p = list;
    while (*p != '\0')
    {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t')
            p++;

        size_t len = (size_t)(p - start);
        if (len == 0)
            continue;
        if (len == 1 && start[0] == '*')
            return true;
        if (start[0] == '.')
        {
            start++;
            len--;
        }
        if (len == 0 || len > hostLen)
            continue;

        // Match the whole host, or a dot-bounded tail of it. The dot is what
        // keeps "example.com" from covering "notexample.com".
        const char *tail = host + (hostLen - len);
        if (len != hostLen && tail[-1] != '.')
            continue;

        size_t i = 0;
        while (i < len && tail[i] == lower_ascii(start[i]))
            i++;
        if (i == len)
            return true;
    }
    return false;
}

// WHICH PROXY, IF ANY, CARRIES THIS URL. The variable is chosen by the URL's
// SCHEME — `$https_proxy` for https, `$http_proxy` for http — which is the
// convention every fetcher has followed since these names appeared, and here
// it is load-bearing rather than pedantry: os64's whole reason for having a
// proxy is TLS, so the ordinary setup is a proxy for https and a direct road
// for everything else. One variable covering both would silently reroute the
// plain-HTTP fetches that were working perfectly, through a machine that may
// have no way to reach them at all. There is deliberately NO fallback between
// the two: which variable answers should be predictable from the address.
static bool proxy_for(const http_url_t *url, proxy_t *out)
{
    out->inUse = false;

    const char *variable = os64_streq(url->scheme, "https") ? "https_proxy" : "http_proxy";
    const char *value = os64_getenv(variable);
    if (value == NULL || value[0] == '\0')
        return true;

    if (no_proxy_covers(url->host))
        return true;

    return proxy_take(value, variable, out);
}

// A DEST names a place on THIS machine, so an operand shaped like a URL in
// that slot is somebody meaning "fetch this" and being understood as "write
// it there". Refuse rather than obey: os64 paths are '/'-separated, so
// "https://example.com/" as a file name is not a thing anyone has ever meant,
// and the failure it produces otherwise is a complaint about a directory that
// does not exist — which sends the reader looking at their filesystem for a
// mistake they made in their address.
//
// Guarding the SHAPE and not just the wording of one message: this program
// prints several addresses a person might reasonably paste back at it, and
// the sentence that caused it the first time will not be the last one.
static bool dest_is_a_url(const char *dest)
{
    if (dest == NULL)
        return false;
    http_url_t probe;
    return http_url_parse(dest, &probe) != HTTP_URL_NOT_A_URL;
}

// ── WHERE A REDIRECT POINTS, AND WHETHER THIS FETCH MAY GO THERE ────────
//
// FOLLOWING IS WHAT A FETCHER DOES, and the plain-HTTP web leaves no choice:
// most of it is forwarding addresses now, and a program that stops at the
// first one can read almost none of it. What following COSTS is that the
// bytes may arrive from a machine whose name nobody typed — so every hop is
// announced, every hop is judged by the same rules the typed address was,
// and the file's name is settled before the first one (see fetch_url).
//
// ONE FUNCTION DECIDES. The loop that follows a redirect and the sentence
// that explains an unfollowed one read the same verdict out of the same
// call, because two pieces of code answering "is this followable" separately
// is how a program ends up explaining a refusal it did not make. http.c
// carries the scar from the last time one rule was spelled twice in this
// program: a field name judged by two rules let the malformed LONG form
// through while the short one was refused, and chunk markers were published
// as a file.

// HOW FAR A FETCH IS WILLING TO BE SENT. Five is RFC 2068 §10.3's own
// recommendation from 1997, and the number matters far less than the fact
// that there is one: a site needing six hops to hand over one file is
// misconfigured, and a site needing infinitely many is a loop with a friendly
// face. wget allows twenty and curl thirty; when this stops, it prints the
// address it stopped at, so the trail can be picked up by hand.
#define URL_REDIRECT_MAX 5

// WHICH REDIRECTS ARE FOLLOWED, and why the famous distinction between them
// does not arise here. 301/302 and 307/308 differ only in what a client may
// do to the METHOD: the older pair were so widely implemented as "retry it
// as a GET" that the newer pair had to be invented to mean "and keep the
// method you had". os64get only ever sends GET, so all four say the same
// thing to it — and 303 (See Other), whose entire meaning is "GET this other
// thing instead", says it too.
//
// The 3xx codes deliberately NOT here:
//   300 Multiple Choices — a list for a person to pick from; its Location is
//       a hint, and choosing on somebody's behalf is not a fetcher's job.
//   304 Not Modified — an answer to a conditional request os64get never makes.
//   305 Use Proxy — a stranger telling this machine to route its traffic
//       through a machine of the stranger's choosing. Every browser dropped
//       it for that reason; so does this.
static bool redirect_is_followed(int32_t status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

typedef enum {
    HOP_FOLLOW = 0,   // a whole address, and a road from here to it
    HOP_NONE,         // a redirect that does not say where to
    HOP_TOO_LONG,     // the address it spells is longer than this will hold
    HOP_UNUSABLE,     // it spells something that is not a usable address
    HOP_SCHEME,       // an address of a kind os64get does not fetch
    HOP_NO_TLS,       // https, and no proxy on this machine to reach it through
    HOP_PROXY,        // the proxy setting that would carry it is unusable
    HOP_SELF          // it points back at the address that just answered
} hop_t;

typedef struct {
    hop_t             verdict;
    char              whole[HTTP_URL_TEXT_MAX];   // the address, spelled out
    http_url_t        target;
    http_url_result_t parse;    // HOP_UNUSABLE: what the parser objected to
    proxy_t           proxy;    // HOP_FOLLOW: who carries the next hop
} redirect_t;

// The same place, twice. Compared by the PIECES rather than the spelling,
// because the parse has already folded the host's case and filled in the
// scheme's default port — so `http://HOST/x` and `http://host:80/x` are one
// address here, exactly as they are on the wire.
static bool same_address(const http_url_t *a, const http_url_t *b)
{
    return a->port == b->port &&
           os64_streq(a->scheme, b->scheme) &&
           os64_streq(a->host, b->host) &&
           os64_streq(a->path, b->path);
}

// THE WHOLE ADDRESS FIRST, AND EVERY QUESTION IS ASKED OF THAT. A Location
// may be root-relative (`/login`), scheme-relative (`//cdn.example/x`) or
// relative to the page (`index.html`), and none of those parses as a URL on
// its own — so a judgement made on the raw header judged an empty host,
// asked the proxy policy about nothing, and let a malformed relative path
// through to be offered as a command os64get itself would refuse. Resolve,
// then parse, then decide. (Codex review round 6, 2026-09-03.)
static void redirect_read(const http_url_t *from, const char *location, redirect_t *out)
{
    os64_memset(out, 0, sizeof(*out));

    if (location[0] == '\0')
    {
        out->verdict = HOP_NONE;
        return;
    }
    if (!http_url_absolute(from, location, out->whole, sizeof(out->whole)))
    {
        out->verdict = HOP_TOO_LONG;
        return;
    }

    out->parse = http_url_parse(out->whole, &out->target);
    if (out->parse == HTTP_URL_SCHEME || out->parse == HTTP_URL_NOT_A_URL)
    {
        // HTTP_URL_NOT_A_URL does not mean here what it means on the command
        // line. What resolution hands back is either `scheme://...` or a
        // reference that named its own scheme and was copied through
        // untouched, so a refusal at this point is the second kind —
        // `mailto:`, `data:`, `tel:` — an address of a sort this program
        // does not fetch, rather than a bare word meant for the valet.
        out->verdict = HOP_SCHEME;
        return;
    }
    if (out->parse != HTTP_URL_OK)
    {
        out->verdict = HOP_UNUSABLE;
        return;
    }

    // A REDIRECT TO THE ADDRESS THAT JUST ANSWERED is a server that has lost
    // its place, and the hop limit would eventually say so — five requests
    // later, in words about counting rather than about what happened.
    if (same_address(from, &out->target))
    {
        out->verdict = HOP_SELF;
        return;
    }

    // THE PROXY ASKED ABOUT IS THE TARGET'S, NOT THIS FETCH'S. They are
    // different questions and the scheme decides each one separately — a
    // redirect from http to https is carried by $https_proxy no matter what
    // carried the request that produced it. Passing the current fetch's proxy
    // in here looked obviously right and was obviously wrong the first time a
    // plain-HTTP page redirected to https with $https_proxy set: the answer
    // said "out of reach" about an address one hop away.
    if (!proxy_for(&out->target, &out->proxy))
    {
        out->verdict = HOP_PROXY;
        return;
    }

    // WHETHER https IS REACHABLE IS A QUESTION ABOUT THIS MACHINE, not about
    // the address. Most of the plain-HTTP web now redirects to https; whether
    // that is a dead end or an ordinary next hop is exactly what $https_proxy
    // answers, and it is answered here per hop rather than once per run.
    if (os64_streq(out->target.scheme, "https") && !out->proxy.inUse)
    {
        out->verdict = HOP_NO_TLS;
        return;
    }

    out->verdict = HOP_FOLLOW;
}

// The redirect in words, for the ones this fetch will not follow. Every
// sentence names the ADDRESS: somebody told "cannot follow that" and not
// told where "that" was has to go and read the headers themselves, which is
// the position they were in before they had a fetcher.
static void redirect_explain(const redirect_t *hop)
{
    switch (hop->verdict)
    {
    case HOP_FOLLOW:
        return;
    case HOP_NONE:
        os64_hprintf(OS64_STDERR, "os64get: ...and does not say where to\n");
        return;
    case HOP_TOO_LONG:
        os64_hprintf(OS64_STDERR, "os64get: ...and the address it points at is longer"
                                  " than os64get will hold\n");
        return;
    case HOP_UNUSABLE:
        os64_hprintf(OS64_STDERR, "os64get: it points at %s, which is not a usable"
                                  " address — %s\n", hop->whole, http_url_reason(hop->parse));
        return;
    case HOP_SCHEME:
        os64_hprintf(OS64_STDERR, "os64get: it points at %s, and os64get fetches http"
                                  " and https addresses\n", hop->whole);
        return;
    case HOP_NO_TLS:
        os64_hprintf(OS64_STDERR,
                     "os64get: it points at %s, which is https — and os64 has no TLS of"
                     " its own.\n"
                     "os64get: set $https_proxy to a machine that has one and this becomes"
                     " an ordinary hop.\n", hop->whole);
        return;
    case HOP_PROXY:
        os64_hprintf(OS64_STDERR, "os64get: it points at %s, and the proxy setting that"
                                  " would carry it is unusable (above)\n", hop->whole);
        return;
    case HOP_SELF:
        os64_hprintf(OS64_STDERR, "os64get: it points back at %s, the address that just"
                                  " answered — the server is going in a circle\n", hop->whole);
        return;
    }
}

// A 3xx that is not one of the five. Each of them means something a fetch
// cannot act on by itself, and naming which is the difference between "the
// server said no" and "the server said something os64get chose not to obey".
//
// THESE EARN GET_REFUSED, NOT GET_REDIRECT, and the split is about what
// happened rather than about the first digit: none of them SENT this fetch
// anywhere. The server answered with a list, or with an instruction about
// routing, and that is its final word on the page — the same shape as a 404.
// GET_REDIRECT is for a road that was taken and did not arrive.
static void redirect_unfollowed(const http_response_t *reply)
{
    const char *why;
    switch (reply->status)
    {
    case 300: why = "a list of choices is for a person to pick from";                 break;
    case 304: why = "nothing was asked conditionally, so 'not modified' answers nothing"; break;
    case 305: why = "'use this proxy' is a stranger choosing this machine's route";   break;
    default:  why = "os64get does not know what that one means";                      break;
    }
    if (reply->location[0] != '\0')
        os64_hprintf(OS64_STDERR, "os64get: it names %s, and %s\n", reply->location, why);
    else
        os64_hprintf(OS64_STDERR, "os64get: %s\n", why);
}

// THE COMMAND IS QUOTED, BECAUSE THE ADDRESS IN IT IS THE SERVER'S. husk
// splits an unquoted line at `;` and `&&`, and a URL may legally carry either
// (`/x;reboot` is a path), so printed bare this line was a command that ran
// something the person never typed the moment they copied it. Single quotes
// hide everything from husk except a single quote, and husk has no backslash
// to escape one with — so an address holding one is shown as an address and
// offered as no command at all. (Codex review round 5, 2026-09-03.)
static void print_by_hand(const char *whole)
{
    for (const char *q = whole; *q != '\0'; q++)
        if (*q == '\'')
        {
            os64_hprintf(OS64_STDERR, "os64get: the address is %s — it holds a quote"
                                      " character, so type it with care.\n", whole);
            return;
        }
    os64_hprintf(OS64_STDERR, "os64get: to go on from there:  os64get '%s'\n", whole);
}

// The server's own words for its own decision, or ours saying it had none.
static const char *reply_reason(const http_response_t *reply)
{
    return reply->reason[0] != '\0' ? reply->reason : "(no reason given)";
}

// One conversation with the world: the connection, the stream reading it,
// and the head that came back. They are held together because a redirect
// throws all three away and starts another, and the body that finally
// arrives has to be read from the LAST one. The stream holds a pointer to
// `src`, so one of these is initialised where it lives and never copied.
typedef struct {
    int32_t         conn;
    url_source_t    src;
    http_stream_t   stream;
    http_response_t reply;
    http_url_t      answered;    // the address that served the body
} url_reply_t;

// Ask, and keep asking wherever the answers point, until an answer is the
// thing itself. On GET_OK the connection is open and positioned at the first
// byte of the body and `answered` names who served it; on anything else the
// connection is closed and the reason has been printed.
//
// `urlText` is what the PERSON typed and is what the first hop's complaints
// quote — a complaint about a URL should quote the URL, not a reassembled
// version differing in some way the reader then has to account for. Every
// hop after that quotes the address os64get went to instead, since that is
// the one the answer came from and the one nobody has seen yet.
static int url_ask(url_reply_t *answer, const http_url_t *start, const char *urlText,
                   const proxy_t *startProxy, bool quiet)
{
    http_url_t  current = *start;
    proxy_t     proxy   = *startProxy;
    char        currentText[HTTP_URL_TEXT_MAX];
    const char *say = urlText;
    bool        toldAboutTls = false;
    int         hops = 0;

    for (;;)
    {
        if (install_cancelled()) return GET_CANCELLED;
        // THE PROXY NOTICE IS NOT BEHIND -q, deliberately, and the precedent
        // is this program's own: warn_if_server_has_no_lots prints
        // regardless, because a warning you silenced along with the progress
        // is a warning you will not see on the run that mattered. What the
        // reader needs is different for the two schemes — an http fetch
        // through a proxy was never encrypted and the proxy is merely
        // another hop, while an https fetch through one LOOKS like the
        // encrypted thing it is not.
        //
        // The long version is printed ONCE, at the first https hop that
        // needs it: what it warns about is true of the whole fetch, and a
        // trail of redirects would otherwise repeat three lines of prose at
        // every step until the warning became the thing you scroll past.
        if (proxy.inUse)
        {
            if (os64_streq(current.scheme, "https") && !toldAboutTls)
            {
                os64_hprintf(OS64_STDERR,
                             "os64get: via the proxy at %s:%u, which TERMINATES the TLS — it holds this"
                             " page in the clear, and the leg from here to it is plain text."
                             " This is not end-to-end encryption.\n",
                             proxy.host, (unsigned)proxy.port);
                toldAboutTls = true;
            }
            else
            {
                os64_hprintf(OS64_STDERR, "os64get: via the proxy at %s:%u\n",
                             proxy.host, (unsigned)proxy.port);
            }
        }

        // ── Dial: the proxy if there is one, the origin if not ──────────
        // The CONNECTION goes to whoever is answering; the ADDRESS stays in
        // the request line. That split is the whole of proxying.
        const char *peerHost = proxy.inUse ? proxy.host : current.host;
        uint16_t    peerPort = proxy.inUse ? proxy.port : current.port;

        char dialstring[GET_DIAL_MAX];
        if (!build_dialstring(dialstring, sizeof(dialstring), peerHost, peerPort))
            return GET_USAGE;

        int64_t conn = os64_dial(dialstring);
        if (conn < 0)
        {
            os64_hprintf(OS64_STDERR, "os64get: cannot reach %s%s:%u — %s\n",
                         proxy.inUse ? "the proxy at " : "", peerHost,
                         (unsigned)peerPort, os64_dial_reason(conn));
            // At the FIRST hop this is the address that was typed, and 3
            // says so. Past it, a SERVER that cannot be reached is one
            // another server chose, and a script must be able to tell those
            // apart: the road did not arrive, which is what 15 means (Codex
            // review of PR #60). A PROXY that cannot be reached is neither —
            // it is this machine's own setting, the same defect at whichever
            // hop it is noticed, and it keeps the answer the first hop gives.
            return (hops > 0 && !proxy.inUse) ? GET_REDIRECT : GET_DIAL_FAILED;
        }

        // ── Ask ─────────────────────────────────────────────────────────
        char request[HTTP_LINE_MAX];
        if (!http_request(request, sizeof(request), &current, proxy.inUse))
        {
            os64_hprintf(OS64_STDERR, "os64get: the request does not fit — the path is too long\n");
            os64_close((int32_t)conn);
            return GET_REQUEST_FAILED;
        }
        size_t reqlen = os64_strlen(request);
        if (os64_write((int32_t)conn, request, reqlen) != (int64_t)reqlen)
        {
            os64_hprintf(OS64_STDERR, "os64get: could not send the request\n");
            os64_close((int32_t)conn);
            return GET_REQUEST_FAILED;
        }

        // ── The reply's head ────────────────────────────────────────────
        answer->conn = (int32_t)conn;
        answer->src.handle = (int32_t)conn;
        answer->src.silent = false;
        http_stream_init(&answer->stream, url_source_read, &answer->src);

        http_head_result_t hrc = http_head_read(&answer->stream, &answer->reply);
        if (hrc != HTTP_HEAD_OK)
        {
            if (hrc == HTTP_HEAD_SOURCE && answer->src.silent)
                os64_hprintf(OS64_STDERR, "os64get: %s — the server went silent for %u seconds"
                             " before the reply was whole\n", say, URL_IDLE_MS / 1000);
            else
                os64_hprintf(OS64_STDERR, "os64get: %s — %s\n", say, http_head_reason(hrc));
            os64_close(answer->conn);
            // A framing header too long to read, or a 101 that hands the
            // connection to another protocol, is not a MALFORMED reply — it
            // is a legal one this program cannot honestly act on, the same
            // answer the coding refusals give, reached a different way, and
            // it earns the same exit code so a script cannot tell them apart
            // by accident.
            return (hrc == HTTP_HEAD_FRAMING || hrc == HTTP_HEAD_SWITCHED)
                       ? GET_UNSUPPORTED : GET_BAD_HEADER;
        }

        if (redirect_is_followed(answer->reply.status))
        {
            // The redirect's own body is a courtesy page for a browser to
            // display, and it goes with the connection: keep-alive is not
            // spoken here, so the next hop is a fresh dial whatever is left
            // unread on this one.
            os64_close(answer->conn);

            redirect_t hop;
            redirect_read(&current, answer->reply.location, &hop);
            if (hop.verdict != HOP_FOLLOW)
            {
                os64_hprintf(OS64_STDERR, "os64get: %s — %ld %s\n", say,
                             (long)answer->reply.status, reply_reason(&answer->reply));
                redirect_explain(&hop);
                // A PROXY SETTING THIS PROGRAM CANNOT READ IS THE SAME
                // DEFECT WHEREVER IT IS NOTICED. A typed https address
                // refuses the whole command with GET_USAGE before dialling
                // anything; a setting that only the second hop needed is no
                // different in kind, and answering "the redirect failed"
                // would send the reader to the server for a fault that is
                // in the environment.
                return hop.verdict == HOP_PROXY ? GET_USAGE : GET_REDIRECT;
            }
            if (++hops > URL_REDIRECT_MAX)
            {
                os64_hprintf(OS64_STDERR,
                             "os64get: %s — sent somewhere else %d times and still going;"
                             " os64get stops here\n", urlText, URL_REDIRECT_MAX);
                print_by_hand(hop.whole);
                return GET_REDIRECT;
            }

            // EVERY HOP IS ANNOUNCED, because the bytes are about to come
            // from a machine whose name nobody typed. It is narration and
            // not a warning, so -q silences it — unlike the proxy notice
            // above, which is a warning and does not go quiet.
            if (!quiet)
                os64_hprintf(OS64_STDERR, "os64get: %ld %s -> %s\n", (long)answer->reply.status,
                             reply_reason(&answer->reply), hop.whole);

            current = hop.target;
            proxy   = hop.proxy;
            os64_strcopy(currentText, sizeof(currentText), hop.whole);
            say = currentText;
            continue;
        }

        if (answer->reply.status != 200)
        {
            // The server's own words for its own decision. A 404 is a
            // refusal in the same sense the valet's "NO" is, and gets the
            // same exit code.
            os64_hprintf(OS64_STDERR, "os64get: %s — %ld %s\n", say,
                         (long)answer->reply.status, reply_reason(&answer->reply));
            if (answer->reply.status >= 300 && answer->reply.status < 400)
                redirect_unfollowed(&answer->reply);
            os64_close(answer->conn);
            return GET_REFUSED;
        }

        answer->answered = current;
        return GET_OK;
    }
}

// Fetch one URL into one file, staged and published exactly as the valet's
// files are. `urlText` is what the person typed, kept for the diagnostics —
// a complaint about a URL should quote the URL, not a reassembled version of
// it that differs in some way the reader then has to account for.
static int fetch_url(const http_url_t *url, const char *urlText, const proxy_t *proxy,
                     const char *destOverride, bool quiet)
{
    // WHERE THE FILE GOES, and the basename is only needed for some of the
    // answers. The command line's word is final, under cp(1)'s rule since
    // 1971: an existing directory receives the file under its own name,
    // anything else IS the path. Without a DEST the file lands in the current
    // directory — NOT wherever os64get.conf would have sent a valet file of
    // that name.
    //
    // A DEST THAT NAMES A FILE IS ASKED FOR FIRST, before the URL is asked
    // what it would like to be called. The order used to be the other way
    // round, so a URL ending in `/..`, or with a last segment past 255 bytes,
    // was refused with "say where with a DEST" — while HAVING been given one.
    // Advice that the program itself will not accept is the same defect as
    // advice that cannot be typed, which this file has now made twice.
    // (Codex review round 2, 2026-09-02.)
    //
    // AND THE NAME IS SETTLED HERE, BEFORE THE WIRE, so that no redirect can
    // choose it. Where a fetch ends up is the server's to decide; what
    // appears in somebody's directory is not. A server answering `/download`
    // with a redirect to `/.profile` would otherwise be naming a file on this
    // machine, and the person who typed the command would have no idea why
    // that name appeared. wget spells the same rule as a switch that is off
    // by default (--trust-server-names); os64get does not offer the switch,
    // because the DEST operand already says "call it this" for anyone who
    // wants to.
    char dest[GET_PATH_MAX];
    char name[GET_PATH_MAX];
    bool ok;
    bool intoDir = false;

    if (destOverride != NULL)
    {
        os64_dirent_t into;
        intoDir = (os64_stat(destOverride, &into) >= 0) &&
                  (into.flags & OS64_DE_DIR) != 0;
    }

    if (destOverride != NULL && !intoDir)
    {
        ok = join_path(dest, sizeof(dest), NULL, destOverride);
        // The name is still wanted for the progress meter, and a URL that
        // suggests none is no longer a problem now that the destination is
        // spelled out. Fall back to the last thing anybody typed.
        if (!url_basename(url, name, sizeof(name)))
            os64_strcopy(name, sizeof(name), destOverride);
    }
    else
    {
        if (!url_basename(url, name, sizeof(name)))
        {
            os64_hprintf(OS64_STDERR,
                         "os64get: %s does not name a file to save — give a DEST that does,"
                         " such as a path ending in the name you want\n", urlText);
            return GET_USAGE;
        }
        ok = join_path(dest, sizeof(dest), intoDir ? destOverride : NULL, name);
    }

    if (!ok)
    {
        os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
        return GET_USAGE;
    }

    stage_count = 1;
    if (!install_plan(&stages[0], dest)) return GET_WRITE_FAILED;
    const char *partPath = stages[0].part;

    url_reply_t answer;
    int rc = url_ask(&answer, url, urlText, proxy, quiet);
    if (rc != GET_OK)
        return rc;

    // A FRAMING OR A CODING THIS PROGRAM CANNOT UNDO MUST NEVER BECOME A
    // FILE. What would land is the envelope wearing the letter's name, and a
    // `.html` full of chunk lengths or of a compression nothing here speaks
    // is worse than no file at all, because it looks like a successful
    // download. The rule outlives the list: whatever this program learns to
    // read moves out of these branches by being handled (chunked and gzip
    // did), and whatever it has not learned is refused by name.
    http_body_t body;
    if (!http_body_open(&body, &answer.stream, &answer.reply))
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: the reply is framed as '%s', which os64get does not read"
                     " — nothing written\n", answer.reply.transferEncoding);
        os64_close(answer.conn);
        return GET_UNSUPPORTED;
    }
    // gzip is the one content coding this program undoes (BROWSER.md 3(d)),
    // and it is undone in receive_url_body, DOWNSTREAM of the framing: a
    // gzip body may arrive chunked, and the two envelopes come off in the
    // order they went on.
    bool gzipEncoded = os64_streq(answer.reply.contentEncoding, "gzip");
    if (answer.reply.contentEncoding[0] != '\0' &&
        !os64_streq(answer.reply.contentEncoding, "identity") && !gzipEncoded)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: the reply is encoded as '%s', which os64get does not decode"
                     " — nothing written\n", answer.reply.contentEncoding);
        os64_close(answer.conn);
        return GET_UNSUPPORTED;
    }

    // ── Receive into managed scratch ────────────────────────────────────────
    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_close(answer.conn);
        return GET_WRITE_FAILED;
    }

    // THE FRAMING IS THE BODY READER'S BUSINESS (http.h) AND THE CODING IS
    // receive_url_body's. The first takes off the length, the chunks or the
    // close and answers whether the body ENDED or was merely STOPPED — a
    // distinction only a length or chunked framing can draw; without either
    // the close is the length (HTTP/1.0's original framing, RFC 1945
    // §7.2.2, and the reason `Connection: close` is in the request), and a
    // reply with no length cannot be told apart from one that was cut
    // short. The second decides what the bytes ARE: the file, or a gzip
    // stream the file is inside. A Content-Length counts WIRE bytes either
    // way; what lands in the temporary file is counted separately and, for gzip,
    // capped.
    url_body_result_t staged;
    int status = receive_url_body(&body, &answer.reply, (int32_t)out, partPath,
                                  name, quiet, gzipEncoded, &staged);
    if (!quiet)
    {
        // The meter's last tick: a length-framed body printed it when the
        // count came due, but a chunked or close-delimited body learns its
        // total only now, and a meter that stops at the last 4KB boundary
        // reads as a transfer that stopped short.
        if (!answer.reply.hasLength)
            os64_printf("\r%s: %lu%s bytes", name, (unsigned long)staged.received,
                        gzipEncoded ? " wire" : "");
        os64_printf("\n");
    }

    os64_close(answer.conn);

    if (status == GET_OK && body.result != HTTP_BODY_DONE)
    {
        switch (body.result)
        {
        case HTTP_BODY_BROKE:
            if (answer.src.silent)
                os64_hprintf(OS64_STDERR, "os64get: the server went silent for %u seconds"
                             " after %lu bytes\n", URL_IDLE_MS / 1000,
                             (unsigned long)staged.received);
            else
                os64_hprintf(OS64_STDERR, "os64get: the connection broke after %lu bytes\n",
                             (unsigned long)staged.received);
            status = GET_SHORT;
            break;
        case HTTP_BODY_CUT:
            if (answer.reply.hasLength)
                os64_hprintf(OS64_STDERR, "os64get: the reply ended after %lu of %lu bytes\n",
                             (unsigned long)staged.received,
                             (unsigned long)answer.reply.length);
            else
                os64_hprintf(OS64_STDERR, "os64get: the reply ended after %lu bytes,"
                             " before its last chunk\n", (unsigned long)staged.received);
            status = GET_SHORT;
            break;
        default:
            // The server's chunk framing stopped being HTTP: "that was not
            // speech", the same verdict a broken head earns.
            os64_hprintf(OS64_STDERR, "os64get: after %lu bytes, %s\n",
                         (unsigned long)staged.received, http_body_reason(body.result));
            status = GET_BAD_HEADER;
            break;
        }
    }
    if (status == GET_OK && os64_sync((int32_t)out) < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: could not commit %s; %s NOT written\n",
                     partPath, dest);
        status = GET_WRITE_FAILED;
    }
    if (os64_close((int32_t)out) < 0) status = GET_WRITE_FAILED;
    if (install_cancelled()) status = GET_CANCELLED;

    if (install_cancelled()) return GET_CANCELLED;
    if (status != GET_OK) return status;
    const bool downloaded[] = { true };
    rc = publish_run(downloaded, 1, quiet, 0, false);
    if (rc != GET_OK) return rc;

    // THE HOST NAMED IS THE ONE THAT SERVED IT, which after a redirect is not
    // the one that was typed — that is the whole point of saying it. A gzip
    // body names both counts, because the one the meter showed was the wire's.
    if (!quiet)
    {
        if (gzipEncoded)
            os64_printf("%s: %lu bytes (gzip, %lu on the wire) from %s\n", dest,
                        (unsigned long)staged.produced, (unsigned long)staged.received,
                        answer.answered.host);
        else
            os64_printf("%s: %lu bytes from %s\n", dest, (unsigned long)staged.produced,
                        answer.answered.host);
    }
    return GET_OK;
}

static int get_main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operands[4] = {0};
    bool quiet = false;
    bool noArchive = false;
    bool all = false;
    bool force = false;
    bool flgChangesOnly = false;
    const os64_optspec_t specs[] = {
        {'q', "quiet", false, "no progress, just the exit code", .flag = &quiet},
        {'n', "no-archive", false, "replace without backing up the original", .flag = &noArchive},
        {'a', "all", false, "fetch EVERY file the server offers, routing each by the conf", .flag = &all},
        {'f', "force", false, "fetch even files already identical on disk", .flag = &force},
        {'c', "changes-only", false, "display only changed files", .flag = &flgChangesOnly}};

    os64_args_init(&args, argc, argv, specs, 5);
    args.about = "Fetch a file over the network: the build valet's supply line, or any http:// address.";
    args.details = "DEST is a directory to install into, or the full path to install as; "
                   "it defaults to the directory /etc/os64get.conf names for NAME (or the cwd). "
                   "Backs up replaced originals under <archive>/DATE/RUN/destination-path. "
                   "Downloads use managed scratch directories on their destination filesystems. "
                   "With -a, asks the server what it has and fetches all of it — the whole-system refresh. "
                   "An http:// or https:// URL uses the current directory without config routing, under the "
                   "URL's own last path segment unless DEST says otherwise. HTTP framing and gzip checksums "
                   "are checked before publication. A redirect is followed, "
                   "up to five of them, and never gets to choose the file's name. https needs $https_proxy "
                   "set to a machine that has a TLS, since os64 has none; $no_proxy lists the hosts "
                   "that skip it. -a is the valet's verb, and on a URL fetch -f has nothing "
                   "to force (there is no manifest unchanged check). URL downloads do not make backups; "
                   "-n disables backups for valet installs. "
                   "Ctrl+C cleans preparation files; once installation moves start, they finish first.";

    int32_t count = os64_args_parse(&args,
                                    "os64get [-q] [-n] [-f] HOST NAME [DEST]  |  os64get -a [-q] [-n] [-f] HOST"
                                    "  |  os64get [-q] [-n] http://HOST/PATH [DEST]",
                                    operands, 4);
    if (count == OS64_ARG_HELP)
        return GET_OK;
    if (count < 1)
    {
        if (count != OS64_ARG_ERROR)
            os64_hprintf(OS64_STDERR, all ? "os64get: need a HOST\n"
                                          : "os64get: need a HOST and a NAME, or a URL\n");
        return GET_USAGE;
    }

    quiet_run = quiet;
    batch_run = all;
    // ── WHICH DIALECT? THE OPERAND ANSWERS ──────────────────────────────
    // A URL carries "scheme://" and a valet operand is a bare word, so the
    // two never have to be told apart by guesswork. Anything WITH a scheme
    // is committed to the world's path — including a scheme this program
    // cannot speak, which earns an honest refusal rather than a silent
    // fallthrough into the valet's dialect (where "https://x/y" would have
    // been dialled as a host name).
    http_url_t url;
    http_url_result_t urc = http_url_parse(operands[0], &url);
    // The valet installs system files; URL mode saves pages without consulting
    // the install map or making replacement backups. Both use managed staging.
    static conf_t conf;
    if (urc == HTTP_URL_NOT_A_URL) conf_load(&conf);
    if (!install_init(urc == HTTP_URL_NOT_A_URL && !noArchive ? conf.archive : NULL))
        return GET_WRITE_FAILED;
    if (os64_signal_set_handler(OS64_SIGINT, install_cancel) < 0) {
        os64_hprintf(OS64_STDERR, "os64get: cannot install Ctrl+C handler\n");
        return GET_WRITE_FAILED;
    }
    if (urc != HTTP_URL_NOT_A_URL)
    {
        if (all)
        {
            os64_hprintf(OS64_STDERR,
                         "os64get: -a asks a valet for its whole catalogue; a URL names one file\n");
            return GET_USAGE;
        }
        if (count > 2)
        {
            os64_hprintf(OS64_STDERR, "os64get: a URL takes at most a DEST after it\n");
            return GET_USAGE;
        }
        if (urc == HTTP_URL_SCHEME)
        {
            os64_hprintf(OS64_STDERR, "os64get: os64get speaks http and https, not %s (%s)\n",
                         url.scheme, operands[0]);
            return GET_BAD_URL;
        }
        if (urc != HTTP_URL_OK)
        {
            os64_hprintf(OS64_STDERR, "os64get: %s — %s\n", operands[0], http_url_reason(urc));
            return GET_BAD_URL;
        }

        proxy_t proxy;
        if (!proxy_for(&url, &proxy))
            return GET_USAGE;

        // WHETHER https IS REACHABLE IS A QUESTION ABOUT THIS MACHINE, not
        // about the address — which is why the parser above answers only the
        // second. os64 has no TLS and will BORROW one when its day comes
        // (BROWSER.md's first ruling: thirty years of side-channel attacks
        // teach no kernel lessons), so the only way to an https page today is
        // a machine that already has a TLS fetching it for us.
        if (os64_streq(url.scheme, "https") && !proxy.inUse)
        {
            os64_hprintf(OS64_STDERR,
                         "os64get: %s is https, and os64 has no TLS of its own.\n"
                         "os64get: set $https_proxy to a machine that will fetch it for you:\n"
                         "os64get:     export https_proxy=http://<host>:8888/\n"
                         "os64get: (tools/tlsproxy.py is one; it terminates the TLS, so it sees\n"
                         "os64get:  the page in the clear — fine for reading, not for secrets.)\n"
                         "os64get: or try the http:// address if the site still answers on one.\n",
                         operands[0]);
            return GET_BAD_URL;
        }
        // The DEST slot, checked before the wire: a URL there is somebody
        // answering the advice above the way the sentence reads.
        const char *dest = count >= 2 ? operands[1] : NULL;
        if (dest_is_a_url(dest))
        {
            os64_hprintf(OS64_STDERR,
                         "os64get: DEST says where to SAVE the file, and '%s' is an address.\n"
                         "os64get: to fetch it:            os64get %s\n"
                         "os64get: to save it somewhere:   os64get %s <directory-or-path>\n",
                         dest, dest, operands[0]);
            return GET_USAGE;
        }
        return fetch_url(&url, operands[0], &proxy, dest, quiet);
    }

    if (!all && count < 2)
    {
        os64_hprintf(OS64_STDERR, "os64get: need a HOST and a NAME, or a URL\n");
        return GET_USAGE;
    }
    if (all && count > 1)
    {
        // -a routes every file by the conf, so a DEST would have to mean
        // "put all 66 files in one directory", which is never what anyone
        // means. Refuse rather than guess (the house rule: tripwires over
        // silence).
        os64_hprintf(OS64_STDERR, "os64get: -a fetches everything and lets the conf place it — "
                                  "no NAME or DEST goes with it\n");
        return GET_USAGE;
    }

    const char *host = operands[0];

    // Same guard on this side: the valet's DEST is a place on this machine
    // too, and there is no reading of "os64get HOST NAME http://..." where
    // the third operand is a filename.
    if (dest_is_a_url(count >= 3 ? operands[2] : NULL))
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: DEST says where to SAVE the file, and '%s' is an address.\n"
                     "os64get: to fetch an address:  os64get %s\n",
                     operands[2], operands[2]);
        return GET_USAGE;
    }


    if (!all)
    {
        char lot[GET_LOT_MAX] = {0};
        if (count < 3 && conf.nlot > 0)
            lookup_lot(host, operands[1], &conf, lot, sizeof(lot));
        if (install_cancelled()) return GET_CANCELLED;
        char dest[GET_PATH_MAX];
        int rc = resolve_destination(operands[1], count >= 3 ? operands[2] : NULL,
                                     lot, &conf, dest);
        if (rc != GET_OK) return rc;
        stage_count = 1;
        if (!install_plan(&stages[0], dest)) return GET_WRITE_FAILED;
        rc = fetch_stage(host, operands[1], &stages[0], quiet, force);
        if (rc == GET_UNCHANGED) return GET_OK;
        if (rc != GET_OK) return rc;
        const bool downloaded[] = { true };
        return publish_run(downloaded, 1, quiet, 0, false);
    }

    uint64_t totalBytes = 0;
    int32_t n = fetch_list(host, entries, GET_MAX_LIST, &totalBytes);
    if (n < 0) return GET_DIAL_FAILED;
    if (n == 0) {
        if (!quiet) os64_printf("os64get: the server offers nothing\n");
        return GET_OK;
    }
    warn_if_server_has_no_lots(&conf, n);
    if (!quiet)
        os64_printf("os64get: %ld files, %lu bytes, from %s\n",
                    (long)n, (unsigned long)totalBytes, host);

    // Plan and reserve names before network payloads. The staging basenames
    // let the destination filesystem itself answer duplicate/alias checks.
    for (int32_t i = 0; i < n; i++) {
        if (install_cancelled()) return GET_CANCELLED;
        char dest[GET_PATH_MAX];
        int rc = resolve_destination(entries[i].name, NULL, entries[i].lot, &conf, dest);
        if (rc != GET_OK) return rc;
        stage_count = (unsigned)i + 1;
        if (!install_plan(&stages[i], dest)) return GET_WRITE_FAILED;
        for (int32_t j = 0; j < i; j++) {
            if (install_conflicts(&stages[j], &stages[i])) {
                os64_hprintf(OS64_STDERR, "os64get: duplicate destination: %s\n", dest);
                return GET_USAGE;
            }
        }
    }

    static bool downloaded[GET_MAX_LIST];
    unsigned failed = 0, unchanged = 0;
    for (int32_t i = 0; i < n; i++) {
        if (install_cancelled()) return GET_CANCELLED;
        if (!force && entries[i].length != 0 &&
            local_matches(stages[i].dest, entries[i].length, entries[i].crc)) {
            unchanged++;
            if (!quiet && !flgChangesOnly)
                os64_printf("[%ld/%ld] %s — unchanged\n", (long)(i + 1), (long)n, entries[i].name);
            continue;
        }
        if (install_cancelled()) return GET_CANCELLED;
        if (!quiet) os64_printf("[%ld/%ld] %s\n", (long)(i + 1), (long)n, entries[i].name);
        int rc = fetch_stage(host, entries[i].name, &stages[i], quiet, force);
        if (install_cancelled()) return GET_CANCELLED;
        if (rc == GET_OK) downloaded[i] = true;
        else if (rc == GET_UNCHANGED) unchanged++;
        else {
            failed++;
            os64_hprintf(OS64_STDERR, "os64get: %s FAILED (%d)\n", entries[i].name, rc);
        }
    }
    if (failed) {
        os64_hprintf(OS64_STDERR, "os64get: %u transfers failed — INSTALLING NOTHING\n", failed);
        return GET_CORRUPT;
    }
    return publish_run(downloaded, (unsigned)n, quiet, unchanged, true);
}

int main(int argc, char **argv)
{
    int rc = get_main(argc, argv);
    bool cleaned = install_cleanup(stages, stage_count);
    if (!cleaned && rc == GET_OK) rc = GET_WRITE_FAILED;
    if (install_archive()[0] && (!quiet_run || batch_run || rc != GET_OK))
        os64_printf("os64get: originals kept at %s\n", install_archive());
    if (install_cancel_requested()) {
        os64_hprintf(OS64_STDERR, "os64get: %s; temporary-file cleanup %s\n",
                     install_cancelled() ? "cancelled before installation" : "Ctrl+C deferred until installation finished",
                     cleaned ? "complete" : "incomplete (see paths above)");
        if (rc == GET_OK || install_cancelled()) rc = GET_CANCELLED;
    }
    return rc;
}
