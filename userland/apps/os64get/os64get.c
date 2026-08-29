// os64get.c — fetch a file over the wire, put it where it belongs, keep a copy,
// and refuse to install a bad one.
//
// THE MISSION, in one line: `os64 refresh` without a thumb drive. Until
// 2026-08-16 the only way a fresh build reached the P5 was a human carrying a
// USB stick across a room; since 2026-08-21 the P5 boots from its own disk
// and this program is the whole supply line — kernel, menu, bootloader, apps.
//
// The protocol is RTL8125.md's, deliberately 1971-shaped — one TCP
// connection per file, ASCII where a human might read it, binary only where
// a machine must:
//
//     client -> server:   GET <name>\n
//     server -> client:   OK <length-decimal> <crc32-hex8>\n  then <length> bytes
//                    or:  NO <reason>\n
//
// You can drive it by hand from any telnet client, which is not nostalgia:
// a protocol you can type is a protocol you can debug at 1am on a machine
// with no tooling.
//
// ── WHERE A FILE GOES (2026-08-22) ──────────────────────────────────────
//
// The valet serves NAMES, one path component each; where a name lands on
// this machine is THIS machine's business, and it is written in
// /etc/os64get.conf (or /home/os64get.conf, which wins — the same ladder as
// logd.conf and husk.rc): exact names, `*.suffix` patterns, `@lot` labels
// and a `*` default, each mapped to a directory. The kernel goes to
// /fat/boot where Limine reads it, programs go to /bin, and nobody has to
// remember which is which at the prompt.
//
// A LOT (2026-08-29) is the valet's label for the DIRECTORY a file was
// served from, carried in the fourth field of every LIST line. It exists
// because /tests split out of /bin and the fixtures share no suffix and no
// name pattern — nothing about the string "fputest" says where it belongs,
// and forty-seven exact rules would be stale by Tuesday. It is the weakest
// claim in the precedence order (exact, then suffix, then lot, then `*`) so
// that one served directory can still hold files bound for several places.
//
// An explicit DEST on the command line beats the file;
// no file and no DEST means the current directory, as it always did, so the
// refresh that delivers a new os64get works before the one that delivers
// its conf. Lineage: rdist(1), 4.3BSD 1985 — the distfile names the
// destinations and the tool obeys.
//
// ── THE ARCHIVE (same day) ──────────────────────────────────────────────
//
// Before a file is installed it is KEPT, at
//
//     <archive>/YYYY-MM-DD/HHMMSS/<name>          (UTC, the clock's tongue)
//
// and the install is made FROM that copy. Chris was doing this by hand —
// every refresh, a copy to a dated folder, then a move — which is the
// surest sign a program should. The archive is what lets a bad build be
// walked back from a prompt, and every os64get run in the same second
// lands in one folder, so a scripted refresh reads as one install. The
// copy is removed if the install fails: the archive records what LANDED.
// (Plan 9's dump kept every day's tree under /n/dump/YYYY/MMDD; the
// instinct is the same, and so is the reason it lives on /home — the root
// partition is rewritten by every refresh and would torch the history.)
//
// ── THE PART THAT MATTERS: PUBLISHING ───────────────────────────────────
//
// Every file this program creates is written as `<target>.part` BESIDE its
// target and renamed into place only after its length and CRC32 check out.
// That ordering is the entire safety property, and it is why a rename
// syscall got built the morning the driver did:
//
//   - a transfer that dies halfway leaves a .part file and the PREVIOUS
//     version of the real name completely untouched;
//   - a transfer that completes but arrives damaged is DISCARDED, not
//     installed, because "complete" and "correct" are different claims and
//     only the checksum can tell them apart;
//   - the swap itself is atomic — os64's rename replaces the destination
//     with no instant at which the name fails to resolve, which is what
//     4.2BSD invented rename(2) for in 1983.
//
// BESIDE is load-bearing: rename works within ONE filesystem (the kernel
// refuses a cross-mount rename, correctly), and /home, /fat and / are three.
// So the archive copy gets its own .part next to it, the install gets its
// own .part next to IT, and the filesystem boundary is crossed by a plain
// copy in between — never by a rename. The install copy is checksummed as
// it is written, too: "refuse to install a bad one" covers the disk, not
// just the wire.
//
// That last property is what makes this safe to point at /bin. A refresh
// that replaces a program someone is running works: the running copy keeps
// demand-paging the inode it already holds (the ext2 orphan chain), and the
// new binary is there at the next launch.
//
// Exit codes name the step that failed, per the house fixture convention —
// a shell script driving a dozen of these should not have to parse English.

