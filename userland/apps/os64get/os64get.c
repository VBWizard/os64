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

#include "fetch/fetch.h"
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
#define GET_PREPARE_FAILED 11  // staged verification, original backup, or recheck failed
// The destination matches the server's length and CRC, so no payload was
// fetched. A batch may still own an empty staging reservation. This status
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
// this machine cannot reach, or tried to downgrade HTTPS to HTTP. Distinct from
// GET_REFUSED because the next move is different — a refusal is the server's
// final answer about the page, while this is a road that did not arrive, and
// the thing to change is usually on THIS side.
#define GET_REDIRECT       15
#define GET_TLS_FAILED     16
#define GET_CANCELLED      130



#define GET_PORT      6464
// 64KB per write: the fetch loop fills this whole buffer from the socket
// before writing it, so every write() hands ext2 sixteen blocks it can put
// on the disk as a single run — however the wire delivered them. At 4KB
// the file was written a block per syscall, and the transfer waited on the
// disk, not the wire.
#define GET_CHUNK     65536
#define GET_PATH_MAX  256

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
    bool     has_checksum;  // distinguishes an empty file from a name-only LIST entry
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
    if (!stage->part[0] && !install_reserve(stage)) {
        os64_close((int32_t)conn);
        return GET_WRITE_FAILED;
    }
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

    // Check the wire fingerprint, then retain it for the staged-file reread
    // during preparation. A successful write need not have stored these bytes.
    uint32_t actual = os64_crc32_end(crc);
    if (actual != expectCrc)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: CHECKSUM MISMATCH for %s — got %08x, expected %08x. "
                     "temporary %s rejected; %s NOT installed.\n",
                     name, actual, expectCrc, partPath, dest);
        return GET_CORRUPT;
    }

    install_received(stage, expectLen, expectCrc);
    if (!quiet)
        os64_printf("%s: %lu bytes, crc %08x, transfer verified\n",
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
            if (!install_prepare(&stages[i])) return GET_PREPARE_FAILED;
        }
    }
    for (unsigned i = 0; i < count; i++)
        if (downloaded[i] && !install_recheck(&stages[i])) return GET_PREPARE_FAILED;
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
        entries[count].has_checksum = false;
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
            bool length_ok = parse_u64(p, q, &entries[count].length);
            if (length_ok)
                *totalBytes += entries[count].length;
            if (*q == ' ')
            {
                const char *cs = q + 1;
                const char *ce = cs;
                while (*ce != '\0' && *ce != ' ')
                    ce++;
                entries[count].has_checksum = parse_hex32(cs, ce, &entries[count].crc) && length_ok;
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
//     (see fetch_url); letting the far end choose what appears in somebody's
//     directory is not, so the name is settled from the typed address before
//     the first request goes out.
//
// What it KEEPS is the property this whole program is built around: the bytes
// land in managed scratch storage, and only a validated body reaches the real name. On
// ext2 that final replacement is atomic, so a transfer that dies halfway
// leaves the file that was there exactly where it was. A FAT destination has
// syscall 43's documented remove-first publication window.

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
// THE JUDGING IS libfetch's (LIBFETCH.md: what a Location points at and
// which hops are safe to follow are policy about the WIRE, the same for
// every fetcher on the machine). What is here is os64get's SAY: the hop
// callback narrates each followed hop, and the sentences below explain the
// ones the library stopped at, reading the library's verdict rather than
// re-deciding it — two pieces of code answering "is this followable"
// separately is how a program ends up explaining a refusal it did not make.

// HOW FAR A FETCH IS WILLING TO BE SENT. Five is RFC 2068 §10.3's own
// recommendation from 1997, and the number matters far less than the fact
// that there is one: a site needing six hops to hand over one file is
// misconfigured, and a site needing infinitely many is a loop with a friendly
// face. wget allows twenty and curl thirty; when this stops, it prints the
// address it stopped at, so the trail can be picked up by hand.
#define URL_REDIRECT_MAX 5

// The redirect in words, for the ones the library will not follow. Every
// sentence names the ADDRESS: somebody told "cannot follow that" and not
// told where "that" was has to go and read the headers themselves, which is
// the position they were in before they had a fetcher.
static void redirect_explain(const os64_fetch_hop_t *hop)
{
    switch (hop->kind)
    {
    case OS64_FETCH_HOP_WHOLE:
        return;
    case OS64_FETCH_HOP_NONE:
        os64_hprintf(OS64_STDERR, "os64get: ...and does not say where to\n");
        return;
    case OS64_FETCH_HOP_TOO_LONG:
        os64_hprintf(OS64_STDERR, "os64get: ...and the address it points at is longer"
                                  " than os64get will hold\n");
        return;
    case OS64_FETCH_HOP_UNUSABLE:
        os64_hprintf(OS64_STDERR, "os64get: it points at %s, which is not a usable"
                                  " address — %s\n", hop->whole, os64_url_reason(hop->parse));
        return;
    case OS64_FETCH_HOP_SCHEME:
        os64_hprintf(OS64_STDERR, "os64get: it points at %s, and os64get fetches http"
                                  " and https addresses\n", hop->whole);
        return;
    case OS64_FETCH_HOP_DOWNGRADE:
        os64_hprintf(OS64_STDERR,
                     "os64get: refusing HTTPS-to-HTTP downgrade to %s — the target is unencrypted.\n",
                     hop->whole);
        return;
    case OS64_FETCH_HOP_PROXY:
        os64_hprintf(OS64_STDERR, "os64get: it points at %s, and the proxy setting that"
                                  " would carry it is unusable: %s\n", hop->whole, hop->why);
        return;
    case OS64_FETCH_HOP_SELF:
        os64_hprintf(OS64_STDERR, "os64get: it points back at %s, the address that just"
                                  " answered — the server is going in a circle\n", hop->whole);
        return;
    }
}

// A 3xx that is not one of the five the library follows. Each of them means
// something a fetch cannot act on by itself, and naming which is the
// difference between "the server said no" and "the server said something
// os64get chose not to obey".
//
// THESE EARN GET_REFUSED, NOT GET_REDIRECT, and the split is about what
// happened rather than about the first digit: none of them SENT this fetch
// anywhere. The server answered with a list, or with an instruction about
// routing, and that is its final word on the page — the same shape as a 404.
// GET_REDIRECT is for a road that was taken and did not arrive.
static void redirect_unfollowed(const os64_fetch_head_t *head)
{
    const char *why;
    switch (head->status)
    {
    case 300: why = "a list of choices is for a person to pick from";                 break;
    case 304: why = "nothing was asked conditionally, so 'not modified' answers nothing"; break;
    case 305: why = "'use this proxy' is a stranger choosing this machine's route";   break;
    default:  why = "os64get does not know what that one means";                      break;
    }
    if (head->has_location && head->location[0] != '\0')
        os64_hprintf(OS64_STDERR, "os64get: it names %s, and %s\n", head->location, why);
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
static const char *reply_reason(const char *reason)
{
    return reason[0] != '\0' ? reason : "(no reason given)";
}

// What os64get says while the library works: the hop callback and the
// cancel predicate, and what they need to remember.
typedef struct {
    bool  quiet;
    bool  toldAboutTls;
    const char *say;                        // the address to quote in a complaint
    char  last[OS64_FETCH_URL_MAX];         // the last hop's address, `say` past the first
} narration_t;

// THE PROXY NOTICE IS NOT BEHIND -q, deliberately, and the precedent is this
// program's own: warn_if_server_has_no_lots prints regardless, because a
// warning you silenced along with the progress is a warning you will not
// see on the run that mattered. What the reader needs is different for the
// two schemes — an http fetch through a proxy was never encrypted and the
// proxy is merely another hop, while an https fetch through one LOOKS like
// the encrypted thing it is not.
//
// The long version is printed ONCE, at the first https hop that needs it:
// what it warns about is true of the whole fetch, and a trail of redirects
// would otherwise repeat three lines of prose at every step until the
// warning became the thing you scroll past.
static void proxy_notice(narration_t *n, const char *scheme, const char *host, uint16_t port)
{
    if (os64_streq(scheme, "https") && !n->toldAboutTls)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: via the proxy at %s:%u, which TERMINATES the TLS — it holds this"
                     " page in the clear, and the leg from here to it is plain text."
                     " This is not end-to-end encryption.\n", host, (unsigned)port);
        n->toldAboutTls = true;
    }
    else
        os64_hprintf(OS64_STDERR, "os64get: via the proxy at %s:%u\n", host, (unsigned)port);
}

