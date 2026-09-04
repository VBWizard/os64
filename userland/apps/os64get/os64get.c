// os64get.c — fetch a file over the wire, put it where it belongs, keep a copy,
// and refuse to install a bad one.
//
// THE MISSION, in one line: `os64 refresh` without a thumb drive. Until
// 2026-08-16 the only way a fresh build reached the P5 was a human carrying a
// USB stick across a room; since 2026-08-21 the P5 boots from its own disk
// and this program is the whole supply line — kernel, menu, bootloader, apps.
//
// THE SUPPLY LINE'S PROTOCOL is RTL8125.md's, deliberately 1971-shaped — one
// TCP connection per file, ASCII where a human might read it, binary only
// where a machine must:
//
//     client -> server:   GET <name>\n
//     server -> client:   OK <length-decimal> <crc32-hex8>\n  then <length> bytes
//                    or:  NO <reason>\n
//
// You can drive it by hand from any telnet client, which is not nostalgia:
// a protocol you can type is a protocol you can debug at 1am on a machine
// with no tooling.
//
// AND SINCE 2026-09-02 THERE IS A SECOND DIALECT: an operand shaped like
// `http://host[:port]/path` fetches from the world instead of from the
// valet. It is a different conversation with different guarantees — no
// checksum, no routing, no archive — and it is kept apart from the first
// rather than braided through it; see THE WORLD'S DIALECT below, and http.h
// for the protocol itself. This program is BROWSER.md's third rung: the
// ladder that climbs toward a browser by pointing os64's TCP at traffic the
// last rung did not.
//
// An `https://` operand is the same dialect reaching somewhere os64 cannot
// go alone: this machine has no TLS and never will (BROWSER.md borrows one
// when its day comes), so an https fetch is carried by a proxy that DOES
// have a TLS, named by `$https_proxy`. See THE TLS PROXY below for what that
// buys and what it costs.
//
// ── WHERE A VALET FILE GOES (2026-08-22) ────────────────────────────────
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
// ── THE ARCHIVE, WHICH IS THE VALET'S TOO (same day) ───────────────────
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
// target and renamed into place only after what CAN be checked has been.
// That ordering is the entire safety property, and it is why a rename
// syscall got built the morning the driver did:
//
//   - a transfer that dies halfway leaves a .part file and the PREVIOUS
//     version of the real name completely untouched;
//   - a valet transfer that completes but arrives damaged is DISCARDED, not
//     installed, because "complete" and "correct" are different claims and
//     only the checksum can tell them apart;
//   - on ext2, the swap itself is atomic — rename replaces the destination
//     with no instant at which the name fails to resolve. FAT's legacy
//     replacement has a remove-first window, so this caller does not promise
//     crash-safe publication there.
//
// HOW MUCH THE SECOND BULLET CAN PROMISE DEPENDS ON THE DIALECT, and the
// difference is the server's, not this program's. The valet names a CRC, so
// "correct" is answerable. HTTP names a length and nothing else, so a URL
// fetch can prove a body was not CUT SHORT and cannot prove it arrived
// intact — which is exactly what it says, and why it claims no more.
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

#include "http.h"

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
// The operand carried a "scheme://" and was still not a URL this program can
// fetch — an unknown scheme (https, until os64 borrows a TLS), or a shape a
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