#include "os64/os64.h"
#include "os64/crc32.h"
#include "os64/conf.h"
#include "os64/date.h"

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
// Not a failure and not an install: the file already on disk is byte-for-byte
// what the server offered, so nothing was fetched and nothing was staged. It
// never escapes main (which reports it and exits 0) — it exists so the commit
// phase knows there is no `.part` waiting for it.
#define GET_UNCHANGED      12

#define GET_PORT      6464
// 64KB per write: the fetch loop fills this whole buffer from the socket
// before writing it, so every write() hands ext2 sixteen blocks it can put
// on the disk as a single run — however the wire delivered them. At 4KB
// the file was written a block per syscall, and the transfer waited on the
// disk, not the wire.
#define GET_CHUNK     65536
#define GET_PATH_MAX  256

// -a holds the server's whole catalogue in memory before fetching any of it,
// so that a list too long to hold fails BEFORE the system is half-replaced.
// 256 names is comfortably past the 66 the payload has today and past the 443
// app slots app_bases.py can hand out; the table is static (16KB of .bss),
// which is why it can afford to be generous.
#define GET_NAME_MAX  64
#define GET_MAX_LIST  256

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

// ── The config ──────────────────────────────────────────────────────────
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

// mkdir -p, the honest way: stat each prefix, make what is missing.
// os64_mkdir refuses an existing directory, which is right for mkdir and
// wrong for "make sure this path exists", so the stat comes first.
static bool ensure_dir(const char *path)
{
    char walk[GET_PATH_MAX];
    size_t n = os64_strcopy(walk, sizeof(walk), path);
    if (n == 0 || n >= sizeof(walk) - 1)
        return false;

    for (size_t i = 1; i <= n; i++) {
        if (walk[i] != '/' && walk[i] != '\0')
            continue;
        char saved = walk[i];
        walk[i] = '\0';
        os64_dirent_t e;
        if (os64_stat(walk, &e) < 0) {
            if (os64_mkdir(walk) < 0)
                return false;
        } else if (!(e.flags & OS64_DE_DIR)) {
            return false;   // a FILE where a directory is needed
        }
        walk[i] = saved;
    }
    return true;
}

// Build "<dir>/<name>" (or just name when dir is NULL). False if it won't fit.
static bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    int32_t n = dir ? os64_snprintf(out, cap, "%s/%s", dir, name)
                    : os64_snprintf(out, cap, "%s", name);
    return n > 0 && (size_t)n < cap;
}

// Copy `src` to `dst`, checksumming as it goes; `expect` bytes, `crc` must
// match at the end. Returns 0 or the GET_ code that names the failure. The
// destination is opened fresh ("w"), written, and synced — it is a .part
// file, so a failure here leaves the real name untouched.
static int copy_verified(const char *src, const char *dst, uint64_t expect, uint32_t want)
{
    int64_t in = os64_open(src, "r");
    if (in < 0)
        return GET_ARCHIVE_FAILED;
    int64_t out = os64_open(dst, "w");
    if (out < 0) {
        os64_close((int32_t)in);
        return GET_WRITE_FAILED;
    }

    static uint8_t buf[GET_CHUNK];
    uint64_t total = 0;
    uint32_t crc = os64_crc32_begin();
    int rc = GET_OK;
    for (;;) {
        int64_t n = os64_read((int32_t)in, buf, sizeof(buf));
        if (n < 0) { rc = GET_ARCHIVE_FAILED; break; }
        if (n == 0) break;
        if (os64_write((int32_t)out, buf, (size_t)n) != n) { rc = GET_WRITE_FAILED; break; }
        crc = os64_crc32_update(crc, buf, (size_t)n);
        total += (uint64_t)n;
    }
    if (rc == GET_OK && os64_sync((int32_t)out) < 0)
        rc = GET_WRITE_FAILED;
    os64_close((int32_t)in);
    os64_close((int32_t)out);

    if (rc == GET_OK && (total != expect || os64_crc32_end(crc) != want))
        rc = GET_CORRUPT;   // the disk lied between the archive and here
    return rc;
}