// EVERY HOP IS ANNOUNCED, because the bytes are about to come from a machine
// whose name nobody typed. It is narration and not a warning, so -q silences
// it — unlike the proxy notice, which is a warning and does not go quiet.
// The verdict is always the library's: os64get has no opinion a script
// should not share.
static os64_fetch_verdict_t narrate_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    narration_t *n = ctx;
    if (hop->kind == OS64_FETCH_HOP_WHOLE && hop->number <= URL_REDIRECT_MAX)
    {
        if (!n->quiet)
            os64_hprintf(OS64_STDERR, "os64get: %ld %s -> %s\n", (long)hop->status,
                         reply_reason(hop->reason), hop->whole);
        os64_strcopy(n->last, sizeof(n->last), hop->whole);
        n->say = n->last;
        if (hop->via_proxy)
            proxy_notice(n, hop->target.scheme, hop->proxy_host, hop->proxy_port);
    }
    return OS64_FETCH_HOP_DEFAULT;
}

static bool fetch_cancelled(void *ctx)
{
    (void)ctx;
    return install_cancelled();
}

// What open's outcome means to os64get: the exit code, and the sentence. The
// codes are the ones this program has always answered with (OS64GET.md's
// table is a contract with every script written against it); the sentences
// are the library's where the library knows more, and os64get's where the
// answer is about this machine.
static int open_verdict(os64_fetch_t *f, narration_t *n, const char *urlText)
{
    const os64_fetch_detail_t *d = os64_fetch_detail(f);
    os64_fetch_status_t st = os64_fetch_status(f);
    switch (st)
    {
    case OS64_FETCH_OK:
        return GET_OK;
    case OS64_FETCH_INTERRUPTED:
        return GET_CANCELLED;
    case OS64_FETCH_BAD_URL:
    case OS64_FETCH_UNSUPPORTED_SCHEME:
        os64_hprintf(OS64_STDERR, "os64get: %s — %s\n", urlText, os64_fetch_reason(f));
        return GET_BAD_URL;
    case OS64_FETCH_PROXY_BAD:
        // A PROXY SETTING THIS PROGRAM CANNOT READ IS THIS MACHINE'S DEFECT,
        // not the server's, and GET_USAGE says so.
        os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
        return GET_USAGE;
    case OS64_FETCH_DIAL_FAILED:
        os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
        // At the FIRST hop this is the address that was typed, and 3 says
        // so. Past it, a SERVER that cannot be reached is one another server
        // chose, and a script must be able to tell those apart: the road
        // did not arrive, which is what 15 means (Codex review of PR #60).
        // A PROXY that cannot be reached is neither — it is this machine's
        // own setting, the same defect at whichever hop it is noticed, and
        // it keeps the answer the first hop gives.
        return (d->dial_hop > 0 && !d->dial_was_proxy) ? GET_REDIRECT : GET_DIAL_FAILED;
    case OS64_FETCH_TLS_FAILED:
        os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
        if (!d->store_failed &&
            (d->tls == OS64_TLS_BAD_ARGUMENT || d->tls == OS64_TLS_UNSUPPORTED))
            os64_hprintf(OS64_STDERR,
                "os64get: check HTTPS target '%s': TLS requires a supported DNS name; IP literals are not supported\n",
                d->dial_host);
        return GET_TLS_FAILED;
    case OS64_FETCH_REQUEST_FAILED:
        os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
        return GET_REQUEST_FAILED;
    case OS64_FETCH_BAD_HEAD:
    case OS64_FETCH_SILENT:
        os64_hprintf(OS64_STDERR, "os64get: %s — %s\n", n->say, os64_fetch_reason(f));
        return GET_BAD_HEADER;
    case OS64_FETCH_UNSUPPORTED:
        // A framing header too long to read, or a 101 that hands the
        // connection to another protocol, or a framing or coding nothing
        // here undoes: not a MALFORMED reply but a legal one this program
        // cannot honestly act on, and it earns one exit code however it was
        // reached so a script cannot tell them apart by accident.
        os64_hprintf(OS64_STDERR, "os64get: %s — %s — nothing written\n", n->say,
                     os64_fetch_reason(f));
        return GET_UNSUPPORTED;
    case OS64_FETCH_REDIRECT_STOPPED:
    {
        const os64_fetch_head_t *head = os64_fetch_head(f);
        os64_hprintf(OS64_STDERR, "os64get: %s — %ld %s\n", n->say,
                     (long)head->status, reply_reason(head->reason));
        redirect_explain(&d->hop);
        if (d->hop.kind == OS64_FETCH_HOP_DOWNGRADE) print_by_hand(d->hop.whole);
        // A proxy setting that only the second hop needed is the same defect
        // as one the typed address needed, and answering "the redirect
        // failed" would send the reader to the server for a fault that is
        // in the environment.
        return d->hop.kind == OS64_FETCH_HOP_PROXY ? GET_USAGE : GET_REDIRECT;
    }
    case OS64_FETCH_TOO_MANY_HOPS:
        os64_hprintf(OS64_STDERR,
                     "os64get: %s — sent somewhere else %d times and still going;"
                     " os64get stops here\n", urlText, URL_REDIRECT_MAX);
        print_by_hand(d->hop.whole);
        return GET_REDIRECT;
    case OS64_FETCH_NO_MEMORY:
        os64_hprintf(OS64_STDERR, "os64get: out of memory\n");
        return GET_WRITE_FAILED;
    default:
        os64_hprintf(OS64_STDERR, "os64get: %s — %s\n", n->say, os64_fetch_reason(f));
        return GET_BAD_HEADER;
    }
}