// ── DO WE ALREADY HAVE IT? (the valet's unchanged check) ───────────────
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
// The two halves of the second phase. Staging leaves a checked `<dest>.part`
// beside every destination; these either publish it or sweep it away. Both
// verbs serve the valet; a URL fetch borrows commit and keeps its own .part
// (there is no archive copy behind it to make sweeping one away safe).
//
// commit is ONE rename inside ONE directory: no data moves and nothing is
// read. On ext2, before the call the old file is whole and after it the new one
// is, with no instant in between. That is what makes pointing this at /bin on
// the ext2 root a reasonable thing to do rather than a brave one, and it is
// why an 86-file refresh can now flip the whole system over in the time it
// takes to write 86 directory entries instead of crossing a network. A FAT
// destination retains syscall 43's documented remove-first window.
static int stage_commit(const char *dest)
{
    char partPath[GET_PATH_MAX];
    if (os64_snprintf(partPath, sizeof(partPath), "%s.part", dest) >= (int32_t)sizeof(partPath))
        return GET_WRITE_FAILED;

    if (os64_rename(partPath, dest) < 0)
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: %s is staged but could not be renamed to %s "
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
//   - IT DOES NOT ARCHIVE. The archive is the supply line's record of what
//     landed on this machine and when, so a bad build can be walked back by
//     hand. A download is not an install and has nothing to walk back to.
//   - IT HAS NO CHECKSUM, so it cannot tell "complete" from "correct", and
//     says so rather than implying otherwise. HTTP offers a length or chunk
//     framing; a body cut before either is satisfied fails loudly, and
//     everything past that is the server's word.
//   - IT DOES NOT LET A REDIRECT NAME THE FILE. Following one is ordinary
//     (see url_ask); letting the far end choose what appears in somebody's
//     directory is not, so the name is settled from the typed address before
//     the first request goes out.
//
// What it KEEPS is the property this whole program is built around: the bytes
// land in `<dest>.part`, and only a complete body reaches the real name. On
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
// parser, the body loop, the .part discipline and every refusal above are
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
            // says so. Past it, the machine that cannot be reached is one a
            // server chose, and a script must be able to tell those apart:
            // the road did not arrive, which is what 15 means (Codex review
            // of PR #60).
            return hops > 0 ? GET_REDIRECT : GET_DIAL_FAILED;
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

    char partPath[GET_PATH_MAX];
    if (os64_snprintf(partPath, sizeof(partPath), "%s.part", dest) >= (int32_t)sizeof(partPath))
    {
        os64_hprintf(OS64_STDERR, "os64get: path is too long to append '.part'\n");
        return GET_WRITE_FAILED;
    }

    url_reply_t answer;
    int rc = url_ask(&answer, url, urlText, proxy, quiet);
    if (rc != GET_OK)
        return rc;

    // A FRAMING OR A CODING THIS PROGRAM CANNOT UNDO MUST NEVER BECOME A
    // FILE. What would land is the envelope wearing the letter's name, and a
    // `.html` full of DEFLATE is worse than no file at all, because it looks
    // like a successful download. The rule outlives the list: whatever this
    // program learns to read moves out of these branches by being handled
    // (chunked did), and whatever it has not learned is refused by name.
    http_body_t body;
    if (!http_body_open(&body, &answer.stream, &answer.reply))
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: the reply is framed as '%s', which os64get does not read"
                     " — nothing written\n", answer.reply.transferEncoding);
        os64_close(answer.conn);
        return GET_UNSUPPORTED;
    }
    if (answer.reply.contentEncoding[0] != '\0' &&
        !os64_streq(answer.reply.contentEncoding, "identity"))
    {
        os64_hprintf(OS64_STDERR,
                     "os64get: the reply is encoded as '%s', which os64get does not decode yet"
                     " — nothing written\n", answer.reply.contentEncoding);
        os64_close(answer.conn);
        return GET_UNSUPPORTED;
    }

    // ── Receive into <dest>.part ────────────────────────────────────────
    int64_t out = os64_open(partPath, "w");
    if (out < 0)
    {
        os64_hprintf(OS64_STDERR, "os64get: cannot create %s\n", partPath);
        os64_close(answer.conn);
        return GET_WRITE_FAILED;
    }

    // THE FRAMING IS THE BODY READER'S BUSINESS (http.h). What comes out of
    // it is the file's bytes and nothing else, and when it answers 0 its
    // result says whether the body ENDED or was merely STOPPED — a
    // distinction only a length or chunked framing can draw. Without either,
    // the close is the length (HTTP/1.0's original framing, RFC 1945 §7.2.2,
    // and the reason `Connection: close` is in the request): a reply with no
    // length is still readable, it just cannot be told apart from one that
    // was cut short.
    uint8_t buf[GET_CHUNK];
    uint64_t got = 0;
    int status = GET_OK;
    bool over = false;

    while (!over)
    {
        // Fill the buffer before writing it, for the disk's sake: a socket
        // read answers with what has ARRIVED — a segment, or a scheduler
        // pass's worth — and writing each of those hands ext2 a block or two
        // at a time. Progress ticks from INSIDE the fill, every 4KB of
        // arrival, so a slow link reads as slow rather than as hung.
        size_t filled = 0;
        while (filled < sizeof(buf))
        {
            int64_t n = http_body_read(&body, buf + filled, sizeof(buf) - filled);
            if (n <= 0) { over = true; break; }
            filled += (size_t)n;
            uint64_t staged = got + filled;
            if (!quiet && (staged % 4096 < (uint64_t)n ||
                           (answer.reply.hasLength && staged == answer.reply.length)))
            {
                if (answer.reply.hasLength)
                    os64_printf("\r%s: %lu/%lu bytes", name,
                                (unsigned long)staged, (unsigned long)answer.reply.length);
                else
                    os64_printf("\r%s: %lu bytes", name, (unsigned long)staged);
            }
        }

        if (filled > 0 && os64_write((int32_t)out, buf, filled) != (int64_t)filled)
        {
            os64_hprintf(OS64_STDERR, "os64get: write to %s failed (disk full?)\n", partPath);
            status = GET_WRITE_FAILED;
            break;
        }
        got += filled;
    }
    if (!quiet)
    {
        // The meter's last tick: a length-framed body printed it when the
        // count came due, but a chunked or close-delimited body learns its
        // total only now, and a meter that stops at the last 4KB boundary
        // reads as a transfer that stopped short.
        if (!answer.reply.hasLength)
            os64_printf("\r%s: %lu bytes", name, (unsigned long)got);
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
                             " after %lu bytes\n", URL_IDLE_MS / 1000, (unsigned long)got);
            else
                os64_hprintf(OS64_STDERR, "os64get: the connection broke after %lu bytes\n",
                             (unsigned long)got);
            status = GET_SHORT;
            break;
        case HTTP_BODY_CUT:
            if (answer.reply.hasLength)
                os64_hprintf(OS64_STDERR, "os64get: the reply ended after %lu of %lu bytes\n",
                             (unsigned long)got, (unsigned long)answer.reply.length);
            else
                os64_hprintf(OS64_STDERR, "os64get: the reply ended after %lu bytes,"
                             " before its last chunk\n", (unsigned long)got);
            status = GET_SHORT;
            break;
        default:
            // The server's chunk framing stopped being HTTP: "that was not
            // speech", the same verdict a broken head earns.
            os64_hprintf(OS64_STDERR, "os64get: after %lu bytes, %s\n",
                         (unsigned long)got, http_body_reason(body.result));
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
    os64_close((int32_t)out);

    if (status != GET_OK)
    {
        // The fragment stays as <dest>.part ON PURPOSE, exactly as it does on
        // the valet's path: whatever was at the real name is still there and
        // still whole, and the fragment is evidence for whoever is debugging
        // the failure.
        return status;
    }

    // A failed publish keeps the .part too — and here that is a COMPLETE
    // download, not a fragment. The valet's path can sweep one away because
    // the archive holds a verified copy; nothing holds this one but itself.
    rc = stage_commit(dest);
    if (rc != GET_OK)
    {
        os64_hprintf(OS64_STDERR, "os64get: the download is whole, at %s\n", partPath);
        return rc;
    }

    // THE HOST NAMED IS THE ONE THAT SERVED IT, which after a redirect is not
    // the one that was typed — that is the whole point of saying it.
    if (!quiet)
        os64_printf("%s: %lu bytes from %s\n", dest, (unsigned long)got,
                    answer.answered.host);
    return GET_OK;
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
    args.about = "Fetch a file over the network: the build valet's supply line, or any http:// address.";
    args.details = "DEST is a directory to install into, or the full path to install as; "
                   "it defaults to the directory /etc/os64get.conf names for NAME (or the cwd). "
                   "Keeps <archive>/DATE/TIME/NAME first, then installs from that copy via DEST.part + rename. "
                   "With -a, asks the server what it has and fetches all of it — the whole-system refresh. "
                   "An http:// or https:// URL fetches from the world instead: no routing, no archive "
                   "and no checksum (HTTP offers none), landing in the current directory under the "
                   "URL's own last path segment unless DEST says otherwise. A redirect is followed, "
                   "up to five of them, and never gets to choose the file's name. https needs $https_proxy "
                   "set to a machine that has a TLS, since os64 has none; $no_proxy lists the hosts "
                   "that skip it. -a is the valet's verb, and on a URL fetch -n and -f have nothing "
                   "to act on (no archive, no unchanged check).";

    int32_t count = os64_args_parse(&args,
                                    "os64get [-q] [-n] [-f] HOST NAME [DEST]  |  os64get -a [-q] [-n] [-f] HOST"
                                    "  |  os64get [-q] http://HOST/PATH [DEST]",
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

    // ── WHICH DIALECT? THE OPERAND ANSWERS ──────────────────────────────
    // A URL carries "scheme://" and a valet operand is a bare word, so the
    // two never have to be told apart by guesswork. Anything WITH a scheme
    // is committed to the world's path — including a scheme this program
    // cannot speak, which earns an honest refusal rather than a silent
    // fallthrough into the valet's dialect (where "https://x/y" would have
    // been dialled as a host name).
    http_url_t url;
    http_url_result_t urc = http_url_parse(operands[0], &url);
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
            lookup_lot(host, operands[1], &conf, lot, sizeof(lot));

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

    // Before a single byte moves: a whole-system refresh routing everything by
    // name when the conf expects lots is worth interrupting for, and NOT
    // behind !quiet — a warning you silenced along with the progress is a
    // warning you will not see on the run that mattered.
    warn_if_server_has_no_lots(&conf, n);

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