// ── Parsing the header ──────────────────────────────────────────────────

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

// ── DO WE ALREADY HAVE IT? ──────────────────────────────────────────────
//
// Read the file at `path` and say whether it is byte-for-byte what the server
// is offering. LENGTH AND CRC, both: a CRC alone is a one-in-four-billion lie,
// the pair is not.
//
// This is FABLE'S FEATURE (branch fable/dns-and-scripts, 2026-08-22, from an
// idea of Chris's), lifted onto this branch when Chris noticed it missing after
// the -a work landed — it had never been merged here, so it was not lost, just
// somewhere else. His comment there put the case better than I would:
//
//     "That is HTTP's If-None-Match / 304 (1997) and the entire reason rsync
//      exists: the cheapest byte to move is the one you already have, and on
//      the RTL8125 a kernel you already hold is four seconds you don't spend."
//
// And, presciently: "a future --all over twenty files wants 'unchanged' to be
// a yes". That future arrived the same day; see the two callers below.
static bool local_matches(const char *path, uint64_t wantLen, uint32_t wantCrc)
{
    int64_t have = os64_open(path, "r");
    if (have < 0)
        return false;                  // nothing there — everything to fetch

    uint64_t len = 0;
    uint32_t crc = os64_crc32_begin();
    uint8_t buf[GET_CHUNK];
    int64_t n;
    while ((n = os64_read((int32_t)have, buf, sizeof(buf))) > 0)
    {
        crc = os64_crc32_update(crc, buf, (size_t)n);
        len += (uint64_t)n;
    }
    os64_close((int32_t)have);

    // n == 0 is a clean end-of-file; a negative n is a read error, and an
    // unreadable local file is emphatically not a reason to skip the download.
    return n == 0 && len == wantLen && os64_crc32_end(crc) == wantCrc;
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
// "tcp!<host>!6464", built in ONE place because there are now two dialers —
// the fetch below and fetch_list's LIST — and a fix applied to one of two
// identical lines is the oldest bug in the trade.
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

static bool build_dialstring(char *buf, size_t cap, const char *host)
{
    int32_t n = os64_snprintf(buf, cap, "tcp!%s!%d", host, GET_PORT);
    if (n >= 0 && (size_t)n < cap)
        return true;

    os64_hprintf(OS64_STDERR, "os64get: host name is too long to dial (limit %d)\n",
                 OS64_RESOLVE_NAME_MAX);
    return false;
}

static const char *dial_reason(int64_t err)
{
    // The dial error codes are specific on purpose (os64/net.h) — a refusal
    // and a timeout mean very different things to whoever is standing at the
    // other machine, and printing "failed" for both wastes their next ten
    // minutes.
    // The last two arrived with the resolver (2026-08-22): a HOST may now be a
    // name, so "cannot reach" has two new ways to be true that have nothing to
    // do with the network being down.
    return (err == OS64_NET_ERR_REFUSED)      ? "connection refused — is the server running?" :
           (err == OS64_NET_ERR_TIMEOUT)      ? "timed out — is the host reachable?" :
           (err == OS64_NET_ERR_NO_NIC)       ? "no network interface on this boot" :
           (err == OS64_NET_ERR_BAD_ADDRESS)  ? "that host is not a dotted quad or a name" :
           (err == OS64_NET_ERR_NO_SUCH_HOST) ? "no such host — not in /home/hosts, /etc/hosts, or DNS" :
           (err == OS64_NET_ERR_NO_RESOLVER)  ? "that is a name, and there is no name server to ask (see /etc/net.conf)" :
           (err == OS64_NET_ERR_NO_RESOURCES) ? "out of handles or ports" :
                                                "refused";
}

// ── ONE FILE, FETCHED AND STAGED (but NOT installed) ────────────────────
//
// Everything from "where does it go" to "a verified copy is sitting beside it
// as <dest>.part" — and then it stops. Publishing is stage_commit's job,
// deliberately separated on 2026-08-22 (Chris's call, the day the payload
// reached 86 files):
//
//   "if this fails halfway through, it could get ugly. What about holding off
//    on moving all of the files into place until all 86 are transferred?"
//
// He is right, and the reason is worth stating plainly. Installing as you go
// means the window in which the machine is running half the old system and
// half the new one is the WHOLE TRANSFER — every second of network time, with
// a torn system as the outcome of any interruption. Staging everything first
// shrinks that window to the time it takes to rename 86 directory entries, and
// changes the failure mode completely: a transfer that dies at file 40 now
// leaves a system that is entirely, correctly the OLD one.
//
// It is not atomicity and this comment will not pretend otherwise — 86 renames
// can still be interrupted at rename 40. But the window goes from tens of
// seconds of network to milliseconds of metadata, and — the part that matters
// more — every failure that CAN be detected is detected before anything is
// published at all.
//
// `destOverride` is the command line's final word (NULL to let the conf route
// it). `lot` is the server's source label for this file, for the conf's
// `@lot` rules (NULL when unknown — the routing then uses the name alone).
// `archiveDir` is the ONE dated directory the whole run shares, so a
// refresh reads as a single install in the archive rather than 86 of them.
// `outDest` receives the resolved destination path so the caller can later
// commit or discard this file without re-deriving the routing.
static int fetch_stage(const char *host, const char *name, const char *destOverride,
                       const char *lot,
                       const conf_t *conf, const char *archiveDir, bool quiet, bool force,
                       char *outDest, size_t outDestCap)
{
    char dest[GET_PATH_MAX];
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
        if (!join_path(dest, sizeof(dest), intoDir ? destOverride : NULL,
                                           intoDir ? name : destOverride))
        {
            os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
            return GET_USAGE;
        }
    }
    else
    {
        const char *dir = conf_route(conf, name, lot);
        if (!join_path(dest, sizeof(dest), dir, name))
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

    // Hand the resolved destination back BEFORE anything can fail: the caller
    // needs it to sweep up a half-staged file just as much as to commit a
    // whole one, and a routing decision should be made exactly once.
    // os64_strcopy returns the length the source WANTED, so >= cap is the
    // truncation test (str.h's contract, and the reason strlcpy's semantics
    // were kept when its name was not).
    if (outDest != NULL && os64_strcopy(outDest, outDestCap, dest) >= outDestCap)
    {
        os64_hprintf(OS64_STDERR, "os64get: destination path too long\n");
        return GET_USAGE;
    }

    // ── Where it is kept ────────────────────────────────────────────────
    // The dated directory was made by the caller (one per RUN, not one per
    // file); this is just its name plus ours.
    char archiveFile[GET_PATH_MAX];
    bool archiving = archiveDir != NULL;
    if (archiving && !join_path(archiveFile, sizeof(archiveFile), archiveDir, name))
    {
        os64_hprintf(OS64_STDERR, "os64get: archive path too long\n");
        return GET_ARCHIVE_FAILED;
    }

    // THE WIRE ALWAYS LANDS BESIDE ITS DESTINATION (2026-08-22, second pass).
    //
    // It used to land in the ARCHIVE, which was then copied to the
    // destination — the archive as master, the install as its verified copy.
    // That reads well and it is the wrong way round for a MULTI-FILE refresh,
    // because it puts a full cross-filesystem copy (/home/archive -> /bin, a
    // different partition) inside the commit step. Committing 86 files would
    // then mean 86 copies of real data, and the whole point of committing at
    // the end is that the commit be as close to instantaneous as the
    // filesystem allows.
    //
    // Flipped: the wire is verified into `<dest>.part`, the archive is taken
    // as a verified copy OF that, and committing is one rename inside one
    // directory — no data moves at all. Both copies are still checksummed
    // against the wire, which is the property that actually mattered.
    char partPath[GET_PATH_MAX];
    int32_t pathlen = os64_snprintf(partPath, sizeof(partPath), "%s.part", dest);
    if (pathlen < 0 || (size_t)pathlen >= sizeof(partPath))
    {
        os64_hprintf(OS64_STDERR, "os64get: path is too long to append '.part'\n");
        return GET_WRITE_FAILED;
    }

    // ── Dial ────────────────────────────────────────────────────────────
    // Plan 9's bang path, which libos64 parses into an os64_netdest_t below
    // the syscall boundary (nothing textual crosses it).
    char dialstring[GET_DIAL_MAX];
    if (!build_dialstring(dialstring, sizeof(dialstring), host))
        return GET_USAGE;

    int64_t conn = os64_dial(dialstring);
    if (conn < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot reach %s:%d — %s\n",
                     host, GET_PORT, dial_reason(conn));
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
        int64_t n = os64_read((int32_t)conn, &c, 1);
        if (n != 1)
        {
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
    // Unchanged is SUCCESS and leaves NO archive entry, because nothing
    // landed. -f fetches regardless (a re-archive under today's date is the
    // honest reason to want that).
    //
    // `-a` normally settles this from the LIST manifest without dialing at
    // all, so this path is what the SINGLE-file fetch uses — and the backstop
    // for anything -a could not decide in advance.
    if (!force && local_matches(dest, expectLen, expectCrc))
    {
        os64_close((int32_t)conn);
        if (!quiet)
            os64_printf("%s: unchanged (%lu bytes, crc %08x) — not fetched\n",
                        dest, (unsigned long)expectLen, expectCrc);
        return GET_UNCHANGED;
    }

    // ── Receive into <target>.part ──────────────────────────────────────
    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_close((int32_t)conn);
        return archiving ? GET_ARCHIVE_FAILED : GET_WRITE_FAILED;
    }

    uint8_t buf[GET_CHUNK];
    uint64_t got = 0;
    uint32_t crc = os64_crc32_begin();
    int64_t status = GET_OK;

    while (got < expectLen)
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
        while ((uint64_t)n < want)
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

    os64_close((int32_t)out);

    if (status != GET_OK)
    {
        // Leave the .part behind ON PURPOSE. The real name still holds the
        // previous version, which is the point, and the fragment is evidence
        // — a human debugging a failed refresh wants to look at it, and
        // deleting it to be tidy would throw that away.
        return (int)status;
    }

    // ── Verify, THEN publish the wire copy ──────────────────────────────
    uint32_t actual = os64_crc32_end(crc);
    if (actual != expectCrc)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: CHECKSUM MISMATCH for %s — got %08x, expected %08x. "
                     "%s left in place; %s NOT installed.\n",
                     name, actual, expectCrc, partPath, dest);
        return GET_CORRUPT;
    }

    // ── Keep a copy, before anything is published ───────────────────────
    // The archive is a verified copy OF the staged file (the disk gets no
    // more trust than the wire: it is read back and checksummed again). It is
    // written now, while nothing is committed, so that a run which never
    // commits leaves the archive as untouched as the system — see
    // stage_discard, which removes it if this file's run is abandoned.
    if (archiving)
    {
        char archivePart[GET_PATH_MAX];
        int rc = GET_OK;
        if (os64_snprintf(archivePart, sizeof(archivePart), "%s.part", archiveFile) >= (int32_t)sizeof(archivePart))
            rc = GET_WRITE_FAILED;
        else
            rc = copy_verified(partPath, archivePart, expectLen, expectCrc);

        if (rc == GET_OK && os64_rename(archivePart, archiveFile) < 0)
        {
            os64_hprintf(OS64_STDERR,
                         "os64get: could not rename %s to %s (read-only filesystem?)\n",
                         archivePart, archiveFile);
            rc = GET_ARCHIVE_FAILED;
        }

        if (rc != GET_OK)
        {
            os64_hprintf(OS64_STDERR, "os64get: could not archive %s — %s NOT installed\n",
                         name, dest);
            if (os64_unlink(partPath) < 0)
                os64_hprintf(OS64_STDERR, "os64get: (and %s could not be removed — remove it by hand)\n",
                             partPath);
            return rc;
        }
    }

    if (!quiet)
        os64_printf("%s: %lu bytes, crc %08x, verified%s\n",
                    dest, (unsigned long)expectLen, actual,
                    archiving ? " and kept" : "");
    return GET_OK;
}