// Fetch one URL into one file, staged and published exactly as the valet's
// files are. `urlText` is what the person typed, kept for the diagnostics —
// a complaint about a URL should quote the URL, not a reassembled version of
// it that differs in some way the reader then has to account for. Every hop
// after the first quotes the address os64get went to instead, since that is
// the one the answer came from and the one nobody has seen yet.
static int fetch_url(const http_url_t *url, const char *urlText,
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

    // ── Ask, and keep asking wherever the answers point ─────────────────
    // The library dials, follows, and reads the head; os64get narrates the
    // hops, answers Ctrl+C, and says what the outcome means.
    narration_t narr = { .quiet = quiet, .say = urlText };
    os64_fetch_options_t opt = {
        .user_agent = "os64get/1 (os64)",
        .max_hops   = URL_REDIRECT_MAX,
        .on_hop     = narrate_hop,
        .cancelled  = fetch_cancelled,
        .ctx        = &narr,
    };
    os64_fetch_t *f = os64_fetch_open(urlText, &opt);
    if (f == NULL)
    {
        os64_hprintf(OS64_STDERR, "os64get: out of memory\n");
        return GET_WRITE_FAILED;
    }
    int rc = open_verdict(f, &narr, urlText);
    if (rc != GET_OK)
    {
        os64_fetch_close(f);
        return install_cancelled() ? GET_CANCELLED : rc;
    }
    const os64_fetch_head_t *head = os64_fetch_head(f);
    if (head->via_proxy && head->hops == 0)
        proxy_notice(&narr, head->url.scheme, head->proxy_host, head->proxy_port);

    if (head->status != 200)
    {
        // The server's own words for its own decision. A 404 is a
        // refusal in the same sense the valet's "NO" is, and gets the
        // same exit code.
        os64_hprintf(OS64_STDERR, "os64get: %s — %ld %s\n", narr.say,
                     (long)head->status, reply_reason(head->reason));
        if (head->status >= 300 && head->status < 400)
            redirect_unfollowed(head);
        os64_fetch_close(f);
        return GET_REFUSED;
    }

    // ── Receive into managed scratch ────────────────────────────────────
    // The bytes land here and only a validated body reaches the real name;
    // on ext2 that final replacement is atomic. The library has already
    // refused any framing or coding it cannot undo (its UNSUPPORTED, above),
    // so what arrives is the file — identity, or gzip already undone — and
    // stays provisional until the framing says WHOLE and, for gzip, every
    // member trailer verified.
    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_fetch_close(f);
        return GET_WRITE_FAILED;
    }
    uint8_t *buf = (uint8_t *)os64_malloc(GET_CHUNK);
    if (buf == NULL)
    {
        os64_hprintf(OS64_STDERR, "os64get: out of memory\n");
        os64_close((int32_t)out);
        os64_fetch_close(f);
        return GET_WRITE_FAILED;
    }

    const bool gzipEncoded = os64_streq(head->encoding, "gzip");
    const char *unit = gzipEncoded ? " wire" : "";
    const os64_fetch_progress_t *progress = os64_fetch_progress(f);
    uint32_t crc = os64_crc32_begin();
    uint64_t produced = 0;
    uint64_t lastTick = 0;
    int status = GET_OK;

    // FILL THE BUFFER BEFORE WRITING IT, for the disk's sake: a read answers
    // with what has ARRIVED — a segment, or a scheduler pass's worth — and
    // writing each of those hands ext2 a block or two at a time. Progress
    // ticks from INSIDE the fill, every 4KB of arrival, so a slow link reads
    // as slow rather than as hung. (The first libfetch draft wrote each
    // read as it came and lost this — Codex, PR #92.)
    bool over = false;
    while (!over)
    {
        size_t filled = 0;
        while (filled < GET_CHUNK)
        {
            int64_t n = os64_fetch_read(f, buf + filled, GET_CHUNK - filled);
            if (n <= 0) { over = true; break; }
            filled += (size_t)n;

            // The meter counts ARRIVAL, which for gzip is the wire's bytes
            // even though the file being staged grows faster.
            if (!quiet && (progress->wire / 4096 != lastTick ||
                           (head->has_length && progress->wire == head->length)))
            {
                lastTick = progress->wire / 4096;
                if (head->has_length)
                    os64_printf("\r%s: %lu/%lu%s bytes", name,
                                (unsigned long)progress->wire, (unsigned long)head->length, unit);
                else
                    os64_printf("\r%s: %lu%s bytes", name, (unsigned long)progress->wire, unit);
            }
        }
        if (filled != 0 && os64_write((int32_t)out, buf, filled) != (int64_t)filled)
        {
            os64_hprintf(OS64_STDERR, "os64get: write to %s failed (disk full?)\n", partPath);
            status = GET_WRITE_FAILED;
            break;
        }
        crc = os64_crc32_update(crc, buf, filled);
        produced += filled;
    }
    if (!quiet)
    {
        // The meter's last tick: a length-framed body printed it when the
        // count came due, but a chunked or close-delimited body learns its
        // total only now, and a meter that stops at the last 4KB boundary
        // reads as a transfer that stopped short.
        if (!head->has_length)
            os64_printf("\r%s: %lu%s bytes", name, (unsigned long)progress->wire, unit);
        os64_printf("\n");
    }

    if (status == GET_OK)
    {
        switch (os64_fetch_status(f))
        {
        case OS64_FETCH_OK:
            break;
        case OS64_FETCH_INTERRUPTED:
            status = GET_CANCELLED;
            break;
        case OS64_FETCH_CUT:
        case OS64_FETCH_BROKE:
        case OS64_FETCH_SILENT:
            os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
            status = GET_SHORT;
            break;
        case OS64_FETCH_BAD_HEAD:
            // The server's chunk framing stopped being HTTP: "that was not
            // speech", the same verdict a broken head earns.
            os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
            status = GET_BAD_HEADER;
            break;
        case OS64_FETCH_CORRUPT:
            os64_hprintf(OS64_STDERR, "os64get: %s — %s NOT written\n",
                         os64_fetch_reason(f), partPath);
            status = GET_CORRUPT;
            break;
        case OS64_FETCH_LIMIT:
        case OS64_FETCH_UNSUPPORTED:
            os64_hprintf(OS64_STDERR, "os64get: %s — %s NOT written\n",
                         os64_fetch_reason(f), partPath);
            status = GET_UNSUPPORTED;
            break;
        case OS64_FETCH_NO_MEMORY:
            os64_hprintf(OS64_STDERR, "os64get: out of memory — %s NOT written\n", partPath);
            status = GET_WRITE_FAILED;
            break;
        default:
            os64_hprintf(OS64_STDERR, "os64get: %s\n", os64_fetch_reason(f));
            status = GET_SHORT;
            break;
        }
    }
    uint64_t wireBytes = progress->wire;
    char servedBy[OS64_URL_HOST_MAX];
    os64_strcopy(servedBy, sizeof(servedBy), head->url.host);
    os64_fetch_close(f);
    os64_free(buf);

    if (status == GET_OK && os64_sync((int32_t)out) < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: could not commit %s; %s NOT written\n",
                     partPath, dest);
        status = GET_WRITE_FAILED;
    }
    if (os64_close((int32_t)out) < 0) status = GET_WRITE_FAILED;
    if (install_cancelled()) return GET_CANCELLED;
    if (status != GET_OK) return status;
    install_received(&stages[0], produced, os64_crc32_end(crc));
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
                        (unsigned long)produced, (unsigned long)wireBytes, servedBy);
        else
            os64_printf("%s: %lu bytes from %s\n", dest, (unsigned long)produced, servedBy);
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
    args.about = "Fetch a file over the network: the build valet's supply line, or an HTTP/HTTPS address.";
    args.details = "DEST is a directory to install into, or the full path to install as; "
                   "it defaults to the directory /etc/os64get.conf names for NAME (or the cwd). "
                   "Backs up replaced originals under <archive>/DATE/RUN/destination-path. "
                   "Downloads use managed scratch directories on their destination filesystems. "
                   "With -a, asks the server what it has and fetches all of it — the whole-system refresh. "
                   "An http:// or https:// URL uses the current directory without config routing, under the "
                   "URL's own last path segment unless DEST says otherwise. HTTP framing and gzip checksums "
                   "are checked before publication. A redirect is followed, "
                   "up to five of them, and never gets to choose the file's name. HTTPS uses libtls and "
                   "the trust store selected by tls.conf. HTTPS-to-HTTP redirects are refused. "
                   "Explicit $https_proxy uses the terminating helper; $no_proxy bypasses it. -a is the valet's verb, and on a URL fetch -f has nothing "
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

        // Validate the local destination before starting a network request.
        // (The proxy settings are the library's to read, at open.)
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
        return fetch_url(&url, operands[0], dest, quiet);
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
        if (!install_resolve(&stages[0], dest)) return GET_WRITE_FAILED;
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

    // Resolve destinations before reserving scratch. Unchanged files need
    // no writes even on a read-only or full mount. Changed entries reserve
    // basenames so the filesystem can also identify aliases of absent targets.
    static bool unchanged_entry[GET_MAX_LIST];
    for (int32_t i = 0; i < n; i++) {
        if (install_cancelled()) return GET_CANCELLED;
        char dest[GET_PATH_MAX];
        int rc = resolve_destination(entries[i].name, NULL, entries[i].lot, &conf, dest);
        if (rc != GET_OK) return rc;
        stage_count = (unsigned)i + 1;
        if (!install_resolve(&stages[i], dest)) return GET_WRITE_FAILED;
        unchanged_entry[i] = !force && entries[i].has_checksum &&
            local_matches(stages[i].dest, entries[i].length, entries[i].crc);
        if (install_cancelled()) return GET_CANCELLED;
        if (!unchanged_entry[i] && !install_reserve(&stages[i])) return GET_WRITE_FAILED;
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
        if (unchanged_entry[i]) {
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