// ── COMMIT, AND ABANDON ─────────────────────────────────────────────────
//
// The two halves of the second phase. Staging leaves a verified `<dest>.part`
// beside every destination; these either publish it or sweep it away.
//
// commit is ONE rename inside ONE directory: no data moves, nothing is read,
// and before the call the old file is whole while after it the new one is —
// there is no instant in between. That is what makes pointing this at /bin a
// reasonable thing to do rather than a brave one, and it is why an 86-file
// refresh can now flip the whole system over in the time it takes to write 86
// directory entries instead of the time it takes to cross a network.
static int stage_commit(const char *dest)
{
    char partPath[GET_PATH_MAX];
    if (os64_snprintf(partPath, sizeof(partPath), "%s.part", dest) >= (int32_t)sizeof(partPath))
        return GET_WRITE_FAILED;

    if (os64_rename(partPath, dest) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: %s verified but could not be renamed to %s "
                     "(read-only filesystem, or a directory in the way?)\n",
                     partPath, dest);
        return GET_PUBLISH_FAILED;
    }
    return GET_OK;
}

// Undo a staged file: the .part goes, and so does its archive entry. The rule
// the archive has always followed — only a successful install earns an entry —
// applies to an abandoned refresh exactly as it applied to a single failed
// file. The dated directory itself stays; it costs nothing and an empty one is
// its own small record that a refresh was attempted and thrown away.
static void stage_discard(const char *dest, const char *archiveDir, const char *name)
{
    char path[GET_PATH_MAX];

    if (os64_snprintf(path, sizeof(path), "%s.part", dest) < (int32_t)sizeof(path))
        os64_unlink(path);   // may not exist (this file may never have staged) — fine

    if (archiveDir != NULL && join_path(path, sizeof(path), archiveDir, name))
        os64_unlink(path);
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
// The length and crc are not used to decide anything yet; they are here
// because they cost the server nothing, they let this side print an honest
// "66 files, 3.4MB" before committing to the transfer, and they are exactly
// what a future "skip the ones I already have" would need. (Deliberately NOT
// implemented today: the whole payload is under 4MB and the wire does 392KB/s,
// so the entire refresh is ten seconds. Booked, not built.)
//
// Returns the number of names read, or -1. Names beyond `max` are refused
// loudly rather than silently dropped — a refresh that quietly skips the tail
// of the system is worse than one that fails.
static int32_t fetch_list(const char *host, get_entry_t *entries, int32_t max,
                          uint64_t *totalBytes)
{
    char dialstring[GET_DIAL_MAX];
    if (!build_dialstring(dialstring, sizeof(dialstring), host))
        return -1;

    int64_t conn = os64_dial(dialstring);
    if (conn < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot reach %s:%d to ask what it has — %s\n",
                     host, GET_PORT, dial_reason(conn));
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
            int64_t n = os64_read((int32_t)conn, &c, 1);
            if (n != 1)
            {
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

// One file's lot, for the single-file fetch that would otherwise never learn
// it. `-a` already holds every lot in the catalogue it asked for; a lone
// `os64get HOST fputest` asks only for the file.
//
// A LIST that fails is NOT fatal here and must not be: the caller proceeds
// name-routed, which is exactly what os64get did before lots existed. Failing
// the fetch because an optional refinement was unavailable would trade a
// working command for a tidier one.
static void lookup_lot(const char *host, const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    uint64_t ignored = 0;
    int32_t n = fetch_list(host, entries, GET_MAX_LIST, &ignored);
    for (int32_t i = 0; i < n; i++)
        if (os64_streq(entries[i].name, name))
        {
            os64_strcopy(out, cap, entries[i].lot);
            return;
        }
}

int main(int argc, char **argv)
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
        {'n', "no-archive", false, "install only; keep no archive copy", .flag = &noArchive},
        {'a', "all", false, "fetch EVERY file the server offers, routing each by the conf", .flag = &all},
        {'f', "force", false, "fetch even files already identical on disk", .flag = &force},
        {'c', "changes-only", false, "display only changed files", .flag = &flgChangesOnly}};

    os64_args_init(&args, argc, argv, specs, 5);
    args.about = "Fetch a file over the network, archive it, and install it only if it is intact.";
    args.details = "DEST is a directory to install into, or the full path to install as; "
                   "it defaults to the directory /etc/os64get.conf names for NAME (or the cwd). "
                   "Keeps <archive>/DATE/TIME/NAME first, then installs from that copy via DEST.part + rename. "
                   "With -a, asks the server what it has and fetches all of it — the whole-system refresh.";

    int32_t count = os64_args_parse(&args, "os64get [-q] [-n] [-f] HOST NAME [DEST]  |  os64get -a [-q] [-n] [-f] HOST",
                                    operands, 4);
    if (count == OS64_ARG_HELP)
        return GET_OK;
    if (count < 1 || (!all && count < 2))
    {
        if (count != OS64_ARG_ERROR)
            os64_hprintf(OS64_STDERR, all ? "os64get: need a HOST\n"
                                          : "os64get: need a HOST and a NAME\n");
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

    // ── Where things go ─────────────────────────────────────────────────
    static conf_t conf;   // static: two 16-entry tables are too fat for the stack
    conf_load(&conf);

    // ── The archive directory: ONE per run ──────────────────────────────
    // Decided here, before the wire, so that a missing /home fails loudly
    // before a single byte is fetched rather than after the last one — and so
    // that every file of a refresh lands in the same dated folder, which is
    // what makes the archive readable as "this is the system as of then".
    char archiveDir[GET_PATH_MAX];
    bool archiving = !noArchive && conf.archive[0] != '\0';
    if (archiving)
    {
        os64_time_t now;
        os64_date_t d;
        if (os64_time(&now) < 0)
        {
            os64_hprintf(OS64_STDERR, "os64get: cannot read the clock for the archive path\n");
            return GET_ARCHIVE_FAILED;
        }
        os64_date_from_epoch(now.epoch, &d);   // UTC: the system clock's tongue (the clock ruling)

        int32_t n = os64_snprintf(archiveDir, sizeof(archiveDir), "%s/%04d-%02d-%02d/%02d%02d%02d",
                                  conf.archive, d.year, d.month, d.day, d.hour, d.minute, d.second);
        if (n <= 0 || (size_t)n >= sizeof(archiveDir))
        {
            os64_hprintf(OS64_STDERR, "os64get: archive path too long\n");
            return GET_ARCHIVE_FAILED;
        }
        if (!ensure_dir(archiveDir))
        {
            os64_hprintf(OS64_STDERR, "os64get: cannot create archive directory %s "
                         "(is %s mounted? pass -n to install without archiving)\n",
                         archiveDir, conf.archive);
            return GET_ARCHIVE_FAILED;
        }
    }

    if (!all)
    {
        // One file: stage and commit in the same breath. There is nothing to
        // hold back FOR — the two-phase dance below exists to keep a
        // MULTI-file refresh from tearing, and a single file has no siblings
        // to be inconsistent with.
        // A lone fetch asks the server for one file and learns nothing else,
        // so without this it would route a fixture by name alone and quietly
        // install it in /bin. The extra listing is spent ONLY when it can
        // change the answer — never when the command line already spelled a
        // destination, and never when the conf has no `@lot` rule to apply.
        char lot[GET_LOT_MAX] = {0};
        if (count < 3 && conf.nlot > 0)
            lookup_lot(host, operands[1], lot, sizeof(lot));

        char dest[GET_PATH_MAX] = {0};
        int rc = fetch_stage(host, operands[1], count >= 3 ? operands[2] : NULL,
                             lot, &conf, archiving ? archiveDir : NULL, quiet, force,
                             dest, sizeof(dest));
        if (rc == GET_UNCHANGED)
            return GET_OK;   // nothing fetched, nothing staged, nothing to say
        if (rc != GET_OK)
        {
            if (dest[0] != '\0')
                stage_discard(dest, archiving ? archiveDir : NULL, operands[1]);
            return rc;
        }
        rc = stage_commit(dest);
        if (rc != GET_OK)
            stage_discard(dest, archiving ? archiveDir : NULL, operands[1]);
        else if (!quiet)
            os64_printf("%s: installed\n", dest);
        return rc;
    }

    // ── The whole system, in one command ────────────────────────────────
    uint64_t totalBytes = 0;
    int32_t n = fetch_list(host, entries, GET_MAX_LIST, &totalBytes);
    if (n < 0)
        return GET_DIAL_FAILED;
    if (n == 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: the server offers nothing — is it serving the right directories?\n");
        return GET_OK;
    }

    if (!quiet)
        os64_printf("os64get: %ld files, %lu bytes, from %s\n",
                    (long)n, (unsigned long)totalBytes, host);

    // ── PHASE ONE: fetch everything, install nothing ────────────────────
    //
    // Every file is attempted even after one fails, so that ONE run tells you
    // everything that is wrong rather than making you discover the problems
    // one reboot at a time. Nothing is published while this loop runs: each
    // file ends up as a verified `<dest>.part` beside where it will go.
    static char dests[GET_MAX_LIST][GET_PATH_MAX];   // static: 64KB, far too fat for a stack
    static bool staged[GET_MAX_LIST];
    int32_t failed = 0, unchanged = 0;
    for (int32_t i = 0; i < n; i++)
    {
        dests[i][0] = '\0';
        staged[i] = false;

        // THE MANIFEST DECIDES FIRST. LIST already told us every file's length
        // and CRC, so a file we already hold is settled WITHOUT DIALING AT ALL
        // — no connection, no header, no bytes. That is the whole point of
        // putting the sizes in the catalogue, and it turns the second refresh
        // of an unchanged tree from 86 transfers into 86 local checksums.
        // (fetch_stage still carries the same test against the header, for the
        // single-file case and for any server that lists bare names.)
        if (!force && entries[i].length != 0)
        {
            const char *dir = conf_route(&conf, entries[i].name, entries[i].lot);
            char probe[GET_PATH_MAX];
            if (join_path(probe, sizeof(probe), dir, entries[i].name) &&
                local_matches(probe, entries[i].length, entries[i].crc))
            {
                unchanged++;
                if (!quiet && !flgChangesOnly)
                    os64_printf("[%ld/%ld] %s — unchanged\n", (long)(i + 1), (long)n, entries[i].name);
                continue;
            }
        }

        if (!quiet)
            os64_printf("[%ld/%ld] %s\n", (long)(i + 1), (long)n, entries[i].name);
        int rc = fetch_stage(host, entries[i].name, NULL, entries[i].lot, &conf,
                             archiving ? archiveDir : NULL, quiet, force,
                             dests[i], sizeof(dests[i]));
        if (rc == GET_OK)
            staged[i] = true;
        else if (rc == GET_UNCHANGED)
            unchanged++;          // the header settled what the manifest could not
        else
        {
            failed++;
            os64_hprintf(OS64_STDERR, "os64get: %s FAILED (%d)\n", entries[i].name, rc);
        }
    }

    // ── THE DECISION ────────────────────────────────────────────────────
    // All or nothing. A system that is 40/86 new is not a system anybody can
    // reason about — least of all the person who has to fix it, on the
    // machine that just half-updated, with the old tools half gone.
    if (failed != 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: %ld of %ld files did not arrive — INSTALLING NOTHING. "
                     "The system is untouched; fix the problem and run it again.\n",
                     (long)failed, (long)n);
        for (int32_t i = 0; i < n; i++)
            if (dests[i][0] != '\0')
                stage_discard(dests[i], archiving ? archiveDir : NULL, entries[i].name);
        return GET_CORRUPT;
    }

    // ── PHASE TWO: publish, as fast as the filesystem can ───────────────
    //
    // Renames only, one per file, each inside a single directory: no data
    // moves and nothing is read. This is the whole window in which the
    // machine is part-old and part-new, and it is now measured in directory
    // writes rather than in network seconds.
    //
    // A failure HERE is different in kind from a failure above, and is
    // handled differently: we are already committed, and un-renaming the
    // files that succeeded would just be a second half-installed system
    // arrived at the long way round. So each failure is reported and the rest
    // still go in — finishing is strictly better than stopping, and the exit
    // code says plainly that something needs a human.
    int32_t installed = 0, unpublished = 0;
    for (int32_t i = 0; i < n; i++)
    {
        if (!staged[i])
            continue;              // unchanged: there is no .part to publish
        if (stage_commit(dests[i]) == GET_OK)
            installed++;
        else
        {
            unpublished++;
            os64_hprintf(OS64_STDERR, "os64get: %s could not be published — it is staged at %s.part\n",
                         dests[i], dests[i]);
        }
    }

    os64_printf("os64get: %ld installed, %ld unchanged%s%s%s\n",
                (long)installed, (long)unchanged,
                archiving && installed ? ", kept at " : "",
                archiving && installed ? archiveDir : "",
                unpublished ? " — SOME FILES COULD NOT BE PUBLISHED (see above)" : "");
    if (!quiet && installed > 0)
        os64_printf("os64get: a replaced /lib/libos64.so serves the OLD code until reboot "
                    "(the loader keeps it resident) — reboot to finish.\n");

    return unpublished == 0 ? GET_OK : GET_PUBLISH_FAILED;
}
